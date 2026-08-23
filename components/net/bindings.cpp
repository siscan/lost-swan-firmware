#include "net/bindings.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "motion/motion.h"
#include "net/wifi.h"

namespace swan {
namespace net {
namespace {

constexpr const char* TAG = "bindings";

const char* reset_reason_name(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:  return "poweron";
        case ESP_RST_EXT:      return "external";
        case ESP_RST_SW:       return "software";
        case ESP_RST_PANIC:    return "panic";
        case ESP_RST_INT_WDT:  return "int_wdt";
        case ESP_RST_TASK_WDT: return "task_wdt";
        case ESP_RST_WDT:      return "wdt";
        case ESP_RST_DEEPSLEEP:return "deepsleep";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO:     return "sdio";
        default:               return "unknown";
    }
}

void reboot_task(void*) {
    // Let the HTTP response reach the browser before the device goes away.
    vTaskDelay(pdMS_TO_TICKS(400));
    ESP_LOGW(TAG, "rebooting on request");
    esp_restart();
}

}  // namespace

AxisInfo IdfMotionAdmin::info(int col) {
    AxisInfo a{};
    motion::info(col, a);
    return a;
}

MotionParams IdfMotionAdmin::params() { return motion::params(); }

void IdfMotionAdmin::set_params(const MotionParams& p) { motion::set_params(p); }

bool IdfMotionAdmin::home(int col) { return motion::home(col) == ESP_OK; }

bool IdfMotionAdmin::adjust_cal(int col, int32_t delta) {
    return motion::adjust_cal(col, delta) == ESP_OK;
}

bool IdfMotionAdmin::spin_open_loop(int col, int32_t flaps_s, int seconds) {
    const int64_t usteps =
        (static_cast<int64_t>(flaps_s) * seconds * USTEPS_PER_FLAP_NUM) / USTEPS_PER_FLAP_DEN;
    return motion::step_open_loop(col, usteps, flaps_s) == ESP_OK;
}

ColumnConfig IdfMotionAdmin::columns() { return motion::columns(); }

bool IdfMotionAdmin::set_columns(const ColumnConfig& c) {
    motion::set_columns(c);
    // Maintenance reaches the control core through MotionParams, which every
    // tick snapshots - that is how the core suppresses automatic re-homing and
    // opens `go` to a faulted column.
    MotionParams p = motion::params();
    p.maintenance = c.maintenance;
    motion::set_params(p);
    // Persist immediately: a repair half-finished must still be a repair after
    // a power cut, and a simulated column must not quietly revert to real.
    return config::save_columns(c) == ESP_OK;
}

bool IdfMotionAdmin::sim_inject(int col, std::string_view kind, int32_t value) {
    if (kind == "slip") return motion::sim_inject_slip(col, value) == ESP_OK;
    if (kind == "miss") {
        return motion::sim_inject_miss(col, static_cast<uint32_t>(value < 0 ? 0 : value)) ==
               ESP_OK;
    }
    if (kind == "clear") return motion::sim_clear_faults(col) == ESP_OK;
    return false;
}

bool IdfMotionAdmin::sim_available() const {
#if SWAN_SIM_AXES
    return true;
#else
    return false;
#endif
}

bool IdfConfigSink::save_motion(const MotionParams& p) {
    return config::save(p) == ESP_OK;
}

bool IdfConfigSink::save_app(const ModesConfig& m, std::string_view tz) {
    app_.modes = m;
    app_.tz = std::string(tz);
    return config::save_app(app_) == ESP_OK;
}

api::SysInfo IdfSysInfo::get() {
    api::SysInfo s;
    const WifiStatus w = status();
    s.wifi_state = wifi_state_name(w.state);
    s.ssid = w.ssid;
    s.ip = w.ip;
    s.rssi = w.rssi;
    s.hostname = "lost";
    s.heap = esp_get_free_heap_size();
    s.heap_largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    s.uptime_s = static_cast<uint32_t>(esp_timer_get_time() / 1000000);
    s.reset_reason = reset_reason_name(esp_reset_reason());
    const esp_app_desc_t* d = esp_app_get_description();
    s.version = d != nullptr ? d->version : "unknown";
    s.ws_dropped = ws_dropped();
    return s;
}

bool IdfSystemOps::reboot() {
    return xTaskCreate(reboot_task, "swan_reboot", 2048, nullptr, 4, nullptr) == pdPASS;
}

}  // namespace net
}  // namespace swan
