#pragma once
#include <nfc/nfc_poller.h>
#include <furi.h>

typedef struct DeepFlipperNfc DeepFlipperNfc;
DeepFlipperNfc* deepflipper_nfc_alloc(Nfc* nfc, NfcProtocol detected);
bool deepflipper_nfc_done(DeepFlipperNfc* read);
void deepflipper_nfc_result(DeepFlipperNfc* read, char* output, size_t capacity);
bool deepflipper_nfc_save(DeepFlipperNfc* read, const char* path);
void deepflipper_nfc_free(DeepFlipperNfc* read);
