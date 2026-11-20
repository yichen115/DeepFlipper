// Real furi_hal_power OTG disable/hold code with the charger replaced by a flag.
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

typedef struct {
    bool otg_hold;
} FuriHalPower;
static volatile FuriHalPower furi_hal_power;
static int furi_hal_i2c_handle_power;
static bool rail_on = true;
static int i2c_depth;
#define TAG "test"
#define FURI_LOG_D(tag, ...) ((void)(tag))
static void furi_hal_i2c_acquire(int* handle) {
    assert(handle == &furi_hal_i2c_handle_power && i2c_depth == 0);
    i2c_depth++;
}
static void furi_hal_i2c_release(int* handle) {
    assert(handle == &furi_hal_i2c_handle_power && i2c_depth == 1);
    i2c_depth--;
}
static void bq25896_disable_otg(int* handle) {
    assert(handle == &furi_hal_i2c_handle_power && i2c_depth == 1);
    rail_on = false;
}
void furi_hal_power_disable_otg(void);
void furi_hal_power_otg_hold(bool hold);
bool furi_hal_power_is_otg_held(void);
#include "otg-hold-production.inc"

int main(void) {
    // Default: apps may switch the rail off as before.
    furi_hal_power_disable_otg();
    assert(!rail_on && i2c_depth == 0);

    // Assistant on: Sub-GHz's "no external CC1101" path must not cut the ESP32.
    rail_on = true;
    furi_hal_power_otg_hold(true);
    assert(furi_hal_power_is_otg_held());
    for(int i = 0; i < 3; i++) furi_hal_power_disable_otg();
    assert(rail_on && i2c_depth == 0);

    // Fault path / assistant off releases the hold first, then the rail really goes off.
    furi_hal_power_otg_hold(false);
    assert(!furi_hal_power_is_otg_held());
    furi_hal_power_disable_otg();
    assert(!rail_on && i2c_depth == 0);
    puts("PASS: OTG hold keeps the accessory rail on, release restores normal disable");
    return 0;
}
