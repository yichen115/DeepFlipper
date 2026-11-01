#include <assert.h>
#include <stdbool.h>
#define furi_assert assert
#define FURI_LOG_D(...) ((void)0)
#define PB_Main_desktop_is_locked_request_tag 66
typedef enum {
  PB_CommandStatus_OK = 0,
  PB_CommandStatus_ERROR = 1
} PB_CommandStatus;
typedef struct {
  unsigned which_content, command_id;
} PB_Main;
typedef struct {
  int status;
} RpcSession;
typedef struct {
  RpcSession *session;
  bool *desktop;
} RpcDesktop;
static bool desktop_api_is_locked(bool *desktop) { return *desktop; }
static void rpc_send_and_release_empty(RpcSession *session, unsigned id,
                                       PB_CommandStatus status) {
  (void)id;
  session->status = status;
}
#include "desktop-lock-production.inc"
int desktop_reference_lock_status(int locked) {
  RpcSession session = {0};
  bool pin = locked;
  RpcDesktop desktop = {.session = &session, .desktop = &pin};
  PB_Main request = {.which_content = 66, .command_id = 1};
  rpc_desktop_on_is_locked_request(&request, &desktop);
  return session.status;
}
