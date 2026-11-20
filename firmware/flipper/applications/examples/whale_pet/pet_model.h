#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    WhalePetSaveBytes = 36,
    WhalePetHintBytes = 44,
    WhalePetHintEntries = 8,
    // A need shows once its value falls below these thresholds.
    WhalePetNeedFood = 40,
    WhalePetNeedEnergy = 30,
    WhalePetNeedJoy = 40,
    WhalePetLevelMax = 5,
};
typedef struct {
    uint8_t fullness, energy, joy;
    bool sleeping;
    uint16_t bond, xp;
    uint32_t updated, fed_at, patted_at, played_at, revision;
} WhalePet;

typedef enum {
    WhalePetPat,
    WhalePetFeed,
    WhalePetPlay,
    WhalePetRest,
    WhalePetWake,
} WhalePetAction;

typedef enum {
    WhalePetChanged,
    WhalePetCooldown,
    WhalePetFull,
    WhalePetTired,
    WhalePetAsleep,
    WhalePetUnchanged,
} WhalePetResult;

typedef enum {
    WhalePetNeedNone,
    WhalePetHungry,
    WhalePetSleepy,
    WhalePetBored,
} WhalePetNeed;

// What the home screen shows for the pet at a given time.
typedef enum {
    WhalePetWishNone,
    WhalePetWishSleeping,
    WhalePetWishHungry,
    WhalePetWishSleepy,
    WhalePetWishBored,
} WhalePetWish;

// Read-only timeline for the home screen: from `at[i]` (RTC timestamp) on,
// the pet shows `wish[i]`. Entries are sorted and cover the next 48 hours.
typedef struct {
    uint32_t at[WhalePetHintEntries];
    uint8_t wish[WhalePetHintEntries];
    uint8_t count;
} WhalePetHint;

void whale_pet_init(WhalePet* pet, uint32_t now);
bool whale_pet_advance(WhalePet* pet, uint32_t now);
WhalePetResult whale_pet_apply(WhalePet* pet, WhalePetAction action, uint32_t now);
// Catch Rice mini-game result. Shares the play cooldown and energy rules.
WhalePetResult whale_pet_game(WhalePet* pet, unsigned score, uint32_t now);
WhalePetNeed whale_pet_need(const WhalePet* pet);
unsigned whale_pet_level(const WhalePet* pet);
// XP earned inside the current level and XP needed to reach the next one.
void whale_pet_level_progress(const WhalePet* pet, unsigned* have, unsigned* need);
void whale_pet_forecast(const WhalePet* pet, WhalePetHint* hint);
void whale_pet_hint_encode(const WhalePetHint* hint, uint8_t bytes[WhalePetHintBytes]);
void whale_pet_encode(const WhalePet* pet, uint8_t bytes[WhalePetSaveBytes]);
bool whale_pet_decode(WhalePet* pet, const uint8_t* bytes, size_t size);
