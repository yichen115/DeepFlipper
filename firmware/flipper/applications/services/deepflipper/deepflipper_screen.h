#pragma once
#include <stddef.h>

/** Handle "screen" / "screen.page" assistant requests. Must be called WITHOUT the
 * DeepFlipper service lock held: it waits up to ~400 ms for the GUI to redraw. */
void deepflipper_screen_request(
    const char* op,
    const char* json,
    size_t size,
    char* result,
    size_t capacity);

/** Release any capture kept for paging (link closed). */
void deepflipper_screen_reset(void);
