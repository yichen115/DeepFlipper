#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Firmware-internal (not part of the app SDK).
 *
 * While held, furi_hal_power_disable_otg() leaves the 5 V rail on. Many apps switch the
 * rail off when they finish probing their own module (Sub-GHz looks for an external
 * CC1101 on start and every few seconds, iButton after a read, ...), which would also
 * cut an accessory that is powered from the same pin, e.g. the DeepFlipper ESP32.
 *
 * Release the hold before switching the rail off for safety reasons (OTG fault).
 */
void furi_hal_power_otg_hold(bool hold);

bool furi_hal_power_is_otg_held(void);

#ifdef __cplusplus
}
#endif
