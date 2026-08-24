// Persistent configuration (NVS).  Spec 11.
//
// Note on key names: spec 11 says config keys match the spec names exactly, and
// they do everywhere they are user-visible - CLI, REST, MQTT, the web UI.  They
// cannot be the NVS keys themselves: NVS caps a key at 15 characters and
// "motion.flaps_s_normal" is 21.  The mapping lives in config.cpp and is the
// only place the short forms appear.
#pragma once

#include <string>

#include "esp_err.h"
#include "modes/mode_manager.h"
#include "motion/column_mode.h"
#include "motion/motion.h"

namespace swan {
namespace config {

// Everything Phase 2 persists beyond motion: clock/message/countdown settings
// plus the time service (spec 11).
struct AppConfig {
    ModesConfig modes;                          // clock.h24, msg.dwell_s, countdown.*
    std::string tz = "PST8PDT,M3.2.0,M11.1.0";  // time.tz [Q2: default US Pacific]
    std::string ntp = "pool.ntp.org";           // time.ntp

    // Did NVS actually carry a countdown.reveal?  Output only; save_app ignores
    // it.  The distinction matters because the shipped default is the canon
    // five (ModeManager::REVEAL_CANON) resolved by NAME at boot, and "the key
    // is absent" has to be told apart from "somebody deliberately chose five
    // blanks" - which is a legal configuration and must not be overwritten
    // every reboot.
    bool reveal_stored = false;
};

// WiFi STA credentials (spec 11 wifi.ssid / wifi.pass).  Kept separate from
// AppConfig so the CLI can write them without rewriting everything else, and
// so the password never travels through the state payload.
struct WifiConfig {
    std::string ssid;
    std::string pass;
    bool configured() const { return !ssid.empty(); }
};

// MQTT (spec 11 mqtt.*).  Separate from AppConfig for the same reasons as
// WifiConfig: the password never travels through the state payload, and the
// Settings page can rewrite the broker without touching display settings.
//
// OFF until configured, and the firmware never waits on it (spec 10.0) - the
// display is a standalone clock and MQTT is an optional peer.
struct MqttConfig {
    bool enabled = false;
    std::string uri;                    // mqtt://host:1883
    std::string user;
    std::string pass;
    std::string base = "swan/";         // spec 10.3
    std::string ha_prefix = "homeassistant";  // HA's own discovery_prefix
    bool configured() const { return enabled && !uri.empty(); }
};

// Audio (spec 9, 11 audio.*).  Quiet hours are off by default [Q8]: both
// bounds equal means "never quiet", which is a state the wrap-around
// comparison has to handle explicitly.
struct AudioConfig {
    int volume = 70;          // [Q8 default]
    bool mute = false;
    int quiet_start_min = 0;
    int quiet_end_min = 0;
};

esp_err_t load_audio(AudioConfig& c);
esp_err_t save_audio(const AudioConfig& c);

// Opens NVS, initialising the partition if it is new or was reformatted.
esp_err_t init();

// Missing keys are left at their defaults, so a blank device boots on the spec
// defaults rather than failing.
esp_err_t load(MotionParams& p);
esp_err_t save(const MotionParams& p);
esp_err_t load_app(AppConfig& c);
esp_err_t save_app(const AppConfig& c);
esp_err_t load_wifi(WifiConfig& c);
esp_err_t save_wifi(const WifiConfig& c);
esp_err_t load_mqtt(MqttConfig& c);
esp_err_t save_mqtt(const MqttConfig& c);

// Per-column mode and maintenance (spec 5.9).  Separate from AppConfig so a
// repair can be recorded without rewriting display settings, and so a fresh
// NVS provably lands on the ColumnConfig defaults - all real, no maintenance.
esp_err_t load_columns(ColumnConfig& c);
esp_err_t save_columns(const ColumnConfig& c);

// The countdown deadline store (spec 7.3): one write per set, never per tick.
CountdownStore& countdown_store();

// Factory reset of the swan namespace.
esp_err_t erase_all();

}  // namespace config
}  // namespace swan
