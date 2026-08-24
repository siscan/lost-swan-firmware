// The physical button (spec §2, §10.2a, Q6).  The IDF shell: read the pin, feed
// the pure recogniser, dispatch.  All of the behaviour is in gesture.h.
//
// It goes through `api::handle_command` like every other control path, with
// `Origin::Button` - an origin that has existed and been reportable since Phase
// 4 and until now was never passed by anything.  So a countdown started from
// the button says `set_by: "button"` on `swan/countdown`, in the state document
// and in the journal, and a person reading the Pearl printout can tell a thumb
// on the case from a browser on the sofa.
//
// GPIO28 IS THE BOOT STRAPPING PIN.  See gesture.h for what that costs and what
// this does about it; the short version is that the ROM decides the boot mode
// before we run, so the firmware's whole contribution is to refuse to act on a
// button it inherited already pressed.
#include "button/button.h"

#include <cstdio>

#include "button/gesture.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/gpio_bank.h"
#include "hal/pins.h"
#include "modes/mode_manager.h"
#include "webapi/api.h"

namespace swan {
namespace button {

namespace {
constexpr const char* TAG = "button";

// 20 ms, the same cadence as the status LED task.  Fast enough that the 40 ms
// debounce sees two samples and the 2 s hold lands within one poll.
constexpr int POLL_MS = 20;

api::Context* g_ctx = nullptr;
ButtonGesture g_gesture;

int64_t now_ms() { return esp_timer_get_time() / 1000; }

void dispatch(const char* body) {
    if (g_ctx == nullptr) return;
    struct timeval tv {};
    gettimeofday(&tv, nullptr);
    const int64_t utc_ms =
        static_cast<int64_t>(tv.tv_sec) * 1000 + static_cast<int64_t>(tv.tv_usec) / 1000;
    const std::string res = api::handle_command(*g_ctx, body, utc_ms, Origin::Button);
    ESP_LOGI(TAG, "%s -> %s", body, res.c_str());
}

void button_task(void*) {
    // The pin's level at startup, before the first poll, is worth a line: it is
    // the difference between "the button is fine" and "the switch is stuck", and
    // a stuck switch is otherwise completely silent - the recogniser correctly
    // ignores it for ever.
    const bool held_at_boot = (gpio_bank_read() & pin_mask(PIN_BUTTON)) == 0;
    if (held_at_boot) {
        ESP_LOGW(TAG, "the button is DOWN at startup (GPIO%d). It is ignored until released - "
                      "it is the BOOT strap, and a press held across a reset must not turn into "
                      "a command on the boot that follows.",
                 PIN_BUTTON);
    }

    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(POLL_MS));

        // Active LOW: the switch pulls GPIO28 to ground, the board pulls it up.
        const bool pressed = (gpio_bank_read() & pin_mask(PIN_BUTTON)) == 0;
        const Gesture g = g_gesture.feed(pressed, now_ms());
        if (g == Gesture::None) continue;

        // MAINTENANCE IGNORES THE BUTTON ENTIRELY, which is not the same as the
        // dispatcher refusing it.  Maintenance means somebody has their hands in
        // the mechanism, and the button is on the case they are holding; a
        // refusal logged for every knock is noise, and an EXECUTE that "only"
        // arms a deadline is still the wrong thing to do to a person mid-repair.
        // The dispatcher's own gate stays as the backstop.
        if (g_ctx != nullptr && g_ctx->modes.maintenance()) {
            ESP_LOGI(TAG, "ignored (%s): maintenance is on",
                     g == Gesture::Hold ? "hold" : "press");
            continue;
        }

        if (g == Gesture::Press) {
            // The same path as the web UI's EXECUTE, with the Numbers supplied.
            // The ritual's validation lives in the firmware and stays there; a
            // button cannot type, so it presents the canonical Numbers and the
            // journal records WHO by rather than pretending they were entered.
            static const std::string body =
                std::string(R"({"cmd":"countdown.execute","payload":{"numbers":")") +
                ModeManager::THE_NUMBERS + R"("}})";
            dispatch(body.c_str());
        } else {
            dispatch(R"({"cmd":"motion.rehome","payload":{}})");
        }
    }
}
}  // namespace

void init(api::Context& ctx) {
    g_ctx = &ctx;

    gpio_config_t cfg{};
    cfg.pin_bit_mask = 1ULL << PIN_BUTTON;
    cfg.mode = GPIO_MODE_INPUT;
    // Belt and braces, exactly as the halls do it: the DevKitC-1 and the XIAO
    // both carry an external pull-up on BOOT, but an external button on a long
    // loom with no pull-up would otherwise float and fire at random.
    cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    const esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        // Not fatal, and never an abort: this is a convenience control on a
        // wall-mounted display (2026-08-24 sweep - no abort on a path an
        // outsider can reach, and a stuck pin is exactly such a path).
        ESP_LOGE(TAG, "GPIO%d config failed (%s); the button is disabled this boot",
                 PIN_BUTTON, esp_err_to_name(err));
        return;
    }

    // Priority 2, like the status LED: well below the modes tick (5) and the
    // motion control task (19), so a thumb on the case can never delay a frame.
    // 4096 rather than the LED's 3072 because handle_command builds JSON.
    xTaskCreate(button_task, "swan_button", 4096, nullptr, 2, nullptr);
    ESP_LOGI(TAG, "button on GPIO%d: press = execute the Numbers, hold %.1f s = re-home all",
             PIN_BUTTON, GestureConfig{}.hold_ms / 1000.0);
}

}  // namespace button
}  // namespace swan
