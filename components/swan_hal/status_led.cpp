#include "hal/status_led.h"

#include "driver/gpio.h"
#include "hal/pins.h"

#if defined(BOARD_ESP32C5_DEVKITC1)
#include "led_strip.h"
#endif

namespace swan {
namespace {

Status g_status = Status::Boot;
uint32_t g_tick = 0;

#if defined(BOARD_ESP32C5_DEVKITC1)
led_strip_handle_t g_strip = nullptr;

struct Rgb {
    uint8_t r, g, b;
};

Rgb colour_for(Status s) {
    switch (s) {
        case Status::Boot:   return {8, 0, 16};    // dim violet
        case Status::Homing: return {16, 8, 0};    // amber
        case Status::Ok:     return {0, 12, 0};    // green
        case Status::NoTime: return {0, 6, 16};    // blue
        case Status::Fault:  return {24, 0, 0};    // red
    }
    return {0, 0, 0};
}
#endif

// Blink period in status-task ticks (50 Hz).  0 = solid.
uint32_t blink_ticks(Status s) {
    switch (s) {
        case Status::Boot:   return 50;   // 1 s
        case Status::Homing: return 12;   // ~4 Hz
        case Status::Ok:     return 0;
        case Status::NoTime: return 25;   // 2 Hz
        case Status::Fault:  return 5;    // 10 Hz
    }
    return 0;
}

}  // namespace

void status_led_init() {
#if defined(BOARD_ESP32C5_DEVKITC1)
    led_strip_config_t strip_cfg = {};
    strip_cfg.strip_gpio_num = PIN_LED;
    strip_cfg.max_leds = 1;
    strip_cfg.led_model = LED_MODEL_WS2812;
    strip_cfg.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;

    led_strip_rmt_config_t rmt_cfg = {};
    rmt_cfg.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt_cfg.resolution_hz = 10 * 1000 * 1000;

    if (led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &g_strip) != ESP_OK) {
        g_strip = nullptr;  // LED is cosmetic; never block bring-up on it
    }
#else
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << PIN_LED;
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
#endif
    status_led_set(Status::Boot);
}

void status_led_set(Status s) { g_status = s; }

void status_led_tick() {
    const uint32_t period = blink_ticks(g_status);
    const bool on = (period == 0) || ((g_tick % period) < (period / 2));
    ++g_tick;

#if defined(BOARD_ESP32C5_DEVKITC1)
    if (g_strip == nullptr) return;
    static Status last = Status::Fault;
    static bool last_on = false;
    if (g_status == last && on == last_on) return;  // RMT write only on change
    last = g_status;
    last_on = on;

    const Rgb c = colour_for(g_status);
    if (on) {
        led_strip_set_pixel(g_strip, 0, c.r, c.g, c.b);
    } else {
        led_strip_set_pixel(g_strip, 0, 0, 0, 0);
    }
    led_strip_refresh(g_strip);
#else
    gpio_set_level(static_cast<gpio_num_t>(PIN_LED), LED_ACTIVE_LOW ? !on : on);
#endif
}

}  // namespace swan
