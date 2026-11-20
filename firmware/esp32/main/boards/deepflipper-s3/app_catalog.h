#pragma once
#include <cJSON.h>
#include <string>
#include "native_policy.h"

namespace dfcatalog {
inline constexpr const char* kDirectory = "/ext/deepflipper/apps";
inline std::string String(cJSON* value, const char* key) {
    auto field = cJSON_GetObjectItemCaseSensitive(value, key);
    return cJSON_IsString(field) ? field->valuestring : "";
}
inline bool Text(const std::string& text, size_t maximum) {
    if (text.empty() || text.size() > maximum)
        return false;
    for (unsigned char c : text)
        if (c < 32 || c == 127)
            return false;
    return true;
}
inline bool Id(const std::string& text) {
    return Text(text, 48) &&
           text.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789._-") == std::string::npos &&
           text[0] != '.' && text[0] != '_' && text[0] != '-';
}
inline bool FileName(const std::string& name) {
    return dfnative::Suffix(name, ".json") && name.size() > 5 &&
           Id(name.substr(0, name.size() - 5));
}
inline bool Hex(const std::string& text) {
    return text.size() == 32 && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}
inline bool AppPath(const std::string& path) {
    return dfnative::Path(path, "/ext/apps") && dfnative::Suffix(path, ".fap") &&
           path.find("//") == std::string::npos && path.find(':') == std::string::npos;
}
inline bool Validate(cJSON* value, const std::string& filename) {
    if (!cJSON_IsObject(value) || cJSON_GetArraySize(value) != 9)
        return false;
    const char* keys[] = {"format", "v",   "app_id",   "name",   "summary",
                          "app",    "sdk", "fap_size", "fap_md5"};
    for (auto key : keys) {
        int found = 0;
        for (auto field = value->child; field; field = field->next)
            if (field->string && std::string(field->string) == key)
                ++found;
        if (found != 1)
            return false;
    }
    auto version = cJSON_GetObjectItemCaseSensitive(value, "v");
    auto sdk = cJSON_GetObjectItemCaseSensitive(value, "sdk");
    auto size = cJSON_GetObjectItemCaseSensitive(value, "fap_size");
    auto id = String(value, "app_id");
    return String(value, "format") == "deepflipper.app" && cJSON_IsNumber(version) &&
           version->valuedouble == 1 && cJSON_IsNumber(sdk) && sdk->valuedouble == 2 && Id(id) &&
           filename == id + ".json" && Text(String(value, "name"), 64) &&
           Text(String(value, "summary"), 160) && AppPath(String(value, "app")) &&
           cJSON_IsNumber(size) && size->valuedouble >= 4 && size->valuedouble <= 1048576 &&
           size->valuedouble == size->valueint && Hex(String(value, "fap_md5"));
}
}  // namespace dfcatalog
