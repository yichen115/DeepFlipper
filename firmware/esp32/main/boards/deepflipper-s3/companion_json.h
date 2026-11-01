#pragma once
#include <cJSON.h>
#include <cmath>
#include <cstdint>

namespace dfcompanion {
// cJSON::valueint saturates at INT32_MAX. Companion session/command/job IDs
// are uint32_t on Flipper and must be read from the exact double instead.
inline uint32_t UInt32(const cJSON* object, const char* key) {
    auto item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) || item->valuedouble < 0 ||
        item->valuedouble > UINT32_MAX || std::floor(item->valuedouble) != item->valuedouble)
        return 0;
    return static_cast<uint32_t>(item->valuedouble);
}
}  // namespace dfcompanion
