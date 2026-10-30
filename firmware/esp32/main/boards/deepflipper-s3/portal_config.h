#pragma once
#include <cJSON.h>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include "voice_endpoint.h"

namespace dfsetup {
using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
inline Json Parse(const std::string& text) {
    if (text.find("\\u0000") != std::string::npos || text.find('\0') != std::string::npos)
        return Json(nullptr, cJSON_Delete);
    int depth = 0;
    bool quoted = false, escaped = false;
    for (char c : text) {
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                quoted = false;
        } else if (c == '"')
            quoted = true;
        else if (c == '{' || c == '[') {
            if (++depth > 16)
                return Json(nullptr, cJSON_Delete);
        } else if (c == '}' || c == ']') {
            if (--depth < 0)
                return Json(nullptr, cJSON_Delete);
        }
    }
    return Json(cJSON_ParseWithLengthOpts(text.c_str(), text.size() + 1, nullptr, true),
                cJSON_Delete);
}
inline std::string Text(const cJSON* json, const char* key) {
    auto item = cJSON_GetObjectItemCaseSensitive(json, key);
    return cJSON_IsString(item) ? item->valuestring : "";
}
inline int Number(const cJSON* json, const char* key, int fallback = 0) {
    auto item = cJSON_GetObjectItemCaseSensitive(json, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}
inline std::string Encode(const cJSON* json) {
    char* encoded = cJSON_PrintUnformatted(json);
    std::string value = encoded ? encoded : "{}";
    cJSON_free(encoded);
    return value;
}
inline void Put(cJSON* json, const char* key, const std::string& value) {
    cJSON_DeleteItemFromObjectCaseSensitive(json, key);
    cJSON_AddStringToObject(json, key, value.c_str());
}
inline bool Plain(const std::string& text, size_t maximum, bool empty = false) {
    if ((!empty && text.empty()) || text.size() > maximum)
        return false;
    for (unsigned char c : text)
        if (c < 32 || c == 127)
            return false;
    return true;
}
inline bool Token(const std::string& text) {
    if (!Plain(text, 2048))
        return false;
    for (unsigned char c : text)
        if (c <= 32 || c >= 127)
            return false;
    return true;
}
inline bool Wifi(const cJSON* input, std::string& error) {
    auto ssid = cJSON_GetObjectItemCaseSensitive(input, "ssid");
    auto password = cJSON_GetObjectItemCaseSensitive(input, "wifi_password");
    if (!cJSON_IsString(ssid) || !Plain(ssid->valuestring, 32) || !cJSON_IsString(password) ||
        !Plain(password->valuestring, 64, true)) {
        error = "invalid_wifi_fields";
        return false;
    }
    std::string secret = password->valuestring;
    if ((!secret.empty() && secret.size() < 8) ||
        (secret.size() == 64 &&
         secret.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)) {
        error = "invalid_wifi_password";
        return false;
    }
    return true;
}
inline Json ConfirmWifi(const std::string& old, const std::string& ssid,
                        const std::string& password, const std::string& observed_ssid) {
    if (ssid.empty() || ssid != observed_ssid)
        return Json(nullptr, cJSON_Delete);
    auto profile = Parse(old);
    if (!cJSON_IsObject(profile.get()))
        return Json(nullptr, cJSON_Delete);
    Put(profile.get(), "ssid", ssid);
    Put(profile.get(), "wifi_password", password);
    return profile;
}
// Only editable public fields enter the profile. Empty token preserves the old
// secret; explicit clearing is separate. Validate everything before mutating.
inline Json Merge(const std::string& old, const cJSON* input, std::string& error) {
    auto output = Parse(old);
    if (!cJSON_IsObject(input) || !cJSON_IsObject(output.get())) {
        error = "invalid_json";
        return Json(nullptr, cJSON_Delete);
    }
    for (auto key : {"url", "token"}) {
        auto value = cJSON_GetObjectItemCaseSensitive(input, key);
        if (!value)
            continue;
        if (!cJSON_IsString(value) ||
            (std::string(key) == "url" ? !deepflipper::ValidVoiceEndpoint(value->valuestring)
                                       : value->valuestring[0] && !Token(value->valuestring))) {
            error = std::string("invalid_") + key;
            return Json(nullptr, cJSON_Delete);
        }
        if (std::string(key) != "token" || value->valuestring[0])
            Put(output.get(), key, value->valuestring);
    }
    auto clear = cJSON_GetObjectItemCaseSensitive(input, "clear_token");
    if (clear && (!cJSON_IsBool(clear) || (cJSON_IsTrue(clear) && !Text(input, "token").empty()))) {
        error = "invalid_clear_token";
        return Json(nullptr, cJSON_Delete);
    }
    if (cJSON_IsTrue(clear))
        Put(output.get(), "token", "");
    for (auto key : {"volume", "led_brightness"}) {
        auto value = cJSON_GetObjectItemCaseSensitive(input, key);
        if (!value)
            continue;
        if (!cJSON_IsNumber(value) || !std::isfinite(value->valuedouble) ||
            value->valuedouble != value->valueint || value->valueint < 0 || value->valueint > 100) {
            error = std::string("invalid_") + key;
            return Json(nullptr, cJSON_Delete);
        }
        cJSON_DeleteItemFromObjectCaseSensitive(output.get(), key);
        cJSON_AddNumberToObject(output.get(), key, value->valueint);
    }
    for (auto key : {"mute", "microphone"}) {
        auto value = cJSON_GetObjectItemCaseSensitive(input, key);
        if (!value)
            continue;
        if (!cJSON_IsBool(value)) {
            error = std::string("invalid_") + key;
            return Json(nullptr, cJSON_Delete);
        }
        cJSON_DeleteItemFromObjectCaseSensitive(output.get(), key);
        cJSON_AddBoolToObject(output.get(), key, cJSON_IsTrue(value));
    }
    return output;
}
inline bool Connection(const cJSON* input, const std::string& device, const std::string& client,
                       std::string& error) {
    if (!cJSON_IsObject(input) || Text(input, "format") != "deepflipper.connection" ||
        !cJSON_IsNumber(cJSON_GetObjectItem(input, "version")) ||
        cJSON_GetObjectItem(input, "version")->valuedouble != 1 ||
        Text(input, "device_id") != device || Text(input, "client_id") != client) {
        error = "wrong_device_or_format";
        return false;
    }
    if (!deepflipper::ValidVoiceEndpoint(Text(input, "server_url")) ||
        !Token(Text(input, "token"))) {
        error = "invalid_connection_fields";
        return false;
    }
    return true;
}
inline bool Equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size())
        return false;
    unsigned difference = 0;
    for (size_t i = 0; i < a.size(); ++i)
        difference |= uint8_t(a[i]) ^ uint8_t(b[i]);
    return difference == 0;
}
class WebSession {
    std::string token_, csrf_;
    int64_t expires_ = 0;

public:
    void Open(std::string token, std::string csrf, int64_t now) {
        token_ = std::move(token);
        csrf_ = std::move(csrf);
        expires_ = now + 1800000;
    }
    bool Valid(const std::string& token, int64_t now) const {
        return !token_.empty() && now < expires_ && Equal(token_, token);
    }
    bool Write(const std::string& token, const std::string& csrf, int64_t now) const {
        return Valid(token, now) && Equal(csrf_, csrf);
    }
    std::string Csrf() const { return csrf_; }
    void Close() {
        token_.clear();
        csrf_.clear();
        expires_ = 0;
    }
};
}  // namespace dfsetup
