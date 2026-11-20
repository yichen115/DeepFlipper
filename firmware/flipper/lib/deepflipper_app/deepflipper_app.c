#include "deepflipper_app.h"
#include "json_limits.h"
#include <furi_hal.h>
#include <rpc/rpc_app.h>
#include <storage/storage.h>
#include <mjs/common/frozen/frozen.h>
#include <stdatomic.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// SDK code is linked into opt-in FAPs, not the resident firmware.
#define COMMAND_SIZE 768
#define RESULT_SIZE  4096

struct DeepFlipperApp {
    RpcAppSystem* rpc;
    FuriEventFlag* events;
    atomic_bool stop, command_ready;
    uint32_t deadline;
    bool retained;
    bool active_lease;
    char request_id[31];
    char command[COMMAND_SIZE];
};

static void rpc_event(const RpcAppSystemEvent* event, void* context) {
    DeepFlipperApp* app = context;
    if(event->type == RpcAppEventTypeSessionClose || event->type == RpcAppEventTypeAppExit) {
        if(event->type == RpcAppEventTypeAppExit) {
            rpc_system_app_confirm(app->rpc, true);
            rpc_system_app_send_exited(app->rpc);
        }
        rpc_system_app_set_callback(app->rpc, NULL, NULL);
        app->rpc = NULL;
        furi_event_flag_set(app->events, 1);
        // Publish stop last: the app thread may free the context after observing it.
        atomic_store(&app->stop, true);
    } else if(event->type == RpcAppEventTypeDataExchange) {
        bool valid = event->data.type == RpcAppSystemEventDataTypeBytes &&
                     event->data.bytes.size > 0 && event->data.bytes.size < sizeof(app->command) &&
                     !atomic_load(&app->command_ready) &&
                     !memchr(event->data.bytes.ptr, 0, event->data.bytes.size);
        if(valid) {
            memcpy(app->command, event->data.bytes.ptr, event->data.bytes.size);
            app->command[event->data.bytes.size] = 0;
            atomic_store(&app->command_ready, true);
        }
        rpc_system_app_confirm(app->rpc, valid);
        if(valid) furi_event_flag_set(app->events, 1);
    } else {
        rpc_system_app_confirm(app->rpc, false);
    }
}

bool deepflipper_app_cancelled(DeepFlipperApp* app) {
    return atomic_load(&app->stop) || furi_hal_rtc_is_flag_set(FuriHalRtcFlagLock) ||
           (int32_t)(app->deadline - furi_get_tick()) <= 0;
}

const char* deepflipper_app_request_id(const DeepFlipperApp* app) {
    return app->request_id;
}

bool deepflipper_app_retained(const DeepFlipperApp* app) {
    return app->retained;
}

static bool identifier(const char* value, size_t maximum, const char* alphabet) {
    return value && value[0] && strlen(value) <= maximum &&
           strspn(value, alphabet) == strlen(value);
}

static bool publish(
    DeepFlipperApp* app,
    const char* action,
    const char* code,
    const char* data,
    bool session_supported) {
    char* result = malloc(RESULT_SIZE);
    struct json_out out = JSON_OUT_BUF(result, RESULT_SIZE);
    int length = json_printf(
        &out,
        "{v:1,request_id:%Q,action:%Q,ok:%B,code:%Q,keep_open:%B,session_supported:%B,data:%s}",
        app->request_id,
        action ? action : "",
        code == NULL,
        code ? code : "ok",
        app->retained,
        session_supported,
        data);
    const bool complete = length >= 0 && length < RESULT_SIZE;
    if(length < 0 || length >= RESULT_SIZE) {
        out = (struct json_out)JSON_OUT_BUF(result, RESULT_SIZE);
        json_printf(
            &out,
            "{v:1,request_id:%Q,action:%Q,ok:false,code:%Q,keep_open:false,data:{}}",
            app->request_id,
            action ? action : "",
            "result_too_large");
    }
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, EXT_PATH("deepflipper"));
    storage_common_mkdir(storage, EXT_PATH("deepflipper/jobs"));
    char path[96], temporary[100];
    snprintf(path, sizeof(path), "/ext/deepflipper/jobs/%s.json", app->request_id);
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    File* file = storage_file_alloc(storage);
    bool written = storage_file_open(file, temporary, FSAM_WRITE, FSOM_CREATE_ALWAYS) &&
                   storage_file_write(file, result, strlen(result)) == strlen(result);
    if(written) written = storage_file_sync(file);
    storage_file_free(file);
    if(written) written = storage_common_rename(storage, temporary, path) == FSE_OK;
    if(!written) storage_common_remove(storage, temporary);
    furi_record_close(RECORD_STORAGE);
    free(result);
    return complete && written;
}

static void execute(
    DeepFlipperApp* app,
    const char* app_id,
    const DeepFlipperAction* actions,
    size_t action_count,
    void* context,
    const DeepFlipperAppHooks* hooks) {
    char request[31] = {0}, action[49] = {0};
    unsigned timeout = 0;
    struct json_token arguments = JSON_INVALID_TOKEN, version = JSON_INVALID_TOKEN,
                      duration = JSON_INVALID_TOKEN, request_token = JSON_INVALID_TOKEN,
                      action_token = JSON_INVALID_TOKEN, keep = JSON_INVALID_TOKEN,
                      lease = JSON_INVALID_TOKEN;
    int count = 0;
    if(deepflipper_json_within_limits(app->command, COMMAND_SIZE - 1, 8))
        count = json_scanf(
            app->command,
            strlen(app->command),
            "{v:%T,request_id:%T,action:%T,timeout_ms:%T,arguments:%T,keep_open:%T,lease_ms:%T}",
            &version,
            &request_token,
            &action_token,
            &duration,
            &arguments,
            &keep,
            &lease);
    if(request_token.type == JSON_TYPE_STRING && request_token.len > 0 &&
       request_token.len < (int)sizeof(request))
        memcpy(request, request_token.ptr, request_token.len);
    if(action_token.type == JSON_TYPE_STRING && action_token.len > 0 &&
       action_token.len < (int)sizeof(action))
        memcpy(action, action_token.ptr, action_token.len);
    bool valid_duration = duration.type == JSON_TYPE_NUMBER && duration.len <= 5;
    for(int i = 0; valid_duration && i < duration.len; ++i) {
        if(duration.ptr[i] < '0' || duration.ptr[i] > '9')
            valid_duration = false;
        else
            timeout = timeout * 10 + duration.ptr[i] - '0';
    }
    bool valid_id = identifier(request, 30, "0123456789_");
    unsigned lease_ms = 30000;
    bool valid_lease = lease.type == JSON_TYPE_INVALID;
    if(lease.type == JSON_TYPE_NUMBER && lease.len > 0 && lease.len <= 5) {
        lease_ms = 0;
        valid_lease = true;
        for(int i = 0; i < lease.len; ++i) {
            if(lease.ptr[i] < '0' || lease.ptr[i] > '9')
                valid_lease = false;
            else
                lease_ms = lease_ms * 10 + lease.ptr[i] - '0';
        }
        valid_lease = valid_lease && lease_ms >= 5000 && lease_ms <= 60000;
    }
    const bool supported = hooks && hooks->cleanup;
    const uint32_t previous_deadline = app->deadline;
    const bool previous_lease = app->active_lease;
    app->retained = keep.type == JSON_TYPE_TRUE && supported;
    if(valid_id) {
        strcpy(app->request_id, request);
        const char* code = "invalid_request";
        FuriString* result = furi_string_alloc_set("{}");
        bool valid =
            count == 5 + (keep.type != JSON_TYPE_INVALID) + (lease.type != JSON_TYPE_INVALID) &&
            valid_lease &&
            (keep.type == JSON_TYPE_INVALID || keep.type == JSON_TYPE_TRUE ||
             keep.type == JSON_TYPE_FALSE) &&
            version.type == JSON_TYPE_NUMBER && version.len == 1 && version.ptr[0] == '1' &&
            valid_duration && timeout >= 250 && timeout <= 10000 &&
            identifier(action, 48, "abcdefghijklmnopqrstuvwxyz0123456789._") &&
            arguments.type == JSON_TYPE_OBJECT_END && arguments.len <= 512 &&
            json_walk(app->command, strlen(app->command), NULL, NULL) == (int)strlen(app->command);
        // A follow-up command may not extend the old lease until it succeeds.
        const uint32_t action_deadline =
            furi_get_tick() + furi_ms_to_ticks(valid ? timeout + 1000 : 250);
        app->deadline = previous_lease && (int32_t)(previous_deadline - action_deadline) < 0 ?
                            previous_deadline :
                            action_deadline;
        if(valid && keep.type == JSON_TYPE_TRUE && !supported)
            code = "session_not_supported";
        else if(valid && !deepflipper_app_cancelled(app)) {
            if(!strcmp(action, "describe")) {
                const size_t item_size = 1024;
                char* item = malloc(item_size);
                struct json_out out = JSON_OUT_BUF(item, item_size);
                json_printf(
                    &out,
                    "{app_id:%Q,api_version:2,session_supported:%B,actions:[",
                    app_id,
                    supported);
                furi_string_set(result, item);
                code = NULL;
                for(size_t i = 0; i < action_count; ++i) {
                    out = (struct json_out)JSON_OUT_BUF(item, item_size);
                    int length = json_printf(
                        &out,
                        "%s{name:%Q,description:%Q,input_schema:%s,requires_session:%B}",
                        i ? "," : "",
                        actions[i].name,
                        actions[i].description,
                        actions[i].input_schema,
                        (actions[i].flags & DeepFlipperActionRequiresSession) != 0);
                    if(length < 0 || length >= (int)item_size ||
                       furi_string_size(result) + length > 3500) {
                        code = "descriptor_too_large";
                        furi_string_set(result, "{}");
                        break;
                    }
                    furi_string_cat(result, item);
                }
                if(!code) furi_string_cat(result, "]}");
                free(item);
            } else {
                code = "unsupported_action";
                char* json = malloc(arguments.len + 1);
                memcpy(json, arguments.ptr, arguments.len);
                json[arguments.len] = 0;
                for(size_t i = 0; i < action_count; ++i) {
                    if(strcmp(action, actions[i].name)) continue;
                    if((actions[i].flags & DeepFlipperActionRequiresSession) && !app->retained)
                        code = "session_required";
                    else
                        code = actions[i].handler(app, json, timeout, result, context);
                    break;
                }
                free(json);
            }
        } else if(valid)
            code = "cancelled";
        if(!code && deepflipper_app_cancelled(app)) code = "cancelled";
        // A handler must release its hardware before returning, including on cancellation.
        const char* data = furi_string_get_cstr(result);
        struct json_token token = JSON_INVALID_TOKEN;
        if(deepflipper_json_within_limits(data, RESULT_SIZE - 1, 7))
            json_scanf(data, strlen(data), "%T", &token);
        if(token.type != JSON_TYPE_OBJECT_END ||
           json_walk(data, strlen(data), NULL, NULL) != (int)strlen(data)) {
            code = "invalid_result";
            furi_string_set(result, "{}");
        }
        if(code || !app->retained) {
            app->retained = false;
            if(supported) hooks->cleanup(context);
        } else
            app->deadline = furi_get_tick() + furi_ms_to_ticks(lease_ms);
        app->active_lease = app->retained;
        // Enable the next command before publishing the result. All current command
        // values have already been copied; a callback may safely fill the mailbox.
        if(supported) atomic_store(&app->command_ready, false);
        if(!publish(app, action, code, furi_string_get_cstr(result), supported) && supported) {
            app->retained = false;
            app->active_lease = false;
            hooks->cleanup(context);
        }
        furi_string_free(result);
    } else {
        app->retained = false;
        app->active_lease = false;
        if(supported) {
            hooks->cleanup(context);
            atomic_store(&app->command_ready, false);
        }
    }
}

int32_t deepflipper_app_run_ex(
    void* rpc_arguments,
    const char* app_id,
    const DeepFlipperAction* actions,
    size_t action_count,
    void* context,
    const DeepFlipperAppHooks* hooks) {
    if(hooks && !hooks->cleanup) return 0;
    uintptr_t pointer = 0;
    if(!rpc_arguments || sscanf(rpc_arguments, "RPC %" SCNxPTR, &pointer) != 1 || !pointer)
        return 0;
    DeepFlipperApp* app = calloc(1, sizeof(DeepFlipperApp));
    app->rpc = (RpcAppSystem*)pointer;
    app->events = furi_event_flag_alloc();
    rpc_system_app_set_callback(app->rpc, rpc_event, app);
    rpc_system_app_send_started(app->rpc);
    while(!atomic_load(&app->stop)) {
        // A queued command must not renew an already expired lease before cleanup.
        if(app->retained && deepflipper_app_cancelled(app)) {
            hooks->cleanup(context);
            app->retained = false;
            app->active_lease = false;
        }
        if(atomic_load(&app->command_ready)) {
            execute(app, app_id, actions, action_count, context, hooks);
            // A one-shot adapter owns its command until the caller requests exit.
            if(!hooks) {
                while(!atomic_load(&app->stop))
                    furi_event_flag_wait(app->events, 1, FuriFlagWaitAny, 200);
            }
        } else {
            if(app->retained && hooks->tick) hooks->tick(app, context);
            furi_event_flag_wait(
                app->events, 1, FuriFlagWaitAny, app->retained && hooks->tick ? 10 : 50);
        }
    }
    if(hooks && hooks->cleanup) hooks->cleanup(context);
    furi_event_flag_free(app->events);
    free(app);
    return 0;
}

int32_t deepflipper_app_run(
    void* rpc_arguments,
    const char* app_id,
    const DeepFlipperAction* actions,
    size_t action_count,
    void* context) {
    return deepflipper_app_run_ex(rpc_arguments, app_id, actions, action_count, context, NULL);
}
