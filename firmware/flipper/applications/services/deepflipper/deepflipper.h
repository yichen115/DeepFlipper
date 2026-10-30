#pragma once

#include <furi.h>

#define RECORD_DEEPFLIPPER    "deepflipper"
#define DEEPFLIPPER_JSON_SIZE 768

typedef enum {
    DeepFlipperOffline,
    DeepFlipperConfiguring,
    DeepFlipperConnecting,
    DeepFlipperIdle,
    DeepFlipperListening,
    DeepFlipperThinking,
    DeepFlipperSpeaking,
    DeepFlipperExecuting,
    DeepFlipperError,
} DeepFlipperState;

typedef struct {
    bool enabled;
    bool connected;
    bool foreground;
    bool token_set;
    bool muted;
    bool microphone;
    uint8_t volume;
    uint8_t battery;
    uint8_t mouth;
    uint8_t emotion;
    DeepFlipperState state;
    uint32_t session;
    uint32_t job;
    bool ap;
    bool web_changed;
    uint32_t uart_baud;
    char wifi[16];
    char ip[16];
    char ap_ip[16];
    char ap_ssid[25];
    char setup_key[13];
    char server_state[24];
    char notice[40];
    char device_id[18];
    char client_id[37];
} DeepFlipperSnapshot;

void deepflipper_on_system_start(void* context);
void deepflipper_snapshot(DeepFlipperSnapshot* snapshot);
void deepflipper_set_enabled(bool enabled);
// Called by Loader before admission, including manual and RPC application starts.
bool deepflipper_prepare_app_start(void);
void deepflipper_app_finished(void);
bool deepflipper_module_command(const char* field, const char* value);
void deepflipper_request(const char* json, size_t size, char* result, size_t capacity);
void deepflipper_disconnected(uint32_t session);
