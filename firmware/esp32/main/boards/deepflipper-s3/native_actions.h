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
            if (op == "native.capture") {
                auto kind = String(args.get(), "kind"), request = String(args.get(), "request_id");
                int timeout = Integer(args.get(), "timeout_ms", 5000);
                if ((kind != "nfc" && kind != "ir" && kind != "subghz") || request.empty() ||
                    request.size() > 30 ||
                    request.find_first_not_of("0123456789_") != std::string::npos ||
                    timeout < 250 || timeout > 10000)
                    return error("invalid_capture_args");
                auto busy = transport_.Native(17);
                uint64_t active = 0;
                if (!checked(busy, "capture_app_state"))
                    return finish();
                if (busy.message.tag != 18 || !number(busy.message, 1, active) || active)
                    return error("busy");
                auto launch = string_field(1, "/ext/apps/Tools/deepflipper_tools.fap");
                dfwire::String(launch, 2, "RPC");
                if (!checked(transport_.Native(16, launch), "capture_adapter_open"))
                    return finish();
                if (!app_ready())
                    return finish();
                transport_.SetAppName("DeepFlipper Tools");
                std::vector<uint8_t> payload;
                dfwire::String(payload, 1, Serialize(args.get()));
                bool okay = checked(transport_.Native(65, payload), "capture_start"), got = false;
                std::string result_path = "/ext/deepflipper/jobs/" + request + ".json";
                const int64_t end = transport_.NowMs() + timeout + 3000;
                while (okay && transport_.Alive() && transport_.NowMs() < end &&
                       transport_.NowMs() < deadline_ms_) {
                    if (transport_.NativeCancelled()) {
                        error("cancelled", "capture");
                        okay = false;
                        break;
                    }
                    std::string data;
                    auto reply = read_file(result_path, 0, 1536, data);
                    if (reply.status == 7) {
                        transport_.Pump();
                        continue;
                    }
                    if (!checked(reply, "capture_result")) {
                        okay = false;
                        break;
                    }
                    Json parsed(cJSON_Parse(data.c_str()), cJSON_Delete);
                    if (parsed && String(parsed.get(), "request_id") == request) {
                        got = true;
                        bool success = cJSON_IsTrue(cJSON_GetObjectItem(parsed.get(), "ok"));
                        cJSON_AddItemToObject(out, "capture", parsed.release());
                        if (!success) {
                            error("capture_failed", "capture");
                            okay = false;
                        }
                        break;
                    }
                    transport_.Pump();
                }
                if (okay && !got) {
                    error("timeout", "capture_result");
                    okay = false;
                }
                bool stopped = transport_.Alive() && app_exit();
                cJSON_AddBoolToObject(out, "stopped", stopped);
                if (!stopped) {
                    transport_.Reset();
                    return error("stop_unconfirmed", "capture_cleanup");
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
            } else if (op == "native.app.exit") {
                if (!app_exit())
                    return error("app_not_closed_or_rpc_unsupported", "application_exit");
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
