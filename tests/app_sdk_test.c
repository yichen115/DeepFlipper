/* Run the production SDK, including its parser, descriptor and atomic mailbox,
 * against a small deterministic platform. Frozen is the actual firmware parser. */
#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
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
#define FuriFlagWaitAny 0
#define FuriHalRtcFlagLock 1
#define FSAM_WRITE 1
#define FSOM_CREATE_ALWAYS 1
#define FSE_OK 0
typedef struct { char* text; } FuriString;
typedef struct { int value; } FuriEventFlag;
typedef struct { int value; } Storage;
typedef struct { int value; } File;
typedef struct { bool confirmed, detached, exited, accepted; } RpcAppSystem;
enum {RpcAppEventTypeSessionClose, RpcAppEventTypeAppExit, RpcAppEventTypeDataExchange};
enum {RpcAppSystemEventDataTypeBytes};
typedef struct {int type; struct {int type; struct {const uint8_t* ptr;size_t size;} bytes;} data;} RpcAppSystemEvent;
typedef void (*RpcCallback)(const RpcAppSystemEvent*,void*);
static RpcCallback callback;
static void* callback_context;
static const char* input;
static bool pin_locked, short_write, synced, renamed, invoked, exited, rename_ok=true;
static char stored[4096];
static RpcAppSystem rpc;
static FuriEventFlag events;
static Storage storage;
static File file;
static uint32_t now;
static const char* handler_result="{\"value\":7}";
static const char** commands;
static unsigned command_count, command_cursor, published_count, cleanup_calls, tick_calls;
static bool scripted, expire_idle, lock_idle, idle_injected, disconnect_exit, resource_active;
static bool slow_followup;
static char published[8][4096];

static FuriString* furi_string_alloc_set(const char* text) {FuriString* s=malloc(sizeof(*s));s->text=strdup(text);return s;}
static void furi_string_set(FuriString* s,const char* text) {free(s->text);s->text=strdup(text);}
static void furi_string_cat(FuriString* s,const char* text) {size_t n=strlen(s->text);s->text=realloc(s->text,n+strlen(text)+1);strcpy(s->text+n,text);}
static size_t furi_string_size(FuriString* s){return strlen(s->text);}
static const char* furi_string_get_cstr(FuriString* s){return s->text;}
static void furi_string_free(FuriString* s){free(s->text);free(s);}
static FuriEventFlag* furi_event_flag_alloc(void){return &events;}
static void furi_event_flag_free(FuriEventFlag* e){assert(e==&events);}
static void furi_event_flag_set(FuriEventFlag* e,int bits){assert(e==&events&&bits==1);}
static void rpc_system_app_set_callback(RpcAppSystem* r,RpcCallback cb,void* context){assert(r==&rpc);callback=cb;callback_context=context;if(!cb)r->detached=true;}
static void rpc_system_app_send_started(RpcAppSystem* r){assert(r==&rpc);}
static void rpc_system_app_send_exited(RpcAppSystem* r){assert(r==&rpc);r->exited=true;exited=true;}
static void rpc_system_app_confirm(RpcAppSystem* r,bool success){assert(r==&rpc);r->confirmed=true;r->accepted=success;}
static void furi_event_flag_wait(FuriEventFlag* e,int bits,int mode,int timeout){
    UNUSED(e);UNUSED(bits);UNUSED(mode);UNUSED(timeout);
    assert(callback);
    RpcAppSystemEvent event={0};
    if(scripted && command_cursor==command_count && !idle_injected && (expire_idle||lock_idle)) {
        idle_injected=true;if(expire_idle)now+=60001;if(lock_idle)pin_locked=true;return;
    }
    if((scripted&&command_cursor<command_count)||(!scripted&&!rpc.confirmed)){
        const char* value=scripted?commands[command_cursor++]:input;
        event.type=RpcAppEventTypeDataExchange;event.data.type=RpcAppSystemEventDataTypeBytes;
        event.data.bytes.ptr=(const uint8_t*)value;event.data.bytes.size=strlen(value);
    }else {
        if(scripted&&idle_injected)assert(!resource_active);
        event.type=disconnect_exit?RpcAppEventTypeSessionClose:RpcAppEventTypeAppExit;
    }
    callback(&event,callback_context);
}
static bool furi_hal_rtc_is_flag_set(int flag){UNUSED(flag);return pin_locked;}
static uint32_t furi_get_tick(void){return now;}
static uint32_t furi_ms_to_ticks(uint32_t ms){return ms;}
static Storage* furi_record_open(const char* name){assert(!strcmp(name,RECORD_STORAGE));return &storage;}
static void furi_record_close(const char* name){UNUSED(name);}
static void storage_common_mkdir(Storage* s,const char* path){UNUSED(s);UNUSED(path);}
static File* storage_file_alloc(Storage* s){UNUSED(s);return &file;}
static bool storage_file_open(File* f,const char* path,int mode,int operation){UNUSED(f);UNUSED(mode);UNUSED(operation);assert(strstr(path,"/ext/deepflipper/jobs/123_")&&strstr(path,".json.tmp"));return true;}
static size_t storage_file_write(File* f,const void* data,size_t size){UNUSED(f);assert(size<sizeof(stored));memcpy(stored,data,size);stored[size]=0;return short_write?size-1:size;}
static bool storage_file_sync(File* f){UNUSED(f);synced=true;return true;}
static void storage_file_free(File* f){UNUSED(f);}
static int storage_common_rename(Storage* s,const char* from,const char* to){UNUSED(s);UNUSED(from);UNUSED(to);assert(synced);if(!rename_ok)return 1;renamed=true;if(scripted){assert(published_count<8);strcpy(published[published_count++],stored);}return 0;}
static void storage_common_remove(Storage* s,const char* path){UNUSED(s);UNUSED(path);}

#include "firmware/flipper/lib/deepflipper_app/deepflipper_app.c"

static const char* test_handler(DeepFlipperApp* app,const char* args,uint32_t timeout,FuriString* result,void* context){
    UNUSED(context);assert(timeout==500);assert(!strncmp(deepflipper_app_request_id(app),"123_",4));
    assert(!strcmp(args,"{\"value\":7}"));assert(!deepflipper_app_cancelled(app));
    invoked=true;furi_string_set(result,handler_result);return NULL;
}
static const DeepFlipperAction actions[]={{"test.read","Read a test value","{\"type\":\"object\"}",test_handler,0}};
static void run(const char* command){
    rpc=(RpcAppSystem){0};stored[0]=0;synced=renamed=invoked=exited=false;input=command;
    char arguments[64];snprintf(arguments,sizeof(arguments),"RPC %" PRIXPTR,(uintptr_t)&rpc);
    assert(deepflipper_app_run(arguments,"test.app",actions,1,NULL)==0);
    assert(rpc.detached&&rpc.exited&&callback==NULL&&exited);
}
static void check_code(const char* code){
    struct json_token actual=JSON_INVALID_TOKEN;json_scanf(stored,strlen(stored),"{code:%T}",&actual);
    assert(actual.type==JSON_TYPE_STRING&&actual.len==(int)strlen(code)&&!memcmp(actual.ptr,code,actual.len));
    assert(json_walk(stored,strlen(stored),NULL,NULL)==(int)strlen(stored));
}
static const char* session_handler(DeepFlipperApp* app,const char* args,uint32_t timeout,FuriString* result,void* context){
    resource_active=true;const char* code=test_handler(app,args,timeout,result,context);
    if(slow_followup&&command_cursor==2)now+=6000;
    return code;
}
static void session_cleanup(void* context){UNUSED(context);resource_active=false;++cleanup_calls;}
static void session_tick(DeepFlipperApp* app,void* context){UNUSED(app);UNUSED(context);++tick_calls;}
static const DeepFlipperAction session_actions[]={
    {"test.start","start","{\"type\":\"object\"}",session_handler,DeepFlipperActionRequiresSession},
    {"test.state","state","{\"type\":\"object\"}",session_handler,DeepFlipperActionRequiresSession},
    {"test.stop","stop","{\"type\":\"object\"}",session_handler,0},
};
#define actions default_actions
#define nfc_action test_handler
#define ir_action test_handler
#define rf_action test_handler
#define decode_action test_handler
#define deepflipper_radio_start session_handler
#define deepflipper_radio_state session_handler
#define deepflipper_radio_stop session_handler
#include "default-actions-production.inc"
#undef actions
#undef nfc_action
#undef ir_action
#undef rf_action
#undef decode_action
#undef deepflipper_radio_start
#undef deepflipper_radio_state
#undef deepflipper_radio_stop
static void run_session(const char** values,unsigned count){
    scripted=true;commands=values;command_count=count;command_cursor=published_count=cleanup_calls=tick_calls=0;
    idle_injected=resource_active=false;rpc=(RpcAppSystem){0};now=0;
    const DeepFlipperAppHooks hooks={.tick=session_tick,.cleanup=session_cleanup};
    char arguments[64];snprintf(arguments,sizeof(arguments),"RPC %" PRIXPTR,(uintptr_t)&rpc);
    assert(deepflipper_app_run_ex(arguments,"test.app",session_actions,3,NULL,&hooks)==0);
    assert(rpc.detached&&!resource_active&&cleanup_calls>0);
    assert(rpc.exited==!disconnect_exit);
    scripted=false;pin_locked=false;
}
int main(void){
    assert(deepflipper_json_within_limits("{\"value\":\"[{}]\\\"\"}",64,1));
    assert(!deepflipper_json_within_limits("[[[[[[[[[]]]]]]]]]",64,8));
    run("{\"v\":1,\"request_id\":\"123_1\",\"action\":\"describe\",\"timeout_ms\":500,\"arguments\":{}}");
    check_code("ok");assert(!invoked&&renamed&&strstr(stored,"test.read")&&strstr(stored,"input_schema"));
    const char* command="{\"v\":1,\"request_id\":\"123_1\",\"action\":\"test.read\",\"timeout_ms\":500,\"arguments\":{\"value\":7}}";
    run(command);check_code("ok");assert(invoked&&renamed);
    now=UINT32_MAX-100;run(command);check_code("ok");now=0;
    handler_result="{broken";run(command);check_code("invalid_result");
    handler_result="{\"value\":7}";
    pin_locked=true;run(command);check_code("cancelled");assert(!invoked);pin_locked=false;
    short_write=true;run(command);assert(!renamed&&!synced);short_write=false;
    rename_ok=false;run(command);assert(!renamed&&synced);rename_ok=true;
    run("{\"v\":1,\"request_id\":\"123_1\",\"action\":\"missing\",\"timeout_ms\":500,\"arguments\":{}}");check_code("unsupported_action");
    run("{\"v\":1.5,\"request_id\":\"123_1\",\"action\":\"test.read\",\"timeout_ms\":500,\"arguments\":{}}");check_code("invalid_request");
    run("{\"v\":1,\"request_id\":\"123_1\",\"action\":\"test.read\",\"timeout_ms\":500.5,\"arguments\":{}}");check_code("invalid_request");
    run("{\"v\":1,\"request_id\":\"../bad\",\"action\":\"describe\",\"timeout_ms\":500,\"arguments\":{}}");assert(!renamed);
    run("{\"v\":1,\"request_id\":\"123_1\",\"action\":\"describe\",\"timeout_ms\":500,\"arguments\":[]} ");check_code("invalid_request");
    run("{\"v\":1,\"request_id\":\"123_1\",\"action\":\"describe\",\"timeout_ms\":500,\"arguments\":{\"a\":[[[[[[[[]]]]]]]]}}");assert(!renamed);
    run("{\"v\":1,\"request_id\":\"123_1\",\"action\":\"test.read\",\"timeout_ms\":500,\"arguments\":{\"value\":7},\"keep_open\":true}");check_code("session_not_supported");assert(!invoked);
    puts("PASS: production app SDK discovery, dispatch, strict version/arguments, PIN cancellation, short writes and atomic publication");
    const char* sequence[]={
        "{\"v\":1,\"request_id\":\"123_1\",\"action\":\"test.start\",\"timeout_ms\":500,\"arguments\":{\"value\":7},\"keep_open\":true,\"lease_ms\":5000}",
        "{\"v\":1,\"request_id\":\"123_2\",\"action\":\"test.state\",\"timeout_ms\":500,\"arguments\":{\"value\":7},\"keep_open\":true}",
        "{\"v\":1,\"request_id\":\"123_3\",\"action\":\"test.stop\",\"timeout_ms\":500,\"arguments\":{\"value\":7}}",
    };
    run_session(sequence,3);assert(published_count==3&&command_cursor==3);
    assert(strstr(published[0],"\"keep_open\":true")&&strstr(published[1],"\"keep_open\":true")&&strstr(published[2],"\"keep_open\":false"));
    expire_idle=true;run_session(sequence,1);assert(idle_injected&&cleanup_calls>=2);expire_idle=false;
    lock_idle=true;run_session(sequence,1);assert(idle_injected&&cleanup_calls>=2);lock_idle=false;
    disconnect_exit=true;run_session(sequence,1);disconnect_exit=false;
    slow_followup=true;run_session(sequence,2);check_code("cancelled");assert(strstr(published[1],"\"keep_open\":false"));slow_followup=false;
    short_write=true;run_session(sequence,1);assert(published_count==0&&cleanup_calls>=2);short_write=false;
    rename_ok=false;run_session(sequence,1);assert(published_count==0&&cleanup_calls>=2);rename_ok=true;
    const char* invalid[]={"{\"v\":1,\"request_id\":\"123_1\",\"action\":\"test.start\",\"timeout_ms\":500,\"arguments\":{\"value\":7}}"};
    run_session(invalid,1);check_code("session_required");
    {
        const char* describe="{\"v\":1,\"request_id\":\"123_1\",\"action\":\"describe\",\"timeout_ms\":500,\"arguments\":{}}";
        const DeepFlipperAppHooks hooks={.tick=session_tick,.cleanup=session_cleanup};
        rpc=(RpcAppSystem){0};input=describe;stored[0]=0;
        char args[64];snprintf(args,sizeof(args),"RPC %" PRIXPTR,(uintptr_t)&rpc);
        assert(deepflipper_app_run_ex(args,"deepflipper.tools",default_actions,
            sizeof(default_actions)/sizeof(default_actions[0]),NULL,&hooks)==0);
        check_code("ok");assert(strstr(stored,"subghz.rx.start")&&strstr(stored,"subghz.rx.state")&&
            strstr(stored,"subghz.rx.stop")&&strstr(stored,"subghz.decode")&&strstr(stored,"\"session_supported\":true"));
        assert(strlen(stored)<4096&&rpc.detached);
    }
    puts("PASS: production retained SDK command reuse, start/state/stop, lease expiry, PIN/disconnect cleanup and session requirements");
}
