#include <assert.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
enum {RpcAppEventTypeSessionClose, RpcAppEventTypeAppExit, RpcAppEventTypeDataExchange};
enum {RpcAppSystemEventDataTypeBytes};
typedef struct {int type;struct {int type;struct {const uint8_t* ptr;size_t size;} bytes;} data;} RpcAppSystemEvent;
typedef struct {bool detached,exited,confirmed;bool result;} RpcAppSystem;
typedef struct {RpcAppSystem* rpc;void* events;atomic_bool stop,command_ready;char command[256];} ToolContext;
static ToolContext* active;
static void rpc_system_app_set_callback(RpcAppSystem* rpc,void* callback,void* context){(void)callback;(void)context;assert(!atomic_load(&active->stop));rpc->detached=true;}
static void rpc_system_app_confirm(RpcAppSystem* rpc,bool okay){assert(!atomic_load(&active->stop));rpc->confirmed=true;rpc->result=okay;}
static void rpc_system_app_send_exited(RpcAppSystem* rpc){assert(!atomic_load(&active->stop));rpc->exited=true;}
static void furi_event_flag_set(void* events,int flags){(void)events;(void)flags;assert(!atomic_load(&active->stop));}
#include "tool-rpc-production.inc"
int main(void){
    for(int type=0;type<2;++type){
        RpcAppSystem rpc={0};ToolContext ctx={.rpc=&rpc};active=&ctx;
        RpcAppSystemEvent event={.type=type};rpc_event(&event,&ctx);
        assert(ctx.rpc==NULL&&rpc.detached&&atomic_load(&ctx.stop));
        assert(rpc.exited==(type==RpcAppEventTypeAppExit));
    }
    RpcAppSystem rpc={0};ToolContext ctx={.rpc=&rpc};active=&ctx;
    const char* text="{\"kind\":\"ir\"}";
    RpcAppSystemEvent event={.type=RpcAppEventTypeDataExchange,.data={.type=RpcAppSystemEventDataTypeBytes,.bytes={.ptr=(const uint8_t*)text,.size=strlen(text)}}};
    rpc_event(&event,&ctx);assert(rpc.result&&atomic_load(&ctx.command_ready)&&strcmp(ctx.command,text)==0);
    rpc_event(&event,&ctx);assert(!rpc.result&&!atomic_load(&ctx.stop));
    puts("PASS: actual tool adapter RPC detach, stop ordering and one-shot command ownership");
}
