#include <esp_log.h>
#include <esp_network.h>
#include <esp_pm.h>
#include <esp_sntp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include "application.h"
#include "board.h"
#include "button.h"
#include "codecs/no_audio_codec.h"
#include "config.h"
#include "deepflipper_bridge.h"
#include "device_setup.h"
#include "display/display.h"
#include "mcp_server.h"
#include "settings.h"
#include "speaker_pcm.h"
#include "system_info.h"

class FlipperDisplay : public NoDisplay {
public:
    void SetStatus(const char*) override {}
    void SetChatMessage(const char*, const char*) override {}  // No captions or transcript logging.
    void ShowNotification(const char*, int) override {}
    bool SetEmotion(const char* emotion) override {
        DeepFlipperBridge::Get().SetEmotion(emotion);
        return true;
    }
};

class FlipperAudio : public NoAudioCodecSimplex {
    std::array<int32_t, 512> speaker_slots_{};

protected:
    int Write(const int16_t* pcm, int samples) override {
        std::lock_guard<std::mutex> guard(data_if_mutex_);
        int written = 0;
        while (written < samples) {
            size_t frames = std::min<size_t>(samples - written, speaker_slots_.size() / 2);
            dfspeaker::PackStereo(pcm + written, frames, output_volume_, speaker_slots_.data());
            size_t bytes = 0;
            esp_err_t result = i2s_channel_write(tx_handle_, speaker_slots_.data(), frames * 8,
                                                 &bytes, portMAX_DELAY);
            written += bytes / 8;
            if (result != ESP_OK || bytes != frames * 8) {
                ESP_LOGW("FlipperAudio", "I2S short write: error=%d bytes=%u", int(result),
                         unsigned(bytes));
                break;
            }
        }
        return written;
    }

public:
    FlipperAudio()
        : NoAudioCodecSimplex(16000, 24000, DF_SPK_BCLK, DF_SPK_WS, DF_SPK_DATA, DF_MIC_BCLK,
                              DF_MIC_WS, DF_MIC_DATA) {
        i2s_std_slot_config_t slots =
            I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
        slots.slot_mask = I2S_STD_SLOT_BOTH;
        ESP_ERROR_CHECK(i2s_channel_reconfig_std_slot(tx_handle_, &slots));
    }
    void Start() override {
        AudioCodec::Start();
        Settings settings("audio");
        const int volume = static_cast<int>(settings.GetInt("output_volume", 20));
        AudioCodec::SetOutputVolume(std::clamp(volume, 0, 100));
    }
    void SetOutputVolume(int volume) override {
        volume = std::clamp(volume, 0, 100);
        AudioCodec::SetOutputVolume(volume);
        DeepFlipperBridge::Get().VolumeChanged(volume);
    }
    bool InputData(std::vector<int16_t>& pcm) override {
        if (!DeepFlipperBridge::Get().MicrophoneEnabled() || !DeepFlipperBridge::Get().Linked()) {
            vTaskDelay(pdMS_TO_TICKS(20));
            return false;
        }
        return AudioCodec::InputData(pcm);
    }
    void OutputData(std::vector<int16_t>& pcm) override {
        auto& bridge = DeepFlipperBridge::Get();
        if (bridge.Muted() || !bridge.Linked() || output_volume() == 0) {
            std::fill(pcm.begin(), pcm.end(), 0);
            bridge.AudioLevel(-1);
        } else {
            double sum = 0;
            for (int16_t value : pcm)
                sum += static_cast<double>(value) * value;
            double rms = pcm.empty() ? 0 : std::sqrt(sum / pcm.size());
            bridge.AudioLevel(rms > 1600 ? 2 : rms > 250 ? 1 : 0);
        }
        AudioCodec::OutputData(pcm);
    }
};

class DeepFlipperBoard : public Board {
    FlipperDisplay display_;
    Button talk_{DF_TALK_BUTTON, false, 5000};
    static ReturnValue Queue(const char* op, cJSON* args = nullptr, int timeout = 5000) {
        std::string params = "{}";
        if (args) {
            char* encoded = cJSON_PrintUnformatted(args);
            if (encoded) {
                params = encoded;
                cJSON_free(encoded);
            }
            cJSON_Delete(args);
        }
        int job = DeepFlipperBridge::Get().Submit(op, timeout, params);
        if (job <= 0) {
            const char* code = job == -1   ? "link_offline"
                               : job == -2 ? "firmware_update_required"
                               : job == -3 ? "invalid_args"
                                           : "busy";
            return std::string("{\"ok\":false,\"code\":\"") + code + "\"}";
        }
        return std::string("{\"job\":") + std::to_string(job) +
               ",\"state\":\"pending\",\"wait_ms\":" +
               std::to_string(std::string(op).rfind("native.", 0) == 0 ? 30000 : timeout + 3000) +
               "}";
    }
    void RegisterTools() {
        auto& mcp = McpServer::GetInstance();
        mcp.AddTool(
            "flipper_get_status", "Read companion/Flipper status; server waits for the job result.",
            PropertyList(), [](const PropertyList&) -> ReturnValue { return Queue("status"); });
        mcp.AddTool("flipper_nfc_scan",
                    "Identify nearby NFC protocols; does not write/authenticate/read full cards.",
                    PropertyList({Property("timeout_ms", kPropertyTypeInteger, 5000, 250, 10000)}),
                    [](const PropertyList& p) -> ReturnValue {
                        return Queue("tool.nfc.scan", nullptr, p["timeout_ms"].value<int>());
                    });
        mcp.AddTool("flipper_job_get", "Get the actual final result of a submitted local job.",
                    PropertyList({Property("job", kPropertyTypeInteger, 1, 2147483647)}),
                    [](const PropertyList& p) -> ReturnValue {
                        return DeepFlipperBridge::Get().Job(p["job"].value<int>());
                    });
        auto capture = [](const char* kind, const PropertyList& p) -> ReturnValue {
            auto args = cJSON_CreateObject();
            cJSON_AddStringToObject(args, "kind", kind);
            cJSON_AddNumberToObject(args, "timeout_ms", p["timeout_ms"].value<int>());
            if (std::string(kind) == "subghz")
                cJSON_AddNumberToObject(args, "frequency", p["frequency"].value<int>());
            return Queue("native.capture", args);
        };
        mcp.AddTool("flipper_nfc_read",
                    "Read a live card, return UID/type and public data preview; save .nfc. Type2 "
                    "public pages are supported; encrypted/application data may be unavailable. "
                    "Requires SD Tools adapter.",
                    PropertyList({Property("timeout_ms", kPropertyTypeInteger, 5000, 250, 10000)}),
                    [capture](const PropertyList& p) -> ReturnValue { return capture("nfc", p); });
        mcp.AddTool("flipper_infrared_learn",
                    "Learn one live IR button and save a .ir remote with button Captured. Raw "
                    "carrier is assumed 38kHz. Use flipper_run_saved infrared to send it later.",
                    PropertyList({Property("timeout_ms", kPropertyTypeInteger, 5000, 250, 10000)}),
                    [capture](const PropertyList& p) -> ReturnValue { return capture("ir", p); });
        mcp.AddTool(
            "flipper_subghz_record",
            "Receive internal-radio OOK650 RAW at an explicit frequency and save .sub; not "
            "protocol decoding or proof of a valid replay. Use flipper_run_saved subghz to send "
            "later.",
            PropertyList(
                {Property("frequency", kPropertyTypeInteger, 433920000, 300000000, 928000000),
                 Property("timeout_ms", kPropertyTypeInteger, 5000, 250, 10000)}),
            [capture](const PropertyList& p) -> ReturnValue { return capture("subghz", p); });
        mcp.AddTool(
            "flipper_read_file",
            "Read up to 1024 bytes from SD as hex; use offset to continue. No file modification.",
            PropertyList({Property("path", kPropertyTypeString).SetMaxLength(192),
                          Property("offset", kPropertyTypeInteger, 0, 0, 10000000),
                          Property("size", kPropertyTypeInteger, 512, 1, 1024)}),
            [](const PropertyList& p) -> ReturnValue {
                auto a = cJSON_CreateObject();
                cJSON_AddStringToObject(a, "path", p["path"].value<std::string>().c_str());
                cJSON_AddNumberToObject(a, "offset", p["offset"].value<int>());
                cJSON_AddNumberToObject(a, "size", p["size"].value<int>());
                return Queue("native.file.read", a);
            });
        mcp.AddTool("flipper_storage_info", "Read SD total/free space.",
                    PropertyList({Property("path", kPropertyTypeString, "/ext").SetMaxLength(192)}),
                    [](const PropertyList& p) -> ReturnValue {
                        auto a = cJSON_CreateObject();
                        cJSON_AddStringToObject(a, "path", p["path"].value<std::string>().c_str());
                        return Queue("native.storage.info", a);
                    });
        mcp.AddTool(
            "flipper_clock",
            "Read or set Flipper RTC; setting needs all date/time fields including weekday (1..7).",
            PropertyList({Property("action", kPropertyTypeString, "get"),
                          Property("hour", kPropertyTypeInteger, 0, 0, 23),
                          Property("minute", kPropertyTypeInteger, 0, 0, 59),
                          Property("second", kPropertyTypeInteger, 0, 0, 59),
                          Property("day", kPropertyTypeInteger, 1, 1, 31),
                          Property("month", kPropertyTypeInteger, 1, 1, 12),
                          Property("year", kPropertyTypeInteger, 2026, 2000, 2099),
                          Property("weekday", kPropertyTypeInteger, 1, 1, 7)}),
            [](const PropertyList& p) -> ReturnValue {
                auto a = cJSON_CreateObject();
                cJSON_AddStringToObject(a, "action", p["action"].value<std::string>().c_str());
                for (auto key : {"hour", "minute", "second", "day", "month", "year", "weekday"})
                    cJSON_AddNumberToObject(a, key, p[key].value<int>());
                return Queue("native.clock", a);
            });
        mcp.AddTool("flipper_gpio",
                    "Read/input/output/write one free expansion pin PC0 PC1 PC3 PB2 PB3 PA4 PA6 "
                    "PA7. Output mode initializes low; writes require output mode. UART and power "
                    "pins are excluded.",
                    PropertyList({Property("pin", kPropertyTypeString),
                                  Property("action", kPropertyTypeString, "read"),
                                  Property("value", kPropertyTypeInteger, 0, 0, 1)}),
                    [](const PropertyList& p) -> ReturnValue {
                        auto a = cJSON_CreateObject();
                        cJSON_AddStringToObject(a, "pin", p["pin"].value<std::string>().c_str());
                        cJSON_AddStringToObject(a, "action",
                                                p["action"].value<std::string>().c_str());
                        cJSON_AddNumberToObject(a, "value", p["value"].value<int>());
                        return Queue("native.gpio", a);
                    });
        mcp.AddTool("flipper_job_cancel",
                    "Request cancellation; query the job for final stop/result status.",
                    PropertyList({Property("job", kPropertyTypeInteger, 1, 2147483647)}),
                    [](const PropertyList& p) -> ReturnValue {
                        bool requested = DeepFlipperBridge::Get().Cancel(p["job"].value<int>());
                        return std::string("{\"ok\":") + (requested ? "true" : "false") +
                               ",\"requested\":" + (requested ? "true" : "false") + "}";
                    });
        mcp.AddTool("flipper_system_info",
                    "Read native firmware/hardware and power information. Requires matching "
                    "DeepFlipper firmware.",
                    PropertyList(),
                    [](const PropertyList&) -> ReturnValue { return Queue("native.system.info"); });
        mcp.AddTool("flipper_app_state",
                    "Read desktop PIN lock, foreground occupancy and RPC-app state (not arbitrary "
                    "app internals).",
                    PropertyList(),
                    [](const PropertyList&) -> ReturnValue { return Queue("native.app.state"); });
        auto list_properties = [](const std::string& path) {
            return PropertyList({Property("path", kPropertyTypeString, path).SetMaxLength(192),
                                 Property("offset", kPropertyTypeInteger, 0, 0, 10000),
                                 Property("limit", kPropertyTypeInteger, 8, 1, 8)});
        };
        auto list = [](const char* op, const PropertyList& p) -> ReturnValue {
            auto args = cJSON_CreateObject();
            cJSON_AddStringToObject(args, "path", p["path"].value<std::string>().c_str());
            cJSON_AddNumberToObject(args, "offset", p["offset"].value<int>());
            cJSON_AddNumberToObject(args, "limit", p["limit"].value<int>());
            return Queue(op, args);
        };
        mcp.AddTool("flipper_list_files",
                    "List SD directory metadata under /ext, 8 entries/page. Follow next_offset; -1 "
                    "means end. No file writes/deletes.",
                    list_properties("/ext"), [list](const PropertyList& p) -> ReturnValue {
                        return list("native.files", p);
                    });
        mcp.AddTool("flipper_list_apps",
                    "List main launch names and SD apps/categories. Browse category paths under "
                    "/ext/apps; only .fap files are launchable.",
                    list_properties("/ext/apps"), [list](const PropertyList& p) -> ReturnValue {
                        return list("native.apps", p);
                    });
        mcp.AddTool("flipper_app_open",
                    "Open a named main app or existing /ext/apps/...fap. Opening does not execute "
                    "a read/send/write action. rpc_mode only requests app RPC support.",
                    PropertyList({Property("name", kPropertyTypeString).SetMaxLength(192),
                                  Property("rpc_mode", kPropertyTypeBoolean, false)}),
                    [](const PropertyList& p) -> ReturnValue {
                        auto args = cJSON_CreateObject();
                        cJSON_AddStringToObject(args, "name",
                                                p["name"].value<std::string>().c_str());
                        cJSON_AddBoolToObject(args, "rpc_mode", p["rpc_mode"].value<bool>());
                        return Queue("native.app.open", args);
                    });
        mcp.AddTool("flipper_app_exit",
                    "Exit the current RPC-enabled app and verify foreground release. Ordinary apps "
                    "without RPC may require Back key inputs instead.",
                    PropertyList(),
                    [](const PropertyList&) -> ReturnValue { return Queue("native.app.exit"); });
        mcp.AddTool("flipper_press_button",
                    "Deliver one short or logical long Flipper key event. Keys: "
                    "up/down/right/left/ok/back. "
                    "Delivery is not proof of the menu action result. PIN lock is respected.",
                    PropertyList({Property("key", kPropertyTypeString).SetMaxLength(5),
                                  Property("press_type", kPropertyTypeString, "short")}),
                    [](const PropertyList& p) -> ReturnValue {
                        auto args = cJSON_CreateObject();
                        cJSON_AddStringToObject(args, "key", p["key"].value<std::string>().c_str());
                        cJSON_AddStringToObject(args, "press_type",
                                                p["press_type"].value<std::string>().c_str());
                        return Queue("native.input", args);
                    });
        mcp.AddTool("flipper_alert",
                    "Play Flipper's built-in audiovisual alert; this is not ESP32 speech playback.",
                    PropertyList(),
                    [](const PropertyList&) -> ReturnValue { return Queue("native.alert"); });
        mcp.AddTool(
            "flipper_run_saved",
            "Run a saved file for bounded time, then stop/exit and verify release. kind: infrared "
            "(.ir /ext/infrared), subghz (.sub /ext/subghz), nfc (.nfc /ext/nfc), rfid (.rfid "
            "/ext/lfrfid), ibutton (.ibtn /ext/ibutton). IR/SubGHz transmit; card/key kinds "
            "emulate. Not live read/write. A send result never proves the target reacted. Busy "
            "apps are not replaced.",
            PropertyList({Property("kind", kPropertyTypeString).SetMaxLength(8),
                          Property("path", kPropertyTypeString).SetMaxLength(192),
                          Property("button", kPropertyTypeString, std::string("")).SetMaxLength(80),
                          Property("index", kPropertyTypeInteger, 0, 0, 255),
                          Property("duration_ms", kPropertyTypeInteger, 1000, 100, 5000)}),
            [](const PropertyList& p) -> ReturnValue {
                auto args = cJSON_CreateObject();
                cJSON_AddStringToObject(args, "kind", p["kind"].value<std::string>().c_str());
                cJSON_AddStringToObject(args, "path", p["path"].value<std::string>().c_str());
                cJSON_AddStringToObject(args, "button", p["button"].value<std::string>().c_str());
                cJSON_AddNumberToObject(args, "index", p["index"].value<int>());
                cJSON_AddNumberToObject(args, "duration_ms", p["duration_ms"].value<int>());
                return Queue("native.saved", args);
            });
    }

public:
    DeepFlipperBoard() {
#if CONFIG_PM_ENABLE
        esp_pm_config_t power = {
            .max_freq_mhz = 240, .min_freq_mhz = 80, .light_sleep_enable = false};
        ESP_ERROR_CHECK(esp_pm_configure(&power));
#endif
        DeviceSetup::Get().Initialize(SystemInfo::GetMacAddress(), GetUuid());
        DeepFlipperBridge::Get().RestoreSettings();
        DeepFlipperBridge::Get().Start();
        talk_.OnClick([]() { DeviceSetup::Get().Talk(); });
        RegisterTools();
        talk_.OnLongPress([]() { DeviceSetup::Get().Recover(); });
    }
    bool UseDirectVoiceServer() const override { return true; }
    void StartNetwork() override {
        DeviceSetup::Get().Start();
        // TLS time is established through SNTP; certificate checking stays enabled.
        if (esp_sntp_enabled() == false) {
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init();
        }
    }
    Led* GetLed() override { return &DeepFlipperBridge::Get(); }
    void SetNetworkEventCallback(NetworkEventCallback callback) override {
        DeviceSetup::Get().SetCallback(std::move(callback));
    }
    const char* GetNetworkStateIcon() override { return ""; }
    void SetPowerSaveLevel(PowerSaveLevel level) override { DeviceSetup::Get().PowerSave(level); }
    std::string GetDeviceStatusJson() override { return DeviceSetup::Get().Status(false); }
    std::string GetBoardJson() override { return DeviceSetup::Get().Status(false); }
    std::string GetBoardType() override { return "wifi"; }
    NetworkInterface* GetNetwork() override {
        static EspNetwork network;
        return &network;
    }
    void OnVoiceConnectionState(const char* state) override {
        DeviceSetup::Get().VoiceState(state);
    }
    Display* GetDisplay() override { return &display_; }
    AudioCodec* GetAudioCodec() override {
        static FlipperAudio codec;
        return &codec;
    }
};

DECLARE_BOARD(DeepFlipperBoard);
