// LOST Swan split-flap - Phase 1 entry point (spec 15.1).
//
// Boot order matters: config before motion (calibration offsets come from NVS),
// drivers enabled only after VM has settled, then a staggered home of all five
// columns, then the console.

#include "cli/cli.h"
#include "config/config.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/pins.h"
#include "hal/status_led.h"
#include "motion/motion.h"

namespace {

constexpr const char* TAG = "app";

swan::Status derive_status() {
    bool any_fault = false;
    bool any_homing = false;
    bool all_homed = true;

    for (int i = 0; i < swan::N_COLUMNS; ++i) {
        swan::AxisInfo a;
        swan::motion::info(i, a);
        if (a.state == swan::AxisState::Fault) any_fault = true;
        if (a.state == swan::AxisState::Homing) any_homing = true;
        if (a.state == swan::AxisState::Unhomed || a.state == swan::AxisState::Homing) {
            all_homed = false;
        }
    }

    if (any_fault) return swan::Status::Fault;
    if (any_homing) return swan::Status::Homing;
    if (!all_homed) return swan::Status::Boot;
    // Phase 2 replaces this with Status::NoTime until SNTP has synced.
    return swan::Status::Ok;
}

// 50 Hz, lowest useful priority: the LED must never share a task with motion
// (a WS2812 refresh is an RMT transaction, not ISR work).
void status_task(void*) {
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(20));
        swan::status_led_set(derive_status());
        swan::status_led_tick();
    }
}

}  // namespace

extern "C" void app_main() {
    const esp_app_desc_t* desc = esp_app_get_description();
    ESP_LOGI(TAG, "LOST Swan split-flap - %s (%s), board %s", desc->version, desc->idf_ver,
             swan::BOARD_NAME);

    ESP_ERROR_CHECK(swan::config::init());

    swan::MotionParams params;  // spec defaults
    ESP_ERROR_CHECK(swan::config::load(params));
    ESP_LOGI(TAG, "flaps/s normal=%ld alarm=%ld home=%ld  accel=%ld  hall_tol=%ld  active_low=%d",
             static_cast<long>(params.flaps_s_normal), static_cast<long>(params.flaps_s_alarm),
             static_cast<long>(params.flaps_s_home), static_cast<long>(params.accel),
             static_cast<long>(params.hall_tol), params.hall_active_low ? 1 : 0);

    swan::status_led_init();
    ESP_ERROR_CHECK(swan::motion::init(params));

    // Spec 5.5: EN only after the drivers have had VM for >=100 ms.  In practice
    // VM is up long before app_main, but the wait is explicit so it survives any
    // future soft power sequencing.
    vTaskDelay(pdMS_TO_TICKS(100));
    swan::motion::enable(true);

    xTaskCreate(status_task, "swan_status", 3072, nullptr, 2, nullptr);

    // Staggered by HOME_STAGGER_MS inside motion::home to limit inrush.
    ESP_ERROR_CHECK(swan::motion::home(-1));

    ESP_ERROR_CHECK(swan::cli::start());
}
