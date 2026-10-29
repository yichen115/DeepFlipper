#pragma once
#include <array>
#include <cstdint>

namespace dfexpansion {
constexpr uint32_t kDiscoveryBaud = 9600;
constexpr uint32_t kPreferredBaud = 460800;
constexpr uint32_t kFallbackBaud = 115200;

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
