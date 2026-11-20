#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "firmware/flipper/applications/examples/whale_pet/pet_model.h"

int main(void) {
    WhalePet pet;
    whale_pet_init(&pet, 100000);
    assert(pet.fullness == 72 && pet.energy == 80 && !pet.sleeping);
    assert(whale_pet_apply(&pet, WhalePetFeed, 100000) == WhalePetChanged);
    assert(pet.fullness == 94 && pet.xp == 4 && pet.bond == 2);
    assert(whale_pet_apply(&pet, WhalePetFeed, 100010) == WhalePetCooldown);
    assert(pet.xp == 4 && pet.fullness == 94);
    assert(whale_pet_apply(&pet, WhalePetPat, 100010) == WhalePetChanged);
    assert(whale_pet_apply(&pet, WhalePetPat, 100011) == WhalePetCooldown);
    assert(pet.xp == 6);
    assert(whale_pet_apply(&pet, WhalePetPlay, 100020) == WhalePetChanged);
    assert(pet.energy == 81 && pet.xp == 11); /* rice +6, play -5 */
    assert(whale_pet_apply(&pet, WhalePetFeed, 100301) == WhalePetChanged);
    assert(pet.fullness == 100 && whale_pet_level(&pet) == 2);
    assert(whale_pet_apply(&pet, WhalePetFeed, 100601) == WhalePetFull);
    assert(pet.fullness <= 100 && pet.joy <= 100);
    pet.energy = 24;
    assert(whale_pet_apply(&pet, WhalePetPlay, 100602) == WhalePetTired);
    assert(whale_pet_apply(&pet, WhalePetRest, 100603) == WhalePetChanged);
    assert(whale_pet_apply(&pet, WhalePetFeed, 100604) == WhalePetAsleep);
    assert(whale_pet_apply(&pet, WhalePetRest, 100604) == WhalePetUnchanged);
    assert(whale_pet_advance(&pet, 101203));
    assert(pet.energy == 39 && pet.sleeping); /* +15 per 10 min asleep */
    assert(whale_pet_apply(&pet, WhalePetWake, 101203) == WhalePetChanged);
    const uint16_t earned_xp = pet.xp, bond = pet.bond;
    whale_pet_advance(&pet, 101203 + 30 * 86400);
    assert(pet.energy >= 20 && pet.fullness >= 20 && pet.joy >= 20);
    assert(pet.xp == earned_xp && pet.bond == bond);
    assert(whale_pet_advance(&pet, 90000));
    assert(pet.updated == 90000 && pet.fed_at == 0);
    assert(!whale_pet_advance(&pet, 90599));
    assert(whale_pet_advance(&pet, 90600));
    pet.xp = 9998;
    pet.bond = 999;
    assert(whale_pet_apply(&pet, WhalePetPat, 90600) == WhalePetChanged);
    assert(pet.xp == 9999 && pet.bond == 1000 && whale_pet_level(&pet) == 5);

    // She wakes up by herself once fully rested, in the middle of a catch-up.
    WhalePet nap;
    whale_pet_init(&nap, 200000);
    nap.energy = 70;
    assert(whale_pet_apply(&nap, WhalePetRest, 200000) == WhalePetChanged);
    assert(whale_pet_advance(&nap, 200000 + 6 * 600));
    assert(!nap.sleeping && nap.energy == 96 && nap.fullness == 62 && nap.joy == 66);
    // Needs follow the published thresholds and priority.
    nap.fullness = 39;
    assert(whale_pet_need(&nap) == WhalePetHungry);
    nap.fullness = 80; nap.energy = 29;
    assert(whale_pet_need(&nap) == WhalePetSleepy);
    nap.energy = 80; nap.joy = 39;
    assert(whale_pet_need(&nap) == WhalePetBored);
    nap.joy = 80;
    assert(whale_pet_need(&nap) == WhalePetNeedNone);
    nap.sleeping = true; nap.fullness = 10;
    assert(whale_pet_need(&nap) == WhalePetNeedNone);
    // Catch Rice: shares play cooldown and tiredness, score-bounded reward.
    WhalePet game;
    whale_pet_init(&game, 300000);
    assert(whale_pet_game(&game, 0, 300000) == WhalePetUnchanged && game.xp == 0);
    assert(whale_pet_game(&game, 99, 300000) == WhalePetChanged);
    assert(game.energy == 75 && game.joy == 86 && game.xp == 8 && game.bond == 3);
    assert(whale_pet_game(&game, 5, 300060) == WhalePetCooldown && game.xp == 8);
    assert(whale_pet_apply(&game, WhalePetPlay, 300060) == WhalePetCooldown);
    game.energy = 24;
    assert(whale_pet_game(&game, 5, 300500) == WhalePetTired);
    game.sleeping = true;
    assert(whale_pet_game(&game, 5, 300500) == WhalePetAsleep);
    // Level progress.
    unsigned have, need;
    game.xp = 60;
    whale_pet_level_progress(&game, &have, &need);
    assert(whale_pet_level(&game) == 3 && have == 10 && need == 70);
    game.xp = 14; assert(whale_pet_level(&game) == 1);
    game.xp = 15; assert(whale_pet_level(&game) == 2);
    game.xp = 240; whale_pet_level_progress(&game, &have, &need);
    assert(whale_pet_level(&game) == 5 && have == need);
    // Home timeline replays the real rules, including waking and the 6 h cap.
    WhalePetHint hint;
    WhalePet seen;
    whale_pet_init(&seen, 400000);
    whale_pet_forecast(&seen, &hint);
    /* 72 -> below 40 after 17 steps; joy 70 -> below 40 after 31 steps, but
       hunger keeps priority; the 6 h catch-up cap ends the decline at 36. */
    assert(hint.count == 2);
    assert(hint.at[0] == 400000 && hint.wish[0] == WhalePetWishNone);
    assert(hint.at[1] == 400000 + 17 * 600 && hint.wish[1] == WhalePetWishHungry);
    seen.sleeping = true;
    seen.energy = 60;
    whale_pet_forecast(&seen, &hint);
    assert(hint.wish[0] == WhalePetWishSleeping);
    assert(hint.at[1] == 400000 + 3 * 600 && hint.wish[1] == WhalePetWishNone);
    seen.sleeping = false;
    seen.fullness = 80;
    seen.joy = 41;
    whale_pet_forecast(&seen, &hint);
    assert(hint.wish[0] == WhalePetWishNone && hint.wish[1] == WhalePetWishBored);
    assert(hint.at[1] == 400000 + 2 * 600);
    assert(hint.wish[2] == WhalePetWishHungry && hint.at[2] == 400000 + 21 * 600);
    uint8_t hint_bytes[WhalePetHintBytes];
    whale_pet_hint_encode(&hint, hint_bytes);
    assert(!memcmp(hint_bytes, "DFH2", 4));
    assert(hint_bytes[4] == (uint8_t)400000 && hint_bytes[7] == (uint8_t)(400000 >> 24));
    assert(hint_bytes[8] == WhalePetWishNone && hint_bytes[13] == WhalePetWishBored);
    assert(hint_bytes[4 + 7 * 5] == 0xff && hint_bytes[4 + 7 * 5 + 3] == 0xff && hint_bytes[43] == 0);

    uint8_t bytes[WhalePetSaveBytes];
    whale_pet_encode(&pet, bytes);
    WhalePet decoded = {0};
    assert(whale_pet_decode(&decoded, bytes, sizeof(bytes)));
    uint8_t again[WhalePetSaveBytes];
    whale_pet_encode(&decoded, again);
    assert(!memcmp(bytes, again, sizeof(bytes)));
    for(unsigned i = 0; i < sizeof(bytes); ++i) {
        bytes[i] ^= 1;
        assert(!whale_pet_decode(&decoded, bytes, sizeof(bytes)));
        bytes[i] ^= 1;
    }
    for(unsigned i = 0; i < sizeof(bytes); ++i)
        assert(!whale_pet_decode(&decoded, bytes, i));
    pet.fullness = 101;
    whale_pet_encode(&pet, bytes);
    assert(!whale_pet_decode(&decoded, bytes, sizeof(bytes)));
    puts("PASS: offline pet care, self-waking sleep, needs, Catch Rice rewards, level progress, home forecast, white-rice feeding, cooldowns, sleep recovery, bounded absence, RTC rollback, growth and corrupt-save rejection");
}
