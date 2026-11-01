#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t live_bytes;
static void* tracked_alloc(size_t size) {
    size_t* p = calloc(1, sizeof(max_align_t) + size);
    assert(p); *p = size; live_bytes += size;
    return (char*)p + sizeof(max_align_t);
}
static void tracked_free(void* ptr) {
    if(!ptr) return;
    size_t* p = (size_t*)((char*)ptr - sizeof(max_align_t));
    assert(live_bytes >= *p); live_bytes -= *p; free(p);
}
typedef enum { RpcOwnerUnknown, RpcOwnerBle, RpcOwnerUsb, RpcOwnerUart } RpcOwner;
typedef struct Rpc {void* busy_mutex;} Rpc;
typedef struct RpcSession RpcSession;
typedef void* (*RpcSystemAlloc)(RpcSession*);
typedef void (*RpcSystemFree)(void*);
typedef void (*RpcSessionTerminatedCallback)(void*);
typedef struct { struct {bool (*decode)(void*,const void*,void**);} funcs;void* arg;} Callback;
typedef struct {Callback cb_content;char payload[768];} PB_Main;
typedef struct {void (*message_handler)(const PB_Main*,void*);void* decode_submessage;void* context;} RpcHandler;
typedef struct {void* stack;} Thread;
struct RpcSession {
    Rpc* rpc;void* callbacks_mutex;void* stream;atomic_bool terminate;bool decode_error;RpcOwner owner;
    void* handlers;PB_Main* decoded_message;void** system_contexts;Thread* thread;
    RpcSessionTerminatedCallback terminated_callback;void* context;
};
static int gps_allocations, network_allocations, companion_allocations;
static bool companion_profile = true;
static bool deepflipper_rpc_profile_enabled(void){return companion_profile;}
static void* rpc_deepflipper_alloc(RpcSession* s){(void)s;companion_allocations++;return tracked_alloc(12);}
static void rpc_deepflipper_free(void* p){assert(p);tracked_free(p);}
static void* rpc_system_system_alloc(RpcSession* s){(void)s;return NULL;}
static void* rpc_system_storage_alloc(RpcSession* s){(void)s;return tracked_alloc(16);}
static void rpc_system_storage_free(void* p){assert(p);tracked_free(p);}
static void* rpc_system_app_alloc(RpcSession* s){(void)s;return tracked_alloc(32);}
static void rpc_system_app_free(void* p){assert(p);tracked_free(p);}
static void* rpc_system_gui_alloc(RpcSession* s){(void)s;return tracked_alloc(128);}
static void rpc_system_gui_free(void* p){assert(p);tracked_free(p);}
static void* rpc_system_gpio_alloc(RpcSession* s){(void)s;return NULL;}
static void* rpc_system_property_alloc(RpcSession* s){(void)s;return NULL;}
static void* rpc_desktop_alloc(RpcSession* s){(void)s;return tracked_alloc(16);}
static void rpc_desktop_free(void* p){assert(p);tracked_free(p);}
static void* rpc_gps_alloc(RpcSession* s){(void)s;gps_allocations++;return tracked_alloc(sizeof(PB_Main));}
static void rpc_gps_free(void* p){assert(p);tracked_free(p);}
static void* rpc_network_alloc(RpcSession* s){(void)s;network_allocations++;return tracked_alloc(sizeof(PB_Main));}
static void rpc_network_free(void* p){assert(p);tracked_free(p);}
#include "rpc-profile-production.inc"
#define malloc(size) tracked_alloc(size)
#define free(ptr) tracked_free(ptr)
#define COUNT_OF(a) (sizeof(a)/sizeof((a)[0]))
#define furi_check assert
#define UNUSED(x) ((void)(x))
#define FuriWaitForever UINT32_MAX
#define FuriMutexTypeNormal 0
#define FuriStatusOk 0
#define furi_ms_to_ticks(ms) (ms)
#define RPC_BUFFER_SIZE 1024
#define RPC_SESSION_THREAD_NAME "RpcSessionWorker"
#define PB_Main_stop_session_tag 15
#define RpcHandlerDict_init(h) ((h)=tracked_alloc(512))
#define RpcHandlerDict_clear(h) tracked_free(h)
static RpcSession* waiting_session;
static int mutex_mode, mutex_releases;
static void* furi_mutex_alloc(int mode){(void)mode;return tracked_alloc(4);}
static int furi_mutex_acquire(void* mutex, uint32_t timeout){
    (void)mutex;(void)timeout;
    if(mutex_mode==1){waiting_session->terminate=true;return 1;}
    if(mutex_mode==2)waiting_session->terminate=true;
    return 0;
}
static int furi_mutex_release(void* mutex){(void)mutex;mutex_releases++;return 0;}
static void furi_mutex_free(void* p){tracked_free(p);}
static void* furi_stream_buffer_alloc(size_t size,int unit){(void)unit;return tracked_alloc(size);}
static void furi_stream_buffer_free(void* p){tracked_free(p);}
static bool rpc_pb_content_callback(void* a,const void* b,void** c){(void)a;(void)b;(void)c;return true;}
static void rpc_close_session_process(const PB_Main* a,void* b){(void)a;(void)b;}
static void rpc_add_handler(RpcSession* s,int tag,RpcHandler* h){(void)s;(void)tag;(void)h;}
static int32_t rpc_session_worker(void* a){(void)a;return 0;}
static void rpc_session_thread_state_callback(Thread* a,int state,void* c){(void)a;(void)state;(void)c;}
static Thread* furi_thread_alloc_ex(const char* name,size_t size,int32_t(*cb)(void*),void* context){
    (void)name;(void)cb;(void)context;Thread* t=tracked_alloc(sizeof(Thread));t->stack=tracked_alloc(size);return t;
}
static void furi_thread_set_state_context(Thread* t,void* c){(void)t;(void)c;}
static void furi_thread_set_state_callback(Thread* t,void(*cb)(Thread*,int,void*)){(void)t;(void)cb;}
static void furi_thread_start(Thread* t){assert(t);}
static void furi_thread_join(Thread* t){assert(t);}
static void furi_thread_free(Thread* t){tracked_free(t->stack);tracked_free(t);}
#include "rpc-open-production.inc"
#include "rpc-cleanup-production.inc"
#include "rpc-lock-production.inc"
#include "rpc-admission-production.inc"

typedef struct {int count;} FuriSemaphore;
typedef struct {RpcSession* rpc_session;FuriSemaphore* tx_semaphore;FuriSemaphore* rpc_terminated;} ExpansionWorker;
#define RECORD_RPC "rpc"
static void furi_semaphore_release(FuriSemaphore* s){assert(s->count==0);s->count=1;}
static int furi_semaphore_acquire(FuriSemaphore* s,uint32_t timeout){
    assert(timeout==FuriWaitForever&&s->count==1);
    assert(live_bytes==sizeof(FuriSemaphore)*2);s->count=0;return FuriStatusOk;
}
static void furi_semaphore_free(FuriSemaphore* s){tracked_free(s);}
static void rpc_session_close(RpcSession* session){rpc_session_thread_pending_callback(session,0);}
static void furi_record_close(const char* record){assert(strcmp(record,RECORD_RPC)==0);}
#include "expansion-terminated-production.inc"
#include "expansion-close-production.inc"

static int terminated;
static void after_free(void* context){assert(context==&terminated);assert(live_bytes==0);terminated++;}
int main(void){
    Rpc rpc={0};
    for(int i=0;i<100;i++){
        for(int owner=RpcOwnerBle;owner<=RpcOwnerUart;owner++){
            gps_allocations=network_allocations=companion_allocations=0;
            RpcSession* session=rpc_session_open(&rpc,owner);
            assert(gps_allocations==(owner!=RpcOwnerUart));
            assert(network_allocations==(owner!=RpcOwnerUart));
            assert(companion_allocations==(owner==RpcOwnerUart));
            session->terminated_callback=after_free;session->context=&terminated;
            rpc_session_thread_pending_callback(session,0);
            assert(live_bytes==0);
        }
    }
    assert(terminated==300);
    companion_profile=false;
    gps_allocations=network_allocations=0;
    RpcSession* ordinary_uart=rpc_session_open(&rpc,RpcOwnerUart);
    assert(gps_allocations==1&&network_allocations==1); // Other expansion modules retain native bridges.
    ordinary_uart->terminated_callback=after_free;ordinary_uart->context=&terminated;
    rpc_session_thread_pending_callback(ordinary_uart,0);assert(live_bytes==0);
    companion_profile=true;
    ExpansionWorker transport={.rpc_session=rpc_session_open(&rpc,RpcOwnerUart),
        .tx_semaphore=tracked_alloc(sizeof(FuriSemaphore)),
        .rpc_terminated=tracked_alloc(sizeof(FuriSemaphore))};
    transport.rpc_session->context=&transport;
    transport.rpc_session->terminated_callback=expansion_worker_rpc_terminated_callback;
    expansion_worker_rpc_session_close(&transport);
    assert(!transport.rpc_session&&!transport.tx_semaphore&&!transport.rpc_terminated&&live_bytes==0);
    RpcSession session={.rpc=&rpc};waiting_session=&session;
    mutex_mode=1;assert(!rpc_session_lock(&session)); // Shutdown while waiting on Loader barrier.
    session.terminate=false;mutex_mode=2;mutex_releases=0;
    assert(!rpc_session_lock(&session));assert(mutex_releases==1); // Shutdown at acquisition.
    session.terminate=false;mutex_mode=0;
    assert(rpc_session_lock(&session));furi_mutex_release(rpc.busy_mutex);
    assert(rpc_local_app_lock(&rpc));rpc_local_app_unlock(&rpc);
    puts("PASS: actual RPC allocation profiles, 300 leak-free teardowns, post-free notification and cancellable admission");
}
