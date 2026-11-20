#include <furi.h>
#include <furi_hal.h>
#include <deepflipper_app/deepflipper_app.h>
#include <storage/storage.h>
#include <flipper_format/flipper_format.h>
#include <mjs/common/frozen/frozen.h>
#include <infrared/worker/infrared_worker.h>
#include <subghz/devices/devices.h>
#include <nfc/nfc_scanner.h>
#include "deepflipper_nfc.h"
#include "deepflipper_radio.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    DeepFlipperApp* app;
    atomic_bool detected, rf_gate;
    atomic_uint samples;
    NfcProtocol protocol;
    int32_t timings[4096];
    InfraredMessage ir;
    bool decoded;
} ToolContext;

static bool cancelled(ToolContext* ctx) {
    return deepflipper_app_cancelled(ctx->app);
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

static const char* capture_action(
    DeepFlipperApp* app,
    const char* arguments,
    uint32_t timeout,
    FuriString* output,
    void* context) {
    const char* kind = context;
    if(memmgr_get_free_heap() < sizeof(ToolContext) + 8192 ||
       memmgr_heap_get_max_free_block() < sizeof(ToolContext))
        return "insufficient_memory";
    ToolContext* ctx = calloc(1, sizeof(ToolContext));
    ctx->app = app;
    unsigned frequency = 433920000;
    if(!strcmp(kind, "subghz")) {
        struct json_token value = JSON_INVALID_TOKEN;
        json_scanf(arguments, strlen(arguments), "{frequency:%T}", &value);
        if(value.type != JSON_TYPE_NUMBER || value.len > 9) {
            free(ctx);
            return "invalid_frequency";
        }
        frequency = 0;
        for(int i = 0; i < value.len; ++i) {
            if(value.ptr[i] < '0' || value.ptr[i] > '9') {
                free(ctx);
                return "invalid_frequency";
            }
            frequency = frequency * 10 + value.ptr[i] - '0';
        }
        if(!furi_hal_subghz_is_frequency_valid(frequency)) {
            free(ctx);
            return "invalid_frequency";
        }
    }
    const char* request = deepflipper_app_request_id(app);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    const char* folder = !strcmp(kind, "nfc") ? "nfc" :
                         !strcmp(kind, "ir")  ? "infrared" :
                                                "subghz";
    const char* ext = !strcmp(kind, "nfc") ? "nfc" : !strcmp(kind, "ir") ? "ir" : "sub";
    char result[1200] = {0}, path[120], detail[510] = {0}, folder_path[32];
    snprintf(path, sizeof(path), "/ext/%s/deepflipper_%s.%s", folder, request, ext);
    snprintf(folder_path, sizeof(folder_path), "/ext/%s", folder);
    storage_common_mkdir(storage, folder_path);
    FileInfo info;
    bool ok = false;
    if(storage_common_stat(storage, path, &info) == FSE_NOT_EXIST && !cancelled(ctx)) {
        if(!strcmp(kind, "nfc")) {
            ok = nfc_read(ctx, timeout, path, detail, sizeof(detail));
        } else {
            FlipperFormat* file = flipper_format_file_alloc(storage);
            if(flipper_format_file_open_new(file, path))
                ok = !strcmp(kind, "ir") ? ir_learn(ctx, timeout, file) :
                                           rf_record(ctx, timeout, frequency, file);
            flipper_format_free(file);
        }
        if(!ok) storage_common_remove(storage, path);
    }
    struct json_out out = JSON_OUT_BUF(result, sizeof(result));
    json_printf(
        &out,
        "{kind:%Q,path:%Q,samples:%u,raw_unverified:%B,truncated:%B,card:%s}",
        kind,
        ok ? path : "",
        atomic_load(&ctx->samples),
        !strcmp(kind, "subghz"),
        atomic_load(&ctx->samples) == 4096,
        detail[0] ? detail : "{}");
    furi_string_set(output, result);
    const char* code = ok ? NULL : cancelled(ctx) ? "cancelled" : "no_signal_or_read_failed";
    furi_record_close(RECORD_STORAGE);
    free(ctx);
    return code;
}

static const char* nfc_action(
    DeepFlipperApp* app,
    const char* args,
    uint32_t timeout,
    FuriString* output,
    void* context) {
    if(deepflipper_radio_owned(context)) return "radio_busy_stop_first";
    return capture_action(app, args, timeout, output, "nfc");
}
static const char* ir_action(
    DeepFlipperApp* app,
    const char* args,
    uint32_t timeout,
    FuriString* output,
    void* context) {
    if(deepflipper_radio_owned(context)) return "radio_busy_stop_first";
    return capture_action(app, args, timeout, output, "ir");
}
static const char* rf_action(
    DeepFlipperApp* app,
    const char* args,
    uint32_t timeout,
    FuriString* output,
    void* context) {
    if(deepflipper_radio_owned(context)) return "radio_busy_stop_first";
    return capture_action(app, args, timeout, output, "subghz");
}

#include "deepflipper_subghz.h"
static const char* decode_action(
    DeepFlipperApp* app,
    const char* args,
    uint32_t timeout,
    FuriString* output,
    void* context) {
    if(deepflipper_radio_owned(context)) return "radio_busy_stop_first";
    return deepflipper_subghz_decode(app, args, timeout, output, context);
}
static const DeepFlipperAction actions[] = {
    {"nfc.read",
     "Identify/read supported public NFC data and save .nfc; cached Classic keys only.",
     "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
     nfc_action,
     0},
    {"ir.learn",
     "Learn one IR button and save .ir. RAW carrier assumes 38kHz.",
     "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
     ir_action,
     0},
    {"subghz.record",
     "Record bounded OOK650 RAW to .sub; decoding is a separate action.",
     "{\"type\":\"object\",\"properties\":{\"frequency\":{\"type\":\"integer\",\"minimum\":300000000,\"maximum\":928000000}},\"required\":[\"frequency\"],\"additionalProperties\":false}",
     rf_action,
     0},
    {"subghz.decode",
     "Decode saved OOK650 RAW with firmware decoders, up to 4096 timings. No RF output or full FSK support.",
     "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"maxLength\":192}},\"required\":[\"path\"],\"additionalProperties\":false}",
     decode_action,
     0},
    {"subghz.rx.start",
     "Start OOK650 RAW receive in a retained app session. Stop to save; lease expiry discards the buffer.",
     "{\"type\":\"object\",\"properties\":{\"frequency\":{\"type\":\"integer\"}},\"required\":[\"frequency\"],\"additionalProperties\":false}",
     deepflipper_radio_start,
     DeepFlipperActionRequiresSession},
    {"subghz.rx.state",
     "Read receive state and buffered sample count. Keep the session open to continue recording.",
     "{\"type\":\"object\",\"properties\":{},\"additionalProperties\":false}",
     deepflipper_radio_state,
     DeepFlipperActionRequiresSession},
    {"subghz.rx.stop",
     "Stop receive, release radio and save RAW .sub (save:false discards). Can close the app afterward.",
     "{\"type\":\"object\",\"properties\":{\"save\":{\"type\":\"boolean\",\"default\":true}},\"additionalProperties\":false}",
     deepflipper_radio_stop,
     0},
};
int32_t deepflipper_tools_main(void* args) {
    DeepFlipperRadio* radio = deepflipper_radio_alloc();
    const DeepFlipperAppHooks hooks = {
        .tick = deepflipper_radio_tick, .cleanup = deepflipper_radio_cleanup};
    int32_t result = deepflipper_app_run_ex(
        args, "deepflipper.tools", actions, COUNT_OF(actions), radio, &hooks);
    deepflipper_radio_free(radio);
    return result;
}
