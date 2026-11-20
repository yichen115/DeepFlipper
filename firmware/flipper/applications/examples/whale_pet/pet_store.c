#include "pet_store.h"
#include <string.h>

static const char* const slots[] = {
    EXT_PATH("deepflipper/pet/state-a.bin"),
    EXT_PATH("deepflipper/pet/state-b.bin")};

// Keep the last valid slot intact. A torn write to the other slot is rejected by
// length/CRC on next load. Storage's rename is copy+delete, not an atomic replace.
static bool read_slot(Storage* storage, unsigned slot, WhalePet* pet) {
    File* file = storage_file_alloc(storage);
    uint8_t bytes[WhalePetSaveBytes];
    bool okay = storage_file_open(file, slots[slot], FSAM_READ, FSOM_OPEN_EXISTING) &&
                storage_file_size(file) == sizeof(bytes) &&
                storage_file_read(file, bytes, sizeof(bytes)) == sizeof(bytes) &&
                whale_pet_decode(pet, bytes, sizeof(bytes));
    storage_file_free(file);
    return okay;
}

void whale_pet_store_load(WhalePetStore* store, Storage* storage, uint32_t now) {
    *store = (WhalePetStore){.storage = storage, .active = -1, .status = WhalePetStoreNew};
    whale_pet_init(&store->pet, now);
    if(storage_sd_status(storage) != FSE_OK) {
        store->status = WhalePetStoreUnavailable;
        return;
    }
    WhalePet candidates[2];
    bool valid[2] = {false, false}, bad = false;
    for(unsigned i = 0; i < 2; ++i) {
        FileInfo info;
        FS_Error status = storage_common_stat(storage, slots[i], &info);
        if(status == FSE_NOT_EXIST) continue;
        valid[i] = status == FSE_OK && read_slot(storage, i, &candidates[i]);
        bad |= !valid[i];
    }
    if(valid[0] || valid[1]) {
        store->active = valid[0] && valid[1] ?
                            ((int32_t)(candidates[1].revision - candidates[0].revision) > 0) :
                            (valid[1] ? 1 : 0);
        store->pet = candidates[store->active];
        store->status = bad ? WhalePetStoreRecovered : WhalePetStoreReady;
    } else if(bad) {
        // Never silently replace a damaged pet with a newborn.
        store->status = WhalePetStoreDamaged;
    }
}

bool whale_pet_store_writable(const WhalePetStore* store) {
    return store->status <= WhalePetStoreRecovered;
}

bool whale_pet_store_commit(WhalePetStore* store, const WhalePet* next) {
    if(!whale_pet_store_writable(store)) return false;
    WhalePet saved = *next, verified;
    saved.revision = store->pet.revision + 1;
    uint8_t bytes[WhalePetSaveBytes];
    whale_pet_encode(&saved, bytes);
    if(!whale_pet_decode(&verified, bytes, sizeof(bytes))) return false;
    storage_common_mkdir(store->storage, EXT_PATH("deepflipper"));
    storage_common_mkdir(store->storage, EXT_PATH("deepflipper/pet"));
    unsigned target = store->active == 0 ? 1 : 0;
    File* file = storage_file_alloc(store->storage);
    bool okay = storage_file_open(file, slots[target], FSAM_WRITE, FSOM_CREATE_ALWAYS) &&
                storage_file_write(file, bytes, sizeof(bytes)) == sizeof(bytes) &&
                storage_file_sync(file);
    storage_file_free(file);
    uint8_t actual[WhalePetSaveBytes];
    if(okay && read_slot(store->storage, target, &verified)) {
        whale_pet_encode(&verified, actual);
        okay = memcmp(bytes, actual, sizeof(bytes)) == 0;
    } else {
        okay = false;
    }
    if(okay) {
        store->active = target;
        store->pet = saved;
        store->status = WhalePetStoreReady;
        whale_pet_store_publish_hint(store);
    }
    return okay;
}

void whale_pet_store_publish_hint(const WhalePetStore* store) {
    // Best effort, read-only for the home screen. The save slots stay the truth.
    WhalePetHint hint;
    uint8_t bytes[WhalePetHintBytes];
    whale_pet_forecast(&store->pet, &hint);
    whale_pet_hint_encode(&hint, bytes);
    File* file = storage_file_alloc(store->storage);
    if(storage_file_open(file, WHALE_PET_HINT_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS))
        storage_file_write(file, bytes, sizeof(bytes));
    storage_file_free(file);
}
