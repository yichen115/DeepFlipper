#pragma once
#include <deepflipper_app/deepflipper_app.h>

typedef struct DeepFlipperRadio DeepFlipperRadio;
DeepFlipperRadio* deepflipper_radio_alloc(void);
void deepflipper_radio_free(DeepFlipperRadio* radio);
bool deepflipper_radio_owned(const DeepFlipperRadio* radio);
void deepflipper_radio_tick(DeepFlipperApp* app, void* context);
void deepflipper_radio_cleanup(void* context);
const char* deepflipper_radio_start(DeepFlipperApp*, const char*, uint32_t, FuriString*, void*);
const char* deepflipper_radio_state(DeepFlipperApp*, const char*, uint32_t, FuriString*, void*);
const char* deepflipper_radio_stop(DeepFlipperApp*, const char*, uint32_t, FuriString*, void*);
