#include "deepflipper_sprite.h"
#include "whale_data.h"
#include <storage/storage.h>
#include <string.h>
#include <stdio.h>
#include <furi_hal_rtc.h>
#include <locale/locale.h>

static FuriMutex* sprite_mutex;
// Index of the "sing" sequence in df_sequences (scripts/make_character_animations.py STATES).
#define DF_ANIMATION_SING 12
_Static_assert(sizeof(df_sequence_sizes) > DF_ANIMATION_SING, "sing animation missing from whale_data.h");
static uint8_t bitmap[DF_HOME_FRAME_SIZE];
// Only the assistant worker composes frames (room, character, clock, battery,
// bubbles). Keep staging off its small stack; the GUI only copies `bitmap`
// under sprite_mutex and never touches SD.
static uint8_t next[DF_HOME_FRAME_SIZE];
static bool rendered;
static uint32_t frame_tick;
// Set by the desktop on level-up; the worker cheers until this tick.
static volatile uint32_t celebrate_until;
// Read-only pet timeline written by Whale Pet next to its save slots:
// "DFH2", then 8 x {u32 time, u8 wish}, sorted by time. Wish: 0 none,
// 1 sleeping, 2 hungry, 3 sleepy, 4 bored.
static uint8_t pet_hint[44];

// Packed 1-bit glyphs, row-major bit stream, bit 0 of byte 0 = top-left.
// 4x7 digits for the clock/rice-cooker screens: "0"-"9", ":", "%".
static const uint8_t df_digits[12][4] = {
    {0x96,0x99,0x99,0x06},
    {0x32,0x22,0x22,0x07},
    {0x96,0x48,0x12,0x0f},
    {0x87,0x68,0x88,0x07},
    {0x64,0x55,0x4f,0x04},
    {0x1f,0x87,0x98,0x06},
    {0x16,0x71,0x99,0x06},
    {0x8f,0x44,0x22,0x02},
    {0x96,0x69,0x99,0x06},
    {0x96,0xe9,0x88,0x06},
    {0x14,0x00,0x00,0x00},
    {0xbb,0x24,0xd2,0x0d},
};
// 9x9 bubble icons.
typedef enum {
    DfIconMic,
    DfIconDots,
    DfIconWave,
    DfIconMute,
    DfIconQuestion,
    DfIconBang,
    DfIconSleep,
    DfIconRice,
    DfIconBall,
    DfIconLock,
    DfIconStar,
    DfIconNone,
} DfIcon;
static const uint8_t df_icons[DfIconNone][11] = {
    {0x38,0xf8,0xf0,0xe1,0xd3,0x37,0xa7,0x20,0x3e,0x10,0x00},
    {0x00,0x00,0x00,0x00,0xb0,0x6d,0x1b,0x00,0x00,0x00,0x00},
    {0x10,0x28,0x50,0xa9,0x5a,0xb5,0x2a,0x15,0x0a,0x10,0x00},
    {0x08,0x18,0x5c,0x2c,0x55,0xa4,0xd4,0x45,0x06,0x08,0x00},
    {0x3c,0x84,0x00,0x01,0x01,0x01,0x02,0x00,0x08,0x00,0x00},
    {0x10,0x70,0xe0,0xc0,0x01,0x01,0x02,0x00,0x08,0x00,0x00},
    {0xe0,0x01,0x01,0x01,0xff,0x80,0x80,0x80,0x07,0x00,0x00},
    {0x54,0x04,0x05,0xfc,0x1f,0x50,0x10,0x1f,0x14,0x38,0x00},
    {0x7c,0x04,0xe5,0xec,0x1b,0x30,0x60,0x40,0x41,0x7c,0x00},
    {0x38,0x88,0x10,0xf1,0xe7,0xce,0x9d,0x3f,0x00,0x00,0x00},
    {0x10,0x20,0xe0,0xf8,0xef,0x8f,0x0f,0x1b,0x63,0x82,0x00},
};
// Two steam puffs rising from the rice cooker while charging, 9x5 at (107, 11).
static const uint8_t df_steam[2][6] = {{0x02,0x03,0x09,0x24,0x20,0x10}, {0x81,0x04,0x06,0x12,0x48,0x00}};
// Speech bubble with a tail towards her head, 16x14 at (87, 0).
static const uint8_t df_bubble[28] = {0xfe,0x7f,0x01,0x80,0x01,0x80,0x01,0x80,0x01,0x80,0x01,0x80,0x01,0x80,0x01,0x80,0x01,0x80,0x01,0x80,0x01,0x80,0x03,0x80,0xfd,0xff,0x00,0x00};

void deepflipper_sprite_init(void) {
    sprite_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    memcpy(bitmap, df_fallback, sizeof(bitmap));
}

static __attribute__((noinline)) void dot(int x, int y, bool ink) {
    uint8_t* byte = &next[y * 16 + x / 8];
    uint8_t bit = 1U << (x % 8);
    *byte = ink ? *byte | bit : *byte & ~bit;
}

// Opaque glyph from a packed bit stream: ink for 1, paper for 0.
static __attribute__((noinline)) void glyph(int x, int y, int w, int h, const uint8_t* bits) {
    for(int i = 0; i < w * h; i++)
        dot(x + i % w, y + i / w, bits[i / 8] >> (i % 8) & 1);
}

// Centre "12:34" / "86%" on column cx with 4x7 digits and 1 px gaps.
static void digits(int cx, int top, const char* text) {
    int width = -1;
    for(const char* c = text; *c; c++)
        width += (*c == ':' ? 1 : 4) + 1;
    int x = cx - width / 2;
    for(const char* c = text; *c; c++) {
        int w = *c == ':' ? 1 : 4;
        glyph(x, top, w, 7, df_digits[*c == ':' ? 10 : *c == '%' ? 11 : *c - '0']);
        x += w + 1;
    }
}

static void bubble(DfIcon icon) {
    glyph(87, 0, 16, 14, df_bubble);
    dot(86, 12, true);
    dot(85, 13, true);
    glyph(91, 2, 9, 9, df_icons[icon]);
}

static DfIcon ai_icon(const DeepFlipperSnapshot* snapshot) {
    static const uint8_t by_state[] = {
        DfIconQuestion, // offline
        DfIconDots, // configuring
        DfIconDots, // connecting
        DfIconNone, // idle
        DfIconMic, // listening
        DfIconDots, // thinking
        DfIconWave, // speaking
        DfIconDots, // executing
        DfIconBang, // error
    };
    if(!snapshot->enabled) return DfIconNone;
    // Only the UART session is known here, not Internet/server health.
    if(!snapshot->connected) return DfIconQuestion;
    if(strcmp(snapshot->wifi, "sleeping") == 0) return DfIconSleep;
    if(snapshot->state == DeepFlipperIdle && !snapshot->microphone) return DfIconMute;
    if(snapshot->state == DeepFlipperSpeaking && (snapshot->muted || !snapshot->volume))
        return DfIconMute;
    return snapshot->state <= DeepFlipperError ? by_state[snapshot->state] : DfIconBang;
}

static void read_pet(Storage* storage) {
    File* file = storage_file_alloc(storage);
    if(!storage_file_open(
           file, EXT_PATH("deepflipper/pet/home.bin"), FSAM_READ, FSOM_OPEN_EXISTING) ||
       storage_file_read(file, pet_hint, sizeof(pet_hint)) != sizeof(pet_hint))
        pet_hint[0] = 0;
    storage_file_free(file);
}

// The pet's wish for this moment, from the last timeline entry already due.
static uint8_t pet_wish(uint32_t now) {
    uint8_t wish = 0;
    if(memcmp(pet_hint, "DFH2", 4) == 0) {
        for(const uint8_t* entry = pet_hint + 4; entry < pet_hint + sizeof(pet_hint); entry += 5) {
            uint32_t at = entry[0] | entry[1] << 8 | entry[2] << 16 | (uint32_t)entry[3] << 24;
            if(now >= at && entry[4] <= 4) wish = entry[4];
        }
    }
    return wish;
}

void deepflipper_sprite_update(const DeepFlipperSnapshot* snapshot) {
    if(snapshot->foreground) return;
    uint32_t tick = furi_get_tick() / furi_ms_to_ticks(120);
    if(tick == frame_tick) return;
    // The pet timeline changes rarely; refresh it about every five seconds.
    bool refresh = !frame_tick || tick % 40 == 0;
    frame_tick = tick;
    DateTime now;
    furi_hal_rtc_get_datetime(&now);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(refresh) read_pet(storage);
    const DfIcon ai = ai_icon(snapshot);
    const uint8_t wish = pet_wish(datetime_datetime_to_timestamp(&now));
    uint8_t animation = snapshot->emotion < sizeof(df_sequence_sizes) ? snapshot->emotion : 0;
    // Emotion 12 is singing (and practising a song): it replaces the talking mouth while a
    // song plays, and stays on while she practises with nothing else going on.
    const bool singing = animation == DF_ANIMATION_SING &&
                         (snapshot->state == DeepFlipperIdle || snapshot->state == DeepFlipperSpeaking);
    switch(singing ? DeepFlipperIdle : snapshot->state) {
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
        animation = snapshot->muted || snapshot->volume == 0 ? 0 : 3;
        break;
    default:
        break;
    }
    // The pet sleeps on the home screen too, unless the assistant is busy.
    const bool cheering = (int32_t)(celebrate_until - furi_get_tick()) > 0;
    if(!singing && ai == DfIconNone && wish == 1) animation = 10;
    if(!singing && ai == DfIconNone && cheering) animation = 4; // happy
    uint8_t frame = df_sequences[animation][tick % df_sequence_sizes[animation]];
    memcpy(next, df_fallback, sizeof(next));
    const uint32_t bank = now.hour >= 20 || now.hour < 7;
    File* file = storage_file_alloc(storage);
    if(storage_file_open(
           file, EXT_PATH("deepflipper/home_frames.bin"), FSAM_READ, FSOM_OPEN_EXISTING)) {
        if(storage_file_size(file) == DF_FRAME_COUNT * DF_HOME_FRAME_SIZE * DF_HOME_BANKS &&
           storage_file_seek(file, (bank * DF_FRAME_COUNT + frame) * DF_HOME_FRAME_SIZE, true)) {
            if(storage_file_read(file, next, sizeof(next)) != sizeof(next))
                memcpy(next, df_fallback, sizeof(next));
        }
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    if(!singing && snapshot->state == DeepFlipperSpeaking && !snapshot->muted &&
       snapshot->volume > 0) {
        uint8_t mouth = snapshot->mouth <= 2 ? snapshot->mouth : 0;
        for(size_t y = 0; y < 4; y++) {
            for(size_t x = 0; x < 5; x++) {
                dot(DF_HOME_CHARACTER_X + 25 + x, 33 + y, df_mouth[mouth][y] & (1U << x));
            }
        }
    }
    // Time on the alarm clock, battery on the rice cooker's screen.
    uint8_t hour = now.hour;
    if(locale_get_time_format() == LocaleTimeFormat12h) {
        hour %= 12;
        if(!hour) hour = 12;
    }
    char text[8];
    snprintf(text, sizeof(text), "%02u:%02u", hour, now.minute);
    digits(16, 26, text);
    snprintf(text, sizeof(text), "%u%%", snapshot->battery > 100 ? 100 : snapshot->battery);
    digits(113, 29, text);
    if(snapshot->charging) glyph(107, 11, 9, 5, df_steam[tick / 4 % 2]);
    // Lock first, then the assistant; otherwise the pet's own wish blinks gently.
    static const uint8_t wish_icon[5] = {DfIconNone, DfIconSleep, DfIconRice, DfIconSleep, DfIconBall};
    DfIcon icon = furi_hal_rtc_is_flag_set(FuriHalRtcFlagLock) ? DfIconLock :
                  ai != DfIconNone                             ? ai :
                  cheering                                     ? DfIconStar :
                  tick % 60 < 40                               ? wish_icon[wish] :
                                                                 DfIconNone;
    if(icon != DfIconNone) bubble(icon);
    furi_mutex_acquire(sprite_mutex, FuriWaitForever);
    memcpy(bitmap, next, sizeof(bitmap));
    furi_mutex_release(sprite_mutex);
}

void deepflipper_celebrate(void) {
    celebrate_until = furi_get_tick() + furi_ms_to_ticks(4000);
}

void deepflipper_render_reset(void) {
    rendered = false;
}

bool deepflipper_rendered(void) {
    return rendered;
}

void deepflipper_draw(Canvas* canvas) {
    canvas_clear(canvas);
    furi_mutex_acquire(sprite_mutex, FuriWaitForever);
    // Avoid a second full-screen buffer on the GUI's 2 KiB stack.
    canvas_draw_xbm(canvas, 0, 0, 128, 64, bitmap);
    furi_mutex_release(sprite_mutex);
    rendered = true;
}
