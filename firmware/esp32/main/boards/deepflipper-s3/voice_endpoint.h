#pragma once
#include <string>

namespace deepflipper {
// Credentials travel in headers. Reject ambiguous authorities and URL secrets.
inline bool ValidVoiceEndpoint(const std::string& url) {
    const size_t start = url.rfind("ws://", 0) == 0 ? 5 : url.rfind("wss://", 0) == 0 ? 6 : 0;
    if (!start || url.size() > 192 || url.size() <= start)
        return false;
    for (unsigned char c : url) {
        if (c <= 32 || c >= 127 || c == '@' || c == '?' || c == '#' || c == '\\')
            return false;
    }
    const auto authority = url.substr(start, url.find('/', start) - start);
    std::string port;
    if (authority.empty())
        return false;
    // The current transport URI parser supports IPv4/DNS, not bracketed IPv6.
    const auto separator = authority.find(':');
    const auto host = authority.substr(0, separator);
    if (host.empty())
        return false;
    auto normalized = host;
    for (char& c : normalized)
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
    while (!normalized.empty() && normalized.back() == '.')
        normalized.pop_back();
    bool numeric = true;
    for (char c : normalized)
        if ((c < '0' || c > '9') && c != '.')
            numeric = false;
    if (normalized == "localhost" || normalized == "0.0.0.0" ||
        (normalized.size() > 10 &&
         normalized.compare(normalized.size() - 10, 10, ".localhost") == 0) ||
        (numeric && normalized.rfind("127.", 0) == 0))
        return false;
    for (char c : host) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '.'))
            return false;
    }
    if (separator != std::string::npos) {
        port = authority.substr(separator + 1);
        if (port.empty())
            return false;
    }
    if (!port.empty()) {
        if (port.size() > 5)
            return false;
        unsigned number = 0;
        for (char c : port) {
            if (c < '0' || c > '9')
                return false;
            number = number * 10 + c - '0';
        }
        if (!number || number > 65535)
            return false;
    }
    return true;
}
}  // namespace deepflipper
