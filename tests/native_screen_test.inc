// Screen readback / UI control tests, included by native_actions_test.cpp.
#include <cstring>

struct ScreenFake : Fake {
    std::vector<std::string> menu{"Read", "Read RAW", "Saved", "Send Signal", "Add Manually",
                                  "Frequency Analyzer", "Region Info"};
    int sel = 0, top = 0, depth = 1;  // depth 0 = desktop
    bool wrap = true, buttons = false, old_firmware = false, no_highlight = false;
    std::string entered;
    std::vector<int> pressed;
    int screens = 0, pages = 0;
    uint8_t frame[1024];
    ScreenFake() {
        for (int i = 0; i < 1024; ++i)
            frame[i] = uint8_t(i * 7);
        busy = true;
    }
    std::vector<dfscreen::Item> Items() const {
        std::vector<dfscreen::Item> items;
        items.push_back({100, 8, 1, "12:34"});  // status bar clock
        if (depth == 0)
            return items;
        if (!entered.empty()) {
            items.push_back({2, 10, 0, entered});  // inverted title bar
            items.push_back({4, 30, 1, "Frequency: 433.92"});
            items.push_back({10, 61, 0, "Back"});
            items.push_back({128 - 4 * 5 - 10, 61, 0, "Send"});
            return items;
        }
        for (int i = top; i < top + 4 && i < int(menu.size()); ++i)
            items.push_back({4, 11 + 12 * (i - top), (i == sel && !no_highlight) ? 0 : 1, menu[i]});
        if (buttons)
            items.push_back({50, 61, 0, "Config"});
        return items;
    }
    static std::string Page(const std::vector<dfscreen::Item>& items, size_t from, int& next) {
        std::string json;
        size_t i = from;
        for (; i < items.size() && i < from + 3; ++i) {  // three items per page
            if (i > from)
                json += ",";
            json += "[" + std::to_string(items[i].x) + "," + std::to_string(items[i].y) + "," +
                    std::to_string(items[i].color) + ",\"" + items[i].text + "\"]";
        }
        next = i < items.size() ? int(i) : -1;
        return json;
    }
    std::string Companion(const std::string& request, int timeout_ms) override {
        assert(timeout_ms >= 1000);
        if (old_firmware)
            return "{\"ok\":false,\"code\":\"unsupported\"}";
        Result r(request);
        auto op = r.text("op");
        auto items = Items();
        int next = -1;
        if (op == "screen") {
            ++screens;
            bool bitmap = r.flag("bitmap");
            auto page = Page(items, 0, next);
            return std::string("{\"ok\":true,\"seq\":7,\"app\":\"") + (depth ? "Sub-GHz" : "") +
                   "\",\"locked\":" + (locked ? "true" : "false") +
                   ",\"text\":true,\"count\":" + std::to_string(items.size()) +
                   ",\"bitmap\":" + (bitmap ? "1024" : "0") + ",\"items\":[" + page +
                   "],\"next\":" + std::to_string(next) + "}";
        }
        assert(op == "screen.page");
        ++pages;
        auto seq = cJSON_GetObjectItem(r.json, "seq");
        assert(seq && seq->valueint == 7);
        if (auto text = cJSON_GetObjectItem(r.json, "text")) {
            auto page = Page(items, size_t(text->valueint), next);
            return "{\"ok\":true,\"seq\":7,\"items\":[" + page + "],\"next\":" +
                   std::to_string(next) + "}";
        }
        int off = cJSON_GetObjectItem(r.json, "bitmap")->valueint;
        int len = std::min(384, 1024 - off);
        return "{\"ok\":true,\"seq\":7,\"off\":" + std::to_string(off) + ",\"len\":" +
               std::to_string(len) + ",\"data\":\"" +
               dfscreen::Base64(std::string(reinterpret_cast<char*>(frame) + off, len)) + "\"}";
    }
    dfnative::Reply Native(uint32_t tag, const std::vector<uint8_t>& payload = {},
                           dfnative::Sink sink = {}, bool cleanup = false,
                           int short_key = -1) override {
        if (tag == 23) {
            pressed.push_back(short_key);
            int key = short_key % 6;
            int n = int(menu.size());
            if (depth > 0 && entered.empty() && key == dfnative::kDown)
                sel = wrap ? (sel + 1) % n : std::min(sel + 1, n - 1);
            if (depth > 0 && entered.empty() && key == dfnative::kUp)
                sel = wrap ? (sel + n - 1) % n : std::max(sel - 1, 0);
            if (sel < top)
                top = sel;
            if (sel >= top + 4)
                top = sel - 3;
            if (depth > 0 && entered.empty() && key == dfnative::kOk)
                entered = menu[sel];
            else if (key == dfnative::kBack) {
                if (!entered.empty())
                    entered.clear();
                else if (depth > 0 && --depth == 0)
                    busy = false;
            }
        }
        return Fake::Native(tag, payload, sink, cleanup, short_key);
    }
};

static void screen_tests() {
    using dfscreen::Item;
    // Pure view helpers.
    std::vector<Item> items{{100, 8, 1, "12:34"}, {2, 10, 0, "Sub-GHz"}, {4, 23, 0, "Read"},
                            {60, 23, 1, "433.92"}, {4, 35, 1, "Saved"}, {10, 61, 0, "Back"},
                            {50, 61, 0, "OK"}, {100, 61, 0, "Send"}};
    auto view = dfscreen::View(items);
    assert(view == "Sub-GHz\n\xE2\x96\xB6Read  433.92\nSaved\n[\xE2\x97\x80 Back] [OK OK] [Send \xE2\x96\xB6]");
    auto buttons = dfscreen::FindButtons(items);
    assert(buttons.left == "Back" && buttons.ok == "OK" && buttons.right == "Send");
    assert(dfscreen::Selected(items)->text == "Read");
    assert(dfscreen::Find(items, "read")->text == "Read");
    std::string raw("\x00\x01\xff\x10\x20", 5), back;
    assert(dfscreen::Unbase64(dfscreen::Base64(raw), back) && back == raw);
    assert(dfnative::Dangerous("Send") && dfnative::Dangerous("Send Signal") &&
           dfnative::Dangerous("BadUSB: Run") && dfnative::Dangerous("TX"));
    assert(!dfnative::Dangerous("Read RAW") && !dfnative::Dangerous("Sender info") &&
           !dfnative::Dangerous("Saved"));
    assert(dfnative::Keys("down, down,ok,back:long").size() == 4);
    assert(dfnative::Keys("down,jump").empty() && dfnative::Keys("ok:short").empty());
    assert(dfnative::Keys(std::string(17 * 3, ' ') + "up,up,up,up,up,up,up,up,up,up,up,up,up,up,up,up,up").empty());

    {  // screen with paging and bitmap
        ScreenFake f;
        dfnative::Actions a(f, 30000);
        Result r(a.Execute("native.screen", "{\"bitmap\":true}"));
        assert(r.ok() && r.text("app") == "Sub-GHz" && r.text("selected") == "Read");
        assert(r.text("view") == "\xE2\x96\xB6Read\nRead RAW\nSaved\nSend Signal");
        std::string frame;
        assert(dfscreen::Unbase64(r.text("bitmap"), frame) && frame.size() == 1024 &&
               std::memcmp(frame.data(), f.frame, 1024) == 0);
        assert(f.pages == 1 + 3);  // one text page after the first + three bitmap pages
        Result plain(a.Execute("native.screen", "{}"));
        assert(plain.ok() && !cJSON_GetObjectItem(plain.json, "bitmap"));
        f.locked = true;  // the lock screen stays readable
        Result locked(a.Execute("native.screen", "{}"));
        assert(locked.ok() && locked.flag("locked"));
        f.old_firmware = true;
        f.locked = false;
        Result old(a.Execute("native.screen", "{}"));
        assert(!old.ok() && old.text("code") == "firmware_update_required");
    }
    {  // keys: validation, sequence and readback
        ScreenFake f;
        dfnative::Actions a(f, 30000);
        Result bad(a.Execute("native.keys", "{\"keys\":\"down,fly\"}"));
        assert(!bad.ok() && bad.text("code") == "invalid_keys" && f.pressed.empty());
        Result r(a.Execute("native.keys", "{\"keys\":\"down,down,ok\"}"));
        assert(r.ok() && f.entered == "Saved" && r.text("selected") == "Saved");
        assert(cJSON_GetObjectItem(r.json, "pressed")->valueint == 3);
        assert(r.text("view").find("[Send \xE2\x96\xB6]") != std::string::npos);
        // Right would trigger the Send soft button: stop before pressing.
        size_t before = f.pressed.size();
        Result gate(a.Execute("native.keys", "{\"keys\":\"up,right\"}"));
        assert(!gate.ok() && gate.text("code") == "needs_confirmation" &&
               gate.text("action") == "Send" && f.pressed.size() == before + 1);
        assert(cJSON_GetObjectItem(gate.json, "pressed")->valueint == 1 && !gate.text("view").empty());
        Result go(a.Execute("native.keys", "{\"keys\":\"right\",\"confirm\":true,\"bitmap\":true}"));
        assert(go.ok() && f.pressed.back() == dfnative::kRight && !go.text("bitmap").empty());
        Result hold(a.Execute("native.keys", "{\"keys\":\"back:long\"}"));
        assert(hold.ok() && f.pressed.back() == dfnative::kBack + 6);
        f.locked = true;
        Result locked(a.Execute("native.keys", "{\"keys\":\"down\"}"));
        assert(!locked.ok() && locked.text("code") == "locked");
    }
    {  // ui_select moves the highlight, scrolls and gates OK on a dangerous item
        ScreenFake f;
        dfnative::Actions a(f, 30000);
        Result r(a.Execute("native.ui_select", "{\"text\":\"saved\"}"));
        assert(r.ok() && f.entered == "Saved" && r.text("chosen") == "Saved");
        f.entered.clear();
        Result scroll(a.Execute("native.ui_select", "{\"text\":\"Region Info\"}"));
        assert(scroll.ok() && f.entered == "Region Info");
        f.entered.clear();
        Result exact(a.Execute("native.ui_select", "{\"text\":\"Read\"}"));
        assert(exact.ok() && f.entered == "Read");  // not "Read RAW"
        f.entered.clear();
        size_t before = f.pressed.size();
        Result gate(a.Execute("native.ui_select", "{\"text\":\"Send Signal\"}"));
        assert(!gate.ok() && gate.text("code") == "needs_confirmation" && f.entered.empty() &&
               gate.text("selected") == "Send Signal");
        for (size_t i = before; i < f.pressed.size(); ++i)
            assert(f.pressed[i] != dfnative::kOk);
        Result go(a.Execute("native.ui_select", "{\"text\":\"Send Signal\",\"confirm\":true}"));
        assert(go.ok() && f.entered == "Send Signal");
        f.entered.clear();
        Result missing(a.Execute("native.ui_select", "{\"text\":\"Bluetooth\"}"));
        assert(!missing.ok() && missing.text("code") == "not_found" && !missing.text("view").empty());
    }
    {  // non-wrapping list: scan reaches the end, then searches upwards
        ScreenFake f;
        f.wrap = false;
        f.sel = 6;
        f.top = 3;
        dfnative::Actions a(f, 30000);
        Result r(a.Execute("native.ui_select", "{\"text\":\"Read RAW\"}"));
        assert(r.ok() && f.entered == "Read RAW");
    }
    {  // no highlight drawn: report instead of pressing blindly
        ScreenFake f;
        f.no_highlight = true;
        dfnative::Actions a(f, 30000);
        Result r(a.Execute("native.ui_select", "{\"text\":\"Saved\"}"));
        assert(!r.ok() && r.text("code") == "no_highlight" && f.pressed.empty());
    }
    {  // home: Back until the loader is free
        ScreenFake f;
        f.entered = "Saved";
        dfnative::Actions a(f, 30000);
        Result r(a.Execute("native.home", "{}"));
        assert(r.ok() && r.flag("home") && r.text("app").empty() && f.depth == 0);
        assert(cJSON_GetObjectItem(r.json, "pressed")->valueint == 2);
    }
    {  // app.open includes the opened screen
        ScreenFake f;
        f.busy = false;
        f.depth = 1;
        dfnative::Actions a(f, 30000);
        Result r(a.Execute("native.app.open", "{\"name\":\"Sub-GHz\"}"));
        assert(r.ok() && r.text("app") == "Sub-GHz" && r.text("selected") == "Read");
    }
    std::cout << "PASS: screen readback paging/bitmap, view/buttons, key gate, ui_select, home\n";
}
