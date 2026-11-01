#pragma once
#include <stdbool.h>

// Firmware-internal: normal Loader launches yield the assistant's UART heap.
// Never call while an RPC worker is synchronously waiting for Loader.
bool deepflipper_app_suspend_uart(void);
bool deepflipper_rpc_profile_enabled(void);
