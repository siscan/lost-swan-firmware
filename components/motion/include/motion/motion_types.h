// Shared motion types.  Pure: no IDF includes, so the host-side simulated-axis
// suite and the firmware compile the same definitions.
#pragma once

#include <cstdint>

#include "hal/pins.h"  // N_COLUMNS - pins.h is deliberately pure (cstdint only)

namespace swan {

enum class AxisState : unsigned char { Unhomed, Homing, Idle, Moving, Fault };
const char* axis_state_name(AxisState s);

// Tunables.  Names mirror the config keys in spec 11; config/ owns persistence.
struct MotionParams {
    int32_t flaps_s_normal = 15;
    int32_t flaps_s_alarm = 25;
    int32_t flaps_s_home = 8;
    int32_t accel = 82000;  // usteps/s^2, 0 -> alarm speed in ~50 ms
    int32_t hall_tol = 41;  // HALL_TOL_SILENT, 1/4 flap.  Set from bench step 6.
    bool en_idle_off = false;
    // A3144 is open-collector and pulls LOW with the magnet present, but which
    // magnet face works is an assembly convention (handoff 3, UNCERTAIN), so
    // this stays configurable rather than baked in.
    bool hall_active_low = true;
    int32_t cal[N_COLUMNS] = {0, 0, 0, 0, 0};
};

struct AxisInfo {
    AxisState state;
    int index;       // ring index displayed; RING_INVALID after raw stepping
    int dest_index;
    int64_t pos_abs;
    int64_t hall_abs;
    int64_t target_abs;
    int32_t velocity;    // usteps/s
    int32_t cal_offset;
    bool hall_level;     // debounced, true = magnet present
    bool hall_valid;     // at least one edge seen since the last home
    uint32_t flips_total;
    uint32_t revs;
    uint32_t resync_minor;
    uint32_t resync_major;
    uint32_t faults;
    int32_t last_hall_err;
    int32_t hall_to_hall;  // measured usteps between the last two edges
    uint8_t rehome_attempt; // automatic re-home in flight: 0, or 1..REHOME_RETRIES
};

}  // namespace swan
