#include "rpc_i.h"
#include <deepflipper/deepflipper.h>
#include <mjs/common/frozen/frozen.h>
#include <string.h>

typedef struct {
    RpcSession* session;
    uint32_t module_session;
} RpcDeepFlipper;

static void handle(const PB_Main* request, void* context) {
    RpcDeepFlipper* instance = context;
    if(rpc_session_get_owner(instance->session) != RpcOwnerUart) {
        rpc_send_and_release_empty(instance->session, request->command_id, PB_CommandStatus_ERROR);
        return;
    }
    PB_Main* response = calloc(1, sizeof(PB_Main));
    response->command_id = request->command_id;
    response->which_content = PB_Main_deepflipper_response_tag;
    char* output = response->content.deepflipper_response.json;
    const char* input = request->content.deepflipper_request.json;
    deepflipper_request(
        input, strlen(input), output, sizeof(response->content.deepflipper_response.json));
    unsigned session = 0;
    if(json_scanf(output, strlen(output), "{session:%u}", &session) == 1)
        instance->module_session = session;
    rpc_send(instance->session, response);
    free(response);
}

void* rpc_deepflipper_alloc(RpcSession* session) {
    RpcDeepFlipper* instance = calloc(1, sizeof(RpcDeepFlipper));
    instance->session = session;
    RpcHandler handler = {.message_handler = handle, .context = instance};
    rpc_add_handler(session, PB_Main_deepflipper_request_tag, &handler);
    return instance;
}

void rpc_deepflipper_free(void* context) {
    RpcDeepFlipper* instance = context;
    deepflipper_disconnected(instance->module_session);
    free(instance);
}
