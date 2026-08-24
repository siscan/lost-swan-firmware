// Motion subsystem public API: step generation, axis FSM, homing, edge
// verification, calibration.  Spec 5.
//
// The control logic itself lives in the pure core (motion/axis_control.h),
// which the host-side simulated-axis suite drives directly; this header is the
// firmware-facing surface and motion.cpp is the IDF shell around the core.
#pragma once

#include <cstdint>

#include "esp_err.h"
#include "motion/column_mode.h"
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

// --- per-column mode and maintenance (spec 5.9) ---------------------------
//
// Applying a config re-arms the ISR masks and, for a column that has just
// become simulated, resets its modelled drum to a plausible start angle.  A
// column becoming DISABLED is parked on the home slot first when it is homed
// and idle, so the hole in the frame reads as a blank flap rather than as a
// stale digit that a passer-by would read as the time.
void set_columns(const ColumnConfig& c);
ColumnConfig columns();

// How many 1 kHz control ticks have run.  Read by the OTA confirm watcher
// (spec 10.4): a motion tick that has stopped is one of the few things that
// says an image is broken rather than a mechanism.
uint32_t control_ticks();

// The 50 kHz step ISR's own liveness.  The task watchdog watches the 1 kHz
// control task; if the GPTimer stops, that task keeps looping and feeding it
// while the drums stand still, so this is the one component whose death is
// otherwise invisible.  False = no ISR tick for 200 ms.
bool step_isr_alive();
uint32_t step_isr_stalls();
uint32_t step_isr_ticks();

// True when at least one column is simulated.  Everything that reports device
// state consults this; it is the reason the banner, the boot line, the state
// payload and `stats` all say so.
bool any_simulated();

// --- simulated-drum fault injection (spec 5.9) ----------------------------
// No-ops unless the column is simulated.  `slip` displaces the drum relative
// to the motor; `miss` swallows the next N Hall edges - inject it before a
// home for the sensor signature, after one for the jam signature.
esp_err_t sim_inject_slip(int col, int32_t usteps);
esp_err_t sim_inject_miss(int col, uint32_t edges);
esp_err_t sim_clear_faults(int col);  // col < 0 = all

}  // namespace motion
}  // namespace swan
