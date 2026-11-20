/* Rasterize the production Whale Pet drawing code with real fonts and art. */
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pet_fonts.h"

#define UNUSED(x)       (void)(x)
#define FuriWaitForever 0
typedef struct {
    int unused;
} FuriMutex;
typedef struct {
    int unused;
} FuriMessageQueue;
typedef enum { ColorWhite, ColorBlack } Color;
typedef enum { FontPrimary, FontSecondary } Font;
typedef enum { AlignLeft, AlignRight, AlignTop, AlignBottom, AlignCenter } Align;
typedef struct {
    uint8_t px[64][128];
    Color color;
    Font font;
    bool transparent;
    unsigned clipped_text;
} Canvas;
static void furi_mutex_acquire(FuriMutex* m, int t) {
    (void)m;
    (void)t;
}
static void furi_mutex_release(FuriMutex* m) {
    (void)m;
}
static void put(Canvas* c, int x, int y, bool ink) {
    if(x >= 0 && x < 128 && y >= 0 && y < 64) c->px[y][x] = ink;
}
static void canvas_clear(Canvas* c) {
    memset(c->px, 0, sizeof(c->px));
}
static void canvas_set_color(Canvas* c, Color color) {
    c->color = color;
}
static void canvas_set_font(Canvas* c, Font font) {
    c->font = font;
}
static void canvas_set_bitmap_mode(Canvas* c, bool alpha) {
    c->transparent = alpha;
}
static void canvas_draw_dot(Canvas* c, int x, int y) {
    put(c, x, y, c->color == ColorBlack);
}
static void canvas_draw_box(Canvas* c, int x, int y, size_t w, size_t h) {
    for(size_t j = 0; j < h; ++j)
        for(size_t i = 0; i < w; ++i)
            canvas_draw_dot(c, x + i, y + j);
}
static void canvas_draw_line(Canvas* c, int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    for(int err = dx + dy;;) {
        canvas_draw_dot(c, x0, y0);
        if(x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if(e2 >= dy) err += dy, x0 += sx;
        if(e2 <= dx) err += dx, y0 += sy;
    }
}
static void canvas_draw_frame(Canvas* c, int x, int y, size_t w, size_t h) {
    canvas_draw_line(c, x, y, x + w - 1, y);
    canvas_draw_line(c, x, y + h - 1, x + w - 1, y + h - 1);
    canvas_draw_line(c, x, y, x, y + h - 1);
    canvas_draw_line(c, x + w - 1, y, x + w - 1, y + h - 1);
}
static bool in_round(int i, int j, int w, int h, int r) {
    int cx = i < r ? r : i >= w - r ? w - 1 - r : i, cy = j < r ? r : j >= h - r ? h - 1 - r : j;
    int dx = i - cx, dy = j - cy;
    return dx * dx + dy * dy <= r * r + 1;
}
static void canvas_draw_rbox(Canvas* c, int x, int y, size_t w, size_t h, size_t r) {
    for(int j = 0; j < (int)h; ++j)
        for(int i = 0; i < (int)w; ++i)
            if(in_round(i, j, w, h, r)) canvas_draw_dot(c, x + i, y + j);
}
static void canvas_draw_rframe(Canvas* c, int x, int y, size_t w, size_t h, size_t r) {
    for(int j = 0; j < (int)h; ++j)
        for(int i = 0; i < (int)w; ++i)
            if(in_round(i, j, w, h, r) &&
               (!in_round(i - 1, j, w, h, r) || !in_round(i + 1, j, w, h, r) ||
                !in_round(i, j - 1, w, h, r) || !in_round(i, j + 1, w, h, r) || i == 0 ||
                j == 0 || i == (int)w - 1 || j == (int)h - 1))
                canvas_draw_dot(c, x + i, y + j);
}
static void canvas_draw_xbm(Canvas* c, int x, int y, size_t w, size_t h, const uint8_t* data) {
    size_t stride = (w + 7) / 8;
    for(size_t j = 0; j < h; ++j)
        for(size_t i = 0; i < w; ++i) {
            bool bit = data[j * stride + i / 8] >> (i % 8) & 1;
            if(bit)
                canvas_draw_dot(c, x + i, y + j);
            else if(!c->transparent)
                put(c, x + i, y + j, c->color != ColorBlack);
        }
}
static uint16_t canvas_string_width(Canvas* c, const char* text) {
    const HostGlyph* glyphs = host_fonts[c->font].glyphs;
    int width = 0;
    const HostGlyph* last = NULL;
    for(; *text; ++text) {
        last = &glyphs[(unsigned char)*text - 32];
        width += last->advance;
    }
    if(last && last->w) width += last->w + last->dx - last->advance;
    return width;
}
static void canvas_draw_str(Canvas* c, int x, int baseline, const char* text) {
    const HostFont* font = &host_fonts[c->font];
    for(; *text; ++text) {
        const HostGlyph* g = &font->glyphs[(unsigned char)*text - 32];
        for(int py = 0; py < g->h; ++py)
            for(int px = 0; px < g->w; ++px) {
                unsigned bit = g->bits + py * g->w + px;
                if(!(font->bits[bit / 8] >> (bit % 8) & 1)) continue;
                int ax = x + g->dx + px, ay = baseline - g->h - g->dy + py;
                if(ax < 0 || ax > 127 || ay < 0 || ay > 63) ++c->clipped_text;
                canvas_draw_dot(c, ax, ay);
            }
        x += g->advance;
    }
}
static void canvas_draw_str_aligned(Canvas* c, int x, int y, Align h, Align v, const char* text) {
    assert(v == AlignBottom);
    int width = canvas_string_width(c, text);
    canvas_draw_str(c, h == AlignRight ? x - width : h == AlignCenter ? x - width / 2 : x, y, text);
}

#include "firmware/flipper/applications/examples/whale_pet/pet_ui.h"
#include "firmware/flipper/applications/examples/whale_pet/pet_art.h"
typedef enum { WhalePetStoreNew, WhalePetStoreReady, WhalePetStoreRecovered, WhalePetStoreDamaged } StoreStatus;
typedef struct {
    WhalePet pet;
    StoreStatus status;
} WhalePetStore;
#include "pet-app-struct-production.inc"
#include "pet-draw-production.inc"

static uint8_t* frames;
static uint8_t* masks;

static void show(PetApp* app, PetClip clip, unsigned index) {
    uint16_t frame = pet_animations[clip].frames[index % pet_animations[clip].count];
    memcpy(app->bitmap, frames + frame * 512, 512);
    memcpy(app->mask, masks + frame * 512, 512);
}

static void save(const char* directory, const char* name, PetApp* app) {
    Canvas canvas = {0};
    draw(&canvas, app);
    assert(canvas.clipped_text == 0);
    char path[512];
    snprintf(path, sizeof(path), "%s/pet-%s.raw", directory, name);
    FILE* out = fopen(path, "wb");
    assert(out && fwrite(canvas.px, 1, sizeof(canvas.px), out) == sizeof(canvas.px));
    fclose(out);
}

static uint8_t* load(const char* path) {
    FILE* in = fopen(path, "rb");
    assert(in);
    uint8_t* data = malloc(PET_FRAME_COUNT * 512);
    assert(fread(data, 1, PET_FRAME_COUNT * 512, in) == PET_FRAME_COUNT * 512);
    fclose(in);
    return data;
}

int main(int argc, char** argv) {
    assert(argc == 4);
    frames = load(argv[1]);
    masks = load(argv[2]);
    static PetApp app;
    app.save_ok = app.assets_ok = true;
    app.store.status = WhalePetStoreReady;
    whale_pet_init(&app.store.pet, 500000);
    app.store.pet.fullness = 35;
    uint32_t now = 1000;
    app.ui.random = 3;
    pet_ui_open(&app.ui, &app.store.pet, 7200, 14, now);
    show(&app, PetClipWave, 12);
    app.now = now + 1200;
    save(argv[3], "welcome", &app);
    app.now = now + 9000;
    show(&app, PetClipIdle, 0);
    save(argv[3], "menu-feed", &app);
    /* Feed: reaction with flashing food bar and bubble, menu hidden. */
    WhalePet next = app.store.pet;
    WhalePet before = app.store.pet;
    WhalePetResult result = whale_pet_apply(&next, WhalePetFeed, 500000);
    app.store.pet = next;
    pet_ui_cared(&app.ui, &app.store.pet, WhalePetFeed, result, true, &before, now);
    show(&app, PetClipRice, 26);
    app.now = now + 1000;
    save(argv[3], "rice", &app);
    /* Play really lowers Energy and raises Joy; check the actual glyph pixels. */
    before = app.store.pet;
    next = before;
    result = whale_pet_apply(&next, WhalePetPlay, 500001);
    app.store.pet = next;
    pet_ui_cared(&app.ui, &app.store.pet, WhalePetPlay, result, true, &before, now);
    app.now = now + 1200; // visible blink phase
    show(&app, PetClipPlay, 18);
    Canvas signs = {0};
    draw_card(&signs, &app);
    for(unsigned x = 34; x <= 38; ++x) assert(signs.px[13][x]);
    assert(!signs.px[11][36] && !signs.px[12][36] && !signs.px[14][36] && !signs.px[15][36]);
    assert(signs.px[17][36] && signs.px[18][36] && signs.px[19][36] && signs.px[20][36] && signs.px[21][36]);
    save(argv[3], "play", &app);
    /* Each menu entry, so every label is checked for fit. */
    app.ui.busy = false;
    app.ui.say[0] = 0;
    app.ui.flash = 0;
    show(&app, PetClipIdle, 0);
    for(unsigned i = 0; i < app.ui.menu_count; ++i) {
        app.ui.selected = i;
        char name[32];
        snprintf(name, sizeof(name), "item-%s", pet_ui_item_label(app.ui.menu[i]));
        save(argv[3], name, &app);
    }
    app.ui.selected = 2;
    snprintf(app.ui.say, sizeof(app.ui.say), "Welcome back!");
    app.ui.say_until = app.now + 1000;
    save(argv[3], "long-bubble", &app);
    /* Night, sleeping: Wake/Stats menu. */
    app.night = true;
    app.store.pet.sleeping = true;
    pet_ui_open(&app.ui, &app.store.pet, 0, 23, now);
    app.now = now + 500;
    show(&app, PetClipSleeping, 20);
    save(argv[3], "night", &app);
    app.night = false;
    app.store.pet.sleeping = false;
    app.store.pet.xp = 60;
    app.store.pet.bond = 42;
    app.ui.page = PetPageStats;
    show(&app, PetClipContent, 10);
    save(argv[3], "stats", &app);
    app.ui.page = PetPageFaces;
    app.ui.face = PetClipSurprised;
    show(&app, PetClipSurprised, 6);
    save(argv[3], "faces", &app);
    /* Catch Rice mid-round and the result panel. */
    pet_ui_open(&app.ui, &app.store.pet, 0, 14, now);
    app.ui.selected = 3;
    pet_ui_input(&app.ui, &app.store.pet, PetKeyOk, PetPressShort, now);
    assert(app.ui.page == PetPageGame);
    for(unsigned i = 0; i < 90; ++i) {
        now += 50;
        pet_ui_tick(&app.ui, &app.store.pet, 14, now);
    }
    app.ui.game.score = 7;
    app.ui.game.lives = 2;
    app.now = now;
    show(&app, PetClipExcited, 4);
    save(argv[3], "game", &app);
    PetCommand over = pet_ui_input(&app.ui, &app.store.pet, PetKeyBack, PetPressShort, now);
    assert(over.type == PetCommandGameOver);
    before = app.store.pet;
    next = before;
    result = whale_pet_game(&next, app.ui.game.score, 500121);
    app.store.pet = next;
    pet_ui_game_scored(&app.ui, &app.store.pet, result, true, &before, now);
    show(&app, PetClipContent, 4);
    save(argv[3], "game-over", &app);
    /* Read-only save banner stays legible over the room. */
    app.ui.page = PetPageMain;
    app.save_ok = false;
    app.store.status = WhalePetStoreDamaged;
    save(argv[3], "damaged", &app);
    puts("PASS: production pet drawing with real fonts: room, menu labels, bubbles, stats, faces, "
         "Catch Rice and banners stay on screen");
    free(frames);
    free(masks);
}
