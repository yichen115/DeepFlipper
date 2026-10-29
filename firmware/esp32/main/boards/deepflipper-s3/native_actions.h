#pragma once
#include <cJSON.h>
#include <functional>
#include <memory>
#include "native_policy.h"
#include "rpc_wire.h"

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
    std::string Execute(const std::string& op, const std::string& args_json) {
        using Json = std::unique_ptr<cJSON, decltype(&cJSON_Delete)>;
        Json args(cJSON_Parse(args_json.c_str()), cJSON_Delete),
            output(cJSON_CreateObject(), cJSON_Delete);
        auto out = output.get();
        auto finish = [&]() { return Serialize(out); };
        auto error = [&](const std::string& code, const char* stage = "request") {
            cJSON_DeleteItemFromObject(out, "ok");
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
                if (!busy)
                    return true;
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
        if (!cJSON_IsObject(args.get()))
            return error("invalid_args");
        if (transport_.NativeCancelled())
            return error("cancelled");
        if (transport_.NowMs() >= deadline_ms_)
            return error("timeout");
        // This query is read-only and remains available while the desktop is PIN locked.
        auto locked_reply = transport_.Native(66);
        uint64_t locked = 1;
        if (!checked(locked_reply, "desktop_state"))
            return finish();
        if (locked_reply.message.tag != 70 || !number(locked_reply.message, 1, locked))
            return error("invalid_response");
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
        } else {
            if (locked)
                return error("locked");
            if (op == "native.files" || op == "native.apps") {
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
            } else if (op == "native.app.exit") {
                if (!app_exit())
                    return error("app_not_closed_or_rpc_unsupported", "application_exit");
                cJSON_AddStringToObject(out, "result", "application_exited");
            } else if (op == "native.input") {
                int key = dfnative::Key(String(args.get(), "key"));
                if (key < 0)
                    return error("invalid_key");
                if (!checked(transport_.Native(23, {}, {}, false, key), "key_input"))
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
                    if (!checked(state, "active_lock_state")) {
                        okay = false;
                        break;
                    }
                    if (state.message.tag != 70 || !number(state.message, 1, pin_locked)) {
                        error("invalid_response", "active_lock_state");
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
