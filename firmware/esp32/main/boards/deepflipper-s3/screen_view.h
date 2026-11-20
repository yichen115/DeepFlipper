#pragma once
// Turns the Flipper's captured screen text (firmware screen op) into what the
// assistant reads: a compact `view`, the highlighted item and soft-button labels.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace dfscreen {
struct Item {
    int x = 0, y = 0, color = 1;  // color 0 = white on black (highlighted / button)
    std::string text;
};
struct Buttons {
    std::string left, ok, right;
};

inline std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}
inline std::string Trim(const std::string& text) {
    size_t begin = text.find_first_not_of(" \t");
    if (begin == std::string::npos)
        return "";
    size_t end = text.find_last_not_of(" \t");
    return text.substr(begin, end - begin + 1);
}

// elements_button_left/center/right draw inverted labels on a 12 px bar whose
// text baseline is canvas height - 3 (61). Left text starts at x=10.
inline bool IsButton(const Item& item) { return item.color == 0 && item.y >= 59; }
inline int ButtonSlot(const Item& item) {
    const int mid = item.x + static_cast<int>(item.text.size()) * 5 / 2;
    return mid < 40 ? 0 : mid < 92 ? 1 : 2;
}
// Status bar clock / battery percent on the top row.
inline bool IsStatus(const Item& item) {
    if (item.y > 9 || item.text.empty())
        return false;
    for (unsigned char c : item.text)
        if (!std::isdigit(c) && c != ':' && c != '%' && c != ' ' && c != 'A' && c != 'P' &&
            c != 'M')
            return false;
    return true;
}

inline Buttons FindButtons(const std::vector<Item>& items) {
    Buttons buttons;
    for (const auto& item : items) {
        if (!IsButton(item))
            continue;
        std::string& slot = ButtonSlot(item) == 0   ? buttons.left
                            : ButtonSlot(item) == 1 ? buttons.ok
                                                    : buttons.right;
        if (slot.empty())
            slot = Trim(item.text);
    }
    return buttons;
}

// The highlighted list entry: inverted text that is not a soft button. Inverted
// title bars sit on the top row, so prefer entries below it.
inline const Item* Selected(const std::vector<Item>& items) {
    const Item* fallback = nullptr;
    for (const auto& item : items) {
        if (item.color != 0 || IsButton(item) || Trim(item.text).empty())
            continue;
        if (item.y > 12)
            return &item;
        if (!fallback)
            fallback = &item;
    }
    return fallback;
}

inline std::string View(std::vector<Item> items) {
    items.erase(std::remove_if(items.begin(), items.end(),
                               [](const Item& item) { return IsStatus(item); }),
                items.end());
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return a.y != b.y ? a.y < b.y : a.x < b.x;
    });
    const Item* selected = Selected(items);
    std::string view, line;
    int line_y = -100;
    auto flush = [&]() {
        if (!line.empty())
            view += (view.empty() ? "" : "\n") + line;
        line.clear();
    };
    std::vector<const Item*> row;
    auto emit_row = [&]() {
        std::stable_sort(row.begin(), row.end(),
                         [](const Item* a, const Item* b) { return a->x < b->x; });
        for (const Item* item : row) {
            auto text = Trim(item->text);
            if (text.empty())
                continue;
            if (!line.empty())
                line += "  ";
            line += (item == selected ? "\xE2\x96\xB6" : "") + text;  // ▶
        }
        row.clear();
        flush();
    };
    for (const auto& item : items) {
        if (IsButton(item))
            continue;
        if (item.y - line_y > 2)
            emit_row();
        line_y = item.y;
        row.push_back(&item);
    }
    emit_row();
    const auto buttons = FindButtons(items);
    std::string bar;
    if (!buttons.left.empty())
        bar += "[\xE2\x97\x80 " + buttons.left + "]";  // ◀
    if (!buttons.ok.empty())
        bar += (bar.empty() ? "" : " ") + std::string("[OK ") + buttons.ok + "]";
    if (!buttons.right.empty())
        bar += (bar.empty() ? "" : " ") + std::string("[") + buttons.right + " \xE2\x96\xB6]";
    if (!bar.empty())
        view += (view.empty() ? "" : "\n") + bar;
    return view;
}

// Case-insensitive; exact (trimmed) matches win over substrings.
inline const Item* Find(const std::vector<Item>& items, const std::string& target) {
    const auto wanted = Lower(Trim(target));
    if (wanted.empty())
        return nullptr;
    const Item* partial = nullptr;
    for (const auto& item : items) {
        if (IsButton(item) || IsStatus(item))
            continue;
        const auto text = Lower(Trim(item.text));
        if (text == wanted)
            return &item;
        if (!partial && text.find(wanted) != std::string::npos)
            partial = &item;
    }
    return partial;
}
inline bool Matches(const std::string& text, const std::string& target) {
    const auto have = Lower(Trim(text)), wanted = Lower(Trim(target));
    return !wanted.empty() && (have == wanted || have.find(wanted) != std::string::npos);
}

inline std::string Base64(const std::string& data) {
    static const char* alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t n = uint8_t(data[i]) << 16;
        if (i + 1 < data.size())
            n |= uint8_t(data[i + 1]) << 8;
        if (i + 2 < data.size())
            n |= uint8_t(data[i + 2]);
        out += alphabet[(n >> 18) & 63];
        out += alphabet[(n >> 12) & 63];
        out += i + 1 < data.size() ? alphabet[(n >> 6) & 63] : '=';
        out += i + 2 < data.size() ? alphabet[n & 63] : '=';
    }
    return out;
}
inline bool Unbase64(const std::string& text, std::string& out) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        return c == '+' ? 62 : c == '/' ? 63 : -1;
    };
    out.clear();
    if (text.size() % 4)
        return false;
    for (size_t i = 0; i < text.size(); i += 4) {
        int v[4];
        for (int j = 0; j < 4; ++j) {
            v[j] = text[i + j] == '=' ? 0 : value(text[i + j]);
            if (v[j] < 0 || (text[i + j] == '=' && i + 4 != text.size()))
                return false;
        }
        uint32_t n = (v[0] << 18) | (v[1] << 12) | (v[2] << 6) | v[3];
        out += char((n >> 16) & 255);
        if (text[i + 2] != '=')
            out += char((n >> 8) & 255);
        if (text[i + 3] != '=')
            out += char(n & 255);
    }
    return true;
}
}  // namespace dfscreen
