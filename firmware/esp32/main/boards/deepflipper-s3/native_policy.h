#pragma once
#include <string>

namespace dfnative {
inline bool Path(const std::string& value, const std::string& root = "/ext") {
    if (value.empty() || value.size() > 192 || (value != root && value.rfind(root + "/", 0) != 0))
        return false;
    for (unsigned char c : value)
        if (c < 32 || c == 127 || c == '\\')
            return false;
    size_t start = 0;
    while (start < value.size()) {
        size_t end = value.find('/', start);
        const auto part = value.substr(start, end == std::string::npos ? end : end - start);
        if (part == "." || part == "..")
            return false;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return true;
}
inline bool Suffix(const std::string& value, const std::string& suffix) {
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}
struct SavedKind {
    const char* kind;
    const char* app;
    const char* folder;
    const char* extension;
    bool transmit;
};
inline constexpr SavedKind kSaved[] = {
    {"infrared", "Infrared", "/ext/infrared", ".ir", true},
    {"subghz", "Sub-GHz", "/ext/subghz", ".sub", true},
    {"nfc", "NFC", "/ext/nfc", ".nfc", false},
    {"rfid", "125 kHz RFID", "/ext/lfrfid", ".rfid", false},
    {"ibutton", "iButton", "/ext/ibutton", ".ibtn", false},
};
inline const SavedKind* Saved(const std::string& kind, const std::string& path) {
    for (const auto& item : kSaved)
        if (kind == item.kind && Path(path, item.folder) && Suffix(path, item.extension))
            return &item;
    return nullptr;
}
inline constexpr const char* kMainApps[] = {"Sub-GHz",  "Sub-GHz Remote", "125 kHz RFID", "NFC",
                                            "Infrared", "GPIO",           "iButton",      "Bad USB",
                                            "U2F",      "Clock",          "Archive",      "Power",
                                            "Storage",  "System",         "AI Assistant"};
inline bool App(const std::string& name) {
    for (auto app : kMainApps)
        if (name == app)
            return true;
    return Path(name, "/ext/apps") && Suffix(name, ".fap");
}
inline int Pin(const std::string& name) {
    const char* pins[] = {"PC0", "PC1", "PC3", "PB2", "PB3", "PA4", "PA6", "PA7"};
    for (int i = 0; i < 8; ++i)
        if (name == pins[i])
            return i;
    return -1;  // UART PB6/PB7 and the shared 5V rail are never exposed here.
}
inline int Key(const std::string& name) {
    const char* keys[] = {"up", "down", "right", "left", "ok", "back"};
    for (int i = 0; i < 6; ++i)
        if (name == keys[i])
            return i;
    return -1;
}
}  // namespace dfnative
