#include "deepflipper_sprite.h"
#include "whale_data.h"
#include <storage/storage.h>
#include <string.h>
#include <stdio.h>
#include <assets_icons.h>
#include <furi_hal_rtc.h>
#include <locale/locale.h>

static FuriMutex* sprite_mutex;
static uint8_t bitmap[512];
static bool rendered;
static uint32_t frame_tick;

void deepflipper_sprite_init(void) {
    sprite_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    memcpy(bitmap, df_fallback, sizeof(bitmap));
}

void deepflipper_sprite_update(const DeepFlipperSnapshot* snapshot) {
    if(snapshot->foreground) return;
    uint32_t tick = furi_get_tick() / furi_ms_to_ticks(120);
    if(tick == frame_tick) return;
    frame_tick = tick;
    uint8_t animation = snapshot->emotion < 12 ? snapshot->emotion : 0;
    switch(snapshot->state) {
    case DeepFlipperOffline:
        animation = 0;
        break;
    case DeepFlipperListening:
        animation = 1;
        break;
    case DeepFlipperThinking:
    case DeepFlipperExecuting:
        animation = 2;
        break;
    case DeepFlipperSpeaking:
        animation = 3;
        break;
    default:
        break;
    }
    uint8_t frame = df_sequences[animation][tick % df_sequence_sizes[animation]];
    uint8_t next[512];
    memcpy(next, df_fallback, sizeof(next));
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(
           file, EXT_PATH("deepflipper/whale_frames.bin"), FSAM_READ, FSOM_OPEN_EXISTING)) {
        if(storage_file_size(file) == DF_FRAME_COUNT * 512 &&
           storage_file_seek(file, frame * 512, true)) {
            if(storage_file_read(file, next, sizeof(next)) != sizeof(next))
                memcpy(next, df_fallback, sizeof(next));
        }
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    if(snapshot->state == DeepFlipperSpeaking) {
        uint8_t mouth = snapshot->mouth <= 2 ? snapshot->mouth : 0;
        for(size_t y = 0; y < 4; y++) {
            for(size_t x = 0; x < 5; x++) {
                size_t position = (33 + y) * 8 + (25 + x) / 8;
                uint8_t bit = 1U << ((25 + x) % 8);
                next[position] &= ~bit;
                if(df_mouth[mouth][y] & (1U << x)) next[position] |= bit;
            }
        }
    }
    furi_mutex_acquire(sprite_mutex, FuriWaitForever);
    memcpy(bitmap, next, sizeof(bitmap));
    furi_mutex_release(sprite_mutex);
}

void deepflipper_render_reset(void) {
    rendered = false;
}

bool deepflipper_rendered(void) {
    return rendered;
}

static void draw_status_row(Canvas* canvas, int32_t y, const char* label, const char* value) {
    canvas_draw_str(canvas, 70, y, label);
    canvas_draw_str_aligned(canvas, 126, y, AlignRight, AlignBottom, value);
}

static void status_text(
    const DeepFlipperSnapshot* snapshot, const char** title, const char** hint) {
    if(!snapshot->enabled) {
        *title = "AI off";
        *hint = "Local mode";
    } else if(!snapshot->connected) {
        // Only the UART session is known here, not physical power or Internet health.
        *title = "No link";
        *hint = "Check ESP32";
    } else {
        if(strcmp(snapshot->wifi,"sleeping")==0){*title="Standby";*hint="OK: wake";return;}
        static const char* const titles[] = {
            "Offline", "Setup", "Connecting", "Ready", "Listening",
            "Thinking", "Speaking", "Working", "Error"};
        static const char* const hints[] = {
            "AI settings", "AI settings", "Please wait", "OK: talk", "OK: send",
            "Please wait", "Voice reply", "Hold a card", "AI settings"};
        const uint8_t state = snapshot->state <= DeepFlipperError ? snapshot->state : DeepFlipperError;
        *title = titles[state];
        *hint = hints[state];
        if(snapshot->state == DeepFlipperIdle && !snapshot->microphone) {
            *title = "Mic off";
            *hint = "AI settings";
        } else if(
            snapshot->state == DeepFlipperSpeaking &&
            (snapshot->muted || snapshot->volume == 0)) {
            *title = "Reply";
            *hint = "Audio muted";
        }
    }
}

void deepflipper_draw(Canvas* canvas) {
    uint8_t frame[512];
    DeepFlipperSnapshot snapshot;
    deepflipper_snapshot(&snapshot);
    furi_mutex_acquire(sprite_mutex, FuriWaitForever);
    memcpy(frame, bitmap, sizeof(frame));
    furi_mutex_release(sprite_mutex);
    canvas_clear(canvas);
    canvas_draw_xbm(canvas, 0, 0, 64, 64, frame);

    // All information stays in the right half, leaving the native character unobscured.
    DateTime now;
    furi_hal_rtc_get_datetime(&now);
    uint8_t hour = now.hour;
    if(locale_get_time_format() == LocaleTimeFormat12h) {
        hour %= 12;
        if(!hour) hour = 12;
    }
    char text[16];
    snprintf(text, sizeof(text), "%02u:%02u", hour, now.minute);
    canvas_set_font(canvas, FontBatteryPercent);
    canvas_draw_str(canvas, 70, 9, text);

    const int32_t battery_x = 100;
    canvas_draw_icon(canvas, battery_x, 2, &I_Battery_26x8);
    snprintf(text, sizeof(text), "%u", snapshot.battery);
    canvas_draw_str_aligned(canvas, battery_x + 11, 6, AlignCenter, AlignCenter, text);
    canvas_draw_line(canvas, 70, 13, 125, 13);
    if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagLock)) {
        canvas_draw_icon(canvas, 90, 55, &I_Lock_7x8);
    }

    const char *title, *hint;
    status_text(&snapshot, &title, &hint);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 70, 27, title);
    canvas_set_font(canvas, FontBatteryPercent);
    canvas_draw_str(canvas, 70, 38, hint);
    canvas_draw_line(canvas, 70, 43, 125, 43);

    const bool audio_known = snapshot.enabled && snapshot.connected;
    if(audio_known) {
        if(snapshot.muted || snapshot.volume == 0) {
            snprintf(text, sizeof(text), "Muted");
        } else {
            snprintf(text, sizeof(text), "%u%%", snapshot.volume);
        }
    } else {
        snprintf(text, sizeof(text), "--");
    }
    draw_status_row(canvas, 53, "VOL", text);
    draw_status_row(
        canvas, 63, "MIC",
        audio_known ? (snapshot.microphone ? "On" : "Off") : "--");
    rendered = true;
}
