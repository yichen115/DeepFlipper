#include "pet_model.h"
#include "pet_store.h"
#include "pet_ui.h"
#include "pet_art.h"
#include <deepflipper_app/deepflipper_app.h>
#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <mjs/common/frozen/frozen.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    WhalePetStore store;
    FuriMutex* mutex;
    FuriMessageQueue* input;
    atomic_bool exit;
    uint8_t bitmap[512], mask[512];
    PetUi ui;
    uint32_t now; // tick snapshot used by the GUI callback
    bool night, assets_ok, save_ok;
} PetApp;

static uint32_t wall_time(void) {
    DateTime time;
    furi_hal_rtc_get_datetime(&time);
    return datetime_datetime_to_timestamp(&time);
}

static const char* store_error(PetApp* app) {
    return app->store.status == WhalePetStoreDamaged ? "save_damaged" : "storage_unavailable";
}

static void state_result(PetApp* app, FuriString* output, WhalePetResult result) {
    WhalePet current = app->store.pet;
    whale_pet_advance(&current, wall_time());
    char text[512];
    struct json_out out = JSON_OUT_BUF(text, sizeof(text));
    static const char* const results[] = {
        "changed", "cooldown", "full", "tired", "asleep", "unchanged"};
    json_printf(
        &out,
        "{fullness:%u,energy:%u,joy:%u,bond:%u,xp:%u,level:%u,sleeping:%B,"
        "favorite_food:\"white_rice\",effect_applied:%B,displayed:false,revision:%u,care_result:%Q,saved:%B}",
        current.fullness,
        current.energy,
        current.joy,
        current.bond,
        current.xp,
        whale_pet_level(&current),
        current.sleeping,
        result == WhalePetChanged,
        current.revision,
        results[result],
        app->store.active >= 0);
    furi_string_set(output, text);
}

static const char* rpc_state(
    DeepFlipperApp* sdk,
    const char* args,
    uint32_t timeout,
    FuriString* output,
    void* context) {
    UNUSED(args);
    UNUSED(timeout);
    PetApp* app = context;
    if(deepflipper_app_cancelled(sdk)) return "cancelled";
    if(!whale_pet_store_writable(&app->store)) return store_error(app);
    state_result(app, output, WhalePetUnchanged);
    return NULL;
}

static const char* rpc_interact(
    DeepFlipperApp* sdk,
    const char* args,
    uint32_t timeout,
    FuriString* output,
    void* context) {
    UNUSED(timeout);
    PetApp* app = context;
    if(deepflipper_app_cancelled(sdk)) return "cancelled";
    if(!whale_pet_store_writable(&app->store)) return store_error(app);
    struct json_token kind = JSON_INVALID_TOKEN;
    json_scanf(args, strlen(args), "{kind:%T}", &kind);
    const char* names[] = {"pat", "feed", "play", "rest", "wake"};
    unsigned action = 0;
    for(; action < COUNT_OF(names); ++action)
        if(kind.type == JSON_TYPE_STRING && kind.len == (int)strlen(names[action]) &&
           !memcmp(kind.ptr, names[action], kind.len))
            break;
    if(action == COUNT_OF(names)) return "invalid_care_action";
    WhalePet next = app->store.pet;
    WhalePetResult result = whale_pet_apply(&next, action, wall_time());
    if(deepflipper_app_cancelled(sdk)) return "cancelled";
    if(!whale_pet_store_commit(&app->store, &next)) return "save_failed";
    state_result(app, output, result);
    return NULL;
}

static const DeepFlipperAction actions[] = {
    {"pet.state",
     "Read offline pet care state. Favorite food is white rice; no pet screen is opened.",
     "{\"type\":\"object\",\"properties\":{}}",
     rpc_state,
     0},
    {"pet.interact",
     "Care for the pet and save. feed means white rice. Cooldown may prevent a stat reward. No screen is opened.",
     "{\"type\":\"object\",\"properties\":{\"kind\":{\"type\":\"string\",\"enum\":[\"pat\",\"feed\",\"play\",\"rest\",\"wake\"]}},\"required\":[\"kind\"]}",
     rpc_interact,
     0},
};

// Flipper's kernel tick is 1 kHz, so ticks are used directly as milliseconds.
static uint32_t now_ms(void) {
    return furi_get_tick();
}

static unsigned rtc_hour(void) {
    DateTime time;
    furi_hal_rtc_get_datetime(&time);
    return time.hour;
}

// ------------------------------------------------------------------ drawing

static void draw_masked(Canvas* canvas, int x, const uint8_t* ink, const uint8_t* mask) {
    // Paper first (face, clothes and a 1 px halo), then ink, over the room.
    canvas_set_bitmap_mode(canvas, true);
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_xbm(canvas, x, 0, 64, 64, mask);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_xbm(canvas, x, 0, 64, 64, ink);
    canvas_set_bitmap_mode(canvas, false);
}

static void draw_icon(Canvas* canvas, int x, int y, int w, int h, const uint8_t* data) {
    canvas_set_bitmap_mode(canvas, true);
    canvas_draw_xbm(canvas, x, y, w, h, data);
    canvas_set_bitmap_mode(canvas, false);
}

#define ICON(canvas, x, y, name) \
    draw_icon(canvas, x, y, pet_icon_##name##_w, pet_icon_##name##_h, pet_icon_##name)

static void draw_bubble(Canvas* canvas, const char* text) {
    canvas_set_font(canvas, FontSecondary);
    int width = canvas_string_width(canvas, text) + 8;
    int x = 128 - width;
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_rbox(canvas, x, 0, width, 12, 3);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_rframe(canvas, x, 0, width, 12, 3);
    canvas_draw_str(canvas, x + 4, 9, text);
}

static void draw_bar_value(Canvas* canvas, int x, int y, int width, int height, unsigned value) {
    canvas_draw_frame(canvas, x, y, width, height);
    unsigned fill = value * (unsigned)(width - 2) / 100;
    if(fill) canvas_draw_box(canvas, x + 1, y + 1, fill, height - 2);
}

static void draw_card(Canvas* canvas, const PetApp* app) {
    // The pinned note card is part of the room art; values are live.
    const WhalePet* pet = &app->store.pet;
    const uint8_t values[] = {pet->fullness, pet->energy, pet->joy};
    const uint8_t* icons[] = {pet_icon_mini_food, pet_icon_mini_energy, pet_icon_mini_joy};
    const uint8_t bits[] = {PetFlashFood, PetFlashEnergy, PetFlashJoy};
    bool blink = (app->now / 200) % 2;
    for(unsigned row = 0; row < 3; ++row) {
        int y = 5 + row * 6;
        draw_icon(canvas, 3, y, 5, 5, icons[row]);
        draw_bar_value(canvas, 10, y, 19, 5, values[row]);
        if(app->ui.flash & bits[row] && blink) {
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_box(canvas, 33, y - 1, 7, 7);
            canvas_set_color(canvas, ColorBlack);
            if(app->ui.flash_decrease & bits[row])
                canvas_draw_line(canvas, 34, y + 2, 38, y + 2);
            else
                ICON(canvas, 34, y, plus);
        }
    }
}

static void draw_menu(Canvas* canvas, const PetUi* ui, const char* label, PetItem item) {
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_rbox(canvas, 34, 50, 62, 14, 3);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_rframe(canvas, 34, 50, 62, 14, 3);
    ICON(canvas, 37, 54, left);
    ICON(canvas, 90, 54, right);
    static const uint8_t* const icons[PetItemCount] = {
        pet_icon_pat,
        pet_icon_feed,
        pet_icon_play,
        pet_icon_catch,
        pet_icon_sleep,
        pet_icon_stats,
        pet_icon_wake};
    canvas_set_font(canvas, FontPrimary);
    if(item < PetItemCount) {
        draw_icon(canvas, 43, 52, 9, 9, icons[item]);
        canvas_draw_str(canvas, 55, 61, label);
    } else {
        canvas_draw_str_aligned(canvas, 65, 61, AlignCenter, AlignBottom, label);
    }
    UNUSED(ui);
}

static void draw_room(Canvas* canvas, const PetApp* app) {
    canvas_draw_xbm(canvas, 0, 0, 128, 64, app->night ? pet_room_night : pet_room_day);
    draw_masked(canvas, 33, app->bitmap, app->mask);
}

static void draw_stats(Canvas* canvas, const PetApp* app) {
    const WhalePet* pet = &app->store.pet;
    draw_masked(canvas, 0, app->bitmap, app->mask);
    canvas_draw_line(canvas, 61, 0, 61, 63);
    char text[24];
    canvas_set_font(canvas, FontPrimary);
    snprintf(text, sizeof(text), "Lv.%u", whale_pet_level(pet));
    canvas_draw_str(canvas, 65, 9, text);
    draw_icon(canvas, 99, 3, 5, 5, pet_icon_mini_joy);
    canvas_set_font(canvas, FontSecondary);
    snprintf(text, sizeof(text), "%u", pet->bond);
    canvas_draw_str(canvas, 106, 9, text);
    const char* labels[] = {"Food", "Energy", "Joy"};
    const uint8_t values[] = {pet->fullness, pet->energy, pet->joy};
    for(unsigned row = 0; row < 3; ++row) {
        int y = 20 + row * 13;
        canvas_draw_str(canvas, 65, y, labels[row]);
        snprintf(text, sizeof(text), "%u", values[row]);
        canvas_draw_str_aligned(canvas, 127, y, AlignRight, AlignBottom, text);
        draw_bar_value(canvas, 65, y + 2, 63, 4, values[row]);
    }
    unsigned have, need;
    whale_pet_level_progress(pet, &have, &need);
    canvas_draw_str(canvas, 65, 61, "Next");
    draw_bar_value(canvas, 87, 56, 41, 5, have * 100 / need);
    // Expression gallery lives behind Left/Right on this page.
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_rbox(canvas, 9, 53, 44, 11, 3);
    canvas_set_color(canvas, ColorBlack);
    canvas_draw_rframe(canvas, 9, 53, 44, 11, 3);
    canvas_draw_str_aligned(canvas, 31, 62, AlignCenter, AlignBottom, "< Faces >");
}

static void draw_game(Canvas* canvas, const PetApp* app) {
    const PetGame* game = &app->ui.game;
    draw_masked(canvas, 64, app->bitmap, app->mask);
    canvas_draw_line(canvas, 63, 0, 63, 63);
    // Time left shrinks along the top edge.
    unsigned left = game->ticks >= PetGameTicks ? 0 : PetGameTicks - game->ticks;
    if(left) canvas_draw_line(canvas, 0, 0, left * 62 / PetGameTicks, 0);
    char text[16];
    snprintf(text, sizeof(text), "%u", game->score);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 11, text);
    for(unsigned i = 0; i < PetGameLives; ++i) {
        if(i < game->lives)
            draw_icon(canvas, 56 - i * 7, 3, 5, 5, pet_icon_mini_joy);
        else
            canvas_draw_dot(canvas, 58 - i * 7, 5);
    }
    canvas_draw_line(canvas, 0, PetGameBowlY + 6, 62, PetGameBowlY + 6);
    for(unsigned i = 0; i < PetGameDrops; ++i) {
        const PetDrop* drop = &game->drops[i];
        int y = drop->y4 / 4;
        // Grains appear below the score/hearts row instead of over it.
        if(!drop->active || y < 10 || y > 63) continue;
        if(drop->bonus)
            ICON(canvas, drop->x, y, onigiri);
        else
            ICON(canvas, drop->x, y, grain);
    }
    ICON(canvas, game->bowl_x, PetGameBowlY, bowl);
    if(game->over) {
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_rbox(canvas, 3, 16, 57, 34, 3);
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_rframe(canvas, 3, 16, 57, 34, 3);
        snprintf(text, sizeof(text), "Score %u", game->score);
        canvas_draw_str_aligned(canvas, 31, 29, AlignCenter, AlignBottom, text);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(
            canvas, 31, 39, AlignCenter, AlignBottom, game->result ? game->result : "...");
        if(game->rewarded) canvas_draw_str_aligned(canvas, 31, 48, AlignCenter, AlignBottom, "OK");
    }
}

static void draw(Canvas* canvas, void* context) {
    PetApp* app = context;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    const PetUi* ui = &app->ui;
    if(ui->page == PetPageStats) {
        draw_stats(canvas, app);
    } else if(ui->page == PetPageGame) {
        draw_game(canvas, app);
    } else {
        draw_room(canvas, app);
        draw_card(canvas, app);
        if(ui->page == PetPageFaces) {
            draw_menu(canvas, ui, pet_animations[ui->face].name, PetItemCount);
        } else {
            const char* bubble = pet_ui_bubble(ui, app->now);
            if(bubble) draw_bubble(canvas, bubble);
            if(pet_ui_menu_visible(ui, app->now)) {
                PetItem item = ui->menu[ui->selected];
                draw_menu(canvas, ui, pet_ui_item_label(item), item);
            }
        }
    }
    const char* banner = !app->save_ok ? (app->store.status == WhalePetStoreDamaged ?
                                              "Save damaged: read only" :
                                              "Save failed: not stored") :
                         !app->assets_ok ? "Art missing on SD" :
                                           NULL;
    if(banner) {
        canvas_set_font(canvas, FontSecondary);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_box(canvas, 0, 0, 128, 11);
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_line(canvas, 0, 11, 127, 11);
        canvas_draw_str(canvas, 2, 9, banner);
    }
    furi_mutex_release(app->mutex);
}

// ------------------------------------------------------------------ input

static void input(InputEvent* event, void* context) {
    PetApp* app = context;
    if(event->key == InputKeyBack && event->type == InputTypeLong) {
        atomic_store(&app->exit, true);
    } else if(
        event->type == InputTypeShort || event->type == InputTypeLong ||
        event->type == InputTypePress || event->type == InputTypeRelease) {
        // Never block the GUI thread, including while SD writes are in progress.
        furi_message_queue_put(app->input, event, 0);
    }
}

static PetKey pet_key(InputKey key) {
    switch(key) {
    case InputKeyUp:
        return PetKeyUp;
    case InputKeyDown:
        return PetKeyDown;
    case InputKeyLeft:
        return PetKeyLeft;
    case InputKeyRight:
        return PetKeyRight;
    case InputKeyBack:
        return PetKeyBack;
    default:
        return PetKeyOk;
    }
}

static PetPress pet_press(InputType type) {
    return type == InputTypeLong    ? PetPressLong :
           type == InputTypePress   ? PetPressDown :
           type == InputTypeRelease ? PetPressUp :
                                      PetPressShort;
}

// Apply a care/game command to the model, save only real changes, then let
// the controller react. Runs on the app thread; the GUI never waits on SD.
static void execute(PetApp* app, PetCommand command) {
    if(command.type == PetCommandExit) {
        atomic_store(&app->exit, true);
        return;
    }
    if(command.type != PetCommandCare && command.type != PetCommandGameOver) return;
    WhalePetStore pending = app->store;
    WhalePet next = pending.pet;
    const WhalePet before = next;
    WhalePetResult result = command.type == PetCommandCare ?
                                whale_pet_apply(&next, command.action, wall_time()) :
                                whale_pet_game(&next, command.score, wall_time());
    bool changed = result == WhalePetChanged;
    bool saved = changed && whale_pet_store_commit(&pending, &next);
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    if(saved) {
        app->store = pending;
    } else if(!changed) {
        app->store.pet = next; // keep the settled clock; nothing worth a write
    }
    if(changed) app->save_ok = saved;
    if(command.type == PetCommandCare) {
        pet_ui_cared(
            &app->ui, &app->store.pet, command.action, result, saved || !changed, &before, now_ms());
    } else {
        pet_ui_game_scored(
            &app->ui, &app->store.pet, result, saved || !changed, &before, now_ms());
    }
    furi_mutex_release(app->mutex);
}

static bool load_art(File* file, uint16_t frame, uint8_t* out) {
    return storage_file_seek(file, frame * 512U, true) && storage_file_read(file, out, 512) == 512;
}

static void run_ui(PetApp* app) {
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->input = furi_message_queue_alloc(16, sizeof(InputEvent));
    memcpy(app->bitmap, pet_fallback, sizeof(app->bitmap));
    memcpy(app->mask, pet_fallback_mask, sizeof(app->mask));
    app->save_ok = whale_pet_store_writable(&app->store);
    // Measure the time away before settling it.
    uint32_t wall = wall_time();
    uint32_t away = wall > app->store.pet.updated ? wall - app->store.pet.updated : 0;
    whale_pet_advance(&app->store.pet, wall);
    app->ui.random = furi_hal_random_get();
    unsigned hour = rtc_hour();
    app->night = hour >= 20 || hour < 7;
    app->now = now_ms();
    pet_ui_open(&app->ui, &app->store.pet, away, hour, app->now);
    File* frames = storage_file_alloc(app->store.storage);
    File* masks = storage_file_alloc(app->store.storage);
    app->assets_ok =
        storage_file_open(
            frames, EXT_PATH("deepflipper/pet_frames.bin"), FSAM_READ, FSOM_OPEN_EXISTING) &&
        storage_file_size(frames) == PET_FRAME_COUNT * 512 &&
        storage_file_open(
            masks, EXT_PATH("deepflipper/pet_masks.bin"), FSAM_READ, FSOM_OPEN_EXISTING) &&
        storage_file_size(masks) == PET_FRAME_COUNT * 512;
    Gui* gui = furi_record_open(RECORD_GUI);
    ViewPort* view = view_port_alloc();
    view_port_draw_callback_set(view, draw, app);
    view_port_input_callback_set(view, input, app);
    gui_add_view_port(gui, view, GuiLayerFullscreen);
    uint16_t previous = UINT16_MAX;
    uint32_t last_update = 0;
    while(!atomic_load(&app->exit)) {
        InputEvent event;
        if(furi_message_queue_get(app->input, &event, furi_ms_to_ticks(20)) == FuriStatusOk) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            PetCommand command = pet_ui_input(
                &app->ui, &app->store.pet, pet_key(event.key), pet_press(event.type), now_ms());
            furi_mutex_release(app->mutex);
            execute(app, command);
        }
        uint32_t now = now_ms();
        if(now - last_update < PetGameTickMs) continue;
        last_update = now;
        hour = rtc_hour();
        // GUI sees only small snapshots, never storage I/O or a half-filled frame.
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        whale_pet_advance(&app->store.pet, wall_time());
        app->night = hour >= 20 || hour < 7;
        PetCommand command = pet_ui_tick(&app->ui, &app->store.pet, hour, now);
        PetClip clip = pet_ui_clip(&app->ui);
        uint16_t frame = pet_animations[clip].frames[pet_ui_frame(&app->ui, now)];
        app->now = now;
        furi_mutex_release(app->mutex);
        execute(app, command);
        if(frame != previous && app->assets_ok) {
            // Staging stays off the 4 KiB app stack.
            static uint8_t pixels[512], opaque[512];
            bool okay = load_art(frames, frame, pixels) && load_art(masks, frame, opaque);
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            if(okay) {
                memcpy(app->bitmap, pixels, sizeof(pixels));
                memcpy(app->mask, opaque, sizeof(opaque));
            } else {
                memcpy(app->bitmap, pet_fallback, sizeof(app->bitmap));
                memcpy(app->mask, pet_fallback_mask, sizeof(app->mask));
            }
            app->assets_ok = okay;
            furi_mutex_release(app->mutex);
            previous = frame;
        }
        view_port_update(view);
    }
    // Removing the viewport joins in-progress draw/input callbacks before free.
    view_port_enabled_set(view, false);
    gui_remove_view_port(gui, view);
    view_port_free(view);
    furi_record_close(RECORD_GUI);
    storage_file_free(frames);
    storage_file_free(masks);
    if(whale_pet_store_writable(&app->store)) whale_pet_store_commit(&app->store, &app->store.pet);
    furi_message_queue_free(app->input);
    furi_mutex_free(app->mutex);
}

int32_t whale_pet_main(void* arguments) {
    PetApp* app = calloc(1, sizeof(PetApp));
    Storage* storage = furi_record_open(RECORD_STORAGE);
    whale_pet_store_load(&app->store, storage, wall_time());
    if(arguments && !strncmp(arguments, "RPC ", 4)) {
        deepflipper_app_run(arguments, "deepflipper.pet", actions, COUNT_OF(actions), app);
    } else {
        run_ui(app);
    }
    furi_record_close(RECORD_STORAGE);
    free(app);
    return 0;
}
