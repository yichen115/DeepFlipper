#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace dfpreview {
constexpr uint32_t kRate = 24000;
constexpr uint32_t kMaxSamples = kRate * 30;
constexpr size_t kChunkBytes = 65536;
enum class Stage { Idle, Uploading, Queued, Playing, Stopping, Completed, Cancelled, Failed };
inline const char* Name(Stage stage) {
    switch (stage) {
        case Stage::Uploading:
            return "uploading";
        case Stage::Queued:
            return "queued";
        case Stage::Playing:
            return "playing";
        case Stage::Stopping:
            return "stopping";
        case Stage::Completed:
            return "completed";
        case Stage::Cancelled:
            return "cancelled";
        case Stage::Failed:
            return "failed";
        default:
            return "idle";
    }
}
struct Session {
    Stage stage = Stage::Idle;
    std::string id, error;
    size_t total = 0, received = 0;
    int64_t touched = 0;
    bool Busy() const {
        return stage == Stage::Uploading || stage == Stage::Queued || stage == Stage::Playing ||
               stage == Stage::Stopping;
    }
    bool Begin(const std::string& token, uint32_t samples, int64_t now) {
        if (Busy() || token.empty() || !samples || samples > kMaxSamples)
            return false;
        id = token;
        total = size_t(samples) * 2;
        received = 0;
        touched = now;
        error.clear();
        stage = Stage::Uploading;
        return true;
    }
    bool Chunk(const std::string& token, size_t offset, size_t size, int64_t now) {
        if (stage != Stage::Uploading || token != id || offset != received || !size ||
            size > kChunkBytes || size % 2 || size > total - received)
            return false;
        received += size;
        touched = now;
        return true;
    }
    bool Commit(const std::string& token) {
        if (stage != Stage::Uploading || token != id || received != total)
            return false;
        stage = Stage::Queued;
        return true;
    }
    bool Stop(const std::string& token = "") {
        if (!token.empty() && token != id)
            return false;
        if (Busy())
            stage = Stage::Stopping;
        return true;
    }
    bool Expired(int64_t now) const { return stage == Stage::Uploading && now - touched > 15000; }
};
}  // namespace dfpreview
