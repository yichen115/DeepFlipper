#pragma once
#include <deepflipper_app/deepflipper_app.h>

const char* deepflipper_subghz_decode(
    DeepFlipperApp* app,
    const char* arguments,
    uint32_t timeout,
    FuriString* output,
    void* context);
