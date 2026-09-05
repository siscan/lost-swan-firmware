// Drive geometry — the single source of truth for every motion constant.
// Pure: no IDF includes, compiles on the host (CLAUDE.md conventions).
#pragma once

#include <cstdint>

namespace swan {

// ---------------------------------------------------------------------------
// Spec 3.  These five numbers define everything below them.
//
// DIRECT DRIVE, 1:1, since 2026-09-06 (docs/ref/DRIVE_CHANGE.md).  The NEMA 17
// now sits stationary INSIDE the drum and turns it directly; there is no pinion
// and no rim gear.  The rim-gear design died because with 50 flaps loaded the
// pinion collided with the card edges at every angular position - a mesh sweep
// found no collision-free angle at all.
//
// THE PEDIGREE OF THE HALL-TO-HALL NUMBER, which is how a bench measurement now
// identifies which hardware actually got built:
//
//     hall_to_hall ~= 8242   ->  85T/33T rim gear   (the original bridge)
//     hall_to_hall ~= 7555   ->  85T/36T rim gear   (the revision; NEVER built,
//                                and never reached this repo - see 17)
//     hall_to_hall  = 3200   ->  DIRECT DRIVE, 1:1  (what is being built)
//     hall_to_hall ~= 8369   ->  68T/26T            (the stale MECHANICAL prose)
//
// `revs 0 10` on the bench prints the measured value.  If it disagrees with
// 3200, the drum is telling you which machine you have, and the spec is wrong
// rather than the drum (CLAUDE.md).
//
// Change only these five constants and rebuild; nothing else hard-codes a ratio.
// ---------------------------------------------------------------------------
inline constexpr int STEPS_PER_MOTOR_REV = 200;  // 1.8 deg/step        VERIFY(motors)
inline constexpr int MICROSTEPS          = 16;   // TMC2209 MS1=MS2=high
inline constexpr int N_RING              = 50;   // flaps per drum

// Motor revolutions per drum revolution, as an exact fraction.  1:1 today.
//
// THE FRACTION IS KEPT DELIBERATELY even though it is now 1/1 and every derived
// value below is an integer.  It costs nothing at runtime (all constexpr), and
// it is what lets a future geometry change be one edit here instead of an audit
// of every call site - which is exactly the property that made THIS change
// small.  Exactness is a bonus, not a licence to delete the machinery.
inline constexpr int DRIVE_REDUCTION_NUM = 1;  // motor revs ...
inline constexpr int DRIVE_REDUCTION_DEN = 1;  // ... per drum rev

inline constexpr int64_t USTEPS_PER_MOTOR_REV =
    static_cast<int64_t>(STEPS_PER_MOTOR_REV) * MICROSTEPS;  // 3200

// usteps per flap = 3200 * 1 / (1 * 50) = 64 EXACTLY.
static_assert(USTEPS_PER_MOTOR_REV * DRIVE_REDUCTION_NUM % N_RING == 0,
              "usteps/motor-rev * reduction numerator must divide the ring size "
              "exactly, otherwise USTEPS_PER_FLAP_NUM below silently truncates");

inline constexpr int64_t USTEPS_PER_FLAP_NUM =
    USTEPS_PER_MOTOR_REV * DRIVE_REDUCTION_NUM / N_RING;      // 64
inline constexpr int64_t USTEPS_PER_FLAP_DEN = DRIVE_REDUCTION_DEN;  // 1

// usteps per full drum revolution = 3200 EXACTLY.
inline constexpr int64_t USTEPS_PER_SPOOL_REV_NUM = USTEPS_PER_MOTOR_REV * DRIVE_REDUCTION_NUM;
inline constexpr int64_t USTEPS_PER_SPOOL_REV_DEN = DRIVE_REDUCTION_DEN;

// The expected edge-to-edge distance (spec 5.4).  Under the rim gear this was a
// ROUNDED nominal and the 0.42 ustep residue was absorbed at every Hall edge;
// at 1:1 the division is exact and the residue is identically zero.  The
// rounding form is kept so the expression stays correct for any future ratio.
inline constexpr int64_t USTEPS_PER_SPOOL_REV_NOMINAL =
    (2 * USTEPS_PER_SPOOL_REV_NUM + USTEPS_PER_SPOOL_REV_DEN) /
    (2 * USTEPS_PER_SPOOL_REV_DEN);

// ---------------------------------------------------------------------------
// T(i): integer ustep target of ring index i, measured from the ring's index-0
// position.  T(i) = round(i * USTEPS_PER_FLAP).  Spec 3.
//
// i may exceed N_RING: T(50 + i) is index i one revolution later, which is how a
// move that crosses home is expressed before the Hall edge arrives (spec 5.3).
// Rounding error never exceeds 1/2 ustep because this is computed from i
// directly, never by accumulating per-flap increments.  At 1:1 the error is
// exactly zero, but the EDGE-RELATIVE targeting and the mid-move re-basing in
// spec 5.3 are unchanged and stay load-bearing: they exist to absorb a drum
// that has physically slipped, not merely to absorb arithmetic.
// ---------------------------------------------------------------------------
constexpr int64_t ring_target_usteps(int64_t i) {
    return (2 * i * USTEPS_PER_FLAP_NUM + USTEPS_PER_FLAP_DEN) / (2 * USTEPS_PER_FLAP_DEN);
}

// Convert a flaps/second rate to usteps/second (spec 3 speed table).
constexpr int32_t flaps_s_to_usteps_s(int32_t flaps_s) {
    return static_cast<int32_t>(
        (2 * static_cast<int64_t>(flaps_s) * USTEPS_PER_FLAP_NUM + USTEPS_PER_FLAP_DEN) /
        (2 * USTEPS_PER_FLAP_DEN));
}

// Sanity: these are the numbers printed in spec 3.  If a constant above changes,
// these fire and force the spec table to be updated with the bench result rather
// than silently drifting.
static_assert(USTEPS_PER_FLAP_NUM == 64 && USTEPS_PER_FLAP_DEN == 1,
              "usteps/flap is no longer 64/1 - update spec 3 and the decision log");
static_assert(USTEPS_PER_SPOOL_REV_NOMINAL == 3200, "drum rev != 3200 - update spec 3");
static_assert(ring_target_usteps(0) == 0, "T(0) must be 0");
static_assert(ring_target_usteps(1) == 64, "T(1) = 64 exactly at 1:1");
static_assert(ring_target_usteps(50) == 3200, "T(50) = 3200 exactly at 1:1");

// The step-generation budget (spec 5.2).  The show spin is the worst case and it
// is the number that decided the architecture: at the old 85T/33T ratio it
// needed 400 * 164.85 = ~66k usteps/s per column against a hard ceiling of
// TICK_HZ (50k) - impossible from a timer ISR, and unreachable with peripherals
// too, because the C5 has 2 RMT TX channels for 5 axes.  At 1:1 it is 25,600.
static_assert(flaps_s_to_usteps_s(400) == 25600,
              "show spin is no longer 25.6k usteps/s per column - re-check the "
              "step-gen budget in spec 5.2 before changing this");
static_assert(flaps_s_to_usteps_s(25) == 1600, "25 flaps/s should be 1600 usteps/s");

}  // namespace swan
