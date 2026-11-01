#pragma once
#include <driver/uart.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include "device_state.h"
#include "expansion_link.h"
#include "led/led.h"
#include "native_actions.h"
#include "rpc_wire.h"

class DeepFlipperBridge : public Led, public dfnative::Transport {
public:
    static DeepFlipperBridge& Get();
    void Start();
    void OnStateChanged() override;
    void SetEmotion(const char* name);
    void AudioLevel(int level);
    int Submit(const std::string& op, int timeout_ms = 5000, const std::string& args = "{}");
    std::string Job(int id);
    bool Cancel(int id);
    bool MicrophoneEnabled() const { return microphone_.load(); }
    bool Muted() const { return muted_.load(); }
    bool Linked() const { return linked_.load(); }
    void RestoreSettings();
    void RefreshSettings();
    void VolumeChanged(int volume);
    std::string LinkDiagnostics();

private:
    struct JobEntry {
        int id = 0;
        std::string op;
        int timeout_ms = 5000;
        uint32_t remote = 0;
        bool pending = false;
        bool cancel = false;
        bool consumed = false;
        int64_t completed_ms = 0, deadline_ms = 0;
        std::string args;
        std::string result;
    };
    std::array<JobEntry, 4> jobs_;
    std::mutex mutex_;
    std::atomic<int> state_{0}, emotion_{0}, mouth_{0};
    std::atomic<bool> muted_{false}, microphone_{true};
    std::atomic<bool> configured_{false};
    std::atomic<bool> applying_config_{false};
    std::atomic<bool> linked_{false};
    std::atomic<bool> native_supported_{false};
    int next_job_ = 0;
    uint32_t session_ = 0, rpc_id_ = 0, command_id_ = 0;
    std::atomic<uint32_t> audio_tick_ms_{0};
    std::vector<uint8_t> rx_frame_, rpc_stream_;
    uint32_t response_id_ = 0;
    std::string response_;
    std::string module_result_;
    using NativeReply = dfnative::Reply;
    NativeReply native_reply_;
    uint32_t native_id_ = 0;
    uint32_t native_first_id_ = 0;
    std::function<bool(const dfwire::Message&)> native_sink_;
    int native_job_id_ = 0;
    int64_t native_deadline_ms_ = 0;
    bool rpc_app_running_ = false;
    std::string rpc_app_name_;
    TaskHandle_t task_ = nullptr;
    bool status_received_ = false, status_ok_ = false;
    bool discovery_received_ = false;
    std::atomic<dfexpansion::LinkStage> link_stage_{dfexpansion::LinkStage::Starting};
    std::atomic<uint32_t> uart_attempts_{0}, uart_rx_bytes_{0}, uart_frame_errors_{0};
    std::atomic<uint32_t> requested_baud_{dfexpansion::kPreferredBaud};
    std::atomic<int> last_status_error_{-1};
    dfexpansion::LinkStage last_logged_failure_ = dfexpansion::LinkStage::Starting;
    int64_t last_link_log_ms_ = 0;
    dfexpansion::LinkRate link_rate_;
    bool started_ = false;
    bool rpc_transport_active_ = false;
    bool transport_failed_ = false;
    std::atomic<const char*> last_disconnect_{"none"};
    std::atomic<uint32_t> disconnects_{0};
    std::atomic<uint32_t> max_pump_gap_ms_{0}, stack_free_bytes_{0};
    int64_t last_pump_us_ = 0;
    std::string previous_disconnect_;
    int64_t last_heartbeat_us_ = 0;
    int64_t last_led_refresh_us_ = 0;
    std::atomic<int> brightness_{12};
    bool tool_active_ = false;
    bool Frame(uint8_t type, const uint8_t* data, size_t size);
    bool ReadFrame(int timeout_ms);
    void Dispatch(const std::vector<uint8_t>& frame);
    bool WaitStatus(int timeout_ms);
    void FailLink(const char* reason);
    bool Connect();
    std::string Call(const std::string& request);
    bool SendWire(const std::vector<uint8_t>& wire);
    NativeReply Native(uint32_t tag, const std::vector<uint8_t>& payload = {},
                       std::function<bool(const dfwire::Message&)> sink = {}, bool cleanup = false,
                       int short_key = -1) override;
    bool NativeCancelled() override;
    bool Alive() const override { return session_ != 0; }
    int64_t NowMs() const override;
    void Pump() override;
    void Reset() override { FailLink("native_state_unconfirmed"); }
    bool AppRunning() const override { return rpc_app_running_; }
    std::string AppName() const override { return rpc_app_name_; }
    void SetAppName(const std::string& name) override { rpc_app_name_ = name; }
    std::string ExecuteNative(const JobEntry& job);
    void Run();
    void ApplyCommand(const std::string& command);
    std::string PublicResult(uint32_t id, bool success);
    void UpdateLed();
};
