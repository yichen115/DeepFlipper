#include "pet_ui.h"
#include <stdio.h>
#include <string.h>

#define FRAME_MS 100
#define SAY_MS   2600

static uint32_t next_random(PetUi* ui) {
    ui->random = ui->random * 1103515245U + 12345U;
    return ui->random >> 16;
}

static void say(PetUi* ui, const char* text, uint32_t now, uint32_t ms) {
    snprintf(ui->say, sizeof(ui->say), "%s", text);
    ui->say_until = now + ms;
}

static void flash_changes(
    PetUi* ui, const WhalePet* before, const WhalePet* pet, bool applied, uint32_t now) {
    const uint8_t old[] = {before->fullness, before->energy, before->joy};
    const uint8_t current[] = {pet->fullness, pet->energy, pet->joy};
    const uint8_t bits[] = {PetFlashFood, PetFlashEnergy, PetFlashJoy};
    ui->flash = ui->flash_decrease = 0;
    if(applied) {
        for(unsigned i = 0; i < 3; ++i) {
            if(current[i] != old[i]) ui->flash |= bits[i];
            if(current[i] < old[i]) ui->flash_decrease |= bits[i];
        }
    }
    ui->flash_until = ui->flash ? now + 1600 : 0;
}

static void start(PetUi* ui, PetClip clip, uint32_t now) {
    ui->clip = clip;
    ui->clip_started = now;
    ui->next_idle = now + 11000 + next_random(ui) % 9000;
}

static bool late(unsigned hour) {
    return hour >= 23 || hour < 5;
}

static PetClip resting(const WhalePet* pet) {
    return pet->sleeping                       ? PetClipSleeping :
           pet->energy < WhalePetNeedEnergy    ? PetClipSleepy :
           pet->joy >= 85                      ? PetClipContent :
                                                 PetClipIdle;
}

static PetItem suggest(const WhalePet* pet, unsigned hour) {
    if(pet->sleeping) return PetItemWake;
    switch(whale_pet_need(pet)) {
    case WhalePetHungry:
        return PetItemFeed;
    case WhalePetSleepy:
        return PetItemSleep;
    case WhalePetBored:
        return PetItemPlay;
    default:
        break;
    }
    return late(hour) && pet->energy < 60 ? PetItemSleep : PetItemPat;
}

static void build_menu(PetUi* ui, const WhalePet* pet) {
    static const uint8_t awake[] = {
        PetItemPat, PetItemFeed, PetItemPlay, PetItemCatch, PetItemSleep, PetItemStats};
    static const uint8_t asleep[] = {PetItemWake, PetItemStats};
    ui->menu_count = pet->sleeping ? sizeof(asleep) : sizeof(awake);
    memcpy(ui->menu, pet->sleeping ? asleep : awake, ui->menu_count);
    ui->menu_sleeping = pet->sleeping;
    if(ui->selected >= ui->menu_count) ui->selected = 0;
}

static void select_item(PetUi* ui, PetItem item) {
    for(uint8_t i = 0; i < ui->menu_count; ++i)
        if(ui->menu[i] == item) ui->selected = i;
}

static bool user_idle(const PetUi* ui, uint32_t now) {
    return now - ui->last_nav > 10000;
}

const char* pet_ui_item_label(PetItem item) {
    static const char* const labels[PetItemCount] = {
        "Pat", "Feed", "Play", "Catch", "Sleep", "Stats", "Wake"};
    return item < PetItemCount ? labels[item] : "";
}

void pet_ui_open(PetUi* ui, const WhalePet* pet, uint32_t away, unsigned hour, uint32_t now) {
    uint32_t seed = ui->random;
    memset(ui, 0, sizeof(*ui));
    ui->random = seed ? seed : 0x5eedU;
    ui->face = PetClipIdle;
    ui->last_nav = now - 20000;
    build_menu(ui, pet);
    select_item(ui, suggest(pet, hour));
    if(pet->sleeping) {
        start(ui, PetClipSleeping, now);
        say(ui, "Zzz...", now, SAY_MS);
    } else {
        start(ui, PetClipWave, now);
        say(ui,
            away >= 3600              ? "Welcome back!" :
            hour >= 5 && hour < 11    ? "Morning!" :
            late(hour)                ? "Up late?" :
                                        "Hi!",
            now,
            SAY_MS);
    }
}

PetClip pet_ui_clip(const PetUi* ui) {
    return ui->page == PetPageFaces ? (PetClip)ui->face : ui->clip;
}

unsigned pet_ui_frame(const PetUi* ui, uint32_t now) {
    PetClip clip = pet_ui_clip(ui);
    unsigned index = (now - ui->clip_started) / FRAME_MS;
    unsigned count = pet_clip_length[clip];
    if(pet_clip_loop[clip] || ui->page == PetPageFaces) return index % count;
    return index < count ? index : count - 1;
}

bool pet_ui_menu_visible(const PetUi* ui, uint32_t now) {
    (void)now;
    return ui->page == PetPageMain && !ui->busy;
}

const char* pet_ui_bubble(const PetUi* ui, uint32_t now) {
    if(ui->page != PetPageMain) return NULL;
    return ui->say[0] && (int32_t)(ui->say_until - now) > 0 ? ui->say : NULL;
}

// ------------------------------------------------------------------ Catch Rice

static void game_start(PetUi* ui, uint32_t now) {
    PetGame* game = &ui->game;
    memset(game, 0, sizeof(*game));
    game->bowl_x = (PetGameWidth - PetGameBowlW) / 2;
    game->lives = PetGameLives;
    game->spawn_in = 8;
    game->last_step = now;
    ui->page = PetPageGame;
    ui->busy = false;
    ui->say[0] = 0;
    start(ui, PetClipExcited, now);
}

static PetCommand game_end(PetGame* game) {
    game->over = true;
    game->left = game->right = false;
    return (PetCommand){.type = PetCommandGameOver, .score = game->score};
}

static void game_spawn(PetUi* ui) {
    PetGame* game = &ui->game;
    for(unsigned i = 0; i < PetGameDrops; ++i) {
        PetDrop* drop = &game->drops[i];
        if(drop->active) continue;
        drop->bonus = next_random(ui) % 9 == 0;
        int width = drop->bonus ? 7 : 3, height = drop->bonus ? 7 : 4;
        drop->x = next_random(ui) % (PetGameWidth - width - 1) + 1;
        drop->y4 = -height * 4;
        // Gentle ramp: about 3 s to fall at first, never faster than ~1.6 s.
        unsigned speed = 2 + game->score / 8;
        drop->speed = speed > 4 ? 4 : speed;
        drop->active = true;
        break;
    }
    unsigned gap = 34 - (game->score > 32 ? 16 : game->score / 2);
    game->spawn_in = gap < 18 ? 18 : gap;
}

static void game_move(PetGame* game, int dx) {
    int x = game->bowl_x + dx, limit = PetGameWidth - PetGameBowlW;
    game->bowl_x = x < 0 ? 0 : x > limit ? limit : x;
}

static bool game_step(PetUi* ui, uint32_t now) {
    PetGame* game = &ui->game;
    if(game->left) game_move(game, -3);
    if(game->right) game_move(game, 3);
    ++game->ticks;
    for(unsigned i = 0; i < PetGameDrops; ++i) {
        PetDrop* drop = &game->drops[i];
        if(!drop->active) continue;
        drop->y4 += drop->speed;
        int width = drop->bonus ? 7 : 3, height = drop->bonus ? 7 : 4;
        int y = drop->y4 / 4, bottom = y + height;
        if(bottom >= PetGameBowlY - 1 && bottom <= PetGameBowlY + 5 &&
           drop->x + width > game->bowl_x && drop->x < game->bowl_x + PetGameBowlW) {
            game->score += drop->bonus ? 3 : 1;
            drop->active = false;
        } else if(y >= 64) {
            drop->active = false;
            if(!drop->bonus && game->lives) {
                --game->lives;
                start(ui, PetClipWorried, now);
                ui->next_idle = now + 1200;
            }
        }
    }
    if(!--game->spawn_in) game_spawn(ui);
    return !game->lives || game->ticks >= PetGameTicks;
}

static PetCommand game_input(PetUi* ui, const WhalePet* pet, PetKey key, PetPress press, uint32_t now) {
    PetGame* game = &ui->game;
    PetCommand none = {.type = PetCommandNone};
    if(game->over) {
        if(press == PetPressShort && game->rewarded && (key == PetKeyOk || key == PetKeyBack)) {
            ui->page = PetPageMain;
            ui->busy = false;
            start(
                ui,
                game->score >= 10 ? PetClipProud :
                game->score       ? PetClipContent :
                                    resting(pet),
                now);
            char text[16];
            snprintf(text, sizeof(text), "Score %u", game->score);
            say(ui, text, now, SAY_MS);
        }
        return none;
    }
    if(key == PetKeyLeft || key == PetKeyRight) {
        bool* held = key == PetKeyLeft ? &game->left : &game->right;
        if(press == PetPressDown) {
            *held = true;
            // A quick tap also moves, even if released before the next step.
            game_move(game, key == PetKeyLeft ? -4 : 4);
        }
        if(press == PetPressUp) *held = false;
    } else if(key == PetKeyBack && press == PetPressShort) {
        return game_end(game);
    }
    return none;
}

static PetCommand game_tick(PetUi* ui, uint32_t now) {
    PetGame* game = &ui->game;
    if(game->over) return (PetCommand){.type = PetCommandNone};
    if(ui->clip == PetClipWorried && (int32_t)(now - ui->next_idle) >= 0)
        start(ui, PetClipExcited, now);
    // Fixed 50 ms steps; catch up at most a few after a slow frame.
    for(unsigned guard = 0; guard < 4 && now - game->last_step >= PetGameTickMs; ++guard) {
        game->last_step += PetGameTickMs;
        if(game_step(ui, now)) return game_end(game);
    }
    if(now - game->last_step >= PetGameTickMs) game->last_step = now;
    return (PetCommand){.type = PetCommandNone};
}

void pet_ui_game_scored(
    PetUi* ui,
    const WhalePet* pet,
    WhalePetResult result,
    bool saved,
    const WhalePet* before,
    uint32_t now) {
    PetGame* game = &ui->game;
    game->rewarded = true;
    const char* reward = pet->joy > before->joy ? (pet->xp > before->xp ? "+Joy  +XP" : "+Joy") :
                        pet->xp > before->xp  ? "+XP" :
                                               "Good catch!";
    game->result = result == WhalePetChanged ? (saved ? reward : "Not saved!") :
                   result == WhalePetCooldown ? "Just for fun" :
                   result == WhalePetTired    ? "Too tired" :
                   game->score                ? "Nice try" :
                                                "No rice...";
    flash_changes(ui, before, pet, result == WhalePetChanged && saved, now);
    if(result == WhalePetChanged && saved) {
        unsigned level = whale_pet_level(pet);
        if(level > whale_pet_level(before)) ui->pending_level = level;
    }
    start(ui, game->score >= 10 ? PetClipProud : game->score ? PetClipContent : PetClipWorried, now);
}

// ------------------------------------------------------------------ main pages

static PetCommand care(WhalePetAction action) {
    return (PetCommand){.type = PetCommandCare, .action = action};
}

PetCommand pet_ui_input(PetUi* ui, const WhalePet* pet, PetKey key, PetPress press, uint32_t now) {
    PetCommand none = {.type = PetCommandNone};
    if(key == PetKeyBack && press == PetPressLong) return (PetCommand){.type = PetCommandExit};
    if(ui->page == PetPageGame) return game_input(ui, pet, key, press, now);
    if(press != PetPressShort && press != PetPressLong) return none;
    if(ui->page == PetPageStats) {
        if(press != PetPressShort) return none;
        if(key == PetKeyLeft || key == PetKeyRight) {
            ui->page = PetPageFaces;
            ui->face = PetClipIdle;
            ui->clip_started = now;
        } else if(key == PetKeyBack || key == PetKeyOk) {
            ui->page = PetPageMain;
        }
        return none;
    }
    if(ui->page == PetPageFaces) {
        if(press != PetPressShort) return none;
        if(key == PetKeyLeft || key == PetKeyRight) {
            ui->face = (ui->face + (key == PetKeyLeft ? PET_CLIP_COUNT - 1 : 1)) % PET_CLIP_COUNT;
            ui->clip_started = now;
        } else if(key == PetKeyBack || key == PetKeyOk) {
            ui->page = PetPageStats;
        }
        return none;
    }
    if(key == PetKeyBack) return (PetCommand){.type = PetCommandExit};
    if(key == PetKeyOk && press == PetPressLong) {
        ui->page = PetPageStats;
        return none;
    }
    // A care reaction plays to the end before the next one; Back still exits.
    if(ui->busy || press != PetPressShort) return none;
    if(key == PetKeyLeft || key == PetKeyRight) {
        ui->selected = (ui->selected + (key == PetKeyLeft ? ui->menu_count - 1 : 1)) % ui->menu_count;
        ui->last_nav = now;
        return none;
    }
    if(key != PetKeyOk) return none;
    ui->last_nav = now;
    switch(ui->menu[ui->selected]) {
    case PetItemStats:
        ui->page = PetPageStats;
        return none;
    case PetItemCatch:
        if(pet->energy < 25) {
            start(ui, PetClipSleepy, now);
            say(ui, "Too tired", now, SAY_MS);
            return none;
        }
        game_start(ui, now);
        return none;
    case PetItemFeed:
        return care(WhalePetFeed);
    case PetItemPlay:
        return care(WhalePetPlay);
    case PetItemSleep:
        return care(WhalePetRest);
    case PetItemWake:
        return care(WhalePetWake);
    default:
        return care(WhalePetPat);
    }
}

void pet_ui_cared(
    PetUi* ui,
    const WhalePet* pet,
    WhalePetAction action,
    WhalePetResult result,
    bool saved,
    const WhalePet* before,
    uint32_t now) {
    PetClip clip = resting(pet);
    const char* text = NULL;
    if(action == WhalePetPat && result != WhalePetAsleep) {
        // Pats keep working during the reward cooldown, with escalating shyness.
        ui->pat_combo = now - ui->pat_last < 8000 && ui->pat_combo < 9 ? ui->pat_combo + 1 : 1;
        ui->pat_last = now;
        clip = ui->pat_combo >= 5 ? PetClipSurprised : ui->pat_combo >= 3 ? PetClipShy : PetClipPat;
        text = ui->pat_combo >= 5 ? "Hey!" : ui->pat_combo >= 3 ? "Heh..." : "Hehe~";
    } else {
        switch(result) {
        case WhalePetChanged:
            if(action == WhalePetFeed) {
                clip = PetClipRice;
                text = "Yum!";
            } else if(action == WhalePetPlay) {
                clip = PetClipPlay;
                text = "Got it!";
            } else if(action == WhalePetRest) {
                clip = PetClipSleeping;
                text = "Night~";
            } else if(action == WhalePetWake) {
                clip = PetClipWave;
                text = "Morning!";
            }
            break;
        case WhalePetCooldown:
            clip = PetClipContent;
            text = "Later~";
            break;
        case WhalePetFull:
            clip = PetClipFull;
            text = "So full!";
            break;
        case WhalePetTired:
            clip = PetClipSleepy;
            text = "Too tired";
            break;
        case WhalePetAsleep:
            clip = PetClipSleeping;
            text = "Zzz...";
            break;
        default:
            break;
        }
    }
    if(!saved && result == WhalePetChanged) {
        clip = PetClipWorried;
        text = "Not saved!";
    }
    start(ui, clip, now);
    ui->busy = !pet_clip_loop[clip];
    if(text) say(ui, text, now, SAY_MS);
    flash_changes(ui, before, pet, result == WhalePetChanged && saved, now);
    unsigned level = whale_pet_level(pet);
    if(saved && level > whale_pet_level(before)) ui->pending_level = level;
    if(ui->menu_sleeping != pet->sleeping) {
        build_menu(ui, pet);
        select_item(ui, pet->sleeping ? PetItemWake : PetItemPat);
    }
}

PetCommand pet_ui_tick(PetUi* ui, const WhalePet* pet, unsigned hour, uint32_t now) {
    PetCommand none = {.type = PetCommandNone};
    if((int32_t)(ui->flash_until - now) <= 0) ui->flash = ui->flash_decrease = 0;
    if(ui->page == PetPageGame) return game_tick(ui, now);
    if(ui->menu_sleeping != pet->sleeping) {
        // She woke up by herself (or the save changed underneath).
        build_menu(ui, pet);
        select_item(ui, suggest(pet, hour));
        ui->busy = false;
        start(ui, pet->sleeping ? PetClipSleeping : PetClipWave, now);
        if(!pet->sleeping) say(ui, "Morning!", now, SAY_MS);
        return none;
    }
    bool finished = !pet_clip_loop[ui->clip] &&
                    (now - ui->clip_started) / FRAME_MS >= pet_clip_length[ui->clip];
    if(finished) {
        ui->busy = false;
        if(ui->pending_level) {
            char text[16];
            snprintf(text, sizeof(text), "Lv.%u!", ui->pending_level);
            ui->pending_level = 0;
            start(ui, PetClipExcited, now);
            ui->next_idle = now + 3200;
            say(ui, text, now, 3200);
        } else {
            start(ui, resting(pet), now);
        }
        if(user_idle(ui, now)) select_item(ui, suggest(pet, hour));
    } else if(
        ui->page == PetPageMain && !ui->busy && !pet->sleeping &&
        (int32_t)(now - ui->next_idle) >= 0) {
        switch(whale_pet_need(pet)) {
        case WhalePetHungry:
            start(ui, PetClipWorried, now);
            say(ui, "Hungry...", now, SAY_MS);
            break;
        case WhalePetSleepy:
            start(ui, PetClipSleepy, now);
            say(ui, "Sleepy...", now, SAY_MS);
            break;
        case WhalePetBored:
            start(ui, PetClipCurious, now);
            say(ui, "Play?", now, SAY_MS);
            break;
        default:
            if(late(hour) && pet->energy < 60) {
                start(ui, PetClipSleepy, now);
                say(ui, "Bedtime?", now, SAY_MS);
            } else {
                static const PetClip idles[] = {PetClipIdle, PetClipCurious, PetClipContent};
                start(ui, idles[next_random(ui) % 3], now);
            }
        }
        if(user_idle(ui, now)) select_item(ui, suggest(pet, hour));
    }
    return none;
}
