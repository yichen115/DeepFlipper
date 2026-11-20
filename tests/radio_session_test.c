/* Real retained radio adapter with deterministic radio/storage boundaries. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#undef _WIN32
#endif
#include "firmware/flipper/lib/mjs/common/frozen/frozen.h"
#define UNUSED(x) (void)(x)
#define EXT_PATH(x) "/ext/" x
#define RECORD_STORAGE "storage"
#define MIN(a,b) ((a)<(b)?(a):(b))
#define FuriHalSubGhzPresetOok650Async 1
typedef struct {char* text;} FuriString;
typedef struct DeepFlipperApp {bool retained,stop;} DeepFlipperApp;
typedef struct {int value;} SubGhzDevice;
typedef struct {int value;} Storage;
typedef struct {int value;} FlipperFormat;
static const SubGhzDevice device;
static Storage storage;
static FlipperFormat file;
static bool initialized,begun,receiving,begin_ok=true,new_file=true,write_ok=true;
static unsigned stopped,ended,deinitialized,written_samples,removed;
static size_t free_heap=64000,max_block=32000;
static size_t memmgr_get_free_heap(void){return free_heap;}
static size_t memmgr_heap_get_max_free_block(void){return max_block;}
static void (*producer)(bool,uint32_t,void*);
static void* producer_context;
static void furi_string_set(FuriString* s,const char* text){free(s->text);s->text=strdup(text);}
bool deepflipper_app_retained(const DeepFlipperApp* app){return app->retained;}
bool deepflipper_app_cancelled(DeepFlipperApp* app){return app->stop;}
const char* deepflipper_app_request_id(const DeepFlipperApp* app){UNUSED(app);return "123_1";}
static void subghz_devices_init(void){assert(!initialized);initialized=true;}
static const SubGhzDevice* subghz_devices_get_by_name(const char* name){assert(!strcmp(name,"cc1101_int"));return &device;}
static bool subghz_devices_is_frequency_valid(const SubGhzDevice* d,uint32_t hz){assert(d==&device);return hz==433920000;}
static bool subghz_devices_begin(const SubGhzDevice* d){assert(d==&device&&initialized&&!begun);if(begin_ok)begun=true;return begin_ok;}
static void subghz_devices_reset(const SubGhzDevice* d){assert(d==&device&&begun);}
static void subghz_devices_load_preset(const SubGhzDevice* d,int preset,void* data){assert(d==&device&&preset==1&&!data&&begun);}
static uint32_t subghz_devices_set_frequency(const SubGhzDevice* d,uint32_t hz){assert(d==&device&&begun);return hz;}
static void subghz_devices_start_async_rx(const SubGhzDevice* d,void (*callback)(bool,uint32_t,void*),void* context){assert(d==&device&&begun&&!receiving);producer=callback;producer_context=context;receiving=true;}
static void subghz_devices_stop_async_rx(const SubGhzDevice* d){assert(d==&device&&receiving);receiving=false;producer=NULL;++stopped;}
static void subghz_devices_sleep(const SubGhzDevice* d){assert(d==&device&&begun&&!receiving);}
static void subghz_devices_end(const SubGhzDevice* d){assert(d==&device&&begun&&!receiving);begun=false;++ended;}
static void subghz_devices_deinit(void){assert(initialized&&!receiving&&!begun);initialized=false;++deinitialized;}
static float subghz_devices_get_rssi(const SubGhzDevice* d){assert(d==&device&&receiving);return -60;}
static Storage* furi_record_open(const char* name){assert(!strcmp(name,RECORD_STORAGE));return &storage;}
static void furi_record_close(const char* name){UNUSED(name);}
static void storage_common_mkdir(Storage* s,const char* path){assert(s==&storage&&!strcmp(path,"/ext/subghz"));}
static FlipperFormat* flipper_format_file_alloc(Storage* s){assert(s==&storage);return &file;}
static bool flipper_format_file_open_new(FlipperFormat* f,const char* path){assert(f==&file&&!strcmp(path,"/ext/subghz/deepflipper_123_1.sub"));return new_file;}
static bool flipper_format_write_header_cstr(FlipperFormat* f,const char* type,uint32_t version){assert(f==&file&&!strcmp(type,"Flipper SubGhz RAW File")&&version==1);return write_ok;}
static bool flipper_format_write_uint32(FlipperFormat* f,const char* key,uint32_t* value,unsigned count){assert(f==&file&&!strcmp(key,"Frequency")&&*value==433920000&&count==1);return write_ok;}
static bool flipper_format_write_string_cstr(FlipperFormat* f,const char* key,const char* value){UNUSED(key);UNUSED(value);assert(f==&file);return write_ok;}
static bool flipper_format_write_int32(FlipperFormat* f,const char* key,int32_t* values,unsigned count){assert(f==&file&&!strcmp(key,"RAW_Data")&&values&&count<=128&&!receiving);written_samples+=count;return write_ok;}
static void flipper_format_free(FlipperFormat* f){assert(f==&file);}
static void storage_common_remove(Storage* s,const char* path){assert(s==&storage&&strstr(path,".sub"));++removed;}
#include "firmware/flipper/applications/examples/deepflipper_tools/deepflipper_radio.c"

static void signal(DeepFlipperRadio* radio,DeepFlipperApp* app,unsigned count){
    deepflipper_radio_tick(app,radio);
    for(unsigned i=0;i<count;++i)producer(i%2==0,390,producer_context);
}
int main(void){
    DeepFlipperApp app={.retained=true};DeepFlipperRadio* radio=deepflipper_radio_alloc();
    FuriString out={.text=strdup("")};const char* args="{\"frequency\":433920000}";
    app.retained=false;assert(!strcmp(deepflipper_radio_start(&app,args,500,&out,radio),"session_required"));app.retained=true;
    assert(!strcmp(deepflipper_radio_start(&app,"{\"frequency\":-1}",500,&out,radio),"invalid_frequency"));
    free_heap=20000;assert(!strcmp(deepflipper_radio_start(&app,args,500,&out,radio),"insufficient_memory"));free_heap=64000;
    max_block=16000;assert(!strcmp(deepflipper_radio_start(&app,args,500,&out,radio),"insufficient_memory"));max_block=32000;
    assert(!deepflipper_radio_start(&app,args,500,&out,radio)&&receiving&&deepflipper_radio_owned(radio));
    producer(true,390,producer_context);assert(atomic_load(&radio->samples)==0);
    signal(radio,&app,8);assert(!deepflipper_radio_state(&app,"{}",500,&out,radio)&&strstr(out.text,"\"samples\":8"));
    assert(!strcmp(deepflipper_radio_start(&app,args,500,&out,radio),"radio_already_owned"));
    assert(!deepflipper_radio_stop(&app,"{}",500,&out,radio)&&written_samples==8&&!receiving&&!deepflipper_radio_owned(radio));
    assert(stopped==1&&ended==1&&deinitialized==1&&strstr(out.text,"123_1.sub"));
    assert(!deepflipper_radio_start(&app,args,500,&out,radio));signal(radio,&app,4096);
    deepflipper_radio_tick(&app,radio);assert(!receiving&&radio->full&&deepflipper_radio_owned(radio));
    assert(!deepflipper_radio_stop(&app,"{\"save\":false}",500,&out,radio)&&written_samples==8);
    assert(!deepflipper_radio_start(&app,args,500,&out,radio));app.stop=true;deepflipper_radio_tick(&app,radio);app.stop=false;
    assert(!receiving&&!deepflipper_radio_owned(radio));
    begin_ok=false;assert(!strcmp(deepflipper_radio_start(&app,args,500,&out,radio),"radio_unavailable_or_invalid_frequency"));begin_ok=true;
    assert(!initialized&&!begun&&!deepflipper_radio_owned(radio));
    assert(!deepflipper_radio_start(&app,args,500,&out,radio));signal(radio,&app,8);
    assert(!strcmp(deepflipper_radio_stop(&app,"{\"save\":1}",500,&out,radio),"invalid_save")&&!deepflipper_radio_owned(radio));
    assert(!deepflipper_radio_start(&app,args,500,&out,radio));signal(radio,&app,8);write_ok=false;
    assert(!strcmp(deepflipper_radio_stop(&app,"{}",500,&out,radio),"save_failed")&&removed==1&&!deepflipper_radio_owned(radio));write_ok=true;
    assert(!deepflipper_radio_start(&app,args,500,&out,radio));signal(radio,&app,8);new_file=false;
    assert(!strcmp(deepflipper_radio_stop(&app,"{}",500,&out,radio),"save_failed")&&removed==1&&!deepflipper_radio_owned(radio));
    deepflipper_radio_cleanup(radio);deepflipper_radio_free(radio);free(out.text);
    assert(!receiving&&!initialized&&!begun);
    puts("PASS: production retained RF receive, start/state/save/stop, RSSI gate, buffer limit, cancellation, begin/save failures and resource cleanup");
}
