#include "status_led.h"
#include <esp_log.h>
#include <algorithm>
#include "config.h"

namespace {
constexpr const char* kTag = "StatusLed";
}

void StatusLed::Initialize() {
    led_strip_config_t config{};
    config.strip_gpio_num = DF_LED_DATA;
    config.max_leds = 1;
    config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    config.led_model = LED_MODEL_WS2812;
    led_strip_rmt_config_t rmt{};
    rmt.resolution_hz = 10000000;
    auto error = led_strip_new_rmt_device(&config, &rmt, &strip_);
    if (error == ESP_OK)
        error = led_strip_clear(strip_);
    ready_ = error == ESP_OK;
    if (!ready_) {
        ESP_LOGW(kTag, "WS2812 unavailable: %s; voice/UART continue", esp_err_to_name(error));
        if (strip_) {
            led_strip_del(strip_);
            strip_ = nullptr;
        }
        return;
    }
    last_rgb_ = 0;
    ESP_LOGI(kTag, "WS2812 GRB on GPIO%d", int(DF_LED_DATA));
}

void StatusLed::Set(int r, int g, int b) {
    if (!ready_)
        return;
    // Round after scaling so low-brightness breathing remains visible.
    r = (std::clamp(r, 0, 1023) * 255 + 511) / 1023;
    g = (std::clamp(g, 0, 1023) * 255 + 511) / 1023;
    b = (std::clamp(b, 0, 1023) * 255 + 511) / 1023;
    const int rgb = (r << 16) | (g << 8) | b;
    if (rgb == last_rgb_)
        return;
    auto error = led_strip_set_pixel(strip_, 0, r, g, b);
    if (error == ESP_OK)
        error = led_strip_refresh(strip_);
    if (error != ESP_OK) {
        ESP_LOGW(kTag, "WS2812 write failed: %s; disabling LED", esp_err_to_name(error));
        led_strip_del(strip_);
        strip_ = nullptr;
        ready_ = false;
        return;
    }
    last_rgb_ = rgb;
}
