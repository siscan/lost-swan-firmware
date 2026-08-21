// Motion subsystem: step generation, axis FSM, homing, edge verification,
// calibration.  Spec 5.
#pragma once

#include <cstdint>

#include "esp_err.h"
#include "hal/pins.h"
#include "motion/motion_math.h"

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
};

namespace motion {

// Configures GPIO and the 50 kHz step timer.  Leaves the drivers DISABLED -
// EN must not be asserted until the drivers have had VM for >=100 ms (spec 5.5),
// which is the caller's business.
esp_err_t init(const MotionParams& p);

void set_params(const MotionParams& p);
MotionParams params();

void enable(bool on);
bool is_enabled();

esp_err_t home(int col);  // col < 0 homes all five, staggered by HOME_STAGGER_MS
esp_err_t go(int col, int index);

// Open-loop stepping for bring-up (CLI `step` / `spin`).  Leaves the displayed
// index unknown: the axis is still position-tracked, but what is on the front
// of the drum is no longer derivable without a re-home.
esp_err_t step_open_loop(int col, int64_t usteps, int32_t flaps_s);

esp_err_t stop(int col);
esp_err_t set_cal(int col, int32_t usteps);
esp_err_t adjust_cal(int col, int32_t delta);

void info(int col, AxisInfo& out);
bool all_idle();

}  // namespace motion
}  // namespace swan
