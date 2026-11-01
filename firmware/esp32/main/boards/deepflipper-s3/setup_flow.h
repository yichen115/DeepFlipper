#pragma once
#include <cstdint>
#include <string>

namespace dfsetup {
// One explicit Wi-Fi operation. Restoring the previous network cannot turn a
// failed trial into a successful save, and old IP events cannot finish a trial.
class WifiTrial {
    int id_ = 0;
    std::string state_ = "none", ssid_, password_, error_, request_;
    int64_t deadline_ = 0;
    int before_revision_ = -1, revision_ = -1;

public:
    bool Pending() const { return state_ == "queued" || state_ == "connecting"; }
    int Id() const { return id_; }
    const std::string& State() const { return state_; }
    const std::string& Ssid() const { return ssid_; }
    const std::string& Password() const { return password_; }
    const std::string& Error() const { return error_; }
    const std::string& Request() const { return request_; }
    int BeforeRevision() const { return before_revision_; }
    int Revision() const { return revision_; }
    bool Begin(std::string ssid, std::string password, std::string request = "") {
        if (Pending())
            return false;
        id_ = id_ >= 1000000000 ? 1 : id_ + 1;
        state_ = "queued";
        ssid_ = std::move(ssid);
        password_ = std::move(password);
        request_ = std::move(request);
        error_.clear();
        deadline_ = 0;
        before_revision_ = revision_ = -1;
        return true;
    }
    void Start(int64_t now) {
        if (state_ == "queued") {
            state_ = "connecting";
            deadline_ = now + 60000;
        }
    }
    bool CanConfirm(const std::string& ssid, int64_t now) const {
        return state_ == "connecting" && ssid_ == ssid && now < deadline_;
    }
    bool Succeed(const std::string& ssid, int64_t now, int before_revision, int revision) {
        if (!CanConfirm(ssid, now))
            return false;
        state_ = "saved";
        password_.clear();
        before_revision_ = before_revision;
        revision_ = revision;
        return true;
    }
    void Fail(const std::string& reason) {
        if (!Pending())
            return;
        state_ = "failed";
        error_ = reason;
        password_.clear();
        deadline_ = 0;
    }
};

enum class TalkAction { Reject, Start, Stop, Cancel };
inline TalkAction DecideTalk(const std::string& action, bool listening, bool can_start,
                             bool speaking = false) {
    if (action == "cancel" && (listening || speaking))
        return TalkAction::Cancel;
    if ((action == "toggle" || action == "stop") && listening)
        return TalkAction::Stop;
    if ((action == "toggle" || action == "start") && !listening && can_start)
        return TalkAction::Start;
    return TalkAction::Reject;
}
inline const char* TalkError(bool wifi, bool link, bool configured, bool microphone, bool busy,
                             bool ready) {
    if (!wifi)
        return "wifi_offline";
    if (!link)
        return "module_offline";
    if (!configured)
        return "needs_config";
    if (!microphone)
        return "microphone_disabled";
    if (busy || !ready)
        return "busy";
    return nullptr;
}
inline const char* SpeakerError(bool link, bool muted, int volume, bool busy) {
    if (!link)
        return "module_offline";
    if (muted)
        return "speaker_muted";
    if (volume == 0)
        return "volume_zero";
    return busy ? "busy" : nullptr;
}
inline int VisualState(bool configured, bool link, bool wifi, bool ap, const std::string& server,
                       int application_state) {
    if (!configured || (!wifi && ap))
        return 1;
    if (!link || !wifi || server == "connecting")
        return 2;
    if (server == "auth_failed" || server == "connect_failed" || server == "connect_timeout" ||
        server == "dns_failed" || server == "tls_failed" || server == "hello_timeout" ||
        server == "invalid_hello")
        return 8;
    return application_state;
}
}  // namespace dfsetup
