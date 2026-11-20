#include "pet_model.h"
#include <string.h>

static unsigned cap(unsigned value, unsigned maximum) {
    return value > maximum ? maximum : value;
}

static uint8_t decrease(uint8_t value, unsigned amount) {
    // Absence never kills the pet or erases earned affection and progress.
    return value <= 20 || amount >= (unsigned)(value - 20) ? 20 : value - amount;
}

void whale_pet_init(WhalePet* pet, uint32_t now) {
    *pet = (WhalePet){.fullness = 72, .energy = 80, .joy = 70, .updated = now};
}

bool whale_pet_advance(WhalePet* pet, uint32_t now) {
    if(now < pet->updated) {
        pet->updated = now;
        pet->fed_at = pet->patted_at = pet->played_at = 0;
        return true;
    }
    uint32_t periods = (now - pet->updated) / 600;
    if(!periods) return false;
    // At most six hours of catch-up; long absences cannot exhaust the pet.
    unsigned steps = cap(periods, 36);
    for(unsigned step = 0; step < steps; ++step) {
        if(pet->sleeping) {
            pet->fullness = decrease(pet->fullness, 1);
            // About 50 minutes of sleep restore a tired pet (25 -> 100).
            pet->energy = cap(pet->energy + 15, 100);
            // Fully rested: she wakes up by herself instead of sleeping forever.
            if(pet->energy >= 100) pet->sleeping = false;
        } else {
            pet->fullness = decrease(pet->fullness, 2);
            pet->energy = decrease(pet->energy, 1);
            pet->joy = decrease(pet->joy, 1);
        }
    }
    pet->updated += periods * 600;
    return true;
}

static bool ready(uint32_t previous, uint32_t now, unsigned seconds) {
    return !previous || now < previous || now - previous >= seconds;
}

static void reward(WhalePet* pet, unsigned xp, unsigned bond) {
    pet->xp = cap(pet->xp + xp, 9999);
    pet->bond = cap(pet->bond + bond, 1000);
}

WhalePetResult whale_pet_apply(WhalePet* pet, WhalePetAction action, uint32_t now) {
    whale_pet_advance(pet, now);
    if(action == WhalePetRest || action == WhalePetWake) {
        bool sleeping = action == WhalePetRest;
        if(pet->sleeping == sleeping) return WhalePetUnchanged;
        // Settle elapsed time in the old mode before starting the next mode.
        pet->sleeping = sleeping;
        pet->updated = now;
        return WhalePetChanged;
    }
    if(pet->sleeping) return WhalePetAsleep;
    if(action == WhalePetPat) {
        if(!ready(pet->patted_at, now, 60)) return WhalePetCooldown;
        pet->patted_at = now;
        pet->joy = cap(pet->joy + 8, 100);
        reward(pet, 2, 2);
    } else if(action == WhalePetFeed) {
        if(pet->fullness >= 95) return WhalePetFull;
        if(!ready(pet->fed_at, now, 300)) return WhalePetCooldown;
        pet->fed_at = now;
        pet->fullness = cap(pet->fullness + 22, 100);
        pet->joy = cap(pet->joy + 4, 100);
        pet->energy = cap(pet->energy + 6, 100); // a warm bowl of rice perks her up
        reward(pet, 4, 2);
    } else if(action == WhalePetPlay) {
        if(pet->energy < 25) return WhalePetTired;
        if(!ready(pet->played_at, now, 120)) return WhalePetCooldown;
        pet->played_at = now;
        pet->energy -= 5;
        pet->joy = cap(pet->joy + 12, 100);
        reward(pet, 5, 3);
    } else {
        return WhalePetUnchanged;
    }
    return WhalePetChanged;
}

WhalePetResult whale_pet_game(WhalePet* pet, unsigned score, uint32_t now) {
    whale_pet_advance(pet, now);
    if(pet->sleeping) return WhalePetAsleep;
    if(pet->energy < 25) return WhalePetTired;
    if(!ready(pet->played_at, now, 120)) return WhalePetCooldown;
    if(!score) return WhalePetUnchanged;
    pet->played_at = now;
    pet->energy -= 5;
    pet->joy = cap(pet->joy + 6 + cap(score, 20) / 2, 100);
    reward(pet, 2 + cap(score, 24) / 4, 3);
    return WhalePetChanged;
}

WhalePetNeed whale_pet_need(const WhalePet* pet) {
    if(pet->sleeping) return WhalePetNeedNone;
    if(pet->fullness < WhalePetNeedFood) return WhalePetHungry;
    if(pet->energy < WhalePetNeedEnergy) return WhalePetSleepy;
    if(pet->joy < WhalePetNeedJoy) return WhalePetBored;
    return WhalePetNeedNone;
}

static const uint16_t level_xp[WhalePetLevelMax] = {0, 15, 50, 120, 240};

unsigned whale_pet_level(const WhalePet* pet) {
    unsigned level = 1;
    while(level < WhalePetLevelMax && pet->xp >= level_xp[level])
        ++level;
    return level;
}

void whale_pet_level_progress(const WhalePet* pet, unsigned* have, unsigned* need) {
    unsigned level = whale_pet_level(pet);
    if(level >= WhalePetLevelMax) {
        *have = *need = 1;
        return;
    }
    *have = pet->xp - level_xp[level - 1];
    *need = level_xp[level] - level_xp[level - 1];
}

static WhalePetWish wish_of(const WhalePet* pet) {
    if(pet->sleeping) return WhalePetWishSleeping;
    switch(whale_pet_need(pet)) {
    case WhalePetHungry:
        return WhalePetWishHungry;
    case WhalePetSleepy:
        return WhalePetWishSleepy;
    case WhalePetBored:
        return WhalePetWishBored;
    default:
        return WhalePetWishNone;
    }
}

void whale_pet_forecast(const WhalePet* pet, WhalePetHint* hint) {
    memset(hint, 0, sizeof(*hint));
    // Replay the real catch-up rule at each future 10-minute boundary and keep
    // only the changes, so the firmware just picks the last entry already due.
    for(uint32_t k = 0; k <= 288 && hint->count < WhalePetHintEntries; ++k) {
        uint32_t at = pet->updated + k * 600;
        if(at < pet->updated) break;
        WhalePet next = *pet;
        whale_pet_advance(&next, at);
        WhalePetWish wish = wish_of(&next);
        if(hint->count && hint->wish[hint->count - 1] == wish) continue;
        hint->at[hint->count] = at;
        hint->wish[hint->count] = wish;
        ++hint->count;
    }
}

static void put32(uint8_t* bytes, uint32_t value) {
    for(unsigned i = 0; i < 4; ++i)
        bytes[i] = value >> (8 * i);
}

static uint32_t get32(const uint8_t* bytes) {
    uint32_t value = 0;
    for(unsigned i = 0; i < 4; ++i)
        value |= (uint32_t)bytes[i] << (8 * i);
    return value;
}

static uint32_t checksum(const uint8_t* bytes, size_t count) {
    uint32_t crc = UINT32_MAX;
    for(size_t i = 0; i < count; ++i) {
        crc ^= bytes[i];
        for(unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1)));
    }
    return ~crc;
}

void whale_pet_encode(const WhalePet* pet, uint8_t bytes[WhalePetSaveBytes]) {
    memcpy(bytes, "DFP1", 4);
    bytes[4] = pet->fullness;
    bytes[5] = pet->energy;
    bytes[6] = pet->joy;
    bytes[7] = pet->sleeping;
    bytes[8] = pet->bond;
    bytes[9] = pet->bond >> 8;
    bytes[10] = pet->xp;
    bytes[11] = pet->xp >> 8;
    put32(bytes + 12, pet->updated);
    put32(bytes + 16, pet->fed_at);
    put32(bytes + 20, pet->patted_at);
    put32(bytes + 24, pet->played_at);
    put32(bytes + 28, pet->revision);
    put32(bytes + 32, checksum(bytes, 32));
}

void whale_pet_hint_encode(const WhalePetHint* hint, uint8_t bytes[WhalePetHintBytes]) {
    memcpy(bytes, "DFH2", 4);
    for(unsigned i = 0; i < WhalePetHintEntries; ++i) {
        bool used = i < hint->count;
        put32(bytes + 4 + i * 5, used ? hint->at[i] : UINT32_MAX);
        bytes[8 + i * 5] = used ? hint->wish[i] : WhalePetWishNone;
    }
}

bool whale_pet_decode(WhalePet* pet, const uint8_t* bytes, size_t size) {
    if(size != WhalePetSaveBytes || memcmp(bytes, "DFP1", 4) ||
       get32(bytes + 32) != checksum(bytes, 32))
        return false;
    WhalePet next = {
        .fullness = bytes[4],
        .energy = bytes[5],
        .joy = bytes[6],
        .sleeping = bytes[7] != 0,
        .bond = bytes[8] | (uint16_t)bytes[9] << 8,
        .xp = bytes[10] | (uint16_t)bytes[11] << 8,
        .updated = get32(bytes + 12),
        .fed_at = get32(bytes + 16),
        .patted_at = get32(bytes + 20),
        .played_at = get32(bytes + 24),
        .revision = get32(bytes + 28),
    };
    if(next.fullness > 100 || next.energy > 100 || next.joy > 100 || bytes[7] > 1 ||
       next.bond > 1000 || next.xp > 9999)
        return false;
    *pet = next;
    return true;
}
