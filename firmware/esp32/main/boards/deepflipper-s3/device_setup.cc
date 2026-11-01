#include "device_setup.h"
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <psa/crypto.h>
#include <ssid_manager.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include "application.h"
#include "assets/lang_config.h"
#include "audio_preview.h"
#include "companion_json.h"
#include "deepflipper_bridge.h"
#include "settings.h"
#include "voice_hello.h"

extern const char setup_html_start[] asm("_binary_deepflipper_setup_html_start");
extern const char setup_html_end[] asm("_binary_deepflipper_setup_html_end");
extern const char setup_js_start[] asm("_binary_deepflipper_setup_js_start");
extern const char setup_js_end[] asm("_binary_deepflipper_setup_js_end");
extern const char setup_css_start[] asm("_binary_deepflipper_setup_css_start");
extern const char setup_css_end[] asm("_binary_deepflipper_setup_css_end");
extern const char preview_js_start[] asm("_binary_audio_preview_js_start");
extern const char preview_js_end[] asm("_binary_audio_preview_js_end");
extern const char preview_wav_start[] asm("_binary_audio_preview_wav_start");
extern const char preview_wav_end[] asm("_binary_audio_preview_wav_end");

using namespace dfsetup;
namespace {
// API replies are sent after all route-local locks have been released.
struct DeferredReply {
    httpd_req_t* request;
    std::string value, status, cookie;
    ~DeferredReply() {
        if (status.empty())
            return;
        httpd_resp_set_status(request, status.c_str());
        httpd_resp_set_type(request, "application/json");
        httpd_resp_send(request, value.data(), value.size());
    }
};
int64_t Now() { return esp_timer_get_time() / 1000; }
std::string Random(size_t bytes) {
    uint8_t data[32];
    esp_fill_random(data, std::min(bytes, sizeof(data)));
    std::string result;
    for (size_t i = 0; i < std::min(bytes, sizeof(data)); ++i) {
        result += "0123456789abcdef"[data[i] >> 4];
        result += "0123456789abcdef"[data[i] & 15];
    }
    return result;
}
std::string Hash(const std::string& salt, const std::string& password) {
    const auto input = salt + "\n" + password;
    uint8_t digest[32];
    size_t length = 0;
    auto initialized = psa_crypto_init();
    ESP_ERROR_CHECK(initialized == PSA_SUCCESS ? ESP_OK : ESP_FAIL);
    auto hashed = psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const uint8_t*>(input.data()),
                                   input.size(), digest, sizeof(digest), &length);
    ESP_ERROR_CHECK(hashed == PSA_SUCCESS && length == sizeof(digest) ? ESP_OK : ESP_FAIL);
    std::string result;
    for (auto byte : digest) {
        result += "0123456789abcdef"[byte >> 4];
        result += "0123456789abcdef"[byte & 15];
    }
    return result;
}
std::string Header(httpd_req_t* req, const char* name) {
    size_t length = httpd_req_get_hdr_value_len(req, name);
    if (!length || length > 256)
        return "";
    std::string result(length + 1, 0);
    if (httpd_req_get_hdr_value_str(req, name, result.data(), result.size()) != ESP_OK)
        return "";
    result.resize(length);
    return result;
}
std::string Cookie(httpd_req_t* req) {
    auto cookie = Header(req, "Cookie");
    size_t position = 0;
    while (position < cookie.size()) {
        while (position < cookie.size() && (cookie[position] == ' ' || cookie[position] == ';'))
            ++position;
        auto end = cookie.find(';', position);
        auto part = cookie.substr(position, end == std::string::npos ? end : end - position);
        if (part.rfind("df_session=", 0) == 0)
            return part.substr(11);
        if (end == std::string::npos)
            break;
        position = end + 1;
    }
    return "";
}
const char* NetworkCode(const NetworkError& error) {
    switch (error.code) {
        case NetworkErrc::DnsFailed:
            return "dns_failed";
        case NetworkErrc::TlsFailed:
            return "tls_failed";
        case NetworkErrc::AuthRejected:
            return "auth_failed";
        case NetworkErrc::Timeout:
            return "connect_timeout";
        default:
            return "connect_failed";
    }
}
}  // namespace

DeviceSetup& DeviceSetup::Get() {
    static DeviceSetup instance;
    return instance;
}
void DeviceSetup::Initialize(const std::string& device, const std::string& client) {
    std::lock_guard<std::mutex> guard(mutex_);
    device_id_ = device;
    client_id_ = client;
    Settings settings("deepflipper", true);
    profile_ = settings.GetString("profile", "{}");
    auto profile = Parse(profile_);
    if (!cJSON_IsObject(profile.get()))
        profile = Parse("{}");
    // One-time migration. The new profile becomes the sole source of Wi-Fi credentials.
    if (!cJSON_IsTrue(cJSON_GetObjectItem(profile.get(), "web_setup_v1"))) {
        if (Text(profile.get(), "ssid").empty()) {
            const auto& saved = SsidManager::GetInstance().GetSsidList();
            if (!saved.empty()) {
                Put(profile.get(), "ssid", saved.front().ssid);
                Put(profile.get(), "wifi_password", saved.front().password);
            }
        }
        cJSON_AddBoolToObject(profile.get(), "web_setup_v1", true);
    }
    if (!cJSON_GetObjectItem(profile.get(), "volume"))
        cJSON_AddNumberToObject(profile.get(), "volume", 20);
    if (!cJSON_GetObjectItem(profile.get(), "microphone"))
        cJSON_AddBoolToObject(profile.get(), "microphone", true);
    setup_key_ = settings.GetString("setup_key");
    if (setup_key_.empty()) {
        setup_key_ = Random(6);
        settings.SetString("setup_key", setup_key_);
    }
    auth_salt_ = settings.GetString("web_salt");
    if (auth_salt_.empty()) {
        auth_salt_ = Random(16);
        settings.SetString("web_salt", auth_salt_);
    }
    auth_hash_ = settings.GetString("web_hash");
    if (auth_hash_.empty()) {
        auth_hash_ = Hash(auth_salt_, setup_key_);
        settings.SetString("web_hash", auth_hash_);
    }
    password_changed_ = settings.GetBool("web_changed");
    ap_ssid_ = "DeepFlipper-";
    for (char c : device_id_.substr(device_id_.size() > 5 ? device_id_.size() - 5 : 0))
        if (c != ':')
            ap_ssid_ += c;
    profile_ = Encode(profile.get());
    settings.SetString("profile", profile_);
    queue_ = xQueueCreate(6, sizeof(Work));
    configASSERT(queue_);
}
std::string DeviceSetup::Profile() {
    std::lock_guard<std::mutex> guard(mutex_);
    return profile_;
}
void DeviceSetup::SaveLocked(cJSON* profile) {
    int revision = Number(profile, "revision", 0);
    cJSON_DeleteItemFromObject(profile, "revision");
    cJSON_AddNumberToObject(profile, "revision", revision < 1000000000 ? revision + 1 : 1);
    profile_ = Encode(profile);
    Settings settings("deepflipper", true);
    settings.SetString("profile", profile_);
}
void DeviceSetup::SetCallback(NetworkEventCallback callback) {
    std::lock_guard<std::mutex> guard(mutex_);
    callback_ = std::move(callback);
}
void DeviceSetup::Notify(NetworkEvent event, const std::string& data) {
    NetworkEventCallback callback;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        callback = callback_;
    }
    if (callback)
        callback(event, data);
}
void DeviceSetup::Start() {
    if (started_)
        return;
    started_ = true;
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t loop = esp_event_loop_create_default();
    ESP_ERROR_CHECK(loop == ESP_ERR_INVALID_STATE ? ESP_OK : loop);
    station_ = esp_netif_create_default_wifi_sta();
    access_point_ = esp_netif_create_default_wifi_ap();
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    config.nvs_enable = false;
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    // Seed the AP credentials before starting the radio, so recovery never
    // briefly advertises an open network while switching from STA to APSTA.
    wifi_config_t ap = {};
    snprintf(reinterpret_cast<char*>(ap.ap.ssid), sizeof(ap.ap.ssid), "%s", ap_ssid_.c_str());
    snprintf(reinterpret_cast<char*>(ap.ap.password), sizeof(ap.ap.password), "%s",
             setup_key_.c_str());
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 2;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    auto hostname = ap_ssid_;
    std::transform(hostname.begin(), hostname.end(), hostname.begin(), ::tolower);
    ESP_ERROR_CHECK(esp_netif_set_hostname(station_, hostname.c_str()));
    auto handler = [](void* arg, esp_event_base_t base, int32_t id, void* data) {
        static_cast<DeviceSetup*>(arg)->HandleEvent(base, id, data);
    };
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, handler, this,
                                                        &wifi_handler_));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, handler,
                                                        this, &ip_handler_));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    {
        std::lock_guard<std::mutex> guard(mutex_);
        wifi_ready_ = true;
    }
    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.stack_size = 8192;
    http_config.max_uri_handlers = 4;
    http_config.max_open_sockets = 4;
    http_config.lru_purge_enable = true;
    http_config.recv_wait_timeout = 5;
    http_config.uri_match_fn = httpd_uri_match_wildcard;
    ESP_ERROR_CHECK(httpd_start(&http_, &http_config));
    for (auto method : {HTTP_GET, HTTP_POST}) {
        httpd_uri_t uri = {};
        uri.uri = "/*";
        uri.method = method;
        uri.handler = Handler;
        uri.user_ctx = this;
        ESP_ERROR_CHECK(httpd_register_uri_handler(http_, &uri));
    }
    BaseType_t created = xTaskCreate([](void* arg) { static_cast<DeviceSetup*>(arg)->Run(); },
                                     "df_setup", 8192, this, 3, nullptr);
    configASSERT(created == pdPASS);
}
bool DeviceSetup::Queue(CommandType type) {
    Work work{type, Now() + (type == CommandType::Wifi || type == CommandType::Retry ? 300 : 0)};
    return queue_ && xQueueSend(queue_, &work, 0) == pdTRUE;
}
void DeviceSetup::Recover() { Queue(CommandType::Recover); }
bool DeviceSetup::SetupMode() {
    std::lock_guard<std::mutex> guard(mutex_);
    return ap_active_ && !connected_;
}
int DeviceSetup::VisualState(bool configured, int fallback) {
    if (standby_)
        return 0;
    std::lock_guard<std::mutex> guard(mutex_);
    return dfsetup::VisualState(configured, DeepFlipperBridge::Get().Linked(), connected_,
                                ap_active_, server_state_, fallback);
}
void DeviceSetup::PowerSave(PowerSaveLevel level) {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        power_save_ = level;
    }
    RefreshPowerSave();
}
void DeviceSetup::RefreshPowerSave() {
    std::lock_guard<std::mutex> power_guard(power_mutex_);
    wifi_ps_type_t mode;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (!wifi_ready_)
            return;
        mode =
            power_save_ == PowerSaveLevel::LOW_POWER && !ap_active_ && !probing_ && !probing_queued_
                ? WIFI_PS_MIN_MODEM
                : WIFI_PS_NONE;
    }
    // The driver may wait on the Wi-Fi task. Its event callbacks must be able
    // to take the configuration lock while this operation completes.
    if (applied_power_save_ != mode && esp_wifi_set_ps(mode) == ESP_OK)
        applied_power_save_ = mode;
}
const char* DeviceSetup::ActionError(bool speaker) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (standby_)
        return "module_standby";
    auto profile = Parse(profile_);
    auto state = Application::GetInstance().GetDeviceState();
    bool busy = probing_ || probing_queued_ || wifi_trial_.Pending() || Applying() ||
                AudioPreview::Get().Busy();
    if (speaker)
        return dfsetup::SpeakerError(
            DeepFlipperBridge::Get().Linked(),
            cJSON_IsTrue(cJSON_GetObjectItem(profile.get(), "mute")),
            Number(profile.get(), "volume", 20),
            busy || (state != kDeviceStateIdle && state != kDeviceStateStarting));
    return dfsetup::TalkError(connected_, DeepFlipperBridge::Get().Linked(),
                              deepflipper::ValidVoiceEndpoint(Text(profile.get(), "url")) &&
                                  Token(Text(profile.get(), "token")),
                              cJSON_IsTrue(cJSON_GetObjectItem(profile.get(), "microphone")), busy,
                              state == kDeviceStateIdle || state == kDeviceStateSpeaking);
}
bool DeviceSetup::Talk(const std::string& action) {
    if (standby_)
        return Queue(CommandType::Wake);
    auto& app = Application::GetInstance();
    auto decision =
        dfsetup::DecideTalk(action, app.GetDeviceState() == kDeviceStateListening, ReadyToTalk());
    if (decision == dfsetup::TalkAction::Reject)
        return false;
    // UART, GPIO button and HTTP all enter through the same main-task path.
    app.Schedule([this, decision]() {
        auto& app = Application::GetInstance();
        if (decision == dfsetup::TalkAction::Stop) {
            if (app.GetDeviceState() == kDeviceStateListening)
                app.StopListening();
        } else if (ReadyToTalk()) {
            app.StartListening();
        }
    });
    return true;
}
void DeviceSetup::VoiceState(const char* state) {
    std::lock_guard<std::mutex> guard(mutex_);
    voice_open_ = std::string(state) == "authenticated";
    if (std::string(state) != "closed" &&
        !(server_state_ == "auth_failed" && std::string(state) == "hello_timeout")) {
        server_state_ = state;
        last_server_check_ = Now();
    }
}
void DeviceSetup::SetVolume(int volume) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto profile = Parse(profile_);
    if (Number(profile.get(), "volume", 20) == volume)
        return;
    cJSON_DeleteItemFromObject(profile.get(), "volume");
    cJSON_AddNumberToObject(profile.get(), "volume", volume);
    SaveLocked(profile.get());
}
void DeviceSetup::Apply(bool reload_voice) {
    ++apply_pending_;
    Application::GetInstance().Schedule([this, reload_voice]() {
        DeepFlipperBridge::Get().RefreshSettings();
        const auto state = Application::GetInstance().GetDeviceState();
        if (reload_voice && DeviceSetup::Get().Connected() && state != kDeviceStateStarting &&
            state != kDeviceStateActivating)
            Application::GetInstance().ReloadVoiceConfig([this]() { --apply_pending_; });
        else {
            if (!DeepFlipperBridge::Get().MicrophoneEnabled() && state == kDeviceStateListening)
                Application::GetInstance().SuspendVoiceSession();
            --apply_pending_;
        }
    });
}
bool DeviceSetup::Command(const std::string& field, const std::string& value) {
    if (field == "power_mode") {
        AudioPreview::Get().Stop();
        if (Probing() || Applying())
            return false;
        if (value == "standby")
            return Queue(CommandType::Sleep);
        if (value == "awake")
            return Queue(CommandType::Wake);
        return false;
    }
    if (field == "talk")
        return Talk(value);
    if (field == "wifi_portal")
        return Queue(CommandType::Hotspot);
    if (field == "test_connection") {
        std::lock_guard<std::mutex> guard(mutex_);
        if (wifi_trial_.Pending() || probing_ || probing_queued_ || Applying())
            return false;
        probing_queued_ = true;
        if (Queue(CommandType::Probe))
            return true;
        probing_queued_ = false;
        return false;
    }
    bool valid = false;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto profile = Parse(profile_);
        if (field == "volume") {
            char* end = nullptr;
            long volume = strtol(value.c_str(), &end, 10);
            valid = !value.empty() && end && !*end && volume >= 0 && volume <= 100;
            if (valid) {
                cJSON_DeleteItemFromObject(profile.get(), "volume");
                cJSON_AddNumberToObject(profile.get(), "volume", volume);
            }
        } else if (field == "mute" || field == "microphone") {
            bool next = !cJSON_IsTrue(cJSON_GetObjectItem(profile.get(), field.c_str()));
            cJSON_DeleteItemFromObject(profile.get(), field.c_str());
            cJSON_AddBoolToObject(profile.get(), field.c_str(), next);
            valid = true;
        } else if (field == "led_brightness") {
            int next = (Number(profile.get(), "led_brightness", 12) + 12) % 61;
            cJSON_DeleteItemFromObject(profile.get(), "led_brightness");
            cJSON_AddNumberToObject(profile.get(), "led_brightness", next);
            valid = true;
        }
        if (valid)
            SaveLocked(profile.get());
    }
    if (valid)
        Apply(false);
    return valid;
}
void DeviceSetup::AppendFlipper(cJSON* output) {
    std::lock_guard<std::mutex> guard(mutex_);
    cJSON_AddStringToObject(output, "wifi", wifi_state_.c_str());
    cJSON_AddStringToObject(output, "ip", connected_ ? ip_.c_str() : "");
    cJSON_AddStringToObject(output, "ap_ip", ap_ip_.c_str());
    cJSON_AddBoolToObject(output, "ap", ap_active_);
    cJSON_AddStringToObject(output, "ap_ssid", ap_ssid_.c_str());
    cJSON_AddStringToObject(output, "setup_key", setup_key_.c_str());
    cJSON_AddBoolToObject(output, "web_changed", password_changed_);
    cJSON_AddStringToObject(output, "server_state", server_state_.c_str());
}
std::string DeviceSetup::Status(bool include_settings) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto json = Parse("{}");
    auto profile = Parse(profile_);
    auto out = json.get();
    AudioPreview::Get().AppendStatus(out);
    cJSON_AddStringToObject(out, "device_id", device_id_.c_str());
    cJSON_AddStringToObject(out, "client_id", client_id_.c_str());
    cJSON_AddStringToObject(out, "wifi", wifi_state_.c_str());
    cJSON_AddStringToObject(out, "wifi_error", wifi_error_.c_str());
    cJSON_AddStringToObject(out, "ssid", connected_ ? ssid_.c_str() : attempt_ssid_.c_str());
    cJSON_AddStringToObject(out, "ip", connected_ ? ip_.c_str() : "");
    cJSON_AddStringToObject(out, "ap_ip", ap_ip_.c_str());
    cJSON_AddBoolToObject(out, "ap", ap_active_);
    cJSON_AddStringToObject(out, "ap_ssid", ap_ssid_.c_str());
    cJSON_AddBoolToObject(out, "wifi_pending", wifi_trial_.Pending());
    auto trial = cJSON_AddObjectToObject(out, "wifi_trial");
    cJSON_AddNumberToObject(trial, "id", wifi_trial_.Id());
    cJSON_AddStringToObject(trial, "state", wifi_trial_.State().c_str());
    cJSON_AddStringToObject(trial, "ssid", wifi_trial_.Ssid().c_str());
    cJSON_AddStringToObject(trial, "error", wifi_trial_.Error().c_str());
    cJSON_AddStringToObject(trial, "request_id", wifi_trial_.Request().c_str());
    cJSON_AddNumberToObject(trial, "before_revision", wifi_trial_.BeforeRevision());
    cJSON_AddNumberToObject(trial, "revision", wifi_trial_.Revision());
    cJSON_AddBoolToObject(out, "scanning", scanning_);
    cJSON_AddBoolToObject(out, "probing", probing_ || probing_queued_);
    cJSON_AddBoolToObject(out, "applying", Applying());
    cJSON_AddBoolToObject(out, "voice_connected", voice_open_);
    cJSON_AddBoolToObject(out, "standby", standby_);
    cJSON_AddStringToObject(out, "server_state", server_state_.c_str());
    cJSON_AddNumberToObject(out, "server_check_age_s",
                            last_server_check_ ? (Now() - last_server_check_) / 1000 : -1);
    cJSON_AddBoolToObject(out, "flipper_connected", DeepFlipperBridge::Get().Linked());
    cJSON_AddItemToObject(out, "uart",
                          cJSON_Parse(DeepFlipperBridge::Get().LinkDiagnostics().c_str()));
    cJSON_AddNumberToObject(out, "revision", Number(profile.get(), "revision"));
    cJSON_AddNumberToObject(
        out, "ap_close_in_s",
        ap_active_ && ap_close_at_ ? std::max<int64_t>(0, (ap_close_at_ - Now()) / 1000) : -1);
    cJSON_AddNumberToObject(out, "volume", Number(profile.get(), "volume", 20));
    cJSON_AddBoolToObject(out, "mute", cJSON_IsTrue(cJSON_GetObjectItem(profile.get(), "mute")));
    cJSON_AddBoolToObject(out, "microphone",
                          cJSON_IsTrue(cJSON_GetObjectItem(profile.get(), "microphone")));
    cJSON_AddBoolToObject(out, "config_ready",
                          deepflipper::ValidVoiceEndpoint(Text(profile.get(), "url")) &&
                              Token(Text(profile.get(), "token")));
    cJSON_AddStringToObject(
        out, "app_state",
        DeviceStateMachine::GetStateName(Application::GetInstance().GetDeviceState()));
    if (include_settings) {
        cJSON_AddStringToObject(out, "saved_ssid", Text(profile.get(), "ssid").c_str());
        cJSON_AddStringToObject(out, "url", Text(profile.get(), "url").c_str());
        cJSON_AddBoolToObject(out, "token_set", !Text(profile.get(), "token").empty());
        cJSON_AddNumberToObject(out, "led_brightness", Number(profile.get(), "led_brightness", 12));
        cJSON_AddBoolToObject(out, "web_password_changed", password_changed_);
    }
    return Encode(out);
}
void DeviceSetup::HandleEvent(esp_event_base_t base, int32_t id, void* data) {
    if (standby_)
        return;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto event = static_cast<ip_event_got_ip_t*>(data);
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
        {
            std::lock_guard<std::mutex> guard(mutex_);
            ip_ = ip;
        }
        got_ip_ = true;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        associated_ = true;
        std::lock_guard<std::mutex> guard(mutex_);
        wifi_state_ = "getting_ip";
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (standby_) {
            connected_ = associated_ = false;
            return;
        }
        auto event = static_cast<wifi_event_sta_disconnected_t*>(data);
        connected_ = false;
        associated_ = false;
        voice_open_ = false;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            wifi_state_ = attempt_ssid_.empty() ? "setup" : "connecting";
            server_state_ = "wifi_offline";
            last_server_check_ = 0;
            ip_.clear();
            wifi_error_ = "association_failed (" + std::to_string(event->reason) + ")";
            if (!connection_deadline_ && !attempt_ssid_.empty())
                connection_deadline_ = Now() + 60000;
            next_retry_ = attempt_ssid_.empty() ? 0 : Now() + 3000;
        }
        disconnected_ = true;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE)
        scan_done_ = true;
}
void DeviceSetup::SetAp(bool enabled) {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        ap_close_at_ = 0;
        if (ap_active_ == enabled)
            return;
    }
    if (enabled) {
        // Avoid an AP/STA route collision on routers using 192.168.4.0/24.
        esp_netif_ip_info_t station_ip = {}, ap_ip = {};
        esp_netif_get_ip_info(station_, &station_ip);
        IP4_ADDR(&ap_ip.ip, 192, 168, 4, 1);
        if (station_ip.ip.addr && ((station_ip.ip.addr & station_ip.netmask.addr) ==
                                   (ap_ip.ip.addr & station_ip.netmask.addr)))
            IP4_ADDR(&ap_ip.ip, 172, 31, 254, 1);
        ap_ip.gw = ap_ip.ip;
        IP4_ADDR(&ap_ip.netmask, 255, 255, 255, 0);
        esp_netif_dhcps_stop(access_point_);
        ESP_ERROR_CHECK(esp_netif_set_ip_info(access_point_, &ap_ip));
        esp_netif_dhcps_start(access_point_);
        wifi_config_t config = {};
        snprintf(reinterpret_cast<char*>(config.ap.ssid), sizeof(config.ap.ssid), "%s",
                 ap_ssid_.c_str());
        snprintf(reinterpret_cast<char*>(config.ap.password), sizeof(config.ap.password), "%s",
                 setup_key_.c_str());
        config.ap.authmode = WIFI_AUTH_WPA2_PSK;
        config.ap.max_connection = 2;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
        dns_.Start(ap_ip.ip);
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ap_ip.ip));
        {
            std::lock_guard<std::mutex> guard(mutex_);
            ap_active_ = true;
            ap_ip_ = ip;
        }
    } else {
        dns_.Stop();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        std::lock_guard<std::mutex> guard(mutex_);
        ap_active_ = false;
    }
}
void DeviceSetup::ConnectStored(bool use_candidate) {
    // Drain the old association before starting the trial. Otherwise a late IP
    // event for the same SSID could certify credentials that were never tested.
    bool had_link = connected_ || associated_;
    disconnected_ = false;
    esp_wifi_scan_stop();
    auto disconnected = esp_wifi_disconnect();
    if (had_link && disconnected == ESP_OK) {
        const int64_t deadline = Now() + 1000;
        while (!disconnected_ && Now() < deadline)
            vTaskDelay(pdMS_TO_TICKS(10));
        if (!disconnected_) {
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (use_candidate)
                    wifi_trial_.Fail("disconnect_timeout");
            }
            SetAp(true);
            return;
        }
    }
    connected_ = associated_ = got_ip_ = disconnected_ = false;
    std::string ssid, password;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        auto profile = Parse(profile_);
        ssid = use_candidate ? wifi_trial_.Ssid() : Text(profile.get(), "ssid");
        password = use_candidate ? wifi_trial_.Password() : Text(profile.get(), "wifi_password");
        candidate_ = use_candidate;
        scanning_ = false;
        if (use_candidate)
            wifi_trial_.Start(Now());
        attempt_ssid_ = ssid;
        wifi_state_ = ssid.empty() ? "setup" : "connecting";
        wifi_error_.clear();
        server_state_ = "wifi_offline";
        last_server_check_ = 0;
        connection_deadline_ = ssid.empty() ? 0 : Now() + 60000;
        next_retry_ = 0;
        ap_close_at_ = 0;
        ip_.clear();
    }
    Notify(NetworkEvent::Disconnected);
    if (ssid.empty()) {
        SetAp(true);
        return;
    }
    auto trial = Parse("{}");
    Put(trial.get(), "ssid", ssid);
    Put(trial.get(), "wifi_password", password);
    std::string invalid;
    if (!Wifi(trial.get(), invalid)) {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            wifi_state_ = "failed";
            wifi_error_ = invalid;
            connection_deadline_ = next_retry_ = 0;
            candidate_ = false;
            wifi_trial_.Fail(wifi_error_);
        }
        SetAp(true);
        return;
    }
    wifi_config_t config = {};
    memcpy(config.sta.ssid, ssid.data(), std::min(ssid.size(), sizeof(config.sta.ssid)));
    memcpy(config.sta.password, password.data(),
           std::min(password.size(), sizeof(config.sta.password)));
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.failure_retry_cnt = 3;
    auto configured = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (configured != ESP_OK) {
        {
            std::lock_guard<std::mutex> guard(mutex_);
            wifi_state_ = "failed";
            wifi_error_ = "wifi_config_rejected";
            connection_deadline_ = next_retry_ = 0;
            candidate_ = false;
            wifi_trial_.Fail(wifi_error_);
        }
        SetAp(true);
        return;
    }
    esp_wifi_connect();
    Notify(NetworkEvent::Connecting, ssid);
}
void DeviceSetup::Run() {
    ConnectStored();
    while (true) {
        Work work;
        if (xQueueReceive(queue_, &work, pdMS_TO_TICKS(100)) == pdTRUE) {
            if (work.type == CommandType::Sleep && !standby_) {
                AudioPreview::Get().Stop();
                standby_ = true;
                Application::GetInstance().SuspendVoiceSession();
                if (ap_active_)
                    SetAp(false);
                esp_wifi_disconnect();
                esp_wifi_stop();
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    connected_ = associated_ = got_ip_ = disconnected_ = false;
                    connection_deadline_ = next_retry_ = ap_close_at_ = 0;
                    ip_.clear();
                    wifi_state_ = "sleeping";
                    server_state_ = "sleeping";
                    voice_open_ = false;
                }
                Notify(NetworkEvent::Disconnected);
            }
            if ((work.type == CommandType::Wake || work.type == CommandType::Recover ||
                 work.type == CommandType::Hotspot || work.type == CommandType::Retry) &&
                standby_) {
                standby_ = false;
                esp_wifi_start();
                ConnectStored();
            }
            if (standby_)
                continue;
            const int64_t remaining = work.not_before_ms - Now();
            if (remaining > 0)
                vTaskDelay(pdMS_TO_TICKS(remaining));
            if (work.type == CommandType::Wifi) {
                SetAp(true);
                ConnectStored(true);
            }
            if (work.type == CommandType::Retry) {
                SetAp(true);
                ConnectStored();
            }
            if (work.type == CommandType::Hotspot)
                SetAp(true);
            if (work.type == CommandType::CloseHotspot && connected_) {
                std::lock_guard<std::mutex> guard(mutex_);
                ap_close_at_ = Now() + 1500;
            }
            if (work.type == CommandType::Recover) {
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    auth_hash_ = Hash(auth_salt_, setup_key_);
                    password_changed_ = false;
                    session_.Close();
                    Settings settings("deepflipper", true);
                    settings.SetString("web_hash", auth_hash_);
                    settings.SetBool("web_changed", false);
                }
                SetAp(true);
            }
            if (work.type == CommandType::Scan) {
                wifi_scan_config_t scan = {};
                scan.show_hidden = false;
                if (esp_wifi_scan_start(&scan, false) != ESP_OK) {
                    std::lock_guard<std::mutex> guard(mutex_);
                    scanning_ = false;
                }
            }
            if (work.type == CommandType::Probe)
                Probe();
        }
        if (disconnected_.exchange(false))
            Notify(NetworkEvent::Disconnected);
        if (got_ip_.exchange(false)) {
            wifi_ap_record_t ap = {};
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                std::string ssid(reinterpret_cast<char*>(ap.ssid),
                                 strnlen(reinterpret_cast<char*>(ap.ssid), sizeof(ap.ssid)));
                bool matched;
                const int64_t confirmed_at = Now();
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    matched = ssid == attempt_ssid_ &&
                              (!candidate_ || wifi_trial_.CanConfirm(ssid, confirmed_at));
                    if (matched) {
                        connected_ = true;
                        ssid_ = ssid;
                        wifi_state_ = "connected";
                        wifi_error_.clear();
                        connection_deadline_ = next_retry_ = 0;
                        last_server_check_ = 0;
                        auto current = Parse(profile_);
                        server_state_ =
                            deepflipper::ValidVoiceEndpoint(Text(current.get(), "url")) &&
                                    Token(Text(current.get(), "token"))
                                ? "not_tested"
                                : "needs_config";
                        if (candidate_) {
                            auto profile = ConfirmWifi(profile_, wifi_trial_.Ssid(),
                                                       wifi_trial_.Password(), ssid);
                            if (profile) {
                                int before = Number(profile.get(), "revision");
                                SaveLocked(profile.get());
                                wifi_trial_.Succeed(ssid, confirmed_at, before,
                                                    Number(profile.get(), "revision"));
                            }
                        }
                        candidate_ = false;
                        if (ap_active_ && wifi_trial_.State() != "failed")
                            ap_close_at_ = Now() + 15000;
                    }
                }
                if (matched) {
                    // If newly acquired STA subnet overlaps the setup subnet, relocate the AP.
                    esp_netif_ip_info_t sta = {}, ap_info = {};
                    esp_netif_get_ip_info(station_, &sta);
                    esp_netif_get_ip_info(access_point_, &ap_info);
                    bool active;
                    {
                        std::lock_guard<std::mutex> guard(mutex_);
                        active = ap_active_;
                    }
                    if (active &&
                        (sta.ip.addr & sta.netmask.addr) == (ap_info.ip.addr & sta.netmask.addr)) {
                        SetAp(false);
                        SetAp(true);
                        std::lock_guard<std::mutex> guard(mutex_);
                        if (wifi_trial_.State() != "failed")
                            ap_close_at_ = Now() + 15000;
                    }
                    Notify(NetworkEvent::Connected, ssid);
                }
            }
        }
        if (scan_done_.exchange(false)) {
            wifi_ap_record_t records[20] = {};
            uint16_t count = 20;
            if (esp_wifi_scan_get_ap_records(&count, records) != ESP_OK)
                count = 0;
            std::lock_guard<std::mutex> guard(mutex_);
            networks_.clear();
            scanning_ = false;
            for (uint16_t i = 0; i < count; ++i) {
                std::string ssid(
                    reinterpret_cast<char*>(records[i].ssid),
                    strnlen(reinterpret_cast<char*>(records[i].ssid), sizeof(records[i].ssid)));
                if (!ssid.empty())
                    networks_.push_back(
                        {ssid, records[i].rssi, records[i].authmode != WIFI_AUTH_OPEN});
            }
        }
        bool timeout = false, retry = false, close_ap = false;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            timeout = !connected_ && connection_deadline_ && Now() >= connection_deadline_;
            retry = !connected_ && next_retry_ && Now() >= next_retry_;
            close_ap = connected_ && ap_active_ && ap_close_at_ && Now() >= ap_close_at_;
            if (retry)
                next_retry_ = Now() + 5000;
            if (timeout) {
                connection_deadline_ = next_retry_ = 0;
                candidate_ = false;
                wifi_state_ = "failed";
                if (wifi_error_.empty())
                    wifi_error_ = associated_ ? "dhcp_timeout" : "connection_timeout";
                wifi_trial_.Fail(wifi_error_);
            }
        }
        if (timeout) {
            std::string reason;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                reason = wifi_error_;
            }
            SetAp(true);
            ConnectStored();
            {
                std::lock_guard<std::mutex> guard(mutex_);
                wifi_error_ = "new/previous connection failed: " + reason;
            }
        } else if (retry)
            esp_wifi_connect();
        if (close_ap)
            SetAp(false);
        RefreshPowerSave();
    }
}
void DeviceSetup::Probe() {
    auto record = [this](const char* state) {
        std::lock_guard<std::mutex> guard(mutex_);
        server_state_ = state;
        last_server_check_ = Now();
    };
    {
        std::lock_guard<std::mutex> guard(mutex_);
        probing_queued_ = false;
        probing_ = true;
        server_state_ = "connecting";
    }
    auto state = Application::GetInstance().GetDeviceState();
    if (!connected_ || (state != kDeviceStateIdle && state != kDeviceStateStarting)) {
        record(connected_ ? "busy" : "wifi_offline");
        probing_ = false;
        return;
    }
    probing_ = true;
    RefreshPowerSave();
    auto profile = Parse(Profile());
    auto url = Text(profile.get(), "url"), token = Text(profile.get(), "token");
    if (!deepflipper::ValidVoiceEndpoint(url) || token.empty()) {
        record("needs_config");
        probing_ = false;
        return;
    }
    struct ProbeState {
        std::atomic<bool> hello{false}, auth{false}, invalid{false}, closed{false};
    };
    auto result = std::make_shared<ProbeState>();
    auto websocket = Board::GetInstance().GetNetwork()->CreateWebSocket(3);
    if (!websocket) {
        record("connect_failed");
        probing_ = false;
        return;
    }
    websocket->SetHeader("Authorization", ("Bearer " + token).c_str());
    websocket->SetHeader("Device-Id", device_id_.c_str());
    websocket->SetHeader("Client-Id", client_id_.c_str());
    websocket->SetHeader("Protocol-Version", "1");
    websocket->OnData([result](const char* data, size_t size, bool binary) {
        if (binary || size > 4096)
            return;
        std::string text(data, size);
        if (text == "认证失败") {
            result->auth = true;
            return;
        }
        auto json = Parse(text);
        if (voicehello::Valid(json.get()))
            result->hello = true;
        else if (Text(json.get(), "type") == "hello")
            result->invalid = true;
    });
    websocket->OnDisconnected([result]() { result->closed = true; });
    record("connecting");
    auto connected = websocket->Connect(url.c_str());
    if (!connected)
        record(NetworkCode(connected.error()));
    else if (
        !websocket->Send(
            R"({"type":"hello","version":1,"transport":"websocket","features":{},"audio_params":{"format":"opus","sample_rate":16000,"channels":1,"frame_duration":60}})"))
        record("connect_failed");
    else {
        int64_t deadline = Now() + 10000;
        while (!result->hello && !result->auth && !result->invalid && !result->closed &&
               Now() < deadline)
            vTaskDelay(pdMS_TO_TICKS(50));
        record(result->hello     ? "authenticated"
               : result->auth    ? "auth_failed"
               : result->invalid ? "invalid_hello"
                                 : "hello_timeout");
    }
    websocket->Close();
    websocket.reset();
    probing_ = false;
}

esp_err_t DeviceSetup::Handler(httpd_req_t* request) {
    return static_cast<DeviceSetup*>(request->user_ctx)->Http(request);
}
esp_err_t DeviceSetup::Http(httpd_req_t* request) {
    DeferredReply reply{request, "", "", ""};
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Content-Security-Policy",
                       "default-src 'self'; script-src 'self'; style-src 'self'; connect-src "
                       "'self'; media-src 'self' blob:; frame-ancestors 'none'; base-uri 'none'");
    std::string path = request->uri;
    path = path.substr(0, path.find('?'));
    auto respond = [&](const std::string& value, const char* status = "200 OK") {
        reply.value = value;
        reply.status = status;
        return ESP_OK;
    };
    auto error = [&](const char* code, const char* status = "400 Bad Request") {
        return respond(std::string("{\"error\":\"") + code + "\"}", status);
    };
    if (path.rfind("/api/", 0) != 0) {
        if (request->method != HTTP_GET)
            return error("not_found", "404 Not Found");
        const char *begin, *end, *type;
        if (path == "/audio-preview.js") {
            begin = preview_js_start;
            end = preview_js_end;
            type = "application/javascript";
        } else if (path == "/audio-preview.wav") {
            begin = preview_wav_start;
            end = preview_wav_end;
            type = "audio/wav";
        } else if (path == "/app.js") {
            begin = setup_js_start;
            end = setup_js_end;
            type = "application/javascript";
        } else if (path == "/style.css") {
            begin = setup_css_start;
            end = setup_css_end;
            type = "text/css";
        } else {
            begin = setup_html_start;
            end = setup_html_end;
            type = "text/html; charset=utf-8";
        }
        httpd_resp_set_type(request, type);
        return httpd_resp_send(request, begin, end - begin);
    }
    const bool write = request->method == HTTP_POST;
    if (write) {
        auto origin = Header(request, "Origin"), host = Header(request, "Host");
        if (!origin.empty() && origin != "http://" + host)
            return error("foreign_origin", "403 Forbidden");
        const bool raw = path == "/api/audio/chunk";
        if (Header(request, "Content-Type")
                .rfind(raw ? "application/octet-stream" : "application/json", 0) != 0)
            return error("json_required", "415 Unsupported Media Type");
        if (request->content_len <= 0 ||
            request->content_len > (raw ? dfpreview::kChunkBytes : 8192))
            return error("body_too_large", "413 Payload Too Large");
    }
    if (path != "/api/login") {
        bool authorized;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            authorized =
                write ? session_.Write(Cookie(request), Header(request, "X-CSRF-Token"), Now())
                      : session_.Valid(Cookie(request), Now());
        }
        if (!authorized)
            return error("login_required", "401 Unauthorized");
    }
    std::string body;
    if (write) {
        body.resize(request->content_len);
        size_t received = 0;
        while (received < body.size()) {
            int count = httpd_req_recv(request, body.data() + received, body.size() - received);
            if (count <= 0)
                return ESP_FAIL;
            received += count;
        }
    }
    if (path == "/api/audio/chunk" && write) {
        auto offset = Header(request, "X-PCM-Offset");
        if (offset.empty() || offset.size() > 8 ||
            offset.find_first_not_of("0123456789") != std::string::npos)
            return error("invalid_audio_chunk");
        if (!AudioPreview::Get().Chunk(Header(request, "X-Preview-ID"),
                                       strtoul(offset.c_str(), nullptr, 10),
                                       reinterpret_cast<const uint8_t*>(body.data()), body.size()))
            return error("invalid_audio_chunk", "409 Conflict");
        return respond("{\"ok\":true}");
    }
    auto input = Parse(write ? body : "{}");
    if (!cJSON_IsObject(input.get()))
        return error("invalid_json");
    if (path == "/api/login" && write) {
        std::string session, csrf;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (Now() - login_window_ > 30000) {
                login_window_ = Now();
                login_failures_ = 0;
            }
            if (login_failures_ >= 5)
                return error("try_later", "429 Too Many Requests");
            if (!Equal(Hash(auth_salt_, Text(input.get(), "password")), auth_hash_)) {
                ++login_failures_;
                return error("invalid_password", "401 Unauthorized");
            }
            login_failures_ = 0;
            session = Random(32);
            csrf = Random(16);
            session_.Open(session, csrf, Now());
        }
        reply.cookie =
            "df_session=" + session + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=1800";
        httpd_resp_set_hdr(request, "Set-Cookie", reply.cookie.c_str());
        return respond("{\"csrf\":\"" + csrf + "\"}");
    }
    if (path == "/api/session" && !write) {
        std::lock_guard<std::mutex> guard(mutex_);
        return respond("{\"csrf\":\"" + session_.Csrf() + "\"}");
    }
    if (path == "/api/logout" && write) {
        AudioPreview::Get().Stop();
        std::lock_guard<std::mutex> guard(mutex_);
        session_.Close();
        httpd_resp_set_hdr(request, "Set-Cookie",
                           "df_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
        return respond("{\"ok\":true}");
    }
    if (path == "/api/status" && !write)
        return respond(Status(false));
    if (path == "/api/audio/start" && write) {
        if (auto why = ActionError(true))
            return error(why, "409 Conflict");
        if (!Application::GetInstance().GetAudioService().IsPlaybackIdle())
            return error("busy", "409 Conflict");
        auto samples = dfcompanion::UInt32(input.get(), "samples");
        if (dfcompanion::UInt32(input.get(), "sample_rate") != dfpreview::kRate || !samples ||
            samples > dfpreview::kMaxSamples)
            return error("invalid_audio_format");
        auto id = AudioPreview::Get().Begin(samples);
        if (id.empty())
            return error("preview_busy_or_no_memory", "409 Conflict");
        return respond("{\"ok\":true,\"id\":\"" + id + "\"}");
    }
    if (path == "/api/audio/commit" && write) {
        if (!AudioPreview::Get().Commit(Text(input.get(), "id")))
            return error("audio_upload_incomplete", "409 Conflict");
        return respond("{\"ok\":true,\"state\":\"queued\"}", "202 Accepted");
    }
    if (path == "/api/audio/stop" && write) {
        if (!AudioPreview::Get().Stop(Text(input.get(), "id")))
            return error("audio_id_mismatch", "409 Conflict");
        return respond("{\"ok\":true}");
    }
    if (path == "/api/settings" && !write)
        return respond(Status());
    if (path == "/api/identity" && !write) {
        auto json = Parse("{}");
        cJSON_AddStringToObject(json.get(), "format", "deepflipper.identity");
        cJSON_AddNumberToObject(json.get(), "version", 1);
        cJSON_AddStringToObject(json.get(), "device_id", device_id_.c_str());
        cJSON_AddStringToObject(json.get(), "client_id", client_id_.c_str());
        return respond(Encode(json.get()));
    }
    if ((path == "/api/settings" || path == "/api/import") && write) {
        std::string why;
        bool reload = false;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            auto old = Parse(profile_);
            if (probing_ || probing_queued_ || Applying())
                return error("busy", "409 Conflict");
            if (Number(input.get(), "revision", -1) != Number(old.get(), "revision"))
                return error("revision_conflict", "409 Conflict");
            if (path == "/api/import") {
                if (!Connection(input.get(), device_id_, client_id_, why))
                    return error(why.c_str());
                Put(input.get(), "url", Text(input.get(), "server_url"));
            }
            auto next = Merge(profile_, input.get(), why);
            if (!next)
                return error(why.c_str());
            reload = Text(old.get(), "url") != Text(next.get(), "url") ||
                     Text(old.get(), "token") != Text(next.get(), "token");
            if (reload && AudioPreview::Get().Busy())
                return error("busy", "409 Conflict");
            SaveLocked(next.get());
            if (reload) {
                server_state_ = "not_tested";
                last_server_check_ = 0;
            }
        }
        Apply(reload);
        return respond(Status());
    }
    if (path == "/api/wifi" && write) {
        if (AudioPreview::Get().Busy())
            return error("busy", "409 Conflict");
        std::string why;
        auto request_id = Text(input.get(), "request_id");
        if (!Plain(request_id, 64, true))
            return error("invalid_request_id");
        if (request_id.empty())
            request_id = Random(8);
        if (!Wifi(input.get(), why))
            return error(why.c_str());
        {
            std::lock_guard<std::mutex> guard(mutex_);
            auto profile = Parse(profile_);
            if (Number(input.get(), "revision", -1) != Number(profile.get(), "revision"))
                return error("revision_conflict", "409 Conflict");
            if (wifi_trial_.Pending() || probing_ || probing_queued_ || Applying())
                return error("busy", "409 Conflict");
            wifi_trial_.Begin(Text(input.get(), "ssid"), Text(input.get(), "wifi_password"),
                              request_id);
            if (!Queue(CommandType::Wifi)) {
                wifi_trial_.Fail("busy");
                return error("busy", "409 Conflict");
            }
        }
        int job;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            job = wifi_trial_.Id();
        }
        return respond("{\"ok\":true,\"state\":\"pending\",\"job\":" + std::to_string(job) + "}",
                       "202 Accepted");
    }
    if (path == "/api/scan" && !write) {
        auto json = Parse("{}");
        auto list = cJSON_AddArrayToObject(json.get(), "networks");
        std::lock_guard<std::mutex> guard(mutex_);
        cJSON_AddBoolToObject(json.get(), "scanning", scanning_);
        for (const auto& network : networks_) {
            auto item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "ssid", network.ssid.c_str());
            cJSON_AddNumberToObject(item, "rssi", network.rssi);
            cJSON_AddBoolToObject(item, "secure", network.secure);
            cJSON_AddItemToArray(list, item);
        }
        return respond(Encode(json.get()));
    }
    if (path == "/api/password" && write) {
        auto password = Text(input.get(), "password");
        if (password.size() < 8 || !Plain(password, 64))
            return error("password_length_8_64");
        std::lock_guard<std::mutex> guard(mutex_);
        auth_hash_ = Hash(auth_salt_, password);
        password_changed_ = true;
        session_.Close();
        Settings settings("deepflipper", true);
        settings.SetString("web_hash", auth_hash_);
        settings.SetBool("web_changed", true);
        return respond("{\"ok\":true,\"login_required\":true}");
    }
    if (write) {
        if (AudioPreview::Get().Busy() && path != "/api/stop-talk")
            return error("busy", "409 Conflict");
        CommandType action;
        if (path == "/api/hotspot")
            action = CommandType::Hotspot;
        else if (path == "/api/hotspot/close") {
            if (!connected_)
                return error("wifi_offline", "409 Conflict");
            action = CommandType::CloseHotspot;
        } else if (path == "/api/reconnect")
            action = CommandType::Retry;
        else if (path == "/api/scan")
            action = CommandType::Scan;
        else if (path == "/api/test-server")
            action = CommandType::Probe;
        else if (path == "/api/test-speaker") {
            if (auto why = ActionError(true))
                return error(why, "409 Conflict");
            Application::GetInstance().Schedule([this]() {
                if (!ActionError(true))
                    Application::GetInstance().PlaySound(Lang::Sounds::OGG_SUCCESS);
            });
            return respond("{\"ok\":true,\"result\":\"playback_requested\"}");
        } else if (path == "/api/talk") {
            if (auto why = ActionError(false))
                return error(why, "409 Conflict");
            if (!Talk("start"))
                return error("busy", "409 Conflict");
            return respond("{\"ok\":true,\"result\":\"listening_requested\"}");
        } else if (path == "/api/stop-talk") {
            if (Application::GetInstance().GetDeviceState() != kDeviceStateListening)
                return error("not_listening", "409 Conflict");
            Talk("stop");
            return respond("{\"ok\":true}");
        } else
            return error("not_found", "404 Not Found");
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (wifi_trial_.Pending() || probing_ || probing_queued_ || Applying() ||
                (action == CommandType::Scan && scanning_))
                return error("busy", "409 Conflict");
            if (action == CommandType::Scan)
                scanning_ = true;
            if (action == CommandType::Probe)
                probing_queued_ = true;
            if (!Queue(action)) {
                if (action == CommandType::Scan)
                    scanning_ = false;
                if (action == CommandType::Probe)
                    probing_queued_ = false;
                return error("busy", "409 Conflict");
            }
        }
        return respond("{\"ok\":true,\"state\":\"pending\"}", "202 Accepted");
    }
    return error("not_found", "404 Not Found");
}
