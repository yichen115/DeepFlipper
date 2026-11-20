#include "deepflipper_subghz.h"
#include <storage/storage.h>
#include <flipper_format/flipper_format.h>
#include <subghz/receiver.h>
#include <subghz/subghz_protocol_registry.h>
#include <mjs/common/frozen/frozen.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    FuriString* summary;
    FuriString* protocol;
    unsigned frames;
} DecodeContext;

static void decoded(SubGhzReceiver* receiver, SubGhzProtocolDecoderBase* decoder, void* context) {
    UNUSED(receiver);
    DecodeContext* capture = context;
    ++capture->frames;
    // Return the first recognized protocol; its frame count includes repeats.
    if(capture->frames != 1) return;
    furi_string_set(capture->protocol, decoder->protocol->name);
    subghz_protocol_decoder_base_get_string(decoder, capture->summary);
}

static bool raw_path(const char* path) {
    if(!path || strlen(path) > 192 || strncmp(path, "/ext/subghz/", 12) || strlen(path) < 16 ||
       strcmp(path + strlen(path) - 4, ".sub"))
        return false;
    for(const char* p = path; *p; ++p) {
        if((unsigned char)*p < 32 || *p == 127 || *p == '\\') return false;
        if(*p == '/' && (p[1] == '.' && (p[2] == '/' || p[2] == 0 ||
                                         (p[2] == '.' && (p[3] == '/' || p[3] == 0)))))
            return false;
    }
    return true;
}

const char* deepflipper_subghz_decode(
    DeepFlipperApp* app,
    const char* arguments,
    uint32_t timeout,
    FuriString* output,
    void* context) {
    UNUSED(timeout);
    UNUSED(context);
    struct json_token token = JSON_INVALID_TOKEN;
    json_scanf(arguments, strlen(arguments), "{path:%T}", &token);
    char* path = NULL;
    if(token.type == JSON_TYPE_STRING && token.len > 0 && token.len <= 192) {
        path = malloc(token.len + 1);
        int length = json_unescape(token.ptr, token.len, path, token.len);
        if(length < 0 || memchr(path, 0, length)) {
            free(path);
            path = NULL;
        } else
            path[length] = 0;
    }
    if(!raw_path(path)) {
        free(path);
        return "invalid_path";
    }
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FileInfo info;
    if(storage_common_stat(storage, path, &info) != FSE_OK || info.size > 65536) {
        furi_record_close(RECORD_STORAGE);
        free(path);
        return "file_missing_or_too_large";
    }
    FlipperFormat* file = flipper_format_file_alloc(storage);
    FuriString* text = furi_string_alloc();
    const char* code = "invalid_raw_file";
    uint32_t version = 0, frequency = 0;
    if(flipper_format_file_open_existing(file, path) &&
       flipper_format_read_header(file, text, &version) && version == 1 &&
       furi_string_equal(text, "Flipper SubGhz RAW File") &&
       flipper_format_read_uint32(file, "Frequency", &frequency, 1) &&
       flipper_format_read_string(file, "Preset", text) &&
       furi_string_equal(text, "FuriHalSubGhzPresetOok650Async") &&
       flipper_format_read_string(file, "Protocol", text) && furi_string_equal(text, "RAW")) {
        DecodeContext capture = {.summary = furi_string_alloc(), .protocol = furi_string_alloc()};
        unsigned samples = 0;
        uint32_t count = 0;
        int32_t* timings = NULL;
        code = NULL;
        if(memmgr_get_free_heap() < 32768 || memmgr_heap_get_max_free_block() < 16384)
            code = "insufficient_memory";
        else
            timings = malloc(4096 * sizeof(int32_t));
        bool more = false;
        bool truncated = false;
        while(!code && (more = flipper_format_get_value_count(file, "RAW_Data", &count))) {
            if(deepflipper_app_cancelled(app)) {
                code = "cancelled";
                break;
            }
            if(samples == 4096) {
                truncated = true;
                break;
            }
            if(!count || count > 4096) {
                code = "invalid_raw_data";
                break;
            }
            uint32_t take = MIN(count, 4096 - samples);
            if(!flipper_format_read_int32(file, "RAW_Data", timings + samples, take)) {
                code = "invalid_raw_data";
                break;
            }
            for(uint32_t i = 0; i < take; ++i) {
                if((i % 64) == 0 && deepflipper_app_cancelled(app)) {
                    code = "cancelled";
                    break;
                }
                const int32_t timing = timings[samples + i];
                if(!timing || timing > 1000000 || timing < -1000000) {
                    code = "invalid_raw_data";
                    break;
                }
            }
            samples += take;
            if(code) break;
            if(take < count) {
                truncated = true;
                break;
            }
        }
        if(!samples && !code) code = "no_raw_samples";
        SubGhzEnvironment* environment = NULL;
        if(!code) environment = subghz_environment_alloc();
        // The firmware receiver allocates every protocol in its registry, even
        // filtered-out RAW decoders. Instantiate one supported decoder at a time.
        for(size_t index = 0; !code && !capture.frames &&
                              index < subghz_protocol_registry_count(&subghz_protocol_registry);
            ++index) {
            if(deepflipper_app_cancelled(app)) {
                code = "cancelled";
                break;
            }
            const SubGhzProtocol* protocol =
                subghz_protocol_registry_get_by_index(&subghz_protocol_registry, index);
            if(!(protocol->flag & SubGhzProtocolFlag_Decodable) || !protocol->decoder ||
               !protocol->decoder->alloc)
                continue;
            if(memmgr_get_free_heap() < 12288 || memmgr_heap_get_max_free_block() < 8192) {
                code = "insufficient_memory";
                break;
            }
            const SubGhzProtocolRegistry single = {.items = &protocol, .size = 1};
            subghz_environment_set_protocol_registry(environment, &single);
            SubGhzReceiver* receiver = subghz_receiver_alloc_init(environment);
            subghz_receiver_set_filter(receiver, SubGhzProtocolFlag_Decodable);
            subghz_receiver_set_rx_callback(receiver, decoded, &capture);
            for(unsigned i = 0; i < samples; ++i) {
                if((i % 64) == 0 && deepflipper_app_cancelled(app)) {
                    code = "cancelled";
                    break;
                }
                int32_t timing = timings[i];
                subghz_receiver_decode(receiver, timing > 0, timing > 0 ? timing : -timing);
            }
            subghz_receiver_free(receiver);
        }
        free(timings);
        char result[1536];
        struct json_out out = JSON_OUT_BUF(result, sizeof(result));
        // No recognized frame is a valid analysis result, never a claim of decoding success.
        int length = json_printf(
            &out,
            "{path:%Q,frequency:%u,samples:%u,frames:%u,decoded:%B,protocol:%Q,summary:%Q,truncated:%B,scope:%Q}",
            path,
            frequency,
            samples,
            capture.frames,
            capture.frames > 0,
            furi_string_get_cstr(capture.protocol),
            furi_string_get_cstr(capture.summary),
            truncated,
            "OOK650_RAW_no_keystore");
        if(length < 0 || length >= (int)sizeof(result))
            code = "result_too_large";
        else
            furi_string_set(output, result);
        if(environment) subghz_environment_free(environment);
        furi_string_free(capture.summary);
        furi_string_free(capture.protocol);
    }
    furi_string_free(text);
    flipper_format_free(file);
    furi_record_close(RECORD_STORAGE);
    free(path);
    return code;
}
