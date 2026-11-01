#pragma once
#include <led_strip.h>

// Only the Expansion worker writes the status LED; audio callbacks update state only.
class StatusLed {
public:
    void Initialize();
    void Set(int r, int g, int b);  // Existing status effects use 0..1023.

private:
    bool ready_ = false;
    led_strip_handle_t strip_ = nullptr;
    int last_rgb_ = -1;
};
