/* Exercise the production offline adapter with the actual Princeton feed state
 * machine. Storage/FlipperFormat are deterministic; no radio API is provided. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#undef _WIN32
#endif
#include "firmware/flipper/lib/mjs/common/frozen/frozen.h"
#define UNUSED(x) (void)(x)
#define COUNT_OF(x) (sizeof(x)/sizeof((x)[0]))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define RECORD_STORAGE "storage"
#define FSE_OK 0
#define furi_assert(x) assert(x)
#define DURATION_DIFF(x,y) ((x)>(y)?(x)-(y):(y)-(x))
#define PRINCETON_GUARD_TIME_DEFALUT 30
typedef struct {char* text;} FuriString;
typedef struct DeepFlipperApp {bool stop;} DeepFlipperApp;
typedef struct {int value;} Storage;
typedef struct {uint64_t size;} FileInfo;
typedef struct {size_t cursor;} FlipperFormat;
typedef struct SubGhzProtocolRegistry SubGhzProtocolRegistry;
typedef struct {const SubGhzProtocolRegistry* registry;} SubGhzEnvironment;
typedef struct {void* (*alloc)(SubGhzEnvironment*);} SubGhzProtocolDecoder;
typedef struct {const char* name;unsigned flag;const SubGhzProtocolDecoder* decoder;} SubGhzProtocol;
struct SubGhzProtocolRegistry {const SubGhzProtocol* const* items;size_t size;};
typedef struct SubGhzProtocolDecoderBase SubGhzProtocolDecoderBase;
struct SubGhzProtocolDecoderBase {const SubGhzProtocol* protocol;void (*callback)(SubGhzProtocolDecoderBase*,void*);void* context;};
typedef struct {uint32_t parser_step,te_last;uint64_t decode_data;uint8_t decode_count_bit;} SubGhzBlockDecoder;
typedef struct {
    SubGhzProtocolDecoderBase base;
    SubGhzBlockDecoder decoder;
    struct {uint64_t data;uint8_t data_count_bit;} generic;
    uint32_t te,last_data,guard_time;
} SubGhzProtocolDecoderPrinceton;
enum {PrincetonDecoderStepReset,PrincetonDecoderStepSaveDuration,PrincetonDecoderStepCheckDuration};
static const struct {uint32_t te_short,te_long,te_delta,min_count_bit_for_found;}
    subghz_protocol_princeton_const={390,1170,300,24};
#include "princeton-bit-production.inc"
#include "princeton-feed-production.inc"
typedef struct SubGhzReceiver SubGhzReceiver;
struct SubGhzReceiver {
    SubGhzProtocolDecoderPrinceton decoder;
    void (*callback)(SubGhzReceiver*,SubGhzProtocolDecoderBase*,void*);
    void* context;
};
enum {SubGhzProtocolFlag_Decodable=2};
static void* allocator_marker(SubGhzEnvironment* env){return env;}
static const SubGhzProtocolDecoder marker={allocator_marker};
static const SubGhzProtocol princeton={"Princeton",SubGhzProtocolFlag_Decodable,&marker};
static const SubGhzProtocol raw={"RAW",1,&marker};
static const SubGhzProtocol* const registered[]={&raw,&princeton};
static const SubGhzProtocolRegistry subghz_protocol_registry={registered,COUNT_OF(registered)};
static size_t subghz_protocol_registry_count(const SubGhzProtocolRegistry* registry){return registry->size;}
static const SubGhzProtocol* subghz_protocol_registry_get_by_index(const SubGhzProtocolRegistry* registry,size_t i){return registry->items[i];}
static size_t free_heap=64000,max_block=32000;
static size_t memmgr_get_free_heap(void){return free_heap;}
static size_t memmgr_heap_get_max_free_block(void){return max_block;}
static Storage storage;
static int32_t timings[6000];
static size_t total,row_size=128;
static uint64_t file_size=32000;
static bool valid_preset=true;
static unsigned receivers_allocated,receivers_freed;
static FuriString* furi_string_alloc(void){FuriString* s=malloc(sizeof(*s));s->text=strdup("");return s;}
static void furi_string_set(FuriString* s,const char* text){free(s->text);s->text=strdup(text);}
static const char* furi_string_get_cstr(FuriString* s){return s->text;}
static bool furi_string_equal(FuriString* s,const char* text){return !strcmp(s->text,text);}
static void furi_string_free(FuriString* s){free(s->text);free(s);}
bool deepflipper_app_cancelled(DeepFlipperApp* app){return app->stop;}
static Storage* furi_record_open(const char* name){assert(!strcmp(name,RECORD_STORAGE));return &storage;}
static void furi_record_close(const char* name){UNUSED(name);}
static int storage_common_stat(Storage* s,const char* path,FileInfo* info){UNUSED(s);UNUSED(path);info->size=file_size;return FSE_OK;}
static FlipperFormat* flipper_format_file_alloc(Storage* s){UNUSED(s);return calloc(1,sizeof(FlipperFormat));}
static void flipper_format_free(FlipperFormat* f){free(f);}
static bool flipper_format_file_open_existing(FlipperFormat* f,const char* path){UNUSED(f);UNUSED(path);return true;}
static bool flipper_format_read_header(FlipperFormat* f,FuriString* type,uint32_t* v){UNUSED(f);furi_string_set(type,"Flipper SubGhz RAW File");*v=1;return true;}
static bool flipper_format_read_uint32(FlipperFormat* f,const char* key,uint32_t* value,size_t count){UNUSED(f);UNUSED(key);assert(count==1);*value=433920000;return true;}
static bool flipper_format_read_string(FlipperFormat* f,const char* key,FuriString* value){UNUSED(f);
    furi_string_set(value,!strcmp(key,"Preset")?(valid_preset?"FuriHalSubGhzPresetOok650Async":"custom"):"RAW");return true;}
static bool flipper_format_get_value_count(FlipperFormat* f,const char* key,uint32_t* count){UNUSED(key);if(f->cursor>=total)return false;*count=(uint32_t)((total-f->cursor)<row_size?total-f->cursor:row_size);return true;}
static bool flipper_format_read_int32(FlipperFormat* f,const char* key,int32_t* values,uint32_t count){UNUSED(key);assert(f->cursor+count<=total);memcpy(values,timings+f->cursor,count*sizeof(*values));f->cursor+=count;return true;}
static SubGhzEnvironment* subghz_environment_alloc(void){return calloc(1,sizeof(SubGhzEnvironment));}
static void subghz_environment_free(SubGhzEnvironment* e){free(e);}
static void subghz_environment_set_protocol_registry(SubGhzEnvironment* e,const SubGhzProtocolRegistry* registry){assert(registry->size==1&&registry->items[0]==&princeton);e->registry=registry;}
static void receiver_frame(SubGhzProtocolDecoderBase* base,void* context){SubGhzReceiver* r=context;r->callback(r,base,r->context);}
static SubGhzReceiver* subghz_receiver_alloc_init(SubGhzEnvironment* e){assert(e->registry->size==1);assert(receivers_allocated==receivers_freed);++receivers_allocated;SubGhzReceiver* r=calloc(1,sizeof(*r));r->decoder.base.protocol=e->registry->items[0];r->decoder.base.callback=receiver_frame;r->decoder.base.context=r;return r;}
static void subghz_receiver_free(SubGhzReceiver* r){++receivers_freed;free(r);}
static void subghz_receiver_set_filter(SubGhzReceiver* r,int filter){UNUSED(r);assert(filter==SubGhzProtocolFlag_Decodable);}
static void subghz_receiver_set_rx_callback(SubGhzReceiver* r,void (*callback)(SubGhzReceiver*,SubGhzProtocolDecoderBase*,void*),void* context){r->callback=callback;r->context=context;}
static void subghz_receiver_decode(SubGhzReceiver* r,bool level,uint32_t duration){subghz_protocol_decoder_princeton_feed(&r->decoder,level,duration);}
static bool subghz_protocol_decoder_base_get_string(SubGhzProtocolDecoderBase* base,FuriString* text){
    const SubGhzProtocolDecoderPrinceton* decoder=(const SubGhzProtocolDecoderPrinceton*)base;
    char summary[64];snprintf(summary,sizeof(summary),"Key %06llX",(unsigned long long)decoder->generic.data);furi_string_set(text,summary);return true;
}
#include "firmware/flipper/applications/examples/deepflipper_tools/deepflipper_subghz.c"

static void synthetic_princeton(void){
    total=0;timings[total++]=-14040;
    for(int frame=0;frame<3;++frame){
        for(int bit=23;bit>=0;--bit){bool one=(0x123451u>>bit)&1;
            timings[total++]=one?1170:390;timings[total++]=one?-390:-1170;}
        timings[total++]=390;timings[total++]=-11700;
    }
}
static const char* run(DeepFlipperApp* app,const char* args,FuriString* result){
    furi_string_set(result,"{}");const char* code=deepflipper_subghz_decode(app,args,5000,result,NULL);
    assert(receivers_allocated==receivers_freed);return code;
}
int main(void){
    DeepFlipperApp app={0};FuriString* result=furi_string_alloc();
    const char* args="{\"path\":\"/ext/subghz/test.sub\"}";
    synthetic_princeton();assert(!run(&app,args,result));
    assert(strstr(result->text,"\"decoded\":true")&&strstr(result->text,"Princeton")&&strstr(result->text,"123451"));
    assert(strstr(result->text,"\"frames\":2"));
    free_heap=30000;assert(!strcmp(run(&app,args,result),"insufficient_memory"));free_heap=64000;
    max_block=16000;assert(!strcmp(run(&app,args,result),"insufficient_memory"));max_block=32000;
    total=4097;for(size_t i=0;i<total;++i)timings[i]=390;
    assert(!run(&app,args,result));assert(strstr(result->text,"\"truncated\":true")&&strstr(result->text,"\"samples\":4096")&&strstr(result->text,"\"decoded\":false"));
    app.stop=true;assert(!strcmp(run(&app,args,result),"cancelled"));app.stop=false;
    row_size=5000;total=5000;assert(!strcmp(run(&app,args,result),"invalid_raw_data"));row_size=128;
    total=1;timings[0]=INT32_MIN;assert(!strcmp(run(&app,args,result),"invalid_raw_data"));
    total=0;assert(!strcmp(run(&app,args,result),"no_raw_samples"));
    valid_preset=false;assert(!strcmp(run(&app,args,result),"invalid_raw_file"));valid_preset=true;
    file_size=65537;assert(!strcmp(run(&app,args,result),"file_missing_or_too_large"));file_size=32000;
    for(const char** path=(const char*[]){"{\"path\":\"/ext/subghz/../private.sub\"}","{\"path\":\"/ext/nfc/a.sub\"}","{\"path\":\"/ext/subghz/a.txt\"}",NULL};*path;++path)
        assert(!strcmp(run(&app,*path,result),"invalid_path"));
    furi_string_free(result);
    puts("PASS: production offline RF adapter with actual Princeton decoder, known frames, unknown pulses, truncation, invalid input and cleanup");
}
