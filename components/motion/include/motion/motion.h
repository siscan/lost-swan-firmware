// Motion subsystem public API: step generation, axis FSM, homing, edge
// verification, calibration.  Spec 5.
//
// The control logic itself lives in the pure core (motion/axis_control.h),
// which the host-side simulated-axis suite drives directly; this header is the
// firmware-facing surface and motion.cpp is the IDF shell around the core.
#pragma once

#include <cstdint>

#include "esp_err.h"
#include "motion/motion_math.h"
#include "motion/motion_types.h"

namespace swan {
namespace motion {

// Configures GPIO and the 50 kHz step timer.  Leaves the drivers DISABLED -
// EN must not be asserted until the drivers have had VM for >=100 ms (spec 5.5),
// which is the caller's business.
esp_err_t init(const MotionParams& p);

void set_params(const MotionParams& p);
MotionParams params();

void enable(bool on);
bool is_enabled();

// Commands are posted to a per-axis single-slot mailbox drained by the control
// tick; a newer command replaces an undrained older one (spec 6 semantics).
// Return values validate arguments for immediate CLI feedback; the control
// task re-validates authoritatively when it drains.

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
