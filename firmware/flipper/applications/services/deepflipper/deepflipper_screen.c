// Assistant screen readback: the text drawn in one complete frame (with highlight
// colour) and, on request, the 128x64 framebuffer. Runs on the RPC worker thread.
#include "deepflipper_screen.h"

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/gui_i.h>
#include <gui/canvas_i.h>
#include <loader/loader.h>
#include <mjs/common/frozen/frozen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_BYTES   1024
#define SCREEN_PAGE    384
#define SCREEN_WAIT_MS 400
#define SCREEN_TAIL    24 // room for `],"next":-1}`

typedef struct {
    uint32_t seq;
    size_t count;
    bool want_bitmap;
    volatile bool bitmap_ready;
    CanvasTextItem items[CANVAS_TEXT_ITEMS];
    uint8_t bitmap[SCREEN_BYTES];
} DeepFlipperShot;

// Only the single assistant RPC session uses this (requests are serialized).
static DeepFlipperShot* shot;
static uint32_t shot_seq;

void deepflipper_screen_reset(void) {
    free(shot);
    shot = NULL;
}

static void screen_framebuffer(
    uint8_t* data,
    size_t size,
    CanvasOrientation orientation,
    void* context) {
    UNUSED(orientation);
    DeepFlipperShot* target = context;
    if(target->bitmap_ready || size < SCREEN_BYTES) return;
    memcpy(target->bitmap, data, SCREEN_BYTES);
    target->bitmap_ready = true;
}

static bool screen_capture(bool bitmap) {
    deepflipper_screen_reset();
    shot = calloc(1, sizeof(DeepFlipperShot));
    shot->seq = ++shot_seq;
    shot->want_bitmap = bitmap;

    Gui* gui = furi_record_open(RECORD_GUI);
    canvas_text_capture_arm();
    if(bitmap) gui_add_framebuffer_callback(gui, screen_framebuffer, shot);
    gui_update(gui);
    bool text = false;
    const uint32_t start = furi_get_tick();
    while(furi_get_tick() - start < furi_ms_to_ticks(SCREEN_WAIT_MS)) {
        if(!text) text = canvas_text_capture_take(shot->items, CANVAS_TEXT_ITEMS, &shot->count);
        if(text && (!bitmap || shot->bitmap_ready)) break;
        furi_delay_ms(5);
    }
    if(!text) {
        // Apps using direct draw never run a full GUI frame; the bitmap still works.
        canvas_text_capture_cancel();
        shot->count = 0;
    }
    // Removal takes the canvas lock, so no callback can still be writing after it.
    if(bitmap) gui_remove_framebuffer_callback(gui, screen_framebuffer, shot);
    furi_record_close(RECORD_GUI);
    return text;
}

// Append items[from..] while they fit; returns the next index or -1 when done.
static int screen_items(struct json_out* out, char* result, size_t capacity, size_t from) {
    char item[128];
    size_t index = from;
    for(; index < shot->count; index++) {
        const CanvasTextItem* it = &shot->items[index];
        struct json_out one = JSON_OUT_BUF(item, sizeof(item));
        int len = json_printf(
            &one, "%s[%d,%d,%d,%Q]", index > from ? "," : "", it->x, it->y, it->color, it->text);
        if(len <= 0 || (size_t)len >= sizeof(item)) continue; // pathological: skip item
        if(strlen(result) + (size_t)len + SCREEN_TAIL >= capacity) break;
        json_printf(out, "%s", item);
    }
    return index < shot->count ? (int)index : -1;
}

void deepflipper_screen_request(
    const char* op,
    const char* json,
    size_t size,
    char* result,
    size_t capacity) {
    struct json_out out = JSON_OUT_BUF(result, capacity);
    result[0] = 0;

    if(strcmp(op, "screen") == 0) {
        int bitmap = 0;
        json_scanf(json, size, "{bitmap:%B}", &bitmap);
        bool text = screen_capture(bitmap);

        char app[32] = "";
        Loader* loader = furi_record_open(RECORD_LOADER);
        FuriString* name = furi_string_alloc();
        if(loader_get_application_name(loader, name))
            strlcpy(app, furi_string_get_cstr(name), sizeof(app));
        furi_string_free(name);
        furi_record_close(RECORD_LOADER);

        json_printf(
            &out,
            "{ok:%B,seq:%u,app:%Q,locked:%B,text:%B,count:%u,bitmap:%d,items:[",
            true,
            (unsigned)shot->seq,
            app,
            furi_hal_rtc_is_flag_set(FuriHalRtcFlagLock),
            text,
            (unsigned)shot->count,
            shot->bitmap_ready ? SCREEN_BYTES : 0);
        int next = screen_items(&out, result, capacity, 0);
        json_printf(&out, "],next:%d}", next);
        if(next < 0 && !shot->bitmap_ready) deepflipper_screen_reset();
        return;
    }

    // screen.page: {seq, text:<index>} or {seq, bitmap:<offset>}
    unsigned seq = 0;
    int from = -1, offset = -1;
    json_scanf(json, size, "{seq:%u,text:%d,bitmap:%d}", &seq, &from, &offset);
    if(!shot || seq != shot->seq) {
        snprintf(result, capacity, "{\"ok\":false,\"code\":\"stale_screen\"}");
        return;
    }
    if(from >= 0 && (size_t)from < shot->count) {
        json_printf(&out, "{ok:%B,seq:%u,items:[", true, (unsigned)shot->seq);
        int next = screen_items(&out, result, capacity, from);
        json_printf(&out, "],next:%d}", next);
        if(next < 0 && !shot->bitmap_ready) deepflipper_screen_reset();
    } else if(
        offset >= 0 && offset < SCREEN_BYTES && offset % SCREEN_PAGE == 0 && shot->bitmap_ready) {
        int len = MIN(SCREEN_PAGE, SCREEN_BYTES - offset);
        json_printf(
            &out,
            "{ok:%B,seq:%u,off:%d,len:%d,data:%V}",
            true,
            (unsigned)shot->seq,
            offset,
            len,
            shot->bitmap + offset,
            len);
        if(offset + len >= SCREEN_BYTES) deepflipper_screen_reset();
    } else {
        snprintf(result, capacity, "{\"ok\":false,\"code\":\"invalid_page\"}");
    }
}
