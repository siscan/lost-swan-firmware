// LOST Swan split-flap - Phase 3 entry point (spec 15.2, 15.3).
//
// Boot order: config -> ring table -> motion (drivers enabled only after VM
// has settled, then staggered homing) -> time service -> mode manager ->
// network (WiFi STA + mDNS + web server) -> CLI.
//
// The network comes up last and never blocks the boot: with no credentials the
// display is a standalone clock (spec 10.0) that shows the WiFi glyph on the
// centre column once the grace period expires.

#include <sys/time.h>

#include <string>

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
#include "net/bindings.h"
#include "net/httpd.h"
#include "net/mqtt.h"
#include "net/wifi.h"
#include "ring/ring_store.h"
#include "timesvc/time_service.h"
#include "webapi/api.h"
#include "webapi/event_tap.h"
#include "webapi/mqtt_bridge.h"
#include "webapi/ring_upload.h"

namespace {

constexpr const char* TAG = "app";

swan::FrameScheduler* g_sched = nullptr;
swan::ModeManager* g_modes = nullptr;
swan::api::EventTapPort* g_tap = nullptr;
swan::api::RingStager* g_stager = nullptr;
swan::api::Context* g_api = nullptr;
swan::config::AppConfig g_app;

int64_t utc_ms_now() {
    timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

// Audio arrives in Phase 5; until then a cue is a log line, so the countdown
// choreography is visible on the console.  The web UI sees it either way - the
// event tap forwards every cue onto /ws.
class LogCueSink final : public swan::CueSink {
public:
    void on_cue(swan::Cue c) override { ESP_LOGI("cue", "%s", swan::api::cue_name(c)); }
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
// >> 50 ms cadence), fires cues, and runs frame convergence.  It also owns the
// two things that must happen in the modes context and nowhere else: applying
// a staged ring table (ring_store.h contract) and pushing state on /ws.
void modes_task(void*) {
    TickType_t last = xTaskGetTickCount();
    swan::Mode last_mode = g_modes->mode();
    std::string last_payload;
    int64_t last_push = 0;
    int64_t last_mqtt = 0;
    std::string last_mqtt_slice;
    std::string last_countdown;

    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(50));
        const int64_t now = utc_ms_now();

        // A ring uploaded by the HTTP task was validated into a staging table;
        // the swap happens HERE, in the context that renders - and through
        // ModeManager, so it holds the same lock every command takes and no
        // task can be reading the table mid-assignment.  cmd_ring_swap also
        // forces the re-render a software table change needs.
        if (g_stager->pending()) {
            bool applied = false;
            g_modes->cmd_ring_swap([&] { return applied = g_stager->apply_pending(); }, now);
            if (applied) {
                const std::string body = g_stager->take_accepted_body();
                ESP_LOGI(TAG, "ring.json applied (%u bytes)", static_cast<unsigned>(body.size()));
                // Persist only what validated.  A failure here leaves RAM and
                // flash disagreeing until the next boot, so say so loudly.
                const esp_err_t werr = swan::ring_store::write_accepted(body);
                if (werr != ESP_OK) {
                    ESP_LOGE(TAG, "ring.json accepted but NOT persisted (%s); it will be lost "
                                  "on reboot", esp_err_to_name(werr));
                }
            }
        }

        const swan::MotionParams mp = swan::motion::params();
        g_tap->set_flaps(mp.flaps_s_normal);
        g_sched->set_timing({mp.flaps_s_normal, mp.accel});
        g_modes->tick(now);

        // "Nobody is looking" is about to stop meaning "no browser": a wall
        // clock feeding a terminal prop over MQTT (Phase 4) has no browser
        // open at all.  The gate stays - building a 1.5 KB document 20 times a
        // second for nobody is waste - but it has to ask every consumer.
        if (!swan::net::has_state_consumers()) continue;

        const swan::Mode m = g_modes->mode();
        if (m != last_mode) {
            last_mode = m;
            swan::net::ws_broadcast(swan::api::mode_event(m));
        }

        // On change, plus a 1 Hz heartbeat (spec 10.2), rate-limited: the
        // go/spin/cue events already carry the animation, and a 1.5 KB
        // document at 20 Hz is bandwidth this radio has better uses for.
        //
        // The comparison covers the DISPLAY state only - from "mode" up to the
        // "sys" block.  Free heap and uptime jitter on every tick, so
        // including them made "has anything changed" permanently true and
        // pinned the push rate at the 5 Hz cap forever; measured on the board
        // as 28 pushes in 6.5 s with the display sitting still.  Diagnostics
        // ride the 1 Hz heartbeat, which is as often as anyone reads them.
        const std::string payload = swan::api::build_state(*g_api, now);
        const size_t k = payload.find("\"mode\"");
        const size_t e = payload.find(",\"sys\":");
        std::string tail = (k == std::string::npos)
                               ? payload
                               : payload.substr(k, e == std::string::npos ? std::string::npos
                                                                          : e - k);
        const bool changed = (tail != last_payload) && (now - last_push >= 200);
        if (changed || now - last_push >= 1000) {
            last_push = now;
            last_payload = std::move(tail);
            swan::net::ws_broadcast(payload);
        }

        // MQTT is NOT /ws, and the difference is the retain flag.  A browser
        // is animating, so 5 Hz plus a 1 Hz heartbeat is right for it.  Every
        // retained publish rewrites the broker's store and makes Home
        // Assistant re-evaluate every template that reads it, so the broker
        // gets: on change, at most 1 Hz, and a 30 s floor so a peer learns the
        // display is still there.  The floor pairs with the 30 s keepalive -
        // a prop knows within ~45 s that the display is gone.
        //
        // go/spin/cue are /ws only.  A prop renders from the absolute
        // deadline (spec 7.3); per-flap animation events are the browser's
        // business.
        if (swan::net::mqtt_connected()) {
            std::string slice = swan::api::display_slice(payload);
            const bool mq_changed = (slice != last_mqtt_slice) && (now - last_mqtt >= 1000);
            if (mq_changed || now - last_mqtt >= 30000) {
                last_mqtt = now;
                last_mqtt_slice = std::move(slice);
                swan::net::mqtt_publish(swan::api::TOPIC_STATE, payload, true, 1);

                const std::string cd = swan::api::countdown_doc(
                    swan::cd_phase_name(g_modes->cd_phase()), g_modes->cd_target(),
                    swan::origin_name(g_modes->cd_set_by()), g_modes->cd_seq());
                if (cd != last_countdown) {
                    last_countdown = cd;
                    swan::net::mqtt_publish(swan::api::TOPIC_COUNTDOWN, cd, true, 1);
                }
            }
        } else {
            // Force a full re-assert on the next connect: the retained set is
            // state to restate, and a broker restart must not leave the
            // display's last word stale for ever.
            last_mqtt_slice.clear();
            last_countdown.clear();
        }
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
    ESP_ERROR_CHECK(swan::config::load_app(g_app));
    g_app.modes.alarm_flaps_s = mp.flaps_s_alarm;

    swan::ring_store::init();  // never fails the boot; worst case compiled table

    // Per-column mode and maintenance (spec 5.9), before motion starts, so a
    // simulated or disabled column is never briefly driven for real.  A fresh
    // NVS yields all-real, not-in-maintenance, by ColumnConfig's defaults.
    swan::ColumnConfig cols;
    ESP_ERROR_CHECK(swan::config::load_columns(cols));
    mp.maintenance = cols.maintenance;

    swan::status_led_init();
    ESP_ERROR_CHECK(swan::motion::init(mp));
    swan::motion::set_columns(cols);

    // Loud on every surface, starting with the one you read at 2 a.m.  A
    // simulated display must be impossible to mistake for a real one.
    if (cols.any(swan::ColumnMode::Sim)) {
        ESP_LOGW(TAG, "*** SIMULATED MOTION on %d column(s) - NOT driving real hardware ***",
                 cols.count(swan::ColumnMode::Sim));
    }
    if (cols.any(swan::ColumnMode::Disabled)) {
        ESP_LOGW(TAG, "%d column(s) DISABLED: excluded from frames, never homed",
                 cols.count(swan::ColumnMode::Disabled));
    }

    // Spec 5.5: EN only after the drivers have had VM for >=100 ms.
    vTaskDelay(pdMS_TO_TICKS(100));
    if (cols.maintenance) {
        // Maintenance survives a reboot precisely so pulling power mid-repair
        // does not restart the display on top of your hands: nothing homes,
        // nothing schedules, and EN stays down until you leave.
        ESP_LOGW(TAG, "*** MAINTENANCE MODE - no homing, no frames, EN released ***");
        swan::motion::enable(false);
    } else {
        swan::motion::enable(true);
        ESP_ERROR_CHECK(swan::motion::home(-1));  // staggered inside motion
    }

    swan::time_service::init(g_app.ntp.c_str());

    // Deliberate leaks: these live for the life of the device.
    static LogCueSink log_cues;
    static swan::api::EventTapCues cues(log_cues, swan::net::ws_broadcast);
    g_tap = new swan::api::EventTapPort(swan::motion_port(), swan::net::ws_broadcast);
    g_sched = new swan::FrameScheduler(*g_tap, {mp.flaps_s_normal, mp.accel});
    g_modes = new swan::ModeManager(swan::ring_store::get(), *g_sched,
                                    swan::time_service::source(),
                                    swan::config::countdown_store(), cues);
    g_modes->set_config(g_app.modes);
    // A disabled column is a hole in every frame from the first render, not a
    // column that moves once and then stops.
    g_sched->set_excluded(cols.excluded_mask());
    g_modes->cmd_maintenance(cols.maintenance, utc_ms_now());
    if (!g_modes->set_tz(g_app.tz)) {
        ESP_LOGE(TAG, "time.tz '%s' rejected; running on UTC", g_app.tz.c_str());
    }
    g_modes->begin(utc_ms_now());
    ESP_LOGI(TAG, "mode: %s", swan::mode_name(g_modes->mode()));

    // The web API surface (spec 10.2).  RingStager writes the live table, so
    // it takes the same RingSet the renderers hold.
    static swan::net::IdfMotionAdmin motion_admin;
    static swan::net::IdfConfigSink cfg_sink(g_app);
    static swan::net::IdfSysInfo sysinfo;
    static swan::net::IdfSystemOps ops;
    static swan::net::IdfMqttAdmin mqtt_admin;
    g_stager = new swan::api::RingStager(swan::ring_store::mutable_ring());
    // The stager is BOTH the ring source and the upload sink: it owns the lock
    // that makes a snapshot safe against its own swap.
    g_api = new swan::api::Context{*g_modes, *g_stager, motion_admin, cfg_sink,
                                   sysinfo,  *g_stager, ops, mqtt_admin, {}};

    xTaskCreate(status_task, "swan_status", 3072, nullptr, 2, nullptr);
    xTaskCreate(modes_task, "swan_modes", 8192, nullptr, 5, nullptr);

    // Network last, and never fatal: the display is complete without it.
    if (swan::net::init() != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed; running standalone");
    }
    swan::net::mdns_start("lost", "LOST Swan Timer");
    if (swan::net::httpd_start(*g_api) != ESP_OK) {
        ESP_LOGE(TAG, "web server failed to start");
    }
    // MQTT last: off until configured, never waited on, and the display is a
    // complete standalone clock without it (spec 10.0).
    if (swan::net::mqtt_init(*g_api) != ESP_OK) {
        ESP_LOGE(TAG, "mqtt transport failed to start");
    }

    swan::cli::bind_modes(g_modes, utc_ms_now);
    swan::cli::bind_ring(g_stager);
    ESP_ERROR_CHECK(swan::cli::start());
}
