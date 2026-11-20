#pragma once
#include "pet_model.h"
#include <storage/storage.h>

// Read by the firmware home screen to show hunger/sleep without owning the save.
#define WHALE_PET_HINT_PATH EXT_PATH("deepflipper/pet/home.bin")

typedef enum {
    WhalePetStoreNew,
    WhalePetStoreReady,
    WhalePetStoreRecovered,
    WhalePetStoreDamaged,
    WhalePetStoreUnavailable,
} WhalePetStoreStatus;

typedef struct {
    Storage* storage;
    WhalePet pet;
    int active;
    WhalePetStoreStatus status;
} WhalePetStore;

void whale_pet_store_load(WhalePetStore* store, Storage* storage, uint32_t now);
bool whale_pet_store_commit(WhalePetStore* store, const WhalePet* next);
bool whale_pet_store_writable(const WhalePetStore* store);
void whale_pet_store_publish_hint(const WhalePetStore* store);
