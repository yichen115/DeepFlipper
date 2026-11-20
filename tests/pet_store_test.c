/* Compile the actual save implementation against deterministic SD boundaries. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define EXT_PATH(x) "/ext/" x
typedef enum {FSE_OK, FSE_NOT_EXIST, FSE_NOT_READY} FS_Error;
enum {FSAM_READ, FSAM_WRITE, FSOM_OPEN_EXISTING, FSOM_CREATE_ALWAYS};
typedef struct {uint64_t size;} FileInfo;
typedef struct {int value;} Storage;
typedef struct {int slot; size_t at;} File;
typedef struct {uint8_t bytes[44]; size_t count; bool exists;} Slot;
static Slot slots_in_memory[3];
static Storage storage;
static bool available = true, sync_ok = true, open_ok = true;
static size_t write_limit = 36;
static unsigned writes;
static int slot_index(const char* path) {return strstr(path,"home.bin") ? 2 : strstr(path,"state-a") ? 0 : 1;}
static FS_Error storage_sd_status(Storage* s) {(void)s;return available ? FSE_OK : FSE_NOT_READY;}
static File* storage_file_alloc(Storage* s) {(void)s;return calloc(1,sizeof(File));}
static void storage_file_free(File* f) {free(f);}
static bool storage_file_open(File* f,const char* path,int access,int mode) {
    f->slot=slot_index(path);f->at=0;
    if(!available || (access==FSAM_WRITE && !open_ok))return false;
    if(mode==FSOM_CREATE_ALWAYS){slots_in_memory[f->slot].exists=true;slots_in_memory[f->slot].count=0;}
    return slots_in_memory[f->slot].exists;
}
static uint64_t storage_file_size(File* f){return slots_in_memory[f->slot].count;}
static size_t storage_file_read(File* f,void* out,size_t count){
    Slot* slot=&slots_in_memory[f->slot];
    if(count>slot->count-f->at)count=slot->count-f->at;
    memcpy(out,slot->bytes+f->at,count);f->at+=count;return count;
}
static size_t storage_file_write(File* f,const void* bytes,size_t count){
    ++writes;if(f->slot<2&&count>write_limit)count=write_limit;assert(count<=44);
    memcpy(slots_in_memory[f->slot].bytes,bytes,count);slots_in_memory[f->slot].count=count;return count;
}
static bool storage_file_sync(File* f){(void)f;return sync_ok;}
static FS_Error storage_common_stat(Storage* s,const char* path,FileInfo* info){
    (void)s;Slot* slot=&slots_in_memory[slot_index(path)];
    if(!available)return FSE_NOT_READY;
    info->size=slot->count;return slot->exists ? FSE_OK : FSE_NOT_EXIST;
}
static void storage_common_mkdir(Storage* s,const char* path){(void)s;(void)path;}
#include "firmware/flipper/applications/examples/whale_pet/pet_store.c"

int main(void){
    WhalePetStore store;
    whale_pet_store_load(&store,&storage,100000);
    assert(store.status==WhalePetStoreNew&&store.active==-1&&writes==0);
    assert(whale_pet_store_commit(&store,&store.pet));
    assert(store.active==0&&store.pet.revision==1);
    Slot original=slots_in_memory[0];
    WhalePet next=store.pet;
    whale_pet_apply(&next,WhalePetFeed,100010);
    // Every torn-write prefix preserves the previously committed pet.
    for(size_t prefix=0;prefix<36;++prefix){
        write_limit=prefix;
        assert(!whale_pet_store_commit(&store,&next));
        assert(store.active==0&&store.pet.fullness==72);
        assert(!memcmp(&original,&slots_in_memory[0],sizeof(Slot)));
        WhalePetStore recovered;
        whale_pet_store_load(&recovered,&storage,100020);
        assert(recovered.status==WhalePetStoreRecovered&&recovered.active==0);
    }
    write_limit=36;
    assert(whale_pet_store_commit(&store,&next));
    assert(store.active==1&&store.pet.fullness==94&&store.pet.revision==2);
    // Each verified commit also publishes the home-screen forecast.
    Slot* hint=&slots_in_memory[2];
    assert(hint->exists&&hint->count==44&&!memcmp(hint->bytes,"DFH2",4)&&hint->bytes[8]==WhalePetWishNone);
    uint32_t hungry=hint->bytes[9]|hint->bytes[10]<<8|hint->bytes[11]<<16|(uint32_t)hint->bytes[12]<<24;
    // 94 fullness falls below 40 after 28 ten-minute periods of being awake.
    assert(hungry==store.pet.updated+28*600&&hint->bytes[13]==WhalePetWishHungry);
    whale_pet_store_load(&store,&storage,100030);
    assert(store.active==1&&store.status==WhalePetStoreReady&&store.pet.fullness==94);
    open_ok=false;next=store.pet;next.joy=90;
    assert(!whale_pet_store_commit(&store,&next)&&store.pet.joy!=90);open_ok=true;
    sync_ok=false;assert(!whale_pet_store_commit(&store,&next));sync_ok=true;
    // A failed sync is uncertain; never report it as a successful write.
    slots_in_memory[0].bytes[4]^=1;
    slots_in_memory[1].bytes[4]^=1;
    whale_pet_store_load(&store,&storage,100040);
    assert(store.status==WhalePetStoreDamaged&&!whale_pet_store_writable(&store));
    unsigned prior=writes;
    assert(!whale_pet_store_commit(&store,&next)&&prior==writes);
    available=false;whale_pet_store_load(&store,&storage,100050);
    assert(store.status==WhalePetStoreUnavailable&&!whale_pet_store_commit(&store,&next));
    available=true;
    next.revision=UINT32_MAX;whale_pet_encode(&next,slots_in_memory[0].bytes);
    slots_in_memory[0].count=36;slots_in_memory[0].exists=true;
    next.revision=0;whale_pet_encode(&next,slots_in_memory[1].bytes);
    slots_in_memory[1].count=36;slots_in_memory[1].exists=true;
    whale_pet_store_load(&store,&storage,100050);assert(store.active==1);
    puts("PASS: production pet two-slot storage, home forecast publish, every torn-write prefix, read-back verification, corrupt saves, missing SD and revision wrap");
}
