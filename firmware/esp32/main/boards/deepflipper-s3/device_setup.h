#pragma once
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include "board.h"
#include "dns_server.h"
#include "portal_config.h"
#include "setup_flow.h"

class DeviceSetup {
public:
    static DeviceSetup& Get();
    void Initialize(const std::string& device, const std::string& client);
    void Start();
    void SetCallback(NetworkEventCallback callback);
    std::string Profile();
    std::string Status(bool include_settings = true);
    void AppendFlipper(cJSON* output);
    bool Command(const std::string& field, const std::string& value);
    void SetVolume(int volume);
    void VoiceState(const char* state);
    bool Connected() const { return connected_; }
    bool Probing() const { return probing_ || probing_queued_; }
    bool Applying() const { return apply_pending_ > 0; }
    bool ReadyToTalk() { return !ActionError(false); }
    bool SetupMode();
    int VisualState(bool configured, int fallback);
    void PowerSave(PowerSaveLevel level);
    void Recover();

private:
    enum class CommandType { Wifi, Hotspot, CloseHotspot, Retry, Scan, Probe, Recover };
    struct Work {
        CommandType type;
        int64_t not_before_ms;
    };
    struct AccessPoint {
        std::string ssid;
        int rssi;
        bool secure;
    };
    std::mutex mutex_;
    std::mutex power_mutex_;
    std::string profile_, setup_key_, auth_salt_, auth_hash_, ssid_, ip_;
    std::string device_id_, client_id_, attempt_ssid_;
    std::string wifi_state_ = "starting", wifi_error_, server_state_ = "not_tested";
    std::string ap_ssid_, ap_ip_ = "192.168.4.1";
    NetworkEventCallback callback_;
    std::vector<AccessPoint> networks_;
    dfsetup::WebSession session_;
    dfsetup::WifiTrial wifi_trial_;
    bool password_changed_ = false, started_ = false, ap_active_ = false;
    bool candidate_ = false, scanning_ = false;
    std::atomic<bool> probing_queued_{false};
    std::atomic<int> apply_pending_{0};
    PowerSaveLevel power_save_ = PowerSaveLevel::LOW_POWER;
    bool wifi_ready_ = false;
    int applied_power_save_ = -1;
    int login_failures_ = 0;
    int64_t login_window_ = 0, connection_deadline_ = 0, next_retry_ = 0, ap_close_at_ = 0;
    int64_t last_server_check_ = 0;
    std::atomic<bool> connected_{false}, probing_{false}, voice_open_{false};
    std::atomic<bool> associated_{false};
    std::atomic<bool> got_ip_{false}, disconnected_{false}, scan_done_{false};
    QueueHandle_t queue_ = nullptr;
    esp_netif_t* station_ = nullptr;
    esp_netif_t* access_point_ = nullptr;
    httpd_handle_t http_ = nullptr;
    esp_event_handler_instance_t wifi_handler_ = nullptr, ip_handler_ = nullptr;
    DnsServer dns_;
    bool Queue(CommandType type);
    void Run();
    void SetAp(bool enabled);
    void ConnectStored(bool use_candidate = false);
    void HandleEvent(esp_event_base_t base, int32_t id, void* data);
    void Notify(NetworkEvent event, const std::string& data = "");
    void Probe();
    void SaveLocked(cJSON* profile);
    void Apply(bool reload_voice);
    void RefreshPowerSave();
    const char* ActionError(bool speaker);
    esp_err_t Http(httpd_req_t* request);
    static esp_err_t Handler(httpd_req_t* request);
};
