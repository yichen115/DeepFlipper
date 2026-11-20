#pragma once

#include <furi.h>

/** A cooperative semantic action running on the app thread, never the RPC thread. */
typedef struct DeepFlipperApp DeepFlipperApp;
typedef const char* (*DeepFlipperActionHandler)(
    DeepFlipperApp* app,
    const char* arguments_json,
    uint32_t timeout_ms,
    FuriString* result_json,
    void* context);

typedef struct {
    const char* name;
    const char* description;
    const char* input_schema;
    DeepFlipperActionHandler handler;
    uint32_t flags;
} DeepFlipperAction;

enum {
    DeepFlipperActionRequiresSession = 1
};

typedef struct {
    /** Optional app-thread polling while a retained session is idle. Must return promptly. */
    void (*tick)(DeepFlipperApp* app, void* context);
    /** Required for retained sessions. Idempotently stop/release every owned resource. */
    void (*cleanup)(void* context);
} DeepFlipperAppHooks;

/** Check in every hardware wait loop; includes RPC exit, disconnect, deadline and PIN lock. */
bool deepflipper_app_cancelled(DeepFlipperApp* app);
const char* deepflipper_app_request_id(const DeepFlipperApp* app);
bool deepflipper_app_retained(const DeepFlipperApp* app);

/**
 * Run an RPC-only adapter. "describe" is provided automatically. A handler returns
 * NULL on success, or a stable error code, and sets result_json to a JSON object.
 * Release all hardware before returning. Results use an atomic SD mailbox; the
 * client verifies Loader release separately. Ordinary UI entry points may branch
 * into this function only when launched with RPC arguments.
 */
int32_t deepflipper_app_run(
    void* rpc_arguments,
    const char* app_id,
    const DeepFlipperAction* actions,
    size_t action_count,
    void* context);

/** Retained sessions are opt-in, bounded by a renewable 5–60 second lease.
 * Cleanup runs on RPC exit/disconnect, PIN lock, lease expiry, or action failure. */
int32_t deepflipper_app_run_ex(
    void* rpc_arguments,
    const char* app_id,
    const DeepFlipperAction* actions,
    size_t action_count,
    void* context,
    const DeepFlipperAppHooks* hooks);
