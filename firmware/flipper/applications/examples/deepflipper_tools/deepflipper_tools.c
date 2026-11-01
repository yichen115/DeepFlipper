#include <furi.h>
#include <furi_hal.h>
#include <rpc/rpc_app.h>
#include <storage/storage.h>
#include <flipper_format/flipper_format.h>
#include <mjs/common/frozen/frozen.h>
#include <infrared/worker/infrared_worker.h>
#include <subghz/devices/devices.h>
#include <nfc/nfc_scanner.h>
#include "deepflipper_nfc.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    RpcAppSystem* rpc;
    FuriEventFlag* events;
    atomic_bool stop, detected, rf_gate, command_ready;
    atomic_uint samples;
    NfcProtocol protocol;
    char command[256];
    int32_t timings[4096];
    InfraredMessage ir;
    bool decoded;
} ToolContext;

static bool cancelled(ToolContext* ctx) {
    return atomic_load(&ctx->stop) || furi_hal_rtc_is_flag_set(FuriHalRtcFlagLock);
}
static void rpc_event(const RpcAppSystemEvent* event, void* context) {
    ToolContext* ctx = context;
    if(event->type == RpcAppEventTypeSessionClose) {
        rpc_system_app_set_callback(ctx->rpc, NULL, NULL);
        ctx->rpc = NULL;
        furi_event_flag_set(ctx->events, 1);
        atomic_store(&ctx->stop, true);
    } else if(event->type == RpcAppEventTypeAppExit) {
        rpc_system_app_confirm(ctx->rpc, true);
        rpc_system_app_send_exited(ctx->rpc);
        rpc_system_app_set_callback(ctx->rpc, NULL, NULL);
        ctx->rpc = NULL;
        furi_event_flag_set(ctx->events, 1);
        atomic_store(&ctx->stop, true);
    } else if(event->type == RpcAppEventTypeDataExchange) {
        bool valid = event->data.type == RpcAppSystemEventDataTypeBytes &&
                     event->data.bytes.size < sizeof(ctx->command) && !ctx->command[0];
        if(valid) {
            memcpy(ctx->command, event->data.bytes.ptr, event->data.bytes.size);
            ctx->command[event->data.bytes.size] = 0;
            atomic_store(&ctx->command_ready, true);
        }
        rpc_system_app_confirm(ctx->rpc, valid);
        if(valid) furi_event_flag_set(ctx->events, 1);
    } else
        rpc_system_app_confirm(ctx->rpc, false);
}
static void detected(NfcScannerEvent event, void* context) {
    ToolContext* ctx = context;
    if(event.type == NfcScannerEventTypeDetected && event.data.protocol_num) {
        ctx->protocol = event.data.protocols[0];
        atomic_store(&ctx->detected, true);
    }
}
static bool
    nfc_read(ToolContext* ctx, uint32_t timeout, const char* path, char* result, size_t capacity) {
    if(furi_hal_nfc_is_hal_ready() != FuriHalNfcErrorNone) return false;
    Nfc* nfc = nfc_alloc();
    NfcScanner* scanner = nfc_scanner_alloc(nfc);
    nfc_scanner_start(scanner, detected, ctx);
    uint32_t end = furi_get_tick() + furi_ms_to_ticks(timeout);
    while(!cancelled(ctx) && !atomic_load(&ctx->detected) && (int32_t)(end - furi_get_tick()) > 0)
        furi_delay_ms(20);
    nfc_scanner_stop(scanner);
    nfc_scanner_free(scanner);
    DeepFlipperNfc* read = NULL;
    if(atomic_load(&ctx->detected) && !cancelled(ctx))
        read = deepflipper_nfc_alloc(nfc, ctx->protocol);
    bool success = false;
    if(read) {
        while(!cancelled(ctx) && !deepflipper_nfc_done(read) &&
              (int32_t)(end - furi_get_tick()) > 0)
            furi_delay_ms(20);
        if(deepflipper_nfc_done(read) && !cancelled(ctx)) {
            deepflipper_nfc_result(read, result, capacity);
            success = deepflipper_nfc_save(read, path);
        }
        deepflipper_nfc_free(read);
    }
    nfc_free(nfc);
    return success;
}
static void ir_received(void* context, InfraredWorkerSignal* signal) {
    ToolContext* ctx = context;
    if(atomic_load(&ctx->detected)) return;
    ctx->decoded = infrared_worker_signal_is_decoded(signal);
    if(ctx->decoded)
        ctx->ir = *infrared_worker_get_decoded_signal(signal);
    else {
        const uint32_t* timings;
        size_t size;
        infrared_worker_get_raw_signal(signal, &timings, &size);
        if(!size || size > 1024) return;
        memcpy(ctx->timings, timings, size * sizeof(uint32_t));
        atomic_store(&ctx->samples, size);
    }
    atomic_store(&ctx->detected, true);
}
static bool ir_learn(ToolContext* ctx, uint32_t timeout, FlipperFormat* file) {
    InfraredWorker* worker = infrared_worker_alloc();
    infrared_worker_rx_set_received_signal_callback(worker, ir_received, ctx);
    infrared_worker_rx_enable_signal_decoding(worker, true);
    infrared_worker_rx_start(worker);
    uint32_t end = furi_get_tick() + furi_ms_to_ticks(timeout);
    while(!cancelled(ctx) && !atomic_load(&ctx->detected) && (int32_t)(end - furi_get_tick()) > 0)
        furi_delay_ms(20);
    infrared_worker_rx_stop(worker);
    infrared_worker_free(worker);
    if(cancelled(ctx) || !atomic_load(&ctx->detected)) return false;
    bool ok = flipper_format_write_header_cstr(file, "IR signals file", 1) &&
              flipper_format_write_string_cstr(file, "name", "Captured");
    if(ctx->decoded) {
        ok = ok && flipper_format_write_string_cstr(file, "type", "parsed") &&
             flipper_format_write_string_cstr(
                 file, "protocol", infrared_get_protocol_name(ctx->ir.protocol)) &&
             flipper_format_write_hex(file, "address", (const uint8_t*)&ctx->ir.address, 4) &&
             flipper_format_write_hex(file, "command", (const uint8_t*)&ctx->ir.command, 4);
    } else {
        uint32_t frequency = 38000;
        float duty = 0.33f;
        ok = ok && flipper_format_write_string_cstr(file, "type", "raw") &&
             flipper_format_write_uint32(file, "frequency", &frequency, 1) &&
             flipper_format_write_float(file, "duty_cycle", &duty, 1) &&
             flipper_format_write_uint32(
                 file, "data", (uint32_t*)ctx->timings, atomic_load(&ctx->samples));
    }
    return ok;
}
static void rf_received(bool level, uint32_t duration, void* context) {
    ToolContext* ctx = context;
    if(!atomic_load(&ctx->rf_gate) || !duration || duration > 1000000) return;
    unsigned count = atomic_load(&ctx->samples);
    if(count < COUNT_OF(ctx->timings)) {
        ctx->timings[count] = level ? (int32_t)duration : -(int32_t)duration;
        atomic_store(&ctx->samples, count + 1);
    }
}
static bool
    rf_record(ToolContext* ctx, uint32_t timeout, uint32_t frequency, FlipperFormat* file) {
    subghz_devices_init();
    const SubGhzDevice* radio = subghz_devices_get_by_name("cc1101_int");
    if(!radio || !subghz_devices_is_frequency_valid(radio, frequency) ||
       !subghz_devices_begin(radio)) {
        subghz_devices_deinit();
        return false;
    }
    subghz_devices_reset(radio);
    subghz_devices_load_preset(radio, FuriHalSubGhzPresetOok650Async, NULL);
    frequency = subghz_devices_set_frequency(radio, frequency);
    subghz_devices_start_async_rx(radio, rf_received, ctx);
    uint32_t end = furi_get_tick() + furi_ms_to_ticks(timeout);
    while(!cancelled(ctx) && atomic_load(&ctx->samples) < COUNT_OF(ctx->timings) &&
          (int32_t)(end - furi_get_tick()) > 0) {
        if(subghz_devices_get_rssi(radio) > -75) atomic_store(&ctx->rf_gate, true);
        furi_delay_ms(10);
    }
    subghz_devices_stop_async_rx(radio);
    subghz_devices_sleep(radio);
    subghz_devices_end(radio);
    subghz_devices_deinit();
    unsigned count = atomic_load(&ctx->samples);
    if(cancelled(ctx) || count < 8) return false;
    bool ok = flipper_format_write_header_cstr(file, "Flipper SubGhz RAW File", 1) &&
              flipper_format_write_uint32(file, "Frequency", &frequency, 1) &&
              flipper_format_write_string_cstr(file, "Preset", "FuriHalSubGhzPresetOok650Async") &&
              flipper_format_write_string_cstr(file, "Protocol", "RAW");
    for(unsigned i = 0; ok && i < count; i += 128)
        ok = flipper_format_write_int32(file, "RAW_Data", ctx->timings + i, MIN(128U, count - i));
    return ok;
}

int32_t deepflipper_tools_main(void* args) {
    unsigned long pointer = 0;
    if(!args || sscanf(args, "RPC %lX", &pointer) != 1 || !pointer) return 0;
    ToolContext* ctx = calloc(1, sizeof(ToolContext));
    ctx->rpc = (RpcAppSystem*)pointer;
    ctx->events = furi_event_flag_alloc();
    rpc_system_app_set_callback(ctx->rpc, rpc_event, ctx);
    rpc_system_app_send_started(ctx->rpc);
    furi_event_flag_wait(ctx->events, 1, FuriFlagWaitAny, 5000);
    char *kind = NULL, *request = NULL;
    int timeout = 5000;
    unsigned frequency = 433920000;
    if(atomic_load(&ctx->command_ready))
        json_scanf(
            ctx->command,
            strlen(ctx->command),
            "{kind:%Q,request_id:%Q,timeout_ms:%d,frequency:%u}",
            &kind,
            &request,
            &timeout,
            &frequency);
    bool valid = kind &&
                 (!strcmp(kind, "nfc") || !strcmp(kind, "ir") || !strcmp(kind, "subghz")) &&
                 request && strlen(request) <= 30 && timeout >= 250 && timeout <= 10000;
    for(size_t i = 0; valid && request[i]; ++i)
        if((request[i] < '0' || request[i] > '9') && request[i] != '_') valid = false;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_common_mkdir(storage, EXT_PATH("deepflipper"));
    storage_common_mkdir(storage, EXT_PATH("deepflipper/jobs"));
    char result[1200] = {0}, path[120] = {0}, result_path[120] = {0}, detail[510] = {0};
    if(valid && !cancelled(ctx)) {
        const char* folder = !strcmp(kind, "nfc") ? "nfc" :
                             !strcmp(kind, "ir")  ? "infrared" :
                                                    "subghz";
        const char* ext = !strcmp(kind, "nfc") ? "nfc" : !strcmp(kind, "ir") ? "ir" : "sub";
        snprintf(path, sizeof(path), "/ext/%s/deepflipper_%s.%s", folder, request, ext);
        snprintf(result_path, sizeof(result_path), "/ext/deepflipper/jobs/%s.json", request);
        char folder_path[32];
        snprintf(folder_path, sizeof(folder_path), "/ext/%s", folder);
        storage_common_mkdir(storage, folder_path);
        FileInfo info;
        bool ok = false;
        if(storage_common_stat(storage, path, &info) == FSE_NOT_EXIST) {
            if(!strcmp(kind, "nfc"))
                ok = nfc_read(ctx, timeout, path, detail, sizeof(detail));
            else if(!strcmp(kind, "ir") || !strcmp(kind, "subghz")) {
                FlipperFormat* file = flipper_format_file_alloc(storage);
                if(flipper_format_file_open_new(file, path))
                    ok = !strcmp(kind, "ir") ? ir_learn(ctx, timeout, file) :
                                               rf_record(ctx, timeout, frequency, file);
                flipper_format_free(file);
                if(!ok) storage_common_remove(storage, path);
            }
        }
        struct json_out out = JSON_OUT_BUF(result, sizeof(result));
        json_printf(
            &out,
            "{ok:%B,request_id:%Q,kind:%Q,state:%Q,path:%Q,samples:%u,raw_unverified:%B,truncated:%B}",
            ok,
            request,
            kind,
            ok             ? "done" :
            cancelled(ctx) ? "cancelled" :
                             "no_signal_or_read_failed",
            ok ? path : "",
            atomic_load(&ctx->samples),
            !strcmp(kind, "subghz"),atomic_load(&ctx->samples)==4096);
        if(detail[0]) {
            size_t used = strlen(result);
            if(used)
                snprintf(result + used - 1, sizeof(result) - used + 1, ",\"card\":%s}", detail);
        }
        // Store a bounded result after releasing the reader/radio. The UART client
        // reads this file; arbitrary async RPC payloads are not used as results.
        File* output = storage_file_alloc(storage);
        char temporary[128];
        snprintf(temporary, sizeof(temporary), "%s.tmp", result_path);
        if(storage_file_open(output, temporary, FSAM_WRITE, FSOM_CREATE_ALWAYS))
            storage_file_write(output, result, strlen(result));
        storage_file_free(output);
        storage_common_rename(storage, temporary, result_path);
    }
    furi_record_close(RECORD_STORAGE);
    free(kind);
    free(request);
    // A live RPC callback owns ctx until AppExit/SessionClose detaches it.
    // The client confirms actual shutdown by polling Loader's busy state.
    while(!atomic_load(&ctx->stop))
        furi_event_flag_wait(ctx->events, 1, FuriFlagWaitAny, 200);
    furi_event_flag_free(ctx->events);
    free(ctx);
    return 0;
}
