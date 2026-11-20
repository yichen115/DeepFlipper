#pragma once
#include <cJSON.h>
#include <algorithm>
#include <functional>
#include <memory>
#include "../../../../flipper/lib/deepflipper_app/json_limits.h"
#include "app_catalog.h"
#include "native_policy.h"
#include "rpc_wire.h"
#include "screen_view.h"

namespace dfnative {
struct Reply {
    bool done = false;
    uint32_t status = 0;
    size_t frames = 0;
    std::string error;
    dfwire::Message message;
};
using Sink = std::function<bool(const dfwire::Message&)>;
class Transport {
public:
    virtual ~Transport() = default;
    virtual Reply Native(uint32_t tag, const std::vector<uint8_t>& payload = {}, Sink sink = {},
                         bool cleanup = false, int short_key = -1) = 0;
    virtual bool NativeCancelled() = 0;
    virtual bool Alive() const = 0;
    virtual int64_t NowMs() const = 0;
    virtual void Pump() = 0;
    virtual void Reset() = 0;
    virtual bool AppRunning() const = 0;
    virtual std::string AppName() const = 0;
    virtual void SetAppName(const std::string& name) = 0;
    virtual std::string RetainedApp() const = 0;
    virtual int64_t RetainedDeadline() const = 0;
    virtual void RetainApp(const std::string& name, int64_t deadline) = 0;
    // DeepFlipper companion JSON request (session added by the transport).
    virtual std::string Companion(const std::string& request, int timeout_ms) {
        (void)request;
        (void)timeout_ms;
        return "{\"ok\":false,\"code\":\"unsupported\"}";
    }
};
class Actions {
    Transport& transport_;
    int64_t deadline_ms_;
    static std::string Serialize(cJSON* object) {
        char* value = cJSON_PrintUnformatted(object);
        std::string text = value ? value : "{}";
        cJSON_free(value);
        return text;
    }
    static std::string String(cJSON* object, const char* key) {
        auto value = cJSON_GetObjectItem(object, key);
        return cJSON_IsString(value) ? value->valuestring : "";
    }
    static int Integer(cJSON* object, const char* key, int fallback = 0) {
        auto value = cJSON_GetObjectItem(object, key);
        return cJSON_IsNumber(value) ? value->valueint : fallback;
    }

public:
    Actions(Transport& transport, int64_t deadline_ms)
        : transport_(transport), deadline_ms_(deadline_ms) {}
    std::string ExpireRetained() {
        if (transport_.RetainedApp().empty() || transport_.NowMs() < transport_.RetainedDeadline())
            return {};
        auto result = Execute("native.app.exit", "{}");
        // An unconfirmed lease cleanup must trigger the existing session-close fallback.
        if (!transport_.RetainedApp().empty())
            transport_.Reset();
        return result;
    }
    std::string Execute(const std::string& op, const std::string& args_json) {
        using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
        Json args(cJSON_Parse(args_json.c_str()), cJSON_Delete),
            output(cJSON_CreateObject(), cJSON_Delete);
        auto out = output.get();
        auto finish = [&]() { return Serialize(out); };
        auto error = [&](const std::string& code, const char* stage = "request") {
            cJSON_DeleteItemFromObject(out, "ok");
            cJSON_DeleteItemFromObject(out, "code");
            cJSON_DeleteItemFromObject(out, "stage");
            cJSON_DeleteItemFromObject(out, "state");
            cJSON_AddBoolToObject(out, "ok", false);
            cJSON_AddStringToObject(out, "code", code.c_str());
            cJSON_AddStringToObject(out, "stage", stage);
            cJSON_AddStringToObject(out, "state", code == "cancelled" ? "cancelled" : "error");
            return finish();
        };
        auto checked = [&](const Reply& reply, const char* stage) {
            if (!reply.error.empty() || reply.status) {
                const char* code = reply.status == 17 || reply.status == 4 ? "busy"
                                   : reply.status == 7                     ? "file_not_found"
                                   : reply.status == 5                     ? "storage_not_ready"
                                   : reply.status == 21                    ? "app_not_running"
                                   : reply.status == 22                    ? "app_command_error"
                                                                           : "rpc_error";
                if (reply.status == 22 && transport_.Alive()) {
                    auto detail = transport_.Native(63, {}, {}, true);
                    if (detail.done && !detail.status && detail.error.empty() &&
                        detail.message.tag == 64) {
                        dfwire::Reader reader(detail.message.payload);
                        dfwire::Field field;
                        while (reader.Next(field)) {
                            if (field.tag == 1 && field.wire == 0)
                                cJSON_AddNumberToObject(out, "app_error", field.number);
                            if (field.tag == 2 && field.wire == 2 && field.bytes.size() <= 256)
                                cJSON_AddStringToObject(out, "message", field.text().c_str());
                        }
                    }
                }
                cJSON_AddNumberToObject(out, "rpc_status", reply.status);
                error(reply.error.empty() ? code : reply.error, stage);
                return false;
            }
            if (!reply.done) {
                error("incomplete_response", stage);
                return false;
            }
            return true;
        };
        auto number = [](const dfwire::Message& message, uint32_t tag, uint64_t& value) {
            dfwire::Reader reader(message.payload);
            dfwire::Field field;
            value = 0;
            while (reader.Next(field))
                if (field.tag == tag) {
                    if (field.wire != 0)
                        return false;
                    value = field.number;
                }
            return reader.Done();
        };
        auto desktop_lock = [&](const Reply& reply, uint64_t& locked, const char* stage) {
            // IsLockedRequest (66) is a legacy boolean RPC: an Empty response
            // with OK means locked, ERROR means unlocked. Desktop.Status (70)
            // is an unsolicited subscription event, not this request's reply.
            if (reply.done && reply.error.empty() && reply.message.tag == 4 && reply.status <= 1) {
                locked = reply.status == 0;
                return true;
            }
            if (!checked(reply, stage))
                return false;
            error("invalid_response", stage);
            return false;
        };
        auto string_field = [](uint32_t tag, const std::string& value) {
            std::vector<uint8_t> payload;
            dfwire::String(payload, tag, value);
            return payload;
        };
        auto app_exit = [&]() {
            const int64_t stop_deadline = transport_.NowMs() + 2500;
            auto reply = transport_.Native(47, {}, {}, true);
            if (!reply.error.empty() || reply.status || !reply.done)
                return false;
            for (int i = 0; i < 20 && transport_.Alive() && transport_.NowMs() < stop_deadline;
                 ++i) {
                auto lock = transport_.Native(17, {}, {}, true);
                uint64_t busy = 1;
                if (!lock.error.empty() || lock.status || lock.message.tag != 18 ||
                    !number(lock.message, 1, busy))
                    return false;
                if (!busy) {
                    transport_.RetainApp("", 0);
                    return true;
                }
                transport_.Pump();
            }
            return false;
        };
        auto app_ready = [&]() {
            const int64_t ready_deadline = transport_.NowMs() + 3000;
            while (transport_.Alive() && !transport_.AppRunning() &&
                   transport_.NowMs() < ready_deadline && transport_.NowMs() < deadline_ms_ &&
                   !transport_.NativeCancelled())
                transport_.Pump();
            if (transport_.Alive() && transport_.AppRunning() && !transport_.NativeCancelled() &&
                transport_.NowMs() < deadline_ms_)
                return true;
            error(transport_.NativeCancelled() ? "cancelled" : "app_start_unconfirmed",
                  "application_ready");
            // A late startup must not leave a headless RPC app behind.
            transport_.Reset();
            return false;
        };
        auto read_file = [&](const std::string& path, int offset, int size, std::string& data) {
            auto payload = string_field(1, path);
            dfwire::UInt(payload, 2, offset);
            dfwire::UInt(payload, 3, size);
            return transport_.Native(9, payload, [&](const dfwire::Message& message) {
                if (message.status)
                    return true;
                if (message.tag != 10)
                    return false;
                dfwire::Reader reader(message.payload);
                dfwire::Field field;
                while (reader.Next(field))
                    if (field.tag == 1 && field.wire == 2) {
                        dfwire::Reader file(field.bytes);
                        dfwire::Field part;
                        while (file.Next(part))
                            if (part.tag == 4 && part.wire == 2) {
                                if (data.size() + part.bytes.size() > size_t(size))
                                    return false;
                                data += part.text();
                            }
                        if (!file.Done())
                            return false;
                    }
                return reader.Done();
            });
        };
        auto flag = [&](const char* key) {
            return cJSON_IsTrue(cJSON_GetObjectItem(args.get(), key));
        };
        auto wait = [&](int ms) {
            const int64_t end = transport_.NowMs() + ms;
            while (transport_.Alive() && transport_.NowMs() < end && !transport_.NativeCancelled())
                transport_.Pump();
        };
        struct Shot {
            std::string app;
            bool locked = false, text = true;
            std::vector<dfscreen::Item> items;
            std::string bitmap;  // raw 1024-byte framebuffer when requested
        };
        auto companion = [&](const std::string& request) {
            return Json(cJSON_Parse(transport_.Companion(request, 1500).c_str()), cJSON_Delete);
        };
        auto parse_items = [](cJSON* reply, Shot& shot) {
            auto items = cJSON_GetObjectItem(reply, "items");
            if (!cJSON_IsArray(items))
                return false;
            cJSON* entry = nullptr;
            cJSON_ArrayForEach(entry, items) {
                if (!cJSON_IsArray(entry) || cJSON_GetArraySize(entry) != 4 || shot.items.size() >= 64)
                    return false;
                auto x = cJSON_GetArrayItem(entry, 0), y = cJSON_GetArrayItem(entry, 1),
                     color = cJSON_GetArrayItem(entry, 2), text = cJSON_GetArrayItem(entry, 3);
                if (!cJSON_IsNumber(x) || !cJSON_IsNumber(y) || !cJSON_IsNumber(color) ||
                    !cJSON_IsString(text))
                    return false;
                shot.items.push_back({x->valueint, y->valueint, color->valueint, text->valuestring});
            }
            return true;
        };
        // Returns an error code, empty on success.
        auto read_screen = [&](bool bitmap, Shot& shot) -> std::string {
            shot = Shot{};
            auto reply = companion(std::string("{\"op\":\"screen\",\"bitmap\":") +
                                   (bitmap ? "true" : "false") + "}");
            if (!cJSON_IsObject(reply.get()))
                return "invalid_response";
            if (!cJSON_IsTrue(cJSON_GetObjectItem(reply.get(), "ok"))) {
                auto code = String(reply.get(), "code");
                return code == "unsupported" ? "firmware_update_required"
                       : code.empty()        ? "screen_unavailable"
                                             : code;
            }
            shot.app = String(reply.get(), "app");
            shot.locked = cJSON_IsTrue(cJSON_GetObjectItem(reply.get(), "locked"));
            shot.text = !cJSON_IsFalse(cJSON_GetObjectItem(reply.get(), "text"));
            const int seq = Integer(reply.get(), "seq");
            int next = Integer(reply.get(), "next", -1);
            const bool has_bitmap = Integer(reply.get(), "bitmap") == 1024;
            if (!parse_items(reply.get(), shot))
                return "invalid_response";
            for (int guard = 0; next >= 0; ++guard) {
                if (guard >= 8)
                    return "invalid_response";
                auto page = companion("{\"op\":\"screen.page\",\"seq\":" + std::to_string(seq) +
                                      ",\"text\":" + std::to_string(next) + "}");
                if (!cJSON_IsTrue(cJSON_GetObjectItem(page.get(), "ok")) ||
                    !parse_items(page.get(), shot))
                    return "invalid_response";
                const int following = Integer(page.get(), "next", -1);
                if (following >= 0 && following <= next)
                    return "invalid_response";
                next = following;
            }
            if (bitmap && has_bitmap) {
                for (int offset = 0; offset < 1024; offset += 384) {
                    auto page = companion("{\"op\":\"screen.page\",\"seq\":" + std::to_string(seq) +
                                          ",\"bitmap\":" + std::to_string(offset) + "}");
                    std::string data;
                    if (!cJSON_IsTrue(cJSON_GetObjectItem(page.get(), "ok")) ||
                        Integer(page.get(), "off", -1) != offset ||
                        !dfscreen::Unbase64(String(page.get(), "data"), data) ||
                        data.size() != size_t(std::min(384, 1024 - offset)))
                        return "invalid_response";
                    shot.bitmap += data;
                }
            }
            return {};
        };
        auto add_screen = [&](const Shot& shot, bool bitmap) {
            cJSON_AddStringToObject(out, "app", shot.app.c_str());
            cJSON_AddBoolToObject(out, "locked", shot.locked);
            cJSON_AddStringToObject(out, "view", dfscreen::View(shot.items).c_str());
            auto selected = dfscreen::Selected(shot.items);
            cJSON_AddStringToObject(out, "selected",
                                    selected ? dfscreen::Trim(selected->text).c_str() : "");
            auto labels = dfscreen::FindButtons(shot.items);
            auto buttons = cJSON_AddObjectToObject(out, "buttons");
            cJSON_AddStringToObject(buttons, "left", labels.left.c_str());
            cJSON_AddStringToObject(buttons, "ok", labels.ok.c_str());
            cJSON_AddStringToObject(buttons, "right", labels.right.c_str());
            if (!shot.text)
                cJSON_AddBoolToObject(out, "text", false);  // direct-draw app: use bitmap
            if (bitmap && shot.bitmap.size() == 1024)
                cJSON_AddStringToObject(out, "bitmap", dfscreen::Base64(shot.bitmap).c_str());
        };
        // The label a key would activate, when it is a send/emulate/write/delete action.
        auto danger = [](int key, const Shot& shot) -> std::string {
            auto buttons = dfscreen::FindButtons(shot.items);
            std::vector<std::string> labels;
            if (key == kOk) {
                if (auto selected = dfscreen::Selected(shot.items))
                    labels.push_back(dfscreen::Trim(selected->text));
                labels.push_back(buttons.ok);
            } else if (key == kRight)
                labels.push_back(buttons.right);
            else if (key == kLeft)
                labels.push_back(buttons.left);
            for (const auto& label : labels)
                if (Dangerous(label))
                    return label;
            return {};
        };
        auto press = [&](const KeyPress& key) {
            return checked(transport_.Native(23, {}, {}, false, key.key + (key.hold ? 6 : 0)),
                           "key_input");
        };
        auto gated = [&](int key, bool confirm, Shot& shot, int pressed) {
            if (confirm || (key != kOk && key != kLeft && key != kRight))
                return false;
            auto label = danger(key, shot);
            if (label.empty())
                return false;
            add_screen(shot, false);
            cJSON_AddNumberToObject(out, "pressed", pressed);
            cJSON_AddStringToObject(out, "action", label.c_str());
            error("needs_confirmation", "confirm");
            return true;
        };
        if (!cJSON_IsObject(args.get()))
            return error("invalid_args");
        if (transport_.NativeCancelled())
            return error("cancelled");
        if (transport_.NowMs() >= deadline_ms_)
            return error("timeout");
        // This query is read-only and remains available while the desktop is PIN locked.
        uint64_t locked = 0;
        // Firmware/power information is read-only and needs no desktop admission.
        if (op != "native.system.info" &&
            !desktop_lock(transport_.Native(66), locked, "desktop_state"))
            return finish();
        if (op == "native.app.state") {
            auto reply = transport_.Native(17);
            uint64_t busy = 0;
            if (!checked(reply, "application_state"))
                return finish();
            if (reply.message.tag != 18 || !number(reply.message, 1, busy))
                return error("invalid_response");
            cJSON_AddBoolToObject(out, "locked", locked);
            cJSON_AddBoolToObject(out, "foreground", busy);
            cJSON_AddBoolToObject(out, "rpc_app_running", transport_.AppRunning());
            cJSON_AddStringToObject(out, "rpc_app", transport_.AppName().c_str());
            cJSON_AddStringToObject(out, "retained_app", transport_.RetainedApp().c_str());
            cJSON_AddNumberToObject(
                out, "lease_remaining_ms",
                transport_.RetainedApp().empty()
                    ? 0
                    : std::max<int64_t>(0, transport_.RetainedDeadline() - transport_.NowMs()));
        } else if (op == "native.system.info") {
            auto info = cJSON_AddObjectToObject(out, "info");
            auto power = cJSON_AddObjectToObject(out, "power");
            size_t output_bytes = 0;
            bool truncated = false;
            for (int section = 0; section < 2; ++section) {
                int count = 0;
                auto reply =
                    transport_.Native(section ? 44 : 32, {}, [&](const dfwire::Message& message) {
                        if (message.status)
                            return true;
                        if (message.tag != uint32_t(section ? 45 : 33))
                            return false;
                        dfwire::Reader reader(message.payload);
                        dfwire::Field field;
                        std::string key, value;
                        while (reader.Next(field)) {
                            if (field.tag == 1 && field.wire == 2)
                                key = field.text();
                            if (field.tag == 2 && field.wire == 2)
                                value = field.text();
                        }
                        if (!reader.Done() || key.empty() || key.size() > 96 || value.size() > 256)
                            return false;
                        if (++count > 96 || output_bytes + key.size() + value.size() + 16 > 6000) {
                            truncated = true;
                            return true;
                        }
                        output_bytes += key.size() + value.size() + 16;
                        cJSON_AddStringToObject(section ? power : info, key.c_str(), value.c_str());
                        return true;
                    });
                if (!checked(reply, section ? "power_info" : "device_info"))
                    return finish();
            }
            cJSON_AddBoolToObject(out, "truncated", truncated);
        } else if (op == "native.screen") {
            // Read-only: also reports the PIN lock screen.
            Shot shot;
            const bool bitmap = flag("bitmap");
            auto code = read_screen(bitmap, shot);
            if (!code.empty())
                return error(code, "screen");
            add_screen(shot, bitmap);
        } else {
            if (locked && op != "native.app.exit")
                return error("locked");
            if (op == "native.gpio" && !transport_.RetainedApp().empty() &&
                String(args.get(), "action") != "read")
                return error("busy", "retained_app_resource");
            if (op == "native.app.catalog") {
                const int offset = Integer(args.get(), "offset", 0),
                          limit = Integer(args.get(), "limit", 4);
                if (offset < 0 || offset > 1000 || limit < 1 || limit > 4)
                    return error("invalid_catalog_range");
                struct Candidate {
                    std::string name;
                    uint64_t size;
                };
                std::vector<Candidate> candidates;
                int seen = 0;
                bool truncated = false;
                auto listed = transport_.Native(
                    7, string_field(1, dfcatalog::kDirectory), [&](const dfwire::Message& message) {
                        if (message.status)
                            return true;
                        if (message.tag != 8)
                            return false;
                        dfwire::Reader list(message.payload);
                        dfwire::Field entry;
                        while (list.Next(entry)) {
                            if (entry.tag != 1 || entry.wire != 2)
                                continue;
                            dfwire::Reader file(entry.bytes);
                            dfwire::Field field;
                            std::string name;
                            uint64_t type = 0, size = 0;
                            while (file.Next(field)) {
                                if (field.tag == 1 && field.wire == 0)
                                    type = field.number;
                                if (field.tag == 2 && field.wire == 2)
                                    name = field.text();
                                if (field.tag == 3 && field.wire == 0)
                                    size = field.number;
                            }
                            if (!file.Done())
                                return false;
                            if (type || !dfcatalog::FileName(name))
                                continue;
                            if (seen == 1000) {
                                truncated = true;
                                continue;
                            }
                            int position = seen++;
                            if (position >= offset && candidates.size() < size_t(limit))
                                candidates.push_back({name, size});
                        }
                        return list.Done();
                    });
                auto items = cJSON_AddArrayToObject(out, "items");
                auto rejected = cJSON_AddArrayToObject(out, "rejected");
                auto reject = [&](const std::string& name, const char* code) {
                    auto item = cJSON_CreateObject();
                    cJSON_AddStringToObject(item, "registration", name.c_str());
                    cJSON_AddStringToObject(item, "code", code);
                    cJSON_AddItemToArray(rejected, item);
                };
                if (listed.status != 7 && !checked(listed, "catalog_list"))
                    return finish();
                for (const auto& candidate : candidates) {
                    if (transport_.NativeCancelled())
                        return error("cancelled", "catalog");
                    if (transport_.NowMs() >= deadline_ms_)
                        return error("timeout", "catalog");
                    if (!candidate.size || candidate.size > 767) {
                        reject(candidate.name, "invalid_registration_size");
                        continue;
                    }
                    std::string data;
                    auto read = read_file(std::string(dfcatalog::kDirectory) + "/" + candidate.name,
                                          0, 768, data);
                    if (read.status == 7) {
                        reject(candidate.name, "registration_missing");
                        continue;
                    }
                    if (!checked(read, "catalog_read"))
                        return finish();
                    Json value(data.size() == candidate.size &&
                                       data.find('\0') == std::string::npos &&
                                       deepflipper_json_within_limits(data.c_str(), 767, 3)
                                   ? cJSON_ParseWithOpts(data.c_str(), nullptr, true)
                                   : nullptr,
                               cJSON_Delete);
                    if (!dfcatalog::Validate(value.get(), candidate.name)) {
                        reject(candidate.name, "invalid_registration");
                        continue;
                    }
                    auto path = dfcatalog::String(value.get(), "app");
                    auto stat = transport_.Native(24, string_field(1, path));
                    if (stat.status == 7) {
                        reject(candidate.name, "app_missing");
                        continue;
                    }
                    if (!checked(stat, "catalog_app_stat"))
                        return finish();
                    uint64_t bytes = 0, type = 0;
                    bool found = false;
                    dfwire::Reader reader(stat.message.payload);
                    dfwire::Field field;
                    while (reader.Next(field)) {
                        if (field.tag != 1 || field.wire != 2)
                            continue;
                        if (found)
                            return error("invalid_response", "catalog_app_stat");
                        found = true;
                        dfwire::Reader file(field.bytes);
                        dfwire::Field info;
                        while (file.Next(info)) {
                            if (info.tag == 1 && info.wire == 0)
                                type = info.number;
                            if (info.tag == 3 && info.wire == 0)
                                bytes = info.number;
                        }
                        if (!file.Done())
                            return error("invalid_response", "catalog_app_stat");
                    }
                    if (stat.message.tag != 25 || !reader.Done() || !found)
                        return error("invalid_response", "catalog_app_stat");
                    if (type ||
                        bytes != uint64_t(cJSON_GetObjectItem(value.get(), "fap_size")->valueint)) {
                        reject(candidate.name, "app_size_changed");
                        continue;
                    }
                    auto digest = transport_.Native(14, string_field(1, path));
                    if (digest.status == 7) {
                        reject(candidate.name, "app_missing");
                        continue;
                    }
                    if (!checked(digest, "catalog_app_digest"))
                        return finish();
                    std::string md5;
                    dfwire::Reader hash(digest.message.payload);
                    while (hash.Next(field))
                        if (field.tag == 1 && field.wire == 2)
                            md5 = field.text();
                    if (digest.message.tag != 15 || !hash.Done() || !dfcatalog::Hex(md5))
                        return error("invalid_response", "catalog_app_digest");
                    if (md5 != dfcatalog::String(value.get(), "fap_md5")) {
                        reject(candidate.name, "app_digest_changed");
                        continue;
                    }
                    cJSON_DeleteItemFromObject(value.get(), "format");
                    cJSON_DeleteItemFromObject(value.get(), "v");
                    cJSON_AddBoolToObject(value.get(), "file_verified", true);
                    cJSON_AddBoolToObject(value.get(), "runtime_verified", false);
                    cJSON_AddItemToArray(items, value.release());
                }
                cJSON_AddNumberToObject(
                    out, "next_offset",
                    seen > offset + int(candidates.size()) ? offset + int(candidates.size()) : -1);
                cJSON_AddNumberToObject(out, "total", seen);
                cJSON_AddBoolToObject(out, "truncated", truncated);
            } else if (op == "native.capture" || op == "native.app.describe" ||
                       op == "native.app.call") {
                const bool capture = op == "native.capture";
                auto kind = String(args.get(), "kind"), request = String(args.get(), "request_id");
                auto app =
                    capture ? "/ext/apps/Tools/deepflipper_tools.fap" : String(args.get(), "app");
                auto action =
                    op == "native.app.describe" ? "describe" : String(args.get(), "action");
                int timeout = Integer(args.get(), "timeout_ms", 5000);
                const bool reuse =
                    !capture && transport_.RetainedApp() == app && transport_.AppRunning();
                const bool keep_open =
                    !capture && (cJSON_IsTrue(cJSON_GetObjectItem(args.get(), "keep_open")) ||
                                 (op == "native.app.describe" && reuse));
                const int lease_ms = Integer(args.get(), "lease_ms", 30000);
                if (request.empty() || request.size() > 30 ||
                    request.find_first_not_of("0123456789_") != std::string::npos ||
                    timeout < 250 || timeout > 10000 || lease_ms < 5000 || lease_ms > 60000)
                    return error("invalid_app_request");
                Json command(cJSON_CreateObject(), cJSON_Delete);
                cJSON* arguments = nullptr;
                if (capture) {
                    action = kind == "nfc"      ? "nfc.read"
                             : kind == "ir"     ? "ir.learn"
                             : kind == "subghz" ? "subghz.record"
                                                : "";
                    arguments = cJSON_AddObjectToObject(command.get(), "arguments");
                    if (kind == "subghz")
                        cJSON_AddNumberToObject(arguments, "frequency",
                                                Integer(args.get(), "frequency", 433920000));
                } else {
                    const auto text =
                        op == "native.app.describe" ? "{}" : String(args.get(), "arguments");
                    if (text.size() > 512)
                        return error("arguments_too_large");
                    if (!deepflipper_json_within_limits(text.c_str(), 512, 6))
                        return error("invalid_arguments");
                    const char* end = nullptr;
                    arguments = cJSON_ParseWithOpts(text.c_str(), &end, true);
                    if (!cJSON_IsObject(arguments)) {
                        cJSON_Delete(arguments);
                        return error("invalid_arguments");
                    }
                    cJSON_AddItemToObject(command.get(), "arguments", arguments);
                }
                if (!Path(app, "/ext/apps") || !Suffix(app, ".fap") || action.empty() ||
                    action.size() > 48 ||
                    action.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789._") !=
                        std::string::npos)
                    return error("invalid_app_or_action");
                cJSON_AddNumberToObject(command.get(), "v", 1);
                cJSON_AddStringToObject(command.get(), "request_id", request.c_str());
                cJSON_AddStringToObject(command.get(), "action", action.c_str());
                cJSON_AddNumberToObject(command.get(), "timeout_ms", timeout);
                cJSON_AddBoolToObject(command.get(), "keep_open", keep_open);
                cJSON_AddNumberToObject(command.get(), "lease_ms", lease_ms);
                const auto command_json = Serialize(command.get());
                if (command_json.size() >= 768)
                    return error("request_too_large");
                // Refuse an old mailbox rather than risk accepting a prior execution's result.
                const std::string result_path = "/ext/deepflipper/jobs/" + request + ".json";
                std::string previous;
                auto prior = read_file(result_path, 0, 1, previous);
                if (prior.status != 7) {
                    if (!checked(prior, "mailbox_check"))
                        return finish();
                    return error("request_id_reused");
                }
                auto busy = transport_.Native(17);
                uint64_t active = 0;
                if (!checked(busy, "capture_app_state"))
                    return finish();
                if (busy.message.tag != 18 || !number(busy.message, 1, active) ||
                    (active && !reuse))
                    return error("busy");
                if (reuse && !active) {
                    transport_.RetainApp("", 0);
                    transport_.Reset();
                    return error("app_session_lost");
                }
                if (!reuse) {
                    auto launch = string_field(1, app);
                    dfwire::String(launch, 2, "RPC");
                    if (!checked(transport_.Native(16, launch), "capture_adapter_open"))
                        return finish();
                    if (!app_ready())
                        return finish();
                    transport_.SetAppName(app);
                }
                std::vector<uint8_t> payload;
                dfwire::String(payload, 1, command_json);
                bool okay = checked(transport_.Native(65, payload), "capture_start"), got = false;
                const int64_t end = transport_.NowMs() + timeout + 3000;
                while (okay && transport_.Alive() && transport_.NowMs() < end &&
                       transport_.NowMs() < deadline_ms_) {
                    if (transport_.NativeCancelled()) {
                        error("cancelled", "capture");
                        okay = false;
                        break;
                    }
                    uint64_t pin = 0;
                    if (!desktop_lock(transport_.Native(66), pin, "app_lock") || pin) {
                        if (pin)
                            error("locked", "app_action");
                        okay = false;
                        break;
                    }
                    std::string data;
                    auto reply = read_file(result_path, 0, 4096, data);
                    if (reply.status == 7) {
                        transport_.Pump();
                        continue;
                    }
                    if (!checked(reply, "capture_result")) {
                        okay = false;
                        break;
                    }
                    Json parsed(deepflipper_json_within_limits(data.c_str(), 4095, 8)
                                    ? cJSON_ParseWithOpts(data.c_str(), nullptr, true)
                                    : nullptr,
                                cJSON_Delete);
                    auto version = cJSON_GetObjectItemCaseSensitive(parsed.get(), "v");
                    if (cJSON_IsObject(parsed.get()) && cJSON_IsNumber(version) &&
                        version->valuedouble == 1 &&
                        String(parsed.get(), "request_id") == request &&
                        String(parsed.get(), "action") == action &&
                        cJSON_IsObject(cJSON_GetObjectItem(parsed.get(), "data")) &&
                        cJSON_IsBool(cJSON_GetObjectItem(parsed.get(), "ok"))) {
                        got = true;
                        bool success = cJSON_IsTrue(cJSON_GetObjectItem(parsed.get(), "ok"));
                        auto code = String(parsed.get(), "code");
                        if (success && keep_open &&
                            (!cJSON_IsTrue(
                                 cJSON_GetObjectItem(parsed.get(), "session_supported")) ||
                             !cJSON_IsTrue(cJSON_GetObjectItem(parsed.get(), "keep_open")))) {
                            success = false;
                            code = "session_not_supported";
                        }
                        cJSON_AddItemToObject(out, capture ? "capture" : "app_result",
                                              parsed.release());
                        if (!success) {
                            error(code.empty() ? "app_action_failed" : code, "app_action");
                            okay = false;
                        }
                        break;
                    }
                    error("invalid_app_result", "app_result");
                    okay = false;
                    break;
                }
                if (okay && !got) {
                    error("timeout", "capture_result");
                    okay = false;
                }
                if (okay && keep_open && !transport_.AppRunning()) {
                    error("app_session_lost", "app_retention");
                    okay = false;
                }
                const bool retained = okay && got && keep_open && transport_.Alive();
                bool stopped = !retained && transport_.Alive() && app_exit();
                if (retained)
                    transport_.RetainApp(app, transport_.NowMs() + lease_ms);
                cJSON_AddBoolToObject(out, "app_retained", retained);
                cJSON_AddBoolToObject(out, "stopped", stopped);
                if (!retained && !stopped) {
                    transport_.Reset();
                    return error("stop_unconfirmed", "capture_cleanup");
                }
                if (got) {
                    auto removed = transport_.Native(12, string_field(1, result_path), {}, true);
                    cJSON_AddBoolToObject(out, "mailbox_removed",
                                          removed.done && removed.error.empty() && !removed.status);
                }
                if (retained && (!transport_.Alive() || !transport_.AppRunning())) {
                    cJSON_DeleteItemFromObject(out, "app_retained");
                    cJSON_AddBoolToObject(out, "app_retained", false);
                    transport_.Reset();
                    return error("app_session_lost", "app_retention");
                }
                if (!okay)
                    return finish();
            } else if (op == "native.file.read") {
                auto path = String(args.get(), "path");
                int offset = Integer(args.get(), "offset", 0),
                    size = Integer(args.get(), "size", 512);
                if (!Path(path) || offset < 0 || size < 1 || size > 1024)
                    return error("invalid_path_or_range");
                std::string data;
                auto reply = read_file(path, offset, size, data);
                if (!checked(reply, "file_read"))
                    return finish();
                std::string hex;
                const char* digits = "0123456789ABCDEF";
                for (unsigned char byte : data) {
                    hex += digits[byte >> 4];
                    hex += digits[byte & 15];
                }
                cJSON_AddStringToObject(out, "path", path.c_str());
                cJSON_AddNumberToObject(out, "offset", offset);
                cJSON_AddNumberToObject(out, "bytes", data.size());
                cJSON_AddStringToObject(out, "hex", hex.c_str());
                cJSON_AddBoolToObject(out, "more_possible", data.size() == size_t(size));
            } else if (op == "native.storage.info") {
                auto path = String(args.get(), "path");
                if (!Path(path))
                    return error("invalid_path");
                auto reply = transport_.Native(28, string_field(1, path));
                uint64_t total = 0, free = 0;
                if (!checked(reply, "storage_info"))
                    return finish();
                if (reply.message.tag != 29 || !number(reply.message, 1, total) ||
                    !number(reply.message, 2, free))
                    return error("invalid_response");
                cJSON_AddNumberToObject(out, "total_bytes", double(total));
                cJSON_AddNumberToObject(out, "free_bytes", double(free));
            } else if (op == "native.clock") {
                auto action = String(args.get(), "action");
                if (action == "set") {
                    const char* names[] = {"hour",  "minute", "second", "day",
                                           "month", "year",   "weekday"};
                    const int lo[] = {0, 0, 0, 1, 1, 2000, 1}, hi[] = {23, 59, 59, 31, 12, 2099, 7};
                    std::vector<uint8_t> date;
                    for (int i = 0; i < 7; ++i) {
                        int value = Integer(args.get(), names[i], -1);
                        if (value < lo[i] || value > hi[i])
                            return error("invalid_datetime");
                        dfwire::UInt(date, i + 1, value);
                    }
                    std::vector<uint8_t> payload;
                    dfwire::Bytes(payload, 1, date);
                    if (!checked(transport_.Native(37, payload), "clock_set"))
                        return finish();
                } else if (action != "get")
                    return error("invalid_action");
                auto reply = transport_.Native(35);
                if (!checked(reply, "clock_get"))
                    return finish();
                if (reply.message.tag != 36)
                    return error("invalid_response");
                dfwire::Reader reader(reply.message.payload);
                dfwire::Field field;
                const char* names[] = {"hour",  "minute", "second", "day",
                                       "month", "year",   "weekday"};
                while (reader.Next(field))
                    if (field.tag == 1 && field.wire == 2) {
                        dfwire::Reader date(field.bytes);
                        dfwire::Field value;
                        while (date.Next(value))
                            if (value.tag >= 1 && value.tag <= 7 && value.wire == 0)
                                cJSON_AddNumberToObject(out, names[value.tag - 1],
                                                        double(value.number));
                        if (!date.Done())
                            return error("invalid_response");
                    }
                if (!reader.Done())
                    return error("invalid_response");
            } else if (op == "native.gpio") {
                auto pin = String(args.get(), "pin"), action = String(args.get(), "action");
                int id = Pin(pin);
                if (id < 0)
                    return error("invalid_pin");
                std::vector<uint8_t> payload;
                dfwire::UInt(payload, 1, id);
                if (action == "input" || action == "output") {
                    auto mode = payload;
                    dfwire::UInt(mode, 2, action == "input" ? 1 : 0);
                    if (!checked(transport_.Native(51, mode), "gpio_mode"))
                        return finish();
                } else if (action == "write") {
                    int level = Integer(args.get(), "value", -1);
                    if (level < 0 || level > 1)
                        return error("invalid_level");
                    auto data = payload;
                    dfwire::UInt(data, 2, level);
                    if (!checked(transport_.Native(57, data), "gpio_write"))
                        return finish();
                } else if (action != "read")
                    return error("invalid_action");
                auto mode = transport_.Native(53, payload), value = transport_.Native(55, payload);
                uint64_t m = 0, v = 0;
                if (!checked(mode, "gpio_mode_read") || !checked(value, "gpio_read"))
                    return finish();
                if (mode.message.tag != 54 || value.message.tag != 56 ||
                    !number(mode.message, 1, m) || !number(value.message, 2, v))
                    return error("invalid_response");
                cJSON_AddStringToObject(out, "pin", pin.c_str());
                cJSON_AddStringToObject(out, "mode", m == 1 ? "input" : "output");
                cJSON_AddNumberToObject(out, "value", double(v));
            } else if (op == "native.files" || op == "native.apps") {
                const bool apps = op == "native.apps";
                auto path = String(args.get(), "path");
                const int offset = Integer(args.get(), "offset", 0),
                          limit = Integer(args.get(), "limit", 8);
                if (!dfnative::Path(path, apps ? "/ext/apps" : "/ext") || offset < 0 ||
                    offset > 10000 || limit < 1 || limit > 8)
                    return error("invalid_path_or_range");
                if (apps) {
                    auto main = cJSON_AddArrayToObject(out, "main_apps");
                    for (auto name : dfnative::kMainApps)
                        cJSON_AddItemToArray(main, cJSON_CreateString(name));
                }
                auto items = cJSON_AddArrayToObject(out, "items");
                int seen = 0, added = 0;
                auto reply = transport_.Native(
                    7, string_field(1, path), [&](const dfwire::Message& message) {
                        if (message.status)
                            return true;
                        if (message.tag != 8)
                            return false;
                        dfwire::Reader list(message.payload);
                        dfwire::Field entry;
                        while (list.Next(entry))
                            if (entry.tag == 1 && entry.wire == 2) {
                                dfwire::Reader file(entry.bytes);
                                dfwire::Field field;
                                std::string name;
                                uint64_t size = 0, type = 0;
                                while (file.Next(field)) {
                                    if (field.tag == 1 && field.wire == 0)
                                        type = field.number;
                                    if (field.tag == 2 && field.wire == 2)
                                        name = field.text();
                                    if (field.tag == 3 && field.wire == 0)
                                        size = field.number;
                                }
                                if (!file.Done() || type > 1 || name.empty() || name.size() > 255)
                                    return false;
                                if (name.find('/') != std::string::npos ||
                                    name.find('\\') != std::string::npos || name == "." ||
                                    name == "..")
                                    return false;
                                if (apps && !type && !dfnative::Suffix(name, ".fap"))
                                    continue;
                                const int position = seen++;
                                if (position < offset || added == limit)
                                    continue;
                                auto item = cJSON_CreateObject();
                                cJSON_AddItemToArray(items, item);
                                ++added;
                                cJSON_AddStringToObject(item, "name", name.c_str());
                                cJSON_AddStringToObject(item, "path", (path + "/" + name).c_str());
                                cJSON_AddBoolToObject(item, "directory", type == 1);
                                cJSON_AddNumberToObject(item, "size", size);
                            }
                        return list.Done();
                    });
                if (!checked(reply, "directory_list"))
                    return finish();
                cJSON_AddNumberToObject(out, "total", seen);
                cJSON_AddNumberToObject(out, "next_offset",
                                        seen > offset + added ? offset + added : -1);
            } else if (op == "native.app.open") {
                auto name = String(args.get(), "name");
                if (!dfnative::App(name))
                    return error("unknown_app_or_path");
                auto busy = transport_.Native(17);
                uint64_t active = 0;
                if (!checked(busy, "application_state"))
                    return finish();
                if (busy.message.tag != 18 || !number(busy.message, 1, active))
                    return error("invalid_response");
                if (active)
                    return error("busy");
                bool rpc = cJSON_IsTrue(cJSON_GetObjectItem(args.get(), "rpc_mode"));
                auto payload = string_field(1, name);
                if (rpc)
                    dfwire::String(payload, 2, "RPC");
                if (!checked(transport_.Native(16, payload), "application_open"))
                    return finish();
                if (rpc) {
                    if (!app_ready())
                        return finish();
                    transport_.SetAppName(name);
                }
                cJSON_AddStringToObject(out, "application", name.c_str());
                cJSON_AddBoolToObject(out, "rpc_requested", rpc);
                cJSON_AddStringToObject(out, "result",
                                        "application_opened_not_hardware_action_completed");
                // Show what the app opened to; a failed read does not fail the open.
                wait(400);
                Shot shot;
                if (transport_.Alive() && read_screen(false, shot).empty())
                    add_screen(shot, false);
            } else if (op == "native.keys") {
                auto keys = Keys(String(args.get(), "keys"));
                const int delay = Integer(args.get(), "delay_ms", 120);
                if (keys.empty())
                    return error("invalid_keys");
                if (delay < 30 || delay > 1000)
                    return error("invalid_delay");
                const bool confirm = flag("confirm"), bitmap = flag("bitmap");
                Shot shot;
                int pressed = 0;
                for (size_t i = 0; i < keys.size(); ++i) {
                    const auto key = keys[i];
                    if (!confirm && (key.key == kOk || key.key == kLeft || key.key == kRight)) {
                        auto code = read_screen(false, shot);
                        if (!code.empty()) {
                            cJSON_AddNumberToObject(out, "pressed", pressed);
                            return error(code, "screen");
                        }
                        if (gated(key.key, confirm, shot, pressed))
                            return finish();
                    }
                    if (!press(key)) {
                        cJSON_AddNumberToObject(out, "pressed", pressed);
                        return finish();
                    }
                    ++pressed;
                    if (i + 1 < keys.size())
                        wait(delay);
                }
                wait(250);
                cJSON_AddNumberToObject(out, "pressed", pressed);
                auto code = read_screen(bitmap, shot);
                if (!code.empty())
                    return error(code, "screen");
                add_screen(shot, bitmap);
            } else if (op == "native.ui_select") {
                const auto target = String(args.get(), "text");
                if (dfscreen::Trim(target).empty() || target.size() > 64)
                    return error("invalid_text");
                const bool confirm = flag("confirm"), bitmap = flag("bitmap");
                Shot shot;
                auto code = read_screen(false, shot);
                if (!code.empty())
                    return error(code, "screen");
                int moves = 0, scan_down = 0, scan_up = 0;
                std::string last_view = dfscreen::View(shot.items);
                const dfscreen::Item* found = nullptr;
                while (true) {
                    found = dfscreen::Find(shot.items, target);
                    auto selected = dfscreen::Selected(shot.items);
                    if (found && found == selected)
                        break;
                    if (found && !selected) {
                        add_screen(shot, false);
                        return error("no_highlight", "ui_select");
                    }
                    int key;
                    if (found)
                        key = found->y > selected->y ? kDown : kUp;
                    else if (scan_down < 25)
                        key = kDown, ++scan_down;
                    else if (scan_up < 50)
                        key = kUp, ++scan_up;
                    else
                        key = -1;
                    if (key < 0 || moves >= 50 || transport_.NowMs() + 2000 >= deadline_ms_) {
                        add_screen(shot, false);
                        cJSON_AddNumberToObject(out, "moves", moves);
                        return error("not_found", "ui_select");
                    }
                    if (!press({key, false}))
                        return finish();
                    ++moves;
                    wait(60);
                    code = read_screen(false, shot);
                    if (!code.empty())
                        return error(code, "screen");
                    auto view = dfscreen::View(shot.items);
                    // A list that does not wrap stops changing at its end: scan back up.
                    if (!dfscreen::Find(shot.items, target) && key == kDown && view == last_view)
                        scan_down = 25;
                    last_view = view;
                }
                const auto chosen = dfscreen::Trim(found->text);
                if (gated(kOk, confirm, shot, 0))
                    return finish();
                if (!press({kOk, false}))
                    return finish();
                wait(250);
                code = read_screen(bitmap, shot);
                cJSON_AddStringToObject(out, "chosen", chosen.c_str());
                cJSON_AddNumberToObject(out, "moves", moves);
                if (!code.empty())
                    return error(code, "screen");
                add_screen(shot, bitmap);
            } else if (op == "native.home") {
                bool busy = true;
                int pressed = 0;
                for (int i = 0; i <= 13; ++i) {
                    auto state = transport_.Native(17);
                    uint64_t value = 0;
                    if (!checked(state, "application_state"))
                        return finish();
                    if (state.message.tag != 18 || !number(state.message, 1, value))
                        return error("invalid_response");
                    busy = value != 0;
                    if (!busy || i == 13)
                        break;
                    // Twelve short Backs, then one long Back for apps that need it.
                    if (!press({kBack, i == 12}))
                        return finish();
                    ++pressed;
                    wait(300);
                }
                cJSON_AddBoolToObject(out, "home", !busy);
                cJSON_AddNumberToObject(out, "pressed", pressed);
                Shot shot;
                if (read_screen(false, shot).empty())
                    add_screen(shot, false);
                if (busy)
                    return error("app_still_open", "home");
            } else if (op == "native.app.exit") {
                const bool retained_owner = !transport_.RetainedApp().empty();
                if (!app_exit()) {
                    if (retained_owner)
                        transport_.Reset();
                    cJSON_AddBoolToObject(out, "stopped", false);
                    return error("app_not_closed_or_rpc_unsupported", "application_exit");
                }
                cJSON_AddBoolToObject(out, "stopped", true);
                cJSON_AddStringToObject(out, "result", "application_exited");
            } else if (op == "native.input") {
                int key = dfnative::Key(String(args.get(), "key"));
                auto press = String(args.get(), "press_type");
                if (!press.empty() && press != "short" && press != "long")
                    return error("invalid_press_type");
                if (key < 0)
                    return error("invalid_key");
                if (!checked(transport_.Native(23, {}, {}, false, key + (press == "long" ? 6 : 0)),
                             "key_input"))
                    return finish();
                cJSON_AddStringToObject(out, "result",
                                        "input_delivered_not_semantic_action_verified");
            } else if (op == "native.alert") {
                if (!checked(transport_.Native(38), "audiovisual_alert"))
                    return finish();
            } else if (op == "native.saved") {
                const auto path = String(args.get(), "path"), kind = String(args.get(), "kind");
                const auto* spec = dfnative::Saved(kind, path);
                int duration = Integer(args.get(), "duration_ms", 1000),
                    index = Integer(args.get(), "index", 0);
                auto button = String(args.get(), "button");
                if (!spec || duration < 100 || duration > 5000 || index < 0 || index > 255 ||
                    button.size() > 80)
                    return error("invalid_saved_action");
                auto busy = transport_.Native(17);
                uint64_t active = 0;
                if (!checked(busy, "application_state"))
                    return finish();
                if (busy.message.tag != 18 || !number(busy.message, 1, active))
                    return error("invalid_response");
                if (active)
                    return error("busy");
                auto payload = string_field(1, spec->app);
                dfwire::String(payload, 2, "RPC");
                if (!checked(transport_.Native(16, payload), "application_open"))
                    return finish();
                if (!app_ready())
                    return finish();
                transport_.SetAppName(spec->app);
                bool okay =
                    checked(transport_.Native(48, string_field(1, path)), "load_saved_file");
                bool pressed = false;
                if (okay && spec->transmit) {
                    payload.clear();
                    if (!button.empty())
                        dfwire::String(payload, 1, button);
                    else
                        dfwire::UInt(payload, 2, index);
                    okay = checked(transport_.Native(49, payload), "start_transmission");
                    pressed = okay;
                }
                const int64_t end = transport_.NowMs() + duration;
                while (okay && transport_.Alive() && transport_.NowMs() < end) {
                    if (transport_.NativeCancelled()) {
                        error("cancelled", "active_action");
                        okay = false;
                        break;
                    }
                    if (transport_.NowMs() >= deadline_ms_) {
                        error("timeout", "active_action");
                        okay = false;
                        break;
                    }
                    // Keep companion state alive as well as the Expansion heartbeat.
                    transport_.Pump();
                    auto state = transport_.Native(66);
                    uint64_t pin_locked = 0;
                    if (!desktop_lock(state, pin_locked, "active_lock_state")) {
                        okay = false;
                        break;
                    }
                    if (pin_locked) {
                        error("locked", "active_action");
                        okay = false;
                        break;
                    }
                    transport_.Pump();
                }
                if (pressed && transport_.Alive()) {
                    auto released = transport_.Native(50, {}, {}, true);
                    if (!released.done || !released.error.empty() || released.status) {
                        if (okay)
                            error("stop_unconfirmed", "release");
                        okay = false;
                    }
                }
                bool stopped = transport_.Alive() && app_exit();
                cJSON_AddBoolToObject(out, "stopped", stopped);
                cJSON_AddBoolToObject(out, "target_state_verified", false);
                if (!stopped) {
                    transport_.Reset();
                    if (okay)
                        error("stop_unconfirmed", "application_exit");
                    okay = false;
                }
                if (!okay)
                    return finish();
                cJSON_AddStringToObject(
                    out, "result", spec->transmit ? "transmission_finished" : "emulation_finished");
                cJSON_AddStringToObject(out, "kind", kind.c_str());
                cJSON_AddStringToObject(out, "path", path.c_str());
                cJSON_AddNumberToObject(out, "duration_ms", duration);
            } else
                return error("unsupported_native_operation");
        }
        cJSON_AddBoolToObject(out, "ok", true);
        cJSON_AddStringToObject(out, "state", "done");
        return finish();
    }
};
}  // namespace dfnative
