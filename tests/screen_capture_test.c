// Host test for the assistant screen readback (deepflipper_screen.c) with the real
// frozen JSON library. GUI/Loader/canvas boundaries are stubbed below.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Frozen assumes Windows without _MSC_VER uses an old MSVC without stdbool.
#if defined(_WIN32) && !defined(_MSC_VER)
#define _MSC_VER 1700
#include <mjs/common/frozen/frozen.h>
#undef _MSC_VER
#else
#include <mjs/common/frozen/frozen.h>
#endif

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui_i.h>
#include <gui/canvas_i.h>
#include <loader/loader.h>

// ---- canvas text capture stub: hands out a scripted frame -------------------------
static CanvasTextItem scripted[CANVAS_TEXT_ITEMS];
static size_t scripted_count;
static bool armed, frame_drawn, direct_draw;
void canvas_text_capture_arm(void) {
    armed = true;
    frame_drawn = false;
}
void canvas_text_capture_cancel(void) {
    armed = false;
}
bool canvas_text_capture_take(CanvasTextItem* items, size_t capacity, size_t* count) {
    if(!armed || !frame_drawn) return false;
    size_t n = scripted_count < capacity ? scripted_count : capacity;
    memcpy(items, scripted, n * sizeof(*items));
    *count = n;
    armed = false;
    return true;
}

// ---- gui / loader / furi stubs -----------------------------------------------------
static GuiCanvasCommitCallback fb_callback;
static void* fb_context;
static uint8_t framebuffer[1024];
static int callbacks_added, callbacks_removed;
static Gui gui_instance;
void gui_add_framebuffer_callback(Gui* gui, GuiCanvasCommitCallback callback, void* context) {
    (void)gui;
    fb_callback = callback;
    fb_context = context;
    callbacks_added++;
}
void gui_remove_framebuffer_callback(Gui* gui, GuiCanvasCommitCallback callback, void* context) {
    (void)gui;
    assert(callback == fb_callback && context == fb_context);
    fb_callback = NULL;
    callbacks_removed++;
}
void gui_update(Gui* gui) {
    (void)gui;
    if(!direct_draw) frame_drawn = true;
    if(fb_callback) fb_callback(framebuffer, sizeof(framebuffer), 0, fb_context);
}
static const char* app_name = "Sub-GHz";
bool loader_get_application_name(Loader* loader, FuriString* name) {
    (void)loader;
    if(!app_name[0]) return false;
    snprintf(name->text, sizeof(name->text), "%s", app_name);
    return true;
}
static uint32_t ticks;
uint32_t furi_get_tick(void) {
    return ticks += 5;
}
void furi_delay_ms(uint32_t ms) {
    (void)ms;
}
void* furi_record_open(const char* name) {
    return strcmp(name, RECORD_GUI) == 0 ? (void*)&gui_instance : (void*)1;
}
void furi_record_close(const char* name) {
    (void)name;
}
FuriString* furi_string_alloc(void) {
    return calloc(1, sizeof(FuriString));
}
void furi_string_free(FuriString* s) {
    free(s);
}
const char* furi_string_get_cstr(const FuriString* s) {
    return s->text;
}
bool furi_hal_rtc_is_flag_set(int flag) {
    (void)flag;
    return false;
}

#include "../firmware/flipper/applications/services/deepflipper/deepflipper_screen.c"

static void add(int x, int y, int color, const char* text) {
    CanvasTextItem* it = &scripted[scripted_count++];
    it->x = x;
    it->y = y;
    it->color = color;
    snprintf(it->text, sizeof(it->text), "%s", text);
}

static char out[768];
static void call(const char* op, const char* json) {
    memset(out, 'X', sizeof(out));
    deepflipper_screen_request(op, json, strlen(json), out, sizeof(out));
    assert(strlen(out) < 767);
    assert(out[strlen(out) - 1] == '}');
}

int main(void) {
    // 1) A Sub-GHz menu with a highlighted first item, plus a bitmap.
    add(4, 11, 0, "Read");
    add(4, 23, 1, "Read RAW");
    add(4, 35, 1, "Saved \"x\"");
    for(size_t i = 0; i < 20; i++)
        add(4, 47, 1, "filler item 0123456789012345");
    for(size_t i = 0; i < sizeof(framebuffer); i++)
        framebuffer[i] = (uint8_t)i;

    call("screen", "{\"v\":1,\"op\":\"screen\",\"bitmap\":true}");
    unsigned seq = 0, count = 0;
    int bitmap = 0, next = 0, text = 0;
    char* app = NULL;
    assert(
        json_scanf(
            out,
            strlen(out),
            "{seq:%u,app:%Q,text:%B,count:%u,bitmap:%d,next:%d}",
            &seq,
            &app,
            &text,
            &count,
            &bitmap,
            &next) == 6);
    assert(strcmp(app, "Sub-GHz") == 0 && text && count == 23 && bitmap == 1024);
    assert(strstr(out, "[4,11,0,\"Read\"]") && strstr(out, "\"Saved \\\"x\\\"\""));
    assert(next > 3 && next < 23); // paged: not everything fits in 767 bytes
    free(app);

    // 2) Remaining text pages until done.
    size_t seen = (size_t)next;
    char request[96];
    while(next >= 0) {
        snprintf(request, sizeof(request), "{\"seq\":%u,\"text\":%d}", seq, next);
        call("screen.page", request);
        int prev = next;
        next = -2;
        assert(json_scanf(out, strlen(out), "{next:%d}", &next) == 1);
        assert(next == -1 || next > prev);
        seen = next < 0 ? 23 : (size_t)next;
    }
    assert(seen == 23);

    // 3) Bitmap pages round-trip through base64.
    uint8_t copy[1024];
    for(int offset = 0; offset < 1024; offset += 384) {
        snprintf(request, sizeof(request), "{\"seq\":%u,\"bitmap\":%d}", seq, offset);
        call("screen.page", request);
        char* data = NULL;
        int len = 0, off = -1;
        assert(json_scanf(out, strlen(out), "{off:%d,len:%d,data:%V}", &off, &len, &data, &len) >= 2);
        assert(off == offset);
        memcpy(copy + offset, data, len);
        free(data);
    }
    assert(memcmp(copy, framebuffer, sizeof(copy)) == 0);
    assert(callbacks_added == 1 && callbacks_removed == 1);
    // The capture is released after the last page.
    call("screen.page", request);
    assert(strstr(out, "stale_screen"));

    // 4) Desktop, text only, no bitmap: everything fits, capture released at once.
    scripted_count = 0;
    app_name = "";
    add(30, 60, 1, "Back");
    call("screen", "{\"v\":1,\"op\":\"screen\"}");
    assert(strstr(out, "\"app\":\"\"") && strstr(out, "\"bitmap\":0") && strstr(out, "\"next\":-1"));
    assert(shot == NULL);

    // 5) Direct-draw app: no GUI frame, but the bitmap still arrives.
    direct_draw = true;
    call("screen", "{\"v\":1,\"op\":\"screen\",\"bitmap\":true}");
    assert(strstr(out, "\"text\":false") && strstr(out, "\"count\":0") && strstr(out, "\"bitmap\":1024"));
    deepflipper_screen_reset();

    puts("screen_capture_test: ok");
    return 0;
}
