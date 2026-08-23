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
#include "motion/motion.h"

namespace swan {
namespace config {

// Everything Phase 2 persists beyond motion: clock/message/countdown settings
// plus the time service (spec 11).
struct AppConfig {
    ModesConfig modes;                          // clock.h24, msg.dwell_s, countdown.*
    std::string tz = "PST8PDT,M3.2.0,M11.1.0";  // time.tz [Q2: default US Pacific]
    std::string ntp = "pool.ntp.org";           // time.ntp
};

// WiFi STA credentials (spec 11 wifi.ssid / wifi.pass).  Kept separate from
// AppConfig so the CLI can write them without rewriting everything else, and
// so the password never travels through the state payload.
struct WifiConfig {
    std::string ssid;
    std::string pass;
    bool configured() const { return !ssid.empty(); }
};

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

// The countdown deadline store (spec 7.3): one write per set, never per tick.
CountdownStore& countdown_store();

// Factory reset of the swan namespace.
esp_err_t erase_all();

}  // namespace config
}  // namespace swan
