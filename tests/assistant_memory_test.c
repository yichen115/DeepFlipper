#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
typedef struct {bool enabled,foreground;} Snapshot;
typedef struct {int unused;} Thread;
typedef struct {void* link_mutex;Snapshot snapshot;bool uart_suspended,worker_stop,settings_foreground;Thread* thread;} DeepFlipper;
typedef struct {int unused;} Rpc;
typedef struct {int unused;} Expansion;
static DeepFlipper state,*service=&state;
static Rpc rpc;
static Expansion expansion;
static int disabled,resumed,records,barrier_depth;
static bool command_busy,prepare_ok=true,rpc_argument;
static int worker_stacks;
static Thread worker_thread;
#define FuriWaitForever UINT32_MAX
#define FuriHalSerialIdUsart 0
#define RECORD_RPC "rpc"
#define RECORD_EXPANSION "expansion"
#define FURI_LOG_I(tag,format,...) log_message(format,__VA_ARGS__)
static void log_message(const char* format,...){(void)format;}
static int furi_mutex_acquire(void* mutex,uint32_t timeout){(void)mutex;(void)timeout;return 0;}
static void furi_mutex_release(void* mutex){(void)mutex;}
static void lock(void){}
static void unlock(void){}
static void* furi_record_open(const char* name){records++;return strcmp(name,RECORD_RPC)==0?(void*)&rpc:(void*)&expansion;}
static void furi_record_close(const char* name){(void)name;assert(records>0);records--;}
static bool rpc_local_app_lock(Rpc* value){assert(value==&rpc);if(command_busy)return false;barrier_depth++;return true;}
static void rpc_local_app_unlock(Rpc* value){assert(value==&rpc&&barrier_depth==1);barrier_depth--;}
static void expansion_disable(Expansion* value){assert(value==&expansion&&barrier_depth==1);disabled++;}
static void expansion_set_listen_serial(Expansion* value,int serial){assert(value==&expansion&&serial==0);resumed++;}
static size_t memmgr_get_free_heap(void){return disabled?20000:10000;}
static size_t memmgr_heap_get_max_free_block(void){return 10000;}
static int32_t worker(void* context){(void)context;return 0;}
static Thread* furi_thread_alloc_ex(const char* name,size_t bytes,int32_t(*fn)(void*),void* context){
    assert(strcmp(name,"WhaleTools")==0&&bytes==3072&&fn==worker&&context==NULL);
    assert(worker_stacks==0);worker_stacks++;return &worker_thread;
}
static void furi_thread_start(Thread* t){assert(t==&worker_thread&&!service->worker_stop);}
static void furi_thread_join(Thread* t){assert(t==&worker_thread&&service->worker_stop);}
static void furi_thread_free(Thread* t){assert(t==&worker_thread);worker_stacks--;}
#include "assistant-worker-production.inc"
static bool deepflipper_prepare_app_start(void){
    service->snapshot.foreground=true;if(prepare_ok)deepflipper_worker_stop();return prepare_ok;
}
static bool loader_do_args_are_rpc(const char* args){(void)args;return rpc_argument;}
static const char* caller_name;
#define RPC_SESSION_THREAD_NAME "RpcSessionWorker"
static void* furi_thread_get_current_id(void){return NULL;}
static const char* furi_thread_get_name(void* id){(void)id;return caller_name;}
#include "assistant-resume-production.inc"
#include "assistant-profile-production.inc"
#include "assistant-suspend-production.inc"
#include "assistant-finish-production.inc"
#include "loader-prepare-production.inc"
#include "loader-caller-production.inc"
#include "loader-keep-production.inc"
int main(void){
    // Assistant pages keep the link; every other local launch still releases it.
    assert(loader_deepflipper_keeps_link("deepflipper_settings",NULL));
    assert(loader_deepflipper_keeps_link(NULL,"/ext/apps/Games/whale_pet.fap"));
    assert(loader_deepflipper_keeps_link(NULL,"/any/apps/Games/whale_pet.fap"));
    assert(!loader_deepflipper_keeps_link("subghz","Sub-GHz"));
    assert(!loader_deepflipper_keeps_link(NULL,"/ext/apps/Games/whale_pet.fap.bak"));
    assert(!loader_deepflipper_keeps_link(NULL,"/ext/apps/Tools/whale_pet.fap"));
    assert(!loader_deepflipper_keeps_link(NULL,"/int/apps/Games/whale_pet.fap"));
    assert(!loader_deepflipper_keeps_link(NULL,NULL));
    service=NULL;assert(!deepflipper_rpc_profile_enabled());service=&state;
    assert(!deepflipper_rpc_profile_enabled());
    caller_name="Desktop";assert(!loader_caller_is_rpc());
    caller_name=RPC_SESSION_THREAD_NAME;assert(loader_caller_is_rpc());
    service->snapshot.enabled=true;
    assert(deepflipper_rpc_profile_enabled());
    assert(loader_prepare_deepflipper(true,NULL,false));assert(disabled==0); // RPC app.open without args.
    rpc_argument=true;assert(loader_prepare_deepflipper(false,"RPC 1234",false));assert(disabled==0);
    rpc_argument=false;
    assert(loader_prepare_deepflipper(false,NULL,true));
    assert(service->snapshot.foreground&&service->settings_foreground&&!service->uart_suspended);
    assert(disabled==0);
    deepflipper_app_finished();
    assert(!service->settings_foreground&&!service->snapshot.foreground);
    for(int i=0;i<100;i++){
        deepflipper_worker_start();assert(worker_stacks==1);
        assert(loader_prepare_deepflipper(false,NULL,false));assert(service->uart_suspended);
        assert(worker_stacks==0&&service->thread==NULL);
        int before=resumed;deepflipper_resume_uart();assert(resumed==before); // Local app still running.
        deepflipper_app_finished();assert(worker_stacks==1);
        deepflipper_resume_uart();assert(resumed==before+1);
        deepflipper_resume_uart();assert(resumed==before+1); // Resume once only.
        assert(!service->uart_suspended&&records==0&&barrier_depth==0);
    }
    int before=disabled;
    command_busy=true;assert(!loader_prepare_deepflipper(false,NULL,false));
    assert(disabled==before&&!service->uart_suspended&&records==0); // No Loader/RPC deadlock.
    deepflipper_app_finished();assert(worker_stacks==1); // Failed launch restores animation worker.
    command_busy=false;prepare_ok=false;assert(!loader_prepare_deepflipper(false,NULL,false));
    assert(disabled==before);prepare_ok=true;
    assert(loader_prepare_deepflipper(false,NULL,false));
    service->snapshot.enabled=false;service->snapshot.foreground=false;
    before=resumed;deepflipper_resume_uart();assert(resumed==before); // AI off cannot reconnect.
    assert(loader_prepare_deepflipper(false,NULL,false));assert(records==0);
    deepflipper_worker_start();deepflipper_worker_start();assert(worker_stacks==1);
    deepflipper_worker_stop();deepflipper_worker_stop();assert(worker_stacks==0);
    puts("PASS: actual local-app UART release/resume, remote launch preservation, AI-off and busy admission");
}
