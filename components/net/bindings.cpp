#include <mutex>

#include "net/bindings.h"

#include "net/mqtt.h"
#include "net/ota.h"
#include "audio/player.h"
#include "net/provision.h"

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

// Public: app_main writes it into the journal's boot entry, which is what turns
// "it rebooted at some point" into "it panicked at 03:14".
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

namespace {

constexpr const char* TAG = "bindings";

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

api::MotionAdmin::CalOutcome IdfMotionAdmin::adjust_cal(int col, int32_t delta) {
    const esp_err_t err = motion::adjust_cal(col, delta);
    if (err == ESP_OK) return CalOutcome::Moved;
    // The offset was applied; there is simply no home reference to re-seek
    // against, so nothing moved and the caller has to say so.
    if (err == ESP_ERR_INVALID_STATE) return CalOutcome::NotHomed;
    return CalOutcome::BadColumn;
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

bool IdfConfigSink::save_app(const ModesConfig& m, std::string_view tz,
                             std::string_view ntp) {
    app_.modes = m;
    app_.tz = std::string(tz);
    app_.ntp = std::string(ntp);
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
    s.step_isr_alive = motion::step_isr_alive();
    s.step_isr_stalls = motion::step_isr_stalls();
    const esp_app_desc_t* d = esp_app_get_description();
    s.version = d != nullptr ? d->version : "unknown";
    s.ws_dropped = ws_dropped();
    const OtaState o = ota_status();
    s.ota_partition = o.running_partition;
    s.ota_pending_verify = o.pending_verify;
    return s;
}

// The config is read ONCE and cached.  build_state calls this on the modes
// task at up to 20 Hz, and load_mqtt is an nvs_open plus five string reads,
// each taking the global NVS lock - which the same lock an OTA, a `save` and
// the countdown's one-write-per-set all need.  Nothing here changes except
// when mqtt_configure changes it, so asking NVS twenty times a second was pure
// cost on the one task that must not be late.
std::mutex g_mqtt_cache_mu;
config::MqttConfig g_mqtt_cache;
bool g_mqtt_cache_valid = false;

config::MqttConfig mqtt_cached() {
    const std::lock_guard<std::mutex> lock(g_mqtt_cache_mu);
    if (!g_mqtt_cache_valid) {
        config::load_mqtt(g_mqtt_cache);
        g_mqtt_cache_valid = true;
    }
    return g_mqtt_cache;
}

api::MqttStatus IdfMqttAdmin::mqtt_status() {
    const config::MqttConfig c = mqtt_cached();
    api::MqttStatus s;
    s.enabled = c.enabled;
    s.user = c.user;
    s.ha_prefix = c.ha_prefix;
    s.connected = mqtt_connected();
    s.uri = c.uri;
    s.base = c.base;
    s.dropped = mqtt_dropped();
    return s;   // note: the password is deliberately absent
}

bool IdfMqttAdmin::mqtt_configure(const api::MqttAdmin::MqttSettings& in) {
    config::MqttConfig c = mqtt_cached();
    c.enabled = in.enabled;
    // Absent keeps, present replaces - including present-and-empty, which
    // clears.  Overwriting unconditionally is what wiped the credentials on
    // any partial update.
    if (in.uri) c.uri = std::string(*in.uri);
    if (in.user) c.user = std::string(*in.user);
    if (in.pass) c.pass = std::string(*in.pass);
    if (in.base) c.base = std::string(*in.base);
    if (in.ha_prefix) c.ha_prefix = std::string(*in.ha_prefix);
    if (config::save_mqtt(c) != ESP_OK) return false;
    {
        const std::lock_guard<std::mutex> lock(g_mqtt_cache_mu);
        g_mqtt_cache = c;
        g_mqtt_cache_valid = true;
    }
    // Staged, never applied here: stopping the client waits with portMAX_DELAY
    // on a task that may be parked for seconds, and this runs on the single
    // httpd task.
    return mqtt_reconfigure() == ESP_OK;
}

bool IdfSystemOps::ota_confirm() { return swan::net::ota_confirm() == ESP_OK; }
bool IdfSystemOps::ota_rollback() {
    // Does not return on success: the bootloader reboots into the other slot.
    return swan::net::ota_rollback_and_reboot() == ESP_OK;
}
bool IdfSystemOps::ota_pending_verify() { return swan::net::ota_pending_verify(); }

api::AudioState IdfAudioAdmin::audio_state() {
    const audio::AudioSettings s = audio::settings();
    const audio::Status st = audio::status();
    api::AudioState a;
    a.volume = s.volume;
    a.mute = s.mute;
    a.quiet_start_min = s.quiet_start_min;
    a.quiet_end_min = s.quiet_end_min;
    a.playing = st.playing;
    a.cue = st.cue;
    a.cues_total = static_cast<int>(audio::CUE_COUNT);
    for (bool h : st.have) a.cues_present += h ? 1 : 0;
    for (size_t i = 0; i < audio::CUE_COUNT; ++i) {
        api::AudioState::Cue c;
        c.name = audio::cue_id_name(static_cast<audio::CueId>(i));
        c.present = st.have[i];
        c.ms = st.ms[i];
        a.cues.push_back(std::move(c));
    }
    return a;
}

bool IdfAudioAdmin::audio_set(int volume, bool mute, int qs, int qe) {
    audio::AudioSettings s = audio::settings();
    s.volume = volume;
    s.mute = mute;
    s.quiet_start_min = qs;
    s.quiet_end_min = qe;
    audio::set_settings(s);
    config::AudioConfig c;
    c.volume = volume;
    c.mute = mute;
    c.quiet_start_min = qs;
    c.quiet_end_min = qe;
    return config::save_audio(c) == ESP_OK;
}

bool IdfAudioAdmin::audio_play(std::string_view cue) {
    audio::CueId id{};
    if (!audio::cue_id_from_name(cue, id)) return false;
    const audio::Status st = audio::status();
    // Refuse a cue with no file rather than reporting success and playing
    // nothing: "the alarm did nothing" is a bad thing to discover at zero.
    if (!st.have[static_cast<size_t>(id)]) return false;
    // -1: play regardless of quiet hours.  An explicit audio.play is somebody
    // testing the speaker, and silencing that would look like a broken amp.
    audio::play(id, -1);
    return true;
}

bool IdfAudioAdmin::audio_stop() {
    audio::stop();
    return true;
}

bool IdfWifiAdmin::set_credentials(std::string_view ssid, std::string_view pass) {
    return net::set_credentials(std::string(ssid).c_str(), std::string(pass).c_str()) == ESP_OK;
}
bool IdfWifiAdmin::start_portal() { return provision_start() == ESP_OK; }
bool IdfWifiAdmin::stop_portal() { return provision_stop() == ESP_OK; }
bool IdfWifiAdmin::portal_running() { return provisioning(); }
std::string IdfWifiAdmin::portal_ssid() { return provision_ssid(); }
bool IdfWifiAdmin::have_credentials() {
    // From the live WiFi state rather than from NVS: build_state asks at up to
    // 20 Hz on the modes task, and this is the same answer.
    return !status().ssid.empty();
}

bool IdfSystemOps::reboot() {
    // Say goodbye first.  esp_mqtt_client_stop sends a clean DISCONNECT and the
    // broker then DISCARDS the will, so without this a reboot from the UI
    // leaves the display looking online to Home Assistant until the keepalive
    // expires - reporting "available" for a device that is mid-restart.
    mqtt_go_offline();
    return xTaskCreate(reboot_task, "swan_reboot", 2048, nullptr, 4, nullptr) == pdPASS;
}

}  // namespace net
}  // namespace swan
