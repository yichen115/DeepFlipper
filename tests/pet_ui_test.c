/* Run the production pet controller (menu, reactions, needs, Catch Rice). */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "firmware/flipper/applications/examples/whale_pet/pet_ui.h"

static uint32_t now = 1000;

static PetCommand key(PetUi* ui, const WhalePet* pet, PetKey k, PetPress p) {
    return pet_ui_input(ui, pet, k, p, now);
}

static PetCommand press(PetUi* ui, const WhalePet* pet, PetKey k) {
    return key(ui, pet, k, PetPressShort);
}

static PetItem selected(const PetUi* ui) {
    return ui->menu[ui->selected];
}

static bool says(const PetUi* ui, const char* text) {
    const char* bubble = pet_ui_bubble(ui, now);
    return bubble && !strcmp(bubble, text);
}

static void run_until(PetUi* ui, WhalePet* pet, const char* text) {
    for(unsigned i = 0; i < 440 && !says(ui, text); ++i, now += 50)
        assert(pet_ui_tick(ui, pet, 14, now).type == PetCommandNone);
    assert(says(ui, text));
}

static void run(PetUi* ui, WhalePet* pet, uint32_t ms) {
    for(uint32_t end = now + ms; now < end; now += 50) {
        PetCommand command = pet_ui_tick(ui, pet, 14, now);
        assert(command.type == PetCommandNone);
    }
}

/* Mirrors the app glue: apply, "save", then report back. */
static void execute(PetUi* ui, WhalePet* pet, PetCommand command, bool save_ok) {
    assert(command.type == PetCommandCare);
    WhalePet next = *pet;
    const WhalePet before = *pet;
    WhalePetResult result = whale_pet_apply(&next, command.action, 500000 + now / 1000);
    bool saved = result != WhalePetChanged || save_ok;
    if(saved) *pet = next;
    pet_ui_cared(ui, pet, command.action, result, saved, &before, now);
}

static void check_signed_feedback(void) {
    PetUi ui = {0};
    WhalePet before, after;
    const uint32_t stamp = 700000;
    whale_pet_init(&before, stamp);
    pet_ui_open(&ui, &before, 0, 14, 1000);
    after = before;
    WhalePetResult result = whale_pet_apply(&after, WhalePetPlay, stamp + 1);
    assert(result == WhalePetChanged && after.energy == before.energy - 5);
    assert(after.joy == before.joy + 12);
    pet_ui_cared(&ui, &after, WhalePetPlay, result, true, &before, 1000);
    assert(ui.flash == (PetFlashEnergy | PetFlashJoy));
    assert(ui.flash_decrease == PetFlashEnergy);

    before = after;
    result = whale_pet_apply(&after, WhalePetPlay, stamp + 2);
    assert(result == WhalePetCooldown);
    pet_ui_cared(&ui, &after, WhalePetPlay, result, true, &before, 1001);
    assert(ui.flash == 0 && ui.flash_decrease == 0);

    whale_pet_init(&before, stamp);
    before.joy = 100;
    after = before;
    result = whale_pet_apply(&after, WhalePetPlay, stamp + 1);
    pet_ui_cared(&ui, &after, WhalePetPlay, result, true, &before, 2000);
    assert(ui.flash == PetFlashEnergy && ui.flash_decrease == PetFlashEnergy);

    whale_pet_init(&before, stamp);
    after = before;
    result = whale_pet_apply(&after, WhalePetFeed, stamp + 1);
    pet_ui_cared(&ui, &after, WhalePetFeed, result, true, &before, 3000);
    assert(after.energy == before.energy + 6);
    assert(ui.flash == (PetFlashFood | PetFlashEnergy | PetFlashJoy));
    assert(ui.flash_decrease == 0);

    before.energy = before.joy = 100;
    after = before;
    result = whale_pet_apply(&after, WhalePetFeed, stamp + 1);
    pet_ui_cared(&ui, &after, WhalePetFeed, result, true, &before, 4000);
    assert(ui.flash == PetFlashFood && ui.flash_decrease == 0);
    after = before;
    result = whale_pet_apply(&after, WhalePetPat, stamp + 1);
    pet_ui_cared(&ui, &after, WhalePetPat, result, true, &before, 4001);
    assert(ui.flash == 0 && ui.flash_decrease == 0);

    whale_pet_init(&before, stamp);
    after = before;
    result = whale_pet_game(&after, 10, stamp + 1);
    ui.page = PetPageGame;
    ui.game.over = true;
    pet_ui_game_scored(&ui, &after, result, true, &before, 5000);
    assert(ui.flash == (PetFlashEnergy | PetFlashJoy) && ui.flash_decrease == PetFlashEnergy);
    assert(!strcmp(ui.game.result, "+Joy  +XP"));
    pet_ui_tick(&ui, &after, 14, 6601);
    assert(ui.flash == 0 && ui.flash_decrease == 0);

    before.joy = 100;
    before.xp = 9999;
    after = before;
    result = whale_pet_game(&after, 10, stamp + 1);
    pet_ui_game_scored(&ui, &after, result, true, &before, 7000);
    assert(ui.flash == PetFlashEnergy && ui.flash_decrease == PetFlashEnergy);
    assert(!strcmp(ui.game.result, "Good catch!"));
    pet_ui_game_scored(&ui, &before, WhalePetChanged, false, &before, 7001);
    assert(ui.flash == 0 && ui.flash_decrease == 0 && !strcmp(ui.game.result, "Not saved!"));
    pet_ui_cared(&ui, &before, WhalePetPlay, WhalePetChanged, false, &before, 7002);
    assert(ui.flash == 0 && ui.flash_decrease == 0);
}

int main(void) {
    check_signed_feedback();
    PetUi ui = {.random = 7};
    WhalePet pet;
    whale_pet_init(&pet, 500000);

    /* Welcome greets by time away / hour and pre-selects the most needed care. */
    pet.fullness = 30;
    pet_ui_open(&ui, &pet, 7200, 14, now);
    assert(ui.page == PetPageMain && ui.clip == PetClipWave);
    assert(!strcmp(pet_ui_bubble(&ui, now), "Welcome back!"));
    assert(selected(&ui) == PetItemFeed && ui.menu_count == 6);
    assert(pet_ui_menu_visible(&ui, now));
    pet_ui_open(&ui, &pet, 10, 8, now);
    assert(!strcmp(pet_ui_bubble(&ui, now), "Morning!"));
    pet.fullness = 72;
    pet_ui_open(&ui, &pet, 10, 23, now);
    assert(!strcmp(pet_ui_bubble(&ui, now), "Up late?"));
    pet.energy = 40;
    pet_ui_open(&ui, &pet, 10, 23, now);
    assert(selected(&ui) == PetItemSleep);
    pet.energy = 80;
    pet_ui_open(&ui, &pet, 10, 14, now);
    assert(selected(&ui) == PetItemPat && !strcmp(pet_ui_bubble(&ui, now), "Hi!"));

    /* Left/Right wrap through Pat Feed Play Catch Sleep Stats. */
    press(&ui, &pet, PetKeyLeft);
    assert(selected(&ui) == PetItemStats);
    press(&ui, &pet, PetKeyRight);
    press(&ui, &pet, PetKeyRight);
    assert(selected(&ui) == PetItemFeed);
    assert(!strcmp(pet_ui_item_label(PetItemFeed), "Feed"));

    /* Feeding: reaction plays to the end, menu steps aside, bar flashes. */
    PetCommand command = press(&ui, &pet, PetKeyOk);
    assert(command.type == PetCommandCare && command.action == WhalePetFeed);
    execute(&ui, &pet, command, true);
    assert(ui.clip == PetClipRice && ui.busy && !pet_ui_menu_visible(&ui, now));
    assert(!strcmp(pet_ui_bubble(&ui, now), "Yum!"));
    assert(ui.flash == (PetFlashFood | PetFlashEnergy | PetFlashJoy) && ui.flash_decrease == 0);
    assert(press(&ui, &pet, PetKeyOk).type == PetCommandNone);
    assert(press(&ui, &pet, PetKeyRight).type == PetCommandNone && selected(&ui) == PetItemFeed);
    assert(press(&ui, &pet, PetKeyBack).type == PetCommandExit);
    run(&ui, &pet, 4900);
    assert(!ui.busy && pet_ui_menu_visible(&ui, now) && ui.flash == 0);
    assert(ui.clip == PetClipIdle || ui.clip == PetClipContent);

    /* Cooldown is explained instead of silently replaying the reward. */
    execute(&ui, &pet, press(&ui, &pet, PetKeyOk), true);
    assert(!strcmp(pet_ui_bubble(&ui, now), "Later~") && ui.clip == PetClipContent && !ui.busy);

    /* Pat combos escalate: Hehe~ -> Heh... -> Hey! */
    while(selected(&ui) != PetItemPat)
        press(&ui, &pet, PetKeyLeft);
    const char* expected[] = {"Hehe~", "Hehe~", "Heh...", "Heh...", "Hey!"};
    for(unsigned i = 0; i < 5; ++i) {
        execute(&ui, &pet, press(&ui, &pet, PetKeyOk), true);
        assert(!strcmp(pet_ui_bubble(&ui, now), expected[i]));
        run(&ui, &pet, 4000);
    }
    run(&ui, &pet, 9000);
    execute(&ui, &pet, press(&ui, &pet, PetKeyOk), true);
    assert(!strcmp(pet_ui_bubble(&ui, now), "Hehe~"));
    run(&ui, &pet, 4000);

    /* A failed save is never presented as a success. */
    while(selected(&ui) != PetItemPlay)
        press(&ui, &pet, PetKeyRight);
    WhalePet before = pet;
    now += 200000;
    execute(&ui, &pet, press(&ui, &pet, PetKeyOk), false);
    assert(!strcmp(pet_ui_bubble(&ui, now), "Not saved!") && ui.flash == 0);
    assert(pet.joy == before.joy && pet.xp == before.xp);
    run(&ui, &pet, 4000);

    /* Level-up gets its own celebration after the reaction. */
    pet.xp = 13;
    now += 200000;
    execute(&ui, &pet, press(&ui, &pet, PetKeyOk), true);
    assert(whale_pet_level(&pet) == 2 && ui.pending_level == 2);
    run(&ui, &pet, 4200);
    assert(!strcmp(pet_ui_bubble(&ui, now), "Lv.2!") && ui.clip == PetClipExcited);
    run(&ui, &pet, 3400);

    /* Sleeping swaps the menu to Wake/Stats; self-waking is noticed. */
    while(selected(&ui) != PetItemSleep)
        press(&ui, &pet, PetKeyRight);
    execute(&ui, &pet, press(&ui, &pet, PetKeyOk), true);
    assert(pet.sleeping && ui.menu_count == 2 && selected(&ui) == PetItemWake);
    assert(ui.clip == PetClipSleeping && !ui.busy && pet_ui_menu_visible(&ui, now));
    pet.sleeping = false;
    run(&ui, &pet, 100);
    assert(ui.menu_count == 6 && ui.clip == PetClipWave);
    assert(!strcmp(pet_ui_bubble(&ui, now), "Morning!"));
    run(&ui, &pet, 4000);

    /* Needs speak up during idle and steer the default selection. */
    pet.fullness = 20;
    run_until(&ui, &pet, "Hungry...");
    assert(ui.clip == PetClipWorried);
    assert(selected(&ui) == PetItemFeed);
    pet.fullness = 80;
    pet.joy = 20;
    run_until(&ui, &pet, "Play?");
    assert(selected(&ui) == PetItemPlay);
    pet.joy = 80;

    /* Stats (long OK or menu item) with the expression gallery inside. */
    key(&ui, &pet, PetKeyOk, PetPressLong);
    assert(ui.page == PetPageStats);
    press(&ui, &pet, PetKeyRight);
    assert(ui.page == PetPageFaces && pet_ui_clip(&ui) == PetClipIdle);
    press(&ui, &pet, PetKeyLeft);
    assert(pet_ui_clip(&ui) == PET_CLIP_COUNT - 1);
    press(&ui, &pet, PetKeyRight);
    press(&ui, &pet, PetKeyRight);
    assert(pet_ui_clip(&ui) == PetClipContent);
    assert(pet_ui_frame(&ui, now + 100 * pet_clip_length[PetClipContent]) == 0);
    press(&ui, &pet, PetKeyBack);
    assert(ui.page == PetPageStats);
    press(&ui, &pet, PetKeyBack);
    assert(ui.page == PetPageMain);
    assert(key(&ui, &pet, PetKeyBack, PetPressLong).type == PetCommandExit);

    /* Catch Rice: too tired is refused; otherwise a deterministic round. */
    while(selected(&ui) != PetItemCatch)
        press(&ui, &pet, PetKeyRight);
    pet.energy = 20;
    press(&ui, &pet, PetKeyOk);
    assert(ui.page == PetPageMain && !strcmp(pet_ui_bubble(&ui, now), "Too tired"));
    pet.energy = 90;
    press(&ui, &pet, PetKeyOk);
    assert(ui.page == PetPageGame && ui.game.lives == PetGameLives && ui.clip == PetClipExcited);
    int start_x = ui.game.bowl_x;
    key(&ui, &pet, PetKeyRight, PetPressDown);
    for(unsigned i = 0; i < 4; ++i) {
        now += 50;
        assert(pet_ui_tick(&ui, &pet, 14, now).type == PetCommandNone);
    }
    key(&ui, &pet, PetKeyRight, PetPressUp);
    assert(ui.game.bowl_x == start_x + 4 + 4 * 3); /* tap nudge + 4 held steps */
    /* A tap shorter than one step still moves the bowl. */
    key(&ui, &pet, PetKeyLeft, PetPressDown);
    key(&ui, &pet, PetKeyLeft, PetPressUp);
    assert(ui.game.bowl_x == start_x + 12);
    /* Steer under the lowest falling grain, like a player would. */
    PetCommand over = {.type = PetCommandNone};
    unsigned caught_some = 0, guard = 0;
    while(over.type == PetCommandNone && guard++ < 2000) {
        const PetDrop* target = NULL;
        for(unsigned i = 0; i < PetGameDrops; ++i)
            if(ui.game.drops[i].active && (!target || ui.game.drops[i].y4 > target->y4))
                target = &ui.game.drops[i];
        int centre = ui.game.bowl_x + PetGameBowlW / 2;
        int goal = target ? target->x + 1 : centre;
        bool left = goal < centre - 1, right = goal > centre + 1;
        if(left != ui.game.left) key(&ui, &pet, PetKeyLeft, left ? PetPressDown : PetPressUp);
        if(right != ui.game.right) key(&ui, &pet, PetKeyRight, right ? PetPressDown : PetPressUp);
        now += 50;
        over = pet_ui_tick(&ui, &pet, 14, now);
        caught_some = ui.game.score;
        assert(ui.game.bowl_x >= 0 && ui.game.bowl_x <= PetGameWidth - PetGameBowlW);
    }
    assert(over.type == PetCommandGameOver && ui.game.over && over.score == caught_some);
    assert(caught_some >= 10);
    assert(press(&ui, &pet, PetKeyOk).type == PetCommandNone && ui.page == PetPageGame);
    WhalePet scored = pet;
    const WhalePet game_before = pet;
    WhalePetResult result = whale_pet_game(&scored, over.score, pet.played_at + 120);
    assert(result == WhalePetChanged);
    pet = scored;
    pet_ui_game_scored(&ui, &pet, result, true, &game_before, now);
    assert(!strcmp(ui.game.result, "+Joy  +XP"));
    press(&ui, &pet, PetKeyOk);
    assert(ui.page == PetPageMain && ui.clip == PetClipProud);
    char score[16];
    snprintf(score, sizeof(score), "Score %u", over.score);
    assert(!strcmp(pet_ui_bubble(&ui, now), score));
    /* Back ends a round early with the score so far. */
    press(&ui, &pet, PetKeyOk);
    assert(ui.page == PetPageGame);
    over = press(&ui, &pet, PetKeyBack);
    assert(over.type == PetCommandGameOver && over.score == 0);
    pet_ui_game_scored(&ui, &pet, WhalePetUnchanged, true, &pet, now);
    assert(!strcmp(ui.game.result, "No rice..."));
    printf(
        "PASS: production pet controller, smart menu, busy lock, needs, combos, level-up, "
        "sleep/wake menu, stats/faces and Catch Rice (score %u)\n",
        caught_some);
}
