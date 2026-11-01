#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace dfspeaker {
// A mono I2S amplifier may select left, right or (left+right)/2. Equal
// left/right slots preserve level for all three hardware strap settings.
inline void PackStereo(const int16_t* pcm, size_t frames, int volume, int32_t* slots) {
    const int64_t v = std::clamp(volume, 0, 100);
    for (size_t i = 0; i < frames; ++i) {
        // Multiply rather than left-shift a negative signed PCM value.
        const int32_t sample = int32_t(int64_t(pcm[i]) * v * v * 65536 / 10000);
        slots[i * 2] = slots[i * 2 + 1] = sample;
    }
}
}  // namespace dfspeaker
