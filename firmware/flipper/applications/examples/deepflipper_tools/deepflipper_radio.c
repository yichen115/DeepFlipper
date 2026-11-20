#include "deepflipper_radio.h"
#include <furi_hal.h>
#include <subghz/devices/devices.h>
#include <storage/storage.h>
#include <flipper_format/flipper_format.h>
#include <mjs/common/frozen/frozen.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct DeepFlipperRadio {
    const SubGhzDevice* device;
    atomic_uint samples;
    atomic_bool gate;
    int32_t* timings;
    uint32_t frequency;
    bool running, full;
    char request_id[31];
};

DeepFlipperRadio* deepflipper_radio_alloc(void) {
    return calloc(1, sizeof(DeepFlipperRadio));
}

static void rx(bool level, uint32_t duration, void* context) {
    DeepFlipperRadio* radio = context;
    if(!atomic_load(&radio->gate) || !duration || duration > 1000000) return;
    unsigned count = atomic_load(&radio->samples);
    if(count < 4096) {
        radio->timings[count] = level ? (int32_t)duration : -(int32_t)duration;
        atomic_store(&radio->samples, count + 1);
    }
}

static void stop_radio(DeepFlipperRadio* radio) {
    if(!radio->device) return;
    if(radio->running) subghz_devices_stop_async_rx(radio->device);
    radio->running = false;
    // stop_async_rx joins the producer before its timing buffer is freed or read.
    subghz_devices_sleep(radio->device);
    subghz_devices_end(radio->device);
    subghz_devices_deinit();
    radio->device = NULL;
}

void deepflipper_radio_cleanup(void* context) {
    DeepFlipperRadio* radio = context;
    stop_radio(radio);
    free(radio->timings);
    radio->timings = NULL;
    atomic_store(&radio->samples, 0);
    atomic_store(&radio->gate, false);
}

void deepflipper_radio_free(DeepFlipperRadio* radio) {
    deepflipper_radio_cleanup(radio);
    free(radio);
}

bool deepflipper_radio_owned(const DeepFlipperRadio* radio) {
    return radio && radio->timings;
}

void deepflipper_radio_tick(DeepFlipperApp* app, void* context) {
    DeepFlipperRadio* radio = context;
    if(!radio->running) return;
    if(deepflipper_app_cancelled(app)) {
        deepflipper_radio_cleanup(radio);
        return;
    }
    if(subghz_devices_get_rssi(radio->device) > -75) atomic_store(&radio->gate, true);
    if(atomic_load(&radio->samples) == 4096) {
        radio->full = true;
        stop_radio(radio);
    }
}

const char* deepflipper_radio_state(
    DeepFlipperApp* app,
    const char* arguments,
    uint32_t timeout,
    FuriString* output,
    void* context) {
    UNUSED(app);
    UNUSED(arguments);
    UNUSED(timeout);
    DeepFlipperRadio* radio = context;
    char result[256];
    struct json_out out = JSON_OUT_BUF(result, sizeof(result));
    json_printf(
        &out,
        "{running:%B,buffered:%B,samples:%u,frequency:%u,buffer_full:%B,raw_unverified:true}",
        radio->running,
        radio->timings != NULL,
        atomic_load(&radio->samples),
        radio->frequency,
        radio->full);
    furi_string_set(output, result);
    return NULL;
}

const char* deepflipper_radio_start(
    DeepFlipperApp* app,
    const char* arguments,
    uint32_t timeout,
    FuriString* output,
    void* context) {
    UNUSED(timeout);
    DeepFlipperRadio* radio = context;
    if(!deepflipper_app_retained(app)) return "session_required";
    if(deepflipper_radio_owned(radio)) return "radio_already_owned";
    if(deepflipper_app_cancelled(app)) return "cancelled";
    struct json_token frequency = JSON_INVALID_TOKEN;
    json_scanf(arguments, strlen(arguments), "{frequency:%T}", &frequency);
    if(frequency.type != JSON_TYPE_NUMBER || frequency.len < 1 || frequency.len > 9)
        return "invalid_frequency";
    uint32_t hz = 0;
    for(int i = 0; i < frequency.len; ++i) {
        if(frequency.ptr[i] < '0' || frequency.ptr[i] > '9') return "invalid_frequency";
        hz = hz * 10 + frequency.ptr[i] - '0';
    }
    // Furi's allocator is fatal on exhaustion; leave room for the RPC result and
    // Storage worker before committing to a long-lived capture buffer.
    if(memmgr_get_free_heap() < 24576 || memmgr_heap_get_max_free_block() < 16384)
        return "insufficient_memory";
    subghz_devices_init();
    const SubGhzDevice* device = subghz_devices_get_by_name("cc1101_int");
    if(!device || !subghz_devices_is_frequency_valid(device, hz) ||
       !subghz_devices_begin(device)) {
        subghz_devices_deinit();
        return "radio_unavailable_or_invalid_frequency";
    }
    radio->device = device;
    radio->timings = malloc(4096 * sizeof(int32_t));
    radio->frequency = hz;
    radio->full = false;
    atomic_store(&radio->samples, 0);
    atomic_store(&radio->gate, false);
    strcpy(radio->request_id, deepflipper_app_request_id(app));
    subghz_devices_reset(device);
    subghz_devices_load_preset(device, FuriHalSubGhzPresetOok650Async, NULL);
    radio->frequency = subghz_devices_set_frequency(device, hz);
    radio->running = true;
    subghz_devices_start_async_rx(device, rx, radio);
    return deepflipper_radio_state(app, arguments, timeout, output, context);
}

const char* deepflipper_radio_stop(
    DeepFlipperApp* app,
    const char* arguments,
    uint32_t timeout,
    FuriString* output,
    void* context) {
    UNUSED(app);
    UNUSED(timeout);
    DeepFlipperRadio* radio = context;
    stop_radio(radio);
    bool save = true;
    struct json_token value = JSON_INVALID_TOKEN;
    json_scanf(arguments, strlen(arguments), "{save:%T}", &value);
    if(value.type != JSON_TYPE_INVALID && value.type != JSON_TYPE_TRUE &&
       value.type != JSON_TYPE_FALSE) {
        deepflipper_radio_cleanup(radio);
        return "invalid_save";
    }
    if(value.type == JSON_TYPE_FALSE) save = false;
    unsigned count = atomic_load(&radio->samples);
    const char* code = NULL;
    char path[96] = {0};
    if(save && radio->timings && count >= 8) {
        snprintf(path, sizeof(path), "/ext/subghz/deepflipper_%s.sub", radio->request_id);
        Storage* storage = furi_record_open(RECORD_STORAGE);
        storage_common_mkdir(storage, EXT_PATH("subghz"));
        FlipperFormat* file = flipper_format_file_alloc(storage);
        bool ok = flipper_format_file_open_new(file, path);
        bool created = ok;
        ok = ok && flipper_format_write_header_cstr(file, "Flipper SubGhz RAW File", 1) &&
             flipper_format_write_uint32(file, "Frequency", &radio->frequency, 1) &&
             flipper_format_write_string_cstr(file, "Preset", "FuriHalSubGhzPresetOok650Async") &&
             flipper_format_write_string_cstr(file, "Protocol", "RAW");
        for(unsigned i = 0; ok && i < count; i += 128)
            ok = flipper_format_write_int32(
                file, "RAW_Data", radio->timings + i, MIN(128U, count - i));
        flipper_format_free(file);
        if(!ok) {
            if(created) storage_common_remove(storage, path);
            path[0] = 0;
            code = "save_failed";
        }
        furi_record_close(RECORD_STORAGE);
    } else if(save)
        code = "no_signal";
    char result[256];
    struct json_out out = JSON_OUT_BUF(result, sizeof(result));
    json_printf(
        &out,
        "{running:false,path:%Q,samples:%u,truncated:%B,raw_unverified:true}",
        path,
        count,
        radio->full || count == 4096);
    furi_string_set(output, result);
    deepflipper_radio_cleanup(radio);
    return code;
}
