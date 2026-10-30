#pragma once
#include <cJSON.h>
#include <cstring>

namespace voicehello {
inline bool Valid(const cJSON* root, bool require_session = true) {
    auto type = cJSON_GetObjectItem(root, "type");
    auto transport = cJSON_GetObjectItem(root, "transport");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "hello") || !cJSON_IsString(transport) ||
        strcmp(transport->valuestring, "websocket"))
        return false;
    if (!require_session)
        return true;
    auto session = cJSON_GetObjectItem(root, "session_id");
    if (!cJSON_IsString(session) || !session->valuestring[0] || strlen(session->valuestring) > 128)
        return false;
    auto audio = cJSON_GetObjectItem(root, "audio_params");
    if (!audio)
        return true;
    if (!cJSON_IsObject(audio))
        return false;
    auto rate = cJSON_GetObjectItem(audio, "sample_rate");
    if (rate &&
        (!cJSON_IsNumber(rate) ||
         (rate->valuedouble != 8000 && rate->valuedouble != 12000 && rate->valuedouble != 16000 &&
          rate->valuedouble != 24000 && rate->valuedouble != 48000)))
        return false;
    auto duration = cJSON_GetObjectItem(audio, "frame_duration");
    return !duration || (cJSON_IsNumber(duration) &&
                         (duration->valuedouble == 10 || duration->valuedouble == 20 ||
                          duration->valuedouble == 40 || duration->valuedouble == 60));
}
}  // namespace voicehello
