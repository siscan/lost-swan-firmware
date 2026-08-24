// Shared motion types.  Pure: no IDF includes, so the host-side simulated-axis
// suite and the firmware compile the same definitions.
#pragma once

#include <cstdint>

#include "hal/pins.h"  // N_COLUMNS - pins.h is deliberately pure (cstdint only)
#include "motion/column_mode.h"
#include "motion/fault_policy.h"

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
    // Maintenance (spec 5.9).  Snapshotted into every control tick, so the
    // core needs no separate channel: it suppresses automatic re-homing and
    // opens `go` to a faulted column so a suspect drum can be driven by hand.
    bool maintenance = false;
    // The OTA hold, visible to the control core for the same reason
    // maintenance is: it suppresses AUTOMATIC re-homing.  Without it a
    // staggered home posted just before the upload, or a fault-triggered
    // retry, started a 7.5 s homing pass in the middle of a flash write - the
    // one thing "motion is held" (spec 10.4) is supposed to prevent, enforced
    // only at the dispatcher until now.
    bool ota_hold = false;
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
    FaultCause fault_cause; // why it last faulted - sensor-style, or a jam
    ColumnMode mode;        // real / sim / disabled, from NVS
};

}  // namespace swan
