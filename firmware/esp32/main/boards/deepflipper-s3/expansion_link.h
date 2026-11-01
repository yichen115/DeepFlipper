#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dfexpansion {
constexpr uint32_t kDiscoveryBaud = 9600;
constexpr uint32_t kPreferredBaud = 460800;
constexpr uint32_t kFallbackBaud = 115200;
constexpr uint8_t kPresenceByte = 0xAA;  // Same presence signal as Flipper's expansion_test.

inline std::vector<uint8_t> EncodeFrame(uint8_t type, const uint8_t* data, size_t size) {
    if ((size && !data) || type < 1 || type > 5 || (type == 1 && size != 0) ||
        ((type == 2 || type == 4) && size != 1) || (type == 3 && size != 4) ||
        (type == 5 && size > 64))
        return {};
    std::vector<uint8_t> frame{type};
    if (type == 5)
        frame.push_back(size);
    if (size)
        frame.insert(frame.end(), data, data + size);
    uint8_t checksum = 0;
    for (auto byte : frame)
        checksum ^= byte;
    frame.push_back(checksum);
    return frame;
}

enum class LinkStage {
    Starting,
    Discovery,
    NoHeartbeat,
    PresenceWriteFailed,
    BaudAck,
    BaudAckFailed,
    RpcStart,
    RpcStartFailed,
    Hello,
    HelloRejected,
    Online,
    FrameError,
    LinkLost
};
inline const char* StageName(LinkStage stage) {
    switch (stage) {
        case LinkStage::Starting:
            return "starting";
        case LinkStage::Discovery:
            return "discovery";
        case LinkStage::NoHeartbeat:
            return "no_heartbeat";
        case LinkStage::PresenceWriteFailed:
            return "presence_write_failed";
        case LinkStage::BaudAck:
            return "baud_ack";
        case LinkStage::BaudAckFailed:
            return "baud_ack_failed";
        case LinkStage::RpcStart:
            return "rpc_start";
        case LinkStage::RpcStartFailed:
            return "rpc_start_failed";
        case LinkStage::Hello:
            return "hello";
        case LinkStage::HelloRejected:
            return "hello_rejected";
        case LinkStage::Online:
            return "online";
        case LinkStage::FrameError:
            return "frame_error";
        case LinkStage::LinkLost:
            return "link_lost";
    }
    return "unknown";
}

inline std::array<uint8_t, 4> BaudPayload(uint32_t baud) {
    return {static_cast<uint8_t>(baud), static_cast<uint8_t>(baud >> 8),
            static_cast<uint8_t>(baud >> 16), static_cast<uint8_t>(baud >> 24)};
}

enum class ConnectResult { NoPeer, Failed, Ready };

// One attempt per call: the UART task supplies its reconnect backoff. A lost
// established connection or a failed handshake latches the lower rate until
// reboot. An absent/unpowered peer alone does not rule out the preferred rate.
class LinkRate {
    bool fallback_ = false;
    bool active_ = false;

public:
    uint32_t Baud() const { return fallback_ ? kFallbackBaud : kPreferredBaud; }
    bool Fallback() const { return fallback_; }
    bool Active() const { return active_; }

    template <class Attempt>
    bool Connect(Attempt&& attempt) {
        if (active_)
            fallback_ = true;
        active_ = false;
        const auto result = attempt(Baud());
        if (result == ConnectResult::Failed)
            fallback_ = true;
        active_ = result == ConnectResult::Ready;
        return active_;
    }
};
}  // namespace dfexpansion
