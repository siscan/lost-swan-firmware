// Persistent configuration (NVS).  Spec 11.
//
// Note on key names: spec 11 says config keys match the spec names exactly, and
// they do everywhere they are user-visible - CLI, REST, MQTT, the web UI.  They
// cannot be the NVS keys themselves: NVS caps a key at 15 characters and
// "motion.flaps_s_normal" is 21.  The mapping lives in config.cpp and is the
// only place the short forms appear.
#pragma once

#include "esp_err.h"
#include "motion/motion.h"

namespace swan {
namespace config {

// Opens NVS, initialising the partition if it is new or was reformatted.
esp_err_t init();

// Missing keys are left at their defaults, so a blank device boots on the spec
// defaults rather than failing.
esp_err_t load(MotionParams& p);
esp_err_t save(const MotionParams& p);

// Factory reset of the swan namespace.
esp_err_t erase_all();

}  // namespace config
}  // namespace swan
