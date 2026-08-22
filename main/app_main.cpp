// LOST Swan split-flap - Phase 2 entry point (spec 15.2).
//
// Boot order: config -> ring table -> motion (drivers enabled only after VM
// has settled, then staggered homing) -> time service -> mode manager -> CLI.

#include <sys/time.h>

#include "cli/cli.h"
#include "config/config.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "frame/motion_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/pins.h"
#include "hal/status_led.h"
#include "modes/mode_manager.h"
#include "motion/motion.h"
#include "ring/ring_store.h"
#include "timesvc/time_service.h"

namespace {

constexpr const char* TAG = "app";

swan::FrameScheduler* g_sched = nullptr;
swan::ModeManager* g_modes = nullptr;

int64_t utc_ms_now() {
    timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

// Audio arrives in Phase 5; until then a cue is a log line, so the countdown
// choreography is visible on the console.
class LogCueSink final : public swan::CueSink {
public:
    void on_cue(swan::Cue c) override {
        const char* name = c == swan::Cue::Warn4Min    ? "warn_4min"
                           : c == swan::Cue::Warn1Min  ? "warn_1min"
                                                       : "system_failure";
        ESP_LOGI("cue", "%s", name);
    }
};

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
    if (!swan::time_service::source().valid()) return swan::Status::NoTime;
    return swan::Status::Ok;
}

void status_task(void*) {
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(20));
        swan::status_led_set(derive_status());
        swan::status_led_tick();
    }
}

// 20 Hz mode tick: renders, schedules land-on-tick boundaries (margin 700 ms
// >> 50 ms cadence), fires cues, and runs frame convergence.
void modes_task(void*) {
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(50));
        g_modes->tick(utc_ms_now());
    }
}

}  // namespace

extern "C" void app_main() {
    const esp_app_desc_t* desc = esp_app_get_description();
    ESP_LOGI(TAG, "LOST Swan split-flap - %s (%s), board %s", desc->version, desc->idf_ver,
             swan::BOARD_NAME);

    ESP_ERROR_CHECK(swan::config::init());

    swan::MotionParams mp;  // spec defaults
    ESP_ERROR_CHECK(swan::config::load(mp));
    swan::config::AppConfig app;  // spec defaults
    ESP_ERROR_CHECK(swan::config::load_app(app));
    app.modes.alarm_flaps_s = mp.flaps_s_alarm;

    swan::ring_store::init();  // never fails the boot; worst case compiled table

    swan::status_led_init();
    ESP_ERROR_CHECK(swan::motion::init(mp));

    // Spec 5.5: EN only after the drivers have had VM for >=100 ms.
    vTaskDelay(pdMS_TO_TICKS(100));
    swan::motion::enable(true);
    ESP_ERROR_CHECK(swan::motion::home(-1));  // staggered inside motion

    swan::time_service::init(app.ntp.c_str());

    // Deliberate leaks: these live for the life of the device.
    static LogCueSink cues;
    g_sched = new swan::FrameScheduler(swan::motion_port(),
                                       {mp.flaps_s_normal, mp.accel});
    g_modes = new swan::ModeManager(swan::ring_store::get(), *g_sched,
                                    swan::time_service::source(),
                                    swan::config::countdown_store(), cues);
    g_modes->set_config(app.modes);
    if (!g_modes->set_tz(app.tz)) {
        ESP_LOGE(TAG, "time.tz '%s' rejected; running on UTC", app.tz.c_str());
    }
    g_modes->begin(utc_ms_now());
    ESP_LOGI(TAG, "mode: %s", swan::mode_name(g_modes->mode()));

    xTaskCreate(status_task, "swan_status", 3072, nullptr, 2, nullptr);
    xTaskCreate(modes_task, "swan_modes", 6144, nullptr, 5, nullptr);

    swan::cli::bind_modes(g_modes, utc_ms_now);
    ESP_ERROR_CHECK(swan::cli::start());
}
