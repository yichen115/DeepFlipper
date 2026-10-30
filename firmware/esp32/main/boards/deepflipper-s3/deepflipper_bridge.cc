#include "deepflipper_bridge.h"
#include <driver/ledc.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "config.h"
#include "device_setup.h"
#include "native_policy.h"
#include "settings.h"
#include "system_info.h"
#include "voice_endpoint.h"

namespace {
constexpr uart_port_t kUart = UART_NUM_1;
constexpr const char* kLogTag = "FlipperLink";
std::string Serialize(cJSON* object) {
    char* text = cJSON_PrintUnformatted(object);
    std::string result = text ? text : "{}";
    cJSON_free(text);
    return result;
}
std::string String(cJSON* object, const char* key) {
    auto item = cJSON_GetObjectItem(object, key);
    return cJSON_IsString(item) ? item->valuestring : "";
}
int Integer(cJSON* object, const char* key, int fallback = 0) {
    auto item = cJSON_GetObjectItem(object, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}
cJSON* Profile() { return cJSON_Parse(DeviceSetup::Get().Profile().c_str()); }
}  // namespace

DeepFlipperBridge& DeepFlipperBridge::Get() {
    static DeepFlipperBridge bridge;
    return bridge;
}

void DeepFlipperBridge::RestoreSettings() {
    auto p = Profile();
    configured_ = deepflipper::ValidVoiceEndpoint(String(p, "url")) && !String(p, "token").empty();
    muted_ = cJSON_IsTrue(cJSON_GetObjectItem(p, "mute"));
    auto mic = cJSON_GetObjectItem(p, "microphone");
    microphone_ = !cJSON_IsFalse(mic);
    brightness_ = std::clamp(Integer(p, "led_brightness", 12), 0, 100);
    {
        Settings ws("websocket", true);
        ws.SetString("url", String(p, "url"));
        ws.SetString("token", String(p, "token"));
        ws.SetInt("version", 1);
    }
    {
        Settings audio("audio", true);
        audio.SetInt("output_volume", muted_ ? 0 : std::clamp(Integer(p, "volume", 20), 0, 100));
    }
    cJSON_Delete(p);
}

void DeepFlipperBridge::Start() {
    if (started_)
        return;
    started_ = true;
    uart_config_t uart{};
    uart.baud_rate = dfexpansion::kDiscoveryBaud;
    uart.data_bits = UART_DATA_8_BITS;
    uart.parity = UART_PARITY_DISABLE;
    uart.stop_bits = UART_STOP_BITS_1;
    uart.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_driver_install(kUart, 2048, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(kUart, &uart));
    ESP_ERROR_CHECK(
        uart_set_pin(kUart, DF_UART_TX, DF_UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ledc_timer_config_t timer{};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = LEDC_TIMER_10_BIT;
    timer.timer_num = LEDC_TIMER_2;
    timer.freq_hz = 4000;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    const gpio_num_t pins[] = {DF_LED_R, DF_LED_G, DF_LED_B};
    for (int i = 0; i < 3; i++) {
        ledc_channel_config_t c{};
        c.gpio_num = pins[i];
        c.speed_mode = LEDC_LOW_SPEED_MODE;
        c.channel = static_cast<ledc_channel_t>(LEDC_CHANNEL_3 + i);
        c.timer_sel = LEDC_TIMER_2;
        ESP_ERROR_CHECK(ledc_channel_config(&c));
    }
    xTaskCreate([](void* arg) { static_cast<DeepFlipperBridge*>(arg)->Run(); }, "flipper_link",
                6144, this, 3, &task_);
}

bool DeepFlipperBridge::Frame(uint8_t type, const uint8_t* data, size_t size) {
    std::vector<uint8_t> frame{type};
    if (type == 5)
        frame.push_back(size);
    if (size)
        frame.insert(frame.end(), data, data + size);
    uint8_t checksum = 0;
    for (auto byte : frame)
        checksum ^= byte;
    frame.push_back(checksum);
    return uart_write_bytes(kUart, frame.data(), frame.size()) == static_cast<int>(frame.size());
}

bool DeepFlipperBridge::ReadFrame(int timeout_ms) {
    int64_t deadline = esp_timer_get_time() + timeout_ms * 1000LL;
    do {
        if (rpc_transport_active_ && esp_timer_get_time() - last_heartbeat_us_ >= 100000) {
            last_heartbeat_us_ = esp_timer_get_time();
            if (!Frame(1, nullptr, 0)) {
                session_ = 0;
                return false;
            }
        }
        uint8_t byte;
        if (uart_read_bytes(kUart, &byte, 1, pdMS_TO_TICKS(5)) != 1)
            continue;
        rx_frame_.push_back(byte);
        uint8_t type = rx_frame_[0];
        if (type == 5 && rx_frame_.size() >= 2 && rx_frame_[1] > 64) {
            rx_frame_.clear();
            session_ = 0;
            return false;
        }
        size_t expected = type == 1 ? 2 : type == 2 || type == 4 ? 3 : type == 3 ? 6 : 0;
        if (type == 5 && rx_frame_.size() >= 2)
            expected = rx_frame_[1] <= 64 ? rx_frame_[1] + 3 : 0;
        if ((!expected && type != 5) || rx_frame_.size() > 70) {
            rx_frame_.clear();
            session_ = 0;
            return false;
        }
        if (expected && rx_frame_.size() == expected) {
            uint8_t sum = 0;
            for (auto b : rx_frame_)
                sum ^= b;
            if (sum) {
                rx_frame_.clear();
                session_ = 0;
                return false;
            }
            auto frame = std::move(rx_frame_);
            rx_frame_.clear();
            Dispatch(frame);
            return true;
        }
    } while (esp_timer_get_time() < deadline);
    return false;
}

void DeepFlipperBridge::Dispatch(const std::vector<uint8_t>& f) {
    if (f[0] == 1 && !rpc_transport_active_)
        discovery_received_ = true;
    if (f[0] == 2) {
        status_received_ = true;
        status_ok_ = f[1] == 0;
    }
    if (f[0] != 5)
        return;
    uint8_t ok = 0;
    Frame(2, &ok, 1);
    rpc_stream_.insert(rpc_stream_.end(), f.begin() + 2, f.end() - 1);
    if (rpc_stream_.size() > dfwire::kMaxMessage + 128) {
        rpc_stream_.clear();
        session_ = 0;
        rpc_transport_active_ = false;
        return;
    }
    while (!rpc_stream_.empty()) {
        dfwire::Message message;
        auto next = dfwire::Next(rpc_stream_, message);
        if (next == dfwire::NextResult::NeedMore)
            break;
        if (next == dfwire::NextResult::Invalid) {
            rpc_stream_.clear();
            session_ = 0;
            rpc_transport_active_ = false;
            return;
        }
        if (message.tag == 501) {
            std::string json;
            if (dfwire::Envelope(message, json)) {
                response_id_ = message.id;
                response_ = std::move(json);
            } else if (message.id == rpc_id_) {
                response_id_ = message.id;
                response_ = "{\"ok\":false,\"code\":\"invalid_response\"}";
            }
        } else if (message.tag == 58 && message.id == 0) {
            dfwire::Reader reader(message.payload);
            dfwire::Field field;
            bool running = false;
            while (reader.Next(field))
                if (field.tag == 1 && field.wire == 0)
                    running = field.number == 1;
            if (reader.Done()) {
                rpc_app_running_ = running;
                if (!running)
                    rpc_app_name_.clear();
            }
        } else if (native_id_ && message.id >= native_first_id_ && message.id <= native_id_) {
            if (++native_reply_.frames > 2048 || (native_sink_ && !native_sink_(message))) {
                native_reply_.error = "response_too_large_or_invalid";
                native_reply_.done = true;
                session_ = 0;
                rpc_transport_active_ = false;
            } else {
                if (message.status)
                    native_reply_.status = message.status;
                native_reply_.done = message.id == native_id_ && !message.more;
                native_reply_.message = std::move(message);
            }
        }
        // Unsolicited native messages and old command IDs are not companion errors.
    }
}

bool DeepFlipperBridge::WaitStatus(int timeout_ms) {
    int64_t deadline = esp_timer_get_time() + timeout_ms * 1000LL;
    while (!status_received_ && esp_timer_get_time() < deadline)
        ReadFrame(10);
    return status_received_ && status_ok_;
}

bool DeepFlipperBridge::Connect() {
    session_ = 0;
    rpc_id_ = 0;
    rpc_transport_active_ = false;
    native_supported_ = false;
    native_id_ = 0;
    native_sink_ = {};
    rpc_app_running_ = false;
    rpc_app_name_.clear();
    // Silence the old session long enough for the peer's 250 ms Expansion
    // watchdog to close it before driving the next discovery pulse.
    if (link_rate_.Active())
        vTaskDelay(pdMS_TO_TICKS(500));
    const bool connected = link_rate_.Connect([this](uint32_t baud_rate) {
        using dfexpansion::ConnectResult;
        rpc_stream_.clear();
        rx_frame_.clear();
        discovery_received_ = false;
        status_received_ = status_ok_ = false;
        if (uart_wait_tx_done(kUart, pdMS_TO_TICKS(50)) != ESP_OK ||
            uart_set_baudrate(kUart, dfexpansion::kDiscoveryBaud) != ESP_OK)
            return ConnectResult::Failed;
        uart_flush_input(kUart);
        gpio_reset_pin(DF_UART_TX);
        gpio_set_direction(DF_UART_TX, GPIO_MODE_OUTPUT);
        gpio_set_level(DF_UART_TX, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        uart_set_pin(kUart, DF_UART_TX, DF_UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        const int64_t discovery_deadline = esp_timer_get_time() + 180000;
        while (!discovery_received_ && esp_timer_get_time() < discovery_deadline)
            ReadFrame(10);
        if (!discovery_received_)
            return ConnectResult::NoPeer;
        auto baud = dfexpansion::BaudPayload(baud_rate);
        status_received_ = false;
        if (!Frame(3, baud.data(), baud.size()) || !WaitStatus(150))
            return ConnectResult::Failed;
        // The ACK arrives at 9600. Switch only after receiving it and draining TX.
        if (uart_wait_tx_done(kUart, pdMS_TO_TICKS(50)) != ESP_OK ||
            uart_set_baudrate(kUart, baud_rate) != ESP_OK)
            return ConnectResult::Failed;
        vTaskDelay(pdMS_TO_TICKS(30));
        uint8_t start = 0;
        status_received_ = false;
        if (!Frame(4, &start, 1) || !WaitStatus(150))
            return ConnectResult::Failed;
        rpc_transport_active_ = true;
        last_heartbeat_us_ = esp_timer_get_time();
        auto text = Call("{\"op\":\"hello\"}");
        auto result = cJSON_Parse(text.c_str());
        session_ = Integer(result, "session");
        native_supported_ = Integer(result, "native_rpc") == 1;
        cJSON_Delete(result);
        return session_ ? ConnectResult::Ready : ConnectResult::Failed;
    });
    if (!connected) {
        Reset();
        native_supported_ = false;
    } else {
        ESP_LOGI(kLogTag, "Expansion connected at %lu baud%s", (unsigned long)link_rate_.Baud(),
                 link_rate_.Fallback() ? " (fallback)" : "");
    }
    return connected;
}

std::string DeepFlipperBridge::Call(const std::string& request) {
    auto object = cJSON_Parse(request.c_str());
    if (!cJSON_IsObject(object)) {
        cJSON_Delete(object);
        return "{\"ok\":false,\"code\":\"invalid_request\"}";
    }
    cJSON_AddNumberToObject(object, "v", 1);
    if (session_)
        cJSON_AddNumberToObject(object, "session", session_);
    const uint32_t id = ++rpc_id_;
    auto wire = dfwire::Request(id, Serialize(object));
    cJSON_Delete(object);
    response_.clear();
    response_id_ = 0;
    if (wire.empty())
        return "{\"ok\":false,\"code\":\"too_large\"}";
    if (!SendWire(wire)) {
        session_ = 0;
        return "{\"ok\":false,\"code\":\"link_lost\"}";
    }
    int64_t deadline = esp_timer_get_time() + 500000;
    while (response_id_ != id && esp_timer_get_time() < deadline)
        ReadFrame(10);
    if (response_id_ != id) {
        session_ = 0;
        return "{\"ok\":false,\"code\":\"link_lost\"}";
    }
    auto parsed = cJSON_Parse(response_.c_str());
    if (String(parsed, "code") == "invalid_session")
        session_ = 0;
    cJSON_Delete(parsed);
    return response_;
}

bool DeepFlipperBridge::SendWire(const std::vector<uint8_t>& wire) {
    if (wire.empty())
        return false;
    for (size_t pos = 0; pos < wire.size();) {
        const size_t count = std::min<size_t>(64, wire.size() - pos);
        status_received_ = false;
        if (!Frame(5, wire.data() + pos, count) || !WaitStatus(150))
            return false;
        pos += count;
    }
    return true;
}

bool DeepFlipperBridge::NativeCancelled() {
    std::lock_guard<std::mutex> guard(mutex_);
    for (const auto& job : jobs_)
        if (job.id == native_job_id_)
            return job.cancel;
    return false;
}

DeepFlipperBridge::NativeReply DeepFlipperBridge::Native(
    uint32_t tag, const std::vector<uint8_t>& payload,
    std::function<bool(const dfwire::Message&)> sink, bool cleanup, int short_key) {
    native_reply_ = {};
    if (session_) {
        auto heartbeat = cJSON_Parse(Call("{\"op\":\"status\"}").c_str());
        bool accepted = cJSON_IsTrue(cJSON_GetObjectItem(heartbeat, "ok"));
        cJSON_Delete(heartbeat);
        if (!accepted)
            session_ = 0;
    }
    if (!session_) {
        native_reply_.error = "link_lost";
        return native_reply_;
    }
    if (!cleanup && NativeCancelled()) {
        native_reply_.error = "cancelled";
        return native_reply_;
    }
    if (!cleanup && native_deadline_ms_ && esp_timer_get_time() / 1000 >= native_deadline_ms_) {
        native_reply_.error = "timeout";
        return native_reply_;
    }
    native_id_ = native_first_id_ = ++rpc_id_;
    auto wire = short_key >= 0 ? dfwire::ShortInput(native_first_id_, short_key)
                               : dfwire::NativeRequest(native_id_, tag, payload);
    if (short_key >= 0) {
        native_id_ += 2;
        rpc_id_ += 2;
    }
    native_sink_ = std::move(sink);
    if (!SendWire(wire)) {
        native_reply_.error = "link_lost";
        session_ = 0;
        rpc_transport_active_ = false;
    } else {
        const int64_t deadline = esp_timer_get_time() + (cleanup ? 2000000 : 5000000);
        while (!native_reply_.done && session_ && esp_timer_get_time() < deadline) {
            if (!cleanup &&
                (NativeCancelled() || esp_timer_get_time() / 1000 >= native_deadline_ms_)) {
                native_reply_.error = NativeCancelled() ? "cancelled" : "timeout_unknown_result";
                // Do not overlap an unconfirmed app command with another command.
                session_ = 0;
                rpc_transport_active_ = false;
                break;
            }
            ReadFrame(10);
            UpdateLed();
        }
        if (!native_reply_.done && native_reply_.error.empty()) {
            native_reply_.error = "timeout_unknown_result";
            session_ = 0;
            rpc_transport_active_ = false;
        }
    }
    native_id_ = 0;
    native_sink_ = {};
    return native_reply_;
}

std::string DeepFlipperBridge::ExecuteNative(const JobEntry& job) {
    dfnative::Actions actions(*this, job.deadline_ms);
    return actions.Execute(job.op, job.args);
}

int64_t DeepFlipperBridge::NowMs() const { return esp_timer_get_time() / 1000; }

void DeepFlipperBridge::Pump() {
    UpdateLed();
    Call("{\"op\":\"ui\",\"state\":7}");
    ReadFrame(50);
}

void DeepFlipperBridge::OnStateChanged() {
    switch (Application::GetInstance().GetDeviceState()) {
        case kDeviceStateWifiConfiguring:
            state_ = 1;
            break;
        case kDeviceStateStarting:
        case kDeviceStateActivating:
        case kDeviceStateConnecting:
            state_ = 2;
            break;
        case kDeviceStateIdle:
            state_ = 3;
            mouth_ = 0;
            break;
        case kDeviceStateListening:
            state_ = 4;
            break;
        // Network TTS start is not proof of actual playback.
        case kDeviceStateSpeaking:
        case kDeviceStateNotifying:
            state_ = 5;
            break;
        case kDeviceStateFatalError:
            state_ = 8;
            break;
        default:
            break;
    }
}

void DeepFlipperBridge::SetEmotion(const char* name) {
    std::string n = name ? name : "";
    int value = 0;
    if (n == "happy" || n == "laughing" || n == "funny")
        value = 4;
    else if (n == "embarrassed" || n == "loving")
        value = 5;
    else if (n == "surprised" || n == "shocked")
        value = 6;
    else if (n == "sad" || n == "crying")
        value = 8;
    else if (n == "angry")
        value = 9;
    else if (n == "sleepy")
        value = 10;
    else if (n == "thinking" || n == "confused")
        value = 2;
    else if (n == "cancel" || n == "cloud_off")
        state_ = 8;
    emotion_ = value;
}

void DeepFlipperBridge::AudioLevel(int level) {
    mouth_ = muted_ ? 0 : std::clamp(level, 0, 2);
    // This method is called on audio output, never sends UART or writes NVS.
    audio_tick_ms_ = level < 0 ? 0 : static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

void DeepFlipperBridge::RefreshSettings() {
    RestoreSettings();
    auto profile = Profile();
    applying_config_ = true;
    Board::GetInstance().GetAudioCodec()->SetOutputVolume(muted_ ? 0
                                                                 : Integer(profile, "volume", 20));
    applying_config_ = false;
    cJSON_Delete(profile);
    auto result = PublicResult(0, true);
    std::lock_guard<std::mutex> guard(mutex_);
    module_result_ = std::move(result);
}

void DeepFlipperBridge::VolumeChanged(int volume) {
    if (applying_config_)
        return;
    DeviceSetup::Get().SetVolume(std::clamp(volume, 0, 100));
    auto result = PublicResult(0, true);
    std::lock_guard<std::mutex> guard(mutex_);
    module_result_ = std::move(result);
}

int DeepFlipperBridge::Submit(const std::string& op, int timeout_ms, const std::string& args) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!linked_)
        return -1;
    const bool native = op.rfind("native.", 0) == 0;
    if (native && !native_supported_)
        return -2;
    if (args.size() > 1024)
        return -3;
    const bool mutating = native && op != "native.files" && op != "native.apps" &&
                          op != "native.system.info" && op != "native.app.state";
    for (const auto& existing : jobs_) {
        const bool existing_mutating =
            existing.op.rfind("native.", 0) == 0 && existing.op != "native.files" &&
            existing.op != "native.apps" && existing.op != "native.system.info" &&
            existing.op != "native.app.state";
        if (existing.pending && ((mutating && existing.op == "tool.nfc.scan") ||
                                 (op == "tool.nfc.scan" && existing_mutating)))
            return 0;
    }
    if (op == "tool.nfc.scan") {
        for (const auto& job : jobs_)
            if (job.pending && job.op == op)
                return 0;
    }
    for (auto& job : jobs_) {
        if (!job.pending &&
            (!job.id || job.consumed || esp_timer_get_time() / 1000 - job.completed_ms > 30000)) {
            job = JobEntry{};
            next_job_ = next_job_ >= INT32_MAX ? 1 : next_job_ + 1;
            job.id = next_job_;
            job.op = op;
            job.timeout_ms = timeout_ms;
            job.args = args;
            job.deadline_ms = esp_timer_get_time() / 1000 + (native ? 30000 : timeout_ms + 3000);
            job.pending = true;
            return job.id;
        }
    }
    return 0;
}

std::string DeepFlipperBridge::Job(int id) {
    std::lock_guard<std::mutex> guard(mutex_);
    for (auto& j : jobs_)
        if (j.id == id) {
            if (!j.pending)
                j.consumed = true;
            return j.pending ? "{\"state\":\"pending\"}" : j.result;
        }
    return "{\"state\":\"not_found\"}";
}

bool DeepFlipperBridge::Cancel(int id) {
    std::lock_guard<std::mutex> guard(mutex_);
    for (auto& j : jobs_)
        if (j.id == id && j.pending) {
            j.cancel = true;
            return true;
        }
    return false;
}

void DeepFlipperBridge::ApplyCommand(const std::string& command) {
    auto object = cJSON_Parse(command.c_str());
    int id = Integer(object, "id");
    auto field = String(object, "field"), value = String(object, "value");
    cJSON_Delete(object);
    if (!id || static_cast<uint32_t>(id) == command_id_)
        return;
    command_id_ = id;
    // Main-thread application mutations are scheduled; UART keeps its heartbeats.
    bool accepted = DeviceSetup::Get().Command(field, value);
    auto result = PublicResult(id, accepted);
    std::lock_guard<std::mutex> guard(mutex_);
    module_result_ = std::move(result);
}

std::string DeepFlipperBridge::PublicResult(int id, bool success) {
    auto p = Profile();
    auto result = cJSON_CreateObject();
    cJSON_AddStringToObject(result, "op", "module.result");
    cJSON_AddNumberToObject(result, "id", id);
    cJSON_AddBoolToObject(result, "ok", success);
    cJSON_AddNumberToObject(result, "volume", Integer(p, "volume", 20));
    cJSON_AddBoolToObject(result, "token_set", !String(p, "token").empty());
    cJSON_AddBoolToObject(result, "muted", muted_);
    cJSON_AddBoolToObject(result, "microphone", microphone_);
    cJSON_AddStringToObject(result, "device_id", SystemInfo::GetMacAddress().c_str());
    cJSON_AddStringToObject(result, "client_id", Board::GetInstance().GetUuid().c_str());
    DeviceSetup::Get().AppendFlipper(result);
    cJSON_AddNumberToObject(result, "uart_baud", link_rate_.Baud());
    cJSON_Delete(p);
    auto text = Serialize(result);
    cJSON_Delete(result);
    return text;
}

void DeepFlipperBridge::UpdateLed() {
    int64_t now = esp_timer_get_time();
    if (now - last_led_refresh_us_ < 20000)
        return;
    last_led_refresh_us_ = now;
    int state = DeviceSetup::Get().VisualState(configured_, state_);
    uint32_t audio_tick = audio_tick_ms_;
    bool speaking = !muted_ && audio_tick && static_cast<uint32_t>(now / 1000) - audio_tick < 150;
    if (speaking)
        state = 6;
    int r = 0, g = 0, b = 0;
    double breath = 0.15 + 0.85 * (1 - std::cos(now / 1600000.0 * 6.283185)) / 2;
    int level = std::clamp(brightness_.load(), 0, 100) * 10;
    if (tool_active_)
        state = 7;
    switch (state) {
        case 0:
            break;
        case 1:
            r = level;
            g = level;
            b = 0;
            level = (now / 300000) % 5 == 0 ? level : 0;
            break;
        case 2:
            b = level;
            level = level * breath;
            break;
        case 3:
            level /= 8;
            g = level;
            break;
        case 4:
            b = level;
            break;
        case 5:
            r = level;
            b = level;
            level = level * breath;
            break;
        case 6:
            g = level;
            b = level;
            level = level * (0.15 + 0.4 * mouth_);
            break;
        case 7:
            r = level;
            g = level / 3;
            level = level * breath;
            break;
        default:
            r = level;
            level = (now / 200000) % 10 < 2 ? level : 0;
            break;
    }
    int peak = std::max({r, g, b, 1});
    const int channels[] = {r, g, b};
    for (int i = 0; i < 3; i++) {
        auto c = static_cast<ledc_channel_t>(LEDC_CHANNEL_3 + i);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, c, std::clamp(channels[i] * level / peak, 0, 1023));
        ledc_update_duty(LEDC_LOW_SPEED_MODE, c);
    }
}

void DeepFlipperBridge::Run() {
    int64_t last_report = 0;
    while (true) {
        if (!session_) {
            if (linked_.exchange(false))
                Application::GetInstance().SuspendVoiceSession();
            {
                std::lock_guard<std::mutex> guard(mutex_);
                for (auto& job : jobs_)
                    if (job.pending) {
                        job.pending = false;
                        job.completed_ms = esp_timer_get_time() / 1000;
                        job.result = "{\"ok\":false,\"code\":\"link_lost\"}";
                    }
            }
            command_id_ = 0;
            if (!Connect()) {
                UpdateLed();
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            linked_ = true;
            Call(PublicResult(0, true));
        }
        if (esp_timer_get_time() / 1000 - last_report >= 1000) {
            last_report = esp_timer_get_time() / 1000;
            Call(PublicResult(0, true));
        }
        auto poll = cJSON_Parse(Call("{\"op\":\"poll\"}").c_str());
        auto command = cJSON_GetObjectItem(poll, "command");
        if (cJSON_IsObject(command))
            ApplyCommand(Serialize(command));
        cJSON_Delete(poll);
        std::string result;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            result.swap(module_result_);
        }
        if (!result.empty())
            Call(result);
        int s = DeviceSetup::Get().VisualState(configured_, state_);
        uint32_t audio_tick = audio_tick_ms_;
        if (!muted_ && audio_tick &&
            static_cast<uint32_t>(esp_timer_get_time() / 1000) - audio_tick < 150)
            s = 6;
        auto ui = cJSON_CreateObject();
        cJSON_AddStringToObject(ui, "op", "ui");
        cJSON_AddNumberToObject(ui, "state", s);
        cJSON_AddNumberToObject(ui, "mouth", mouth_);
        cJSON_AddNumberToObject(ui, "emotion", emotion_);
        Call(Serialize(ui));
        cJSON_Delete(ui);
        tool_active_ = false;
        for (size_t index = 0; index < jobs_.size() && session_; index++) {
            JobEntry job;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                job = jobs_[index];
            }
            if (!job.pending)
                continue;
            bool expired = esp_timer_get_time() / 1000 >= job.deadline_ms;
            if ((job.cancel || expired) && !job.remote) {
                std::lock_guard<std::mutex> guard(mutex_);
                jobs_[index].pending = false;
                jobs_[index].completed_ms = esp_timer_get_time() / 1000;
                jobs_[index].result = expired ? "{\"ok\":false,\"state\":\"timeout\"}"
                                              : "{\"ok\":false,\"state\":\"cancelled\"}";
                continue;
            }
            if (job.op.rfind("native.", 0) == 0) {
                native_job_id_ = job.id;
                native_deadline_ms_ = job.deadline_ms;
                tool_active_ = true;
                UpdateLed();
                Call("{\"op\":\"ui\",\"state\":7}");
                auto reply = ExecuteNative(job);
                native_job_id_ = 0;
                native_deadline_ms_ = 0;
                std::lock_guard<std::mutex> guard(mutex_);
                if (jobs_[index].id == job.id) {
                    jobs_[index].result = std::move(reply);
                    jobs_[index].pending = false;
                    jobs_[index].completed_ms = esp_timer_get_time() / 1000;
                }
                continue;
            }
            auto req = cJSON_CreateObject();
            if (job.remote) {
                cJSON_AddStringToObject(req, "op",
                                        (job.cancel || expired) ? "job.cancel" : "job.get");
                cJSON_AddNumberToObject(req, "job", job.remote);
            } else {
                cJSON_AddStringToObject(req, "op", job.op.c_str());
                cJSON_AddNumberToObject(req, "timeout_ms", job.timeout_ms);
            }
            auto reply = Call(Serialize(req));
            cJSON_Delete(req);
            auto parsed = cJSON_Parse(reply.c_str());
            if (job.op == "status" && cJSON_IsObject(parsed)) {
                cJSON_AddBoolToObject(parsed, "native_rpc_available", native_supported_);
                cJSON_AddNumberToObject(parsed, "uart_baud", link_rate_.Baud());
                cJSON_AddBoolToObject(parsed, "uart_fallback", link_rate_.Fallback());
                reply = Serialize(parsed);
            }
            auto remote = Integer(parsed, "job");
            auto status = String(parsed, "state");
            bool complete = !remote || (status != "accepted" && status != "running");
            if (!complete)
                tool_active_ = true;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (jobs_[index].id == job.id) {
                    jobs_[index].remote = remote;
                    jobs_[index].result = reply;
                    jobs_[index].pending = !complete;
                    if (complete)
                        jobs_[index].completed_ms = esp_timer_get_time() / 1000;
                }
            }
            cJSON_Delete(parsed);
        }
        UpdateLed();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
