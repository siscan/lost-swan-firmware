// Shared motion types.  Pure: no IDF includes, so the host-side simulated-axis
// suite and the firmware compile the same definitions.
#pragma once

#include <cstdint>

#include "hal/pins.h"  // N_COLUMNS - pins.h is deliberately pure (cstdint only)
#include "ring/geometry.h"  // ring_target_usteps: the tolerances derive from a flap
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
    // 14000 usteps/s^2, DERIVED FROM THE DRUM, 2026-09-12.  Was 82000, which was
    // never a free constant: spec 5.2 set it as "0 -> 4121 usteps/s in ~50 ms",
    // and 4121 is 25 flaps/s x the RIM GEAR's 5440/33 usteps per flap.  Every
    // term in it belongs to a drive that no longer exists, and on the bench it
    // STALLED THE DRUM.
    //
    // Two things changed at once and they multiply.  A ustep became 2.58x more
    // drum angle, AND the gear's 2.576x torque multiplication went away - so an
    // unchanged constant commanded ~6.6x the motor torque it used to.  Note that
    // simply rescaling by the dead ratio (82000 x 33/85 = 31835) restores the old
    // DRUM acceleration but NOT the old margin, because at 1:1 the motor pays for
    // that acceleration directly.  It still asks ~16.6 N.cm against ~15.4
    // available on the pessimistic inertia, i.e. it stalls too.
    //
    // The derivation, from the motor actually in hand (spec 2: LDO
    // 42STH48-2504AH, 55 N.cm at 2.5 A/phase - NOT the 17HS4401 the BOM
    // recommended and nobody bought):
    //
    //   T_avail  = 55 N.cm x 0.7/2.5            = 15.40 N.cm   (torque ~ current)
    //   - static imbalance                      -  3.92 N.cm   (DRIVE_CHANGE 31)
    //   - detent                                -  2.20 N.cm   (DRIVE_CHANGE 33)
    //   T_accel                                 =  9.28 N.cm
    //   with a 2x margin                        =  4.64 N.cm = 0.0464 N.m
    //
    // THE INERTIA IS NOT KNOWN TO BETTER THAN 2.6x, so the number is chosen to
    // survive the whole range rather than computed from one estimate.  The repo
    // never states J.  It states two conflicting kinetic energies at show speed
    // - 0.8 J (HARDWARE_PLAN_2) and 2.1 J (DRIVE_CHANGE) - which back-solve to
    // 6.33e-4 and 1.662e-3 kg.m^2.  A third, better estimate comes from the
    // recorded GEOMETRY plus the bench's 501 g: flaps spanning r = 13.3..60 mm
    // and two discs at R65 give a radius of gyration k ~= 42 mm, so
    // I = m k^2 = 0.501 x 0.042^2 = 8.8e-4 kg.m^2.  (That same geometry is why
    // the 2.1 J figure is the doubtful one: it implies k = 57.6 mm, outboard of
    // a solid disc's own k and nearly at the R60 pin circle, which two 4.5 mm
    // discs plus INWARD-hanging flaps cannot produce.)
    //
    // Margin at 12000, across all three (available 15.40 N.cm, demand is
    // I*alpha + 3.92 imbalance + 2.20 detent, alpha = 12000/509.3 = 23.6 rad/s^2):
    //   optimistic 6.33e-4 -> 7.61 N.cm -> 2.02x
    //   geometric  8.80e-4 -> 8.20 N.cm -> 1.88x
    //   pessimistic 1.662e-3 -> 10.04 N.cm -> 1.53x
    //
    // AND THE 2 s RAMP FLOOR IS WHAT PICKS 12000 OVER ~14000.  Spec 17 keeps a
    // 2 s minimum ramp at show speed until the bench measures the regen it
    // protects against - it is a FLOOR on duration, so a faster ramp violates
    // it.  0 -> 25600 usteps/s at 12000 takes 2.13 s; at 14000 it is 1.83 s and
    // fails.  The binding form is accel <= 25600/2 = 12800.
    //
    // The model earns its keep by predicting the bench: at 82000 it demands
    // 20.4 N.cm (geometric), 16.3 (optimistic) or 32.9 (pessimistic) against
    // 15.40 available - every estimate says stall, which is what happened.
    //
    // Rotor inertia is deliberately absent: at 1:1 there is no N^2 term, and a
    // 54-80 g.cm^2 rotor is under 1% of the drum.  It was not the rotor.
    //
    // This is a DERIVATION, not a measurement, and spec 5.2 keeps motion.accel
    // open: bench step 5 re-derives it once the flap stop and card stock are
    // settled (BRINGUP 28b step 6a - the step loss at card release is mechanical
    // and frozen, so the bench number measured against the OLD stop is not the
    // one to commit).
    int32_t accel = 12000;  // usteps/s^2; 0 -> alarm speed in ~133 ms
    // HALL_TOL_SILENT, a QUARTER FLAP.  Derived, not written out: it was the
    // literal 41 - a quarter of the rim gear's 165-ustep flap - and would have
    // silently become 64% of a flap when the drive went 1:1 and a flap became
    // 64 usteps, widening the silent band to swallow real slips.  VERIFY: bench
    // step 6 measures edge repeatability and sets it from the result.
    int32_t hall_tol = static_cast<int32_t>(ring_target_usteps(1) / 4);  // 16
    // `en_idle_off` USED TO LIVE HERE and is gone (spec 5.7, 2026-09-06).
    // The direct-drive drum is statically unbalanced by 3.92 N.cm against a
    // detent torque of 2.2 N.cm, so an unpowered drum does not merely creep,
    // it SLEWS to its heavy side.  Releasing EN at rest is therefore not a
    // trade against motor heat any more; it is a guarantee that the display
    // loses its position.  Holding current is the holding contract, and the
    // TMC2209 standstill reduction is what makes that affordable.
    // The A1121LUA-T is open-drain and pulls LOW with the magnet present, but which
    // magnet face works is an assembly convention (handoff 3, UNCERTAIN), so
    // this stays configurable rather than baked in.
    bool hall_active_low = true;
    // Which level on the ganged DIR pin turns the drum in the DESCENDING
    // sense (spec 4: one forward flip decrements the digit).  The motor now
    // sits inside the drum facing the opposite way to the old bridge, so the
    // sense is no longer a property of a gear train and cannot be settled on
    // paper.  Bench step 3 sets it.  Ganged: all five, or none.
    bool dir_invert = false;
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

// ---------------------------------------------------------------------------
// PERSISTED VALUES THAT PREDATE THE DRIVE CHANGE MUST NOT SURVIVE A BOOT.
//
// CLAUDE.md's rule is that a constant meaning "a fraction of a flap" must DERIVE
// from a flap.  `hall_tol` does - it is ring_target_usteps(1) / 4 - but the
// derivation only sets the DEFAULT, and config::load overwrites the default with
// whatever NVS holds.  A board that was configured under the 85T/33T rim gear
// carries hall_tol = 41, which was a quarter of that machine's 164.85-ustep flap
// and is 64% of this machine's 64-ustep flap.  Nothing about that is visible: the
// silent-accept band in spec 5.4 simply grows until it swallows real slips, and
// the first symptom is a drum that is wrong and a log that says everything is
// fine.  Found on the bench board 2026-09-12, where it had survived the drive
// change, a reflash and six reboots.
//
// So the load path validates rather than trusts.  The bound is deliberately a
// RULE ABOUT THE CURRENT GEOMETRY and not a list of known-bad numbers: a value
// only means anything if it is smaller than the band above it.  Spec 5.4 grades
// an edge error as silent (<= hall_tol), major resync (<= one flap) or fault
// (> one flap), so a hall_tol at or above one flap erases the middle band
// entirely.  Half a flap is the ceiling: the tolerance is meant to be a QUARTER
// flap, and one that has grown past half accepts more than half of every real
// slip without a word.
// ACCEL HAS THE SAME PROBLEM AND GETS THE SAME TREATMENT.
//
// It is not a fraction of a flap, so the rule above does not catch it, but it is
// still a persisted value whose MEANING changed at the drive swap: it is in
// usteps/s^2, and a ustep became 2.58x more drum angle.  Two separate holes:
//
//   - config::load took any int32 from NVS.  ACCEL == 0 THEN DIVIDES BY ZERO in
//     ramp_next_velocity's brake term (v*v / 2a).  The API refuses <1000, so the
//     only way in is a record written by another build or a corrupted page - but
//     "unreachable through the UI" is not the same as unreachable.
//   - the upper bound was 1e6, which is not a physical number for this drum.
//     At 1:1 the available torque runs out two orders of magnitude below it, so
//     the range was mostly unusable travel on a slider and a foot-gun elsewhere.
//
// ACCEL_MAX is a statement about the mechanism, not a UI preference.  It is set
// DELIBERATELY BELOW THE 82000 THAT STALLED THE DRUM on 2026-09-12, so that a
// control somebody drags cannot re-command a speed already known to stall, and
// comfortably above any value the derivation can justify so the bench can still
// explore upward.  Raising it is a decision that needs a bench result behind it,
// not a slider that felt short.
// ACCEL_MIN is the hazard bound: the ramp divides by accel, and dv =
// accel/CONTROL_HZ is an integer divide at CONTROL_HZ = 1000, so anything under
// 1000 realises as dv = 1 whatever it claims.
// The API, the web slider and the load path all use these two, so they cannot
// drift apart.
inline constexpr int32_t ACCEL_MIN = 1000;
inline constexpr int32_t ACCEL_MAX = 60000;

constexpr bool accel_plausible(int32_t v) {
    return v >= ACCEL_MIN && v <= ACCEL_MAX;
}

static_assert(ACCEL_MIN > 0, "the ramp divides by accel; zero must be unreachable");

inline constexpr int32_t HALL_TOL_MIN = 1;
inline constexpr int32_t HALL_TOL_MAX =
    static_cast<int32_t>(ring_target_usteps(1) / 2);   // half a flap = 32

constexpr bool hall_tol_plausible(int32_t v) {
    return v >= HALL_TOL_MIN && v <= HALL_TOL_MAX;
}

// The value to use given what NVS holds: the stored one when it can mean what
// its own comment says, otherwise the derived quarter flap.
constexpr int32_t hall_tol_migrated(int32_t stored) {
    return hall_tol_plausible(stored) ? stored
                                      : static_cast<int32_t>(ring_target_usteps(1) / 4);
}

static_assert(HALL_TOL_MAX == 32, "half a flap at 1:1");
static_assert(hall_tol_migrated(16) == 16, "the derived quarter flap is kept");
static_assert(hall_tol_migrated(41) == 16, "the rim-gear quarter flap is discarded");
static_assert(hall_tol_migrated(0) == 16, "zero would make every edge a resync");
static_assert(hall_tol_migrated(-5) == 16, "a negative tolerance is meaningless");
static_assert(hall_tol_migrated(32) == 32, "the ceiling itself is allowed through");

}  // namespace swan
