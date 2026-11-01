#pragma once
#include <cstdint>
#include <string>

namespace dfcompanion {
// Keep interactive updates responsive, without repeatedly sending unchanged UI
// or the full module profile. The transport heartbeat is scheduled separately.
class Updates {
    int64_t poll_ms_ = -200, ui_ms_ = -2000, report_ms_ = -5000;
    int state_ = -1, mouth_ = -1, emotion_ = -1;
    std::string report_;

public:
    bool Poll(int64_t now) {
        if (now - poll_ms_ < 200)
            return false;
        poll_ms_ = now;
        return true;
    }
    bool Ui(int64_t now, int state, int mouth, int emotion) {
        if (now - ui_ms_ < 50 ||
            (state == state_ && mouth == mouth_ && emotion == emotion_ && now - ui_ms_ < 2000))
            return false;
        ui_ms_ = now;
        state_ = state;
        mouth_ = mouth;
        emotion_ = emotion;
        return true;
    }
    bool Report(int64_t now, const std::string& report) {
        if (report == report_ && now - report_ms_ < 5000)
            return false;
        report_ms_ = now;
        report_ = report;
        return true;
    }
};
}  // namespace dfcompanion
