// What this IMAGE will accept for speed, acceleration and the Hall tolerance,
// and the one flavour that widens it (2026-10-04).  Pure - no IDF - and host
// tested twice, once per flavour (test/host/test_limits_policy.cpp).
//
// THE SHAPE OF THE DECISION.  bench_policy.h makes the stand-in bench image a
// CAPPED image and says no key lifts the cap.  Nico asked for a way to run
// without limits, was asked what he meant, and chose: the dispatcher's RANGES,
// as a BUILD FLAVOUR, with the protections untouched.  This header is that, in
// the same shape as the cap - compiled in, never a runtime switch, never in a
// release image, announced on every surface - because the cap's own argument
// applies unchanged: a value that can be raised from the Settings page, from
// MQTT or by an NVS record that survived from another build is not a contract.
//
// WHAT -DSWAN_UNLIMITED=ON WIDENS - the live ranges, and only those:
//
//     speed     1..40  ->  1..400 flaps/s     400 is the spec's show spin
//     accel     ..60000 -> ..250 000
//     hall_tol  1..32  ->  1..64              one flap; see HALL_TOL_MAX_UNLIMITED
//
// WHAT IT DOES NOT TOUCH: jam stop (no retry), EN drop on two faults or a
// fault during a spin, the maintenance / EN / OTA refusals, the step ISR's
// liveness check - and the ramp-power guard below, which is the one protection
// the old speed range supplied without anybody writing it down.
//
// LIVE ONLY, ALWAYS.  Every extra unit of range is accepted by the dispatcher
// and nowhere else.  NVS may hold only what a NORMAL image would accept
// (`*_persistable`), in every flavour: `config::save` refuses anything wider
// and `config::load` substitutes anything wider, announced.  Two reasons, and
// the second is the one that matters:
//   - an unlimited image must not leave state a normal image cannot honour -
//     the "surviving state the new image cannot honour" rule of spec 10.4, which
//     is already what refuses a release image onto a board saved all-simulated;
//   - an unlimited image must not boot into its own experiment.  A power blip
//     during a countdown would otherwise come back up with the alarm spin at
//     400 flaps/s and nobody at the bench, which is a countdown starting itself
//     in an empty room by another route.  A reboot returns to the normal ranges.
#pragma once

#include <cstdint>

#include "motion/bench_policy.h"
#include "ring/geometry.h"

namespace swan {
namespace motion {

// True when this image is the unlimited flavour.  Compiled in, so nothing at
// runtime can make a normal image look unlimited or the reverse.
#if defined(SWAN_UNLIMITED) && SWAN_UNLIMITED
inline constexpr bool UNLIMITED_BUILD = true;
#else
inline constexpr bool UNLIMITED_BUILD = false;
#endif

// The two flavours that must never be the same image.  CMake refuses both at
// configure time with a message naming the numbers; these are the backstop for
// a build that bypasses it.
//   bench   - the bench image IS a cap.  An image that is capped and uncapped
//             at once would answer a speed with whichever rule ran first.
//   release - a release image must not be able to fake or exceed the mechanism.
static_assert(!(UNLIMITED_BUILD && BENCH_BUILD),
              "SWAN_UNLIMITED with SWAN_BENCH: a capped bench image and an image "
              "that lifts the ranges are opposites. Pick one.");
#if defined(SWAN_RELEASE) && SWAN_RELEASE
static_assert(!UNLIMITED_BUILD,
              "SWAN_UNLIMITED with SWAN_RELEASE: a release image must not lift "
              "the ranges the dispatcher enforces.");
#endif

// ---------------------------------------------------------------------------
// SPEED
// ---------------------------------------------------------------------------
inline constexpr int32_t FLAPS_S_MIN = 1;
// The NORMAL range, and the one every image persists (see LIVE ONLY above).
// test_ui_ranges.js pins web/index.html's control bounds to THIS literal, so it
// stays a literal.
inline constexpr int32_t FLAPS_S_MAX_NORMAL = 40;
// The show spin (spec 3): 400 flaps/s = 25 600 usteps/s per column, 51 % of the
// 50 kHz step budget by arithmetic.  The ISR's own ceiling is 50 000 usteps/s
// per axis = 781 flaps/s and nothing can exceed it; 400 is the figure the
// architecture was validated at, which is why it is the ceiling and not 781.
inline constexpr int32_t FLAPS_S_MAX_UNLIMITED = SHOW_SPIN_FLAPS_S;

// This image's live range.
inline constexpr int32_t FLAPS_S_MAX = UNLIMITED_BUILD ? FLAPS_S_MAX_UNLIMITED
                                                       : FLAPS_S_MAX_NORMAL;

constexpr bool flaps_s_plausible(int32_t v) { return v >= FLAPS_S_MIN && v <= FLAPS_S_MAX; }
constexpr bool flaps_s_persistable(int32_t v) {
    return v >= FLAPS_S_MIN && v <= FLAPS_S_MAX_NORMAL;
}

// ---------------------------------------------------------------------------
// ACCEL and HALL_TOL.  Their MIN bounds, `accel_plausible` and the rest live in
// motion_types.h with the rationale that produced them; the two ceilings are
// here because they are the ones the flavour changes.
// ---------------------------------------------------------------------------

// 60 000 is DELIBERATELY below the 82 000 that stalled the drum on 2026-09-12
// (motion_types.h).  That stays true of every image's persisted value.
inline constexpr int32_t ACCEL_MAX_NORMAL = 60000;
// 250 000 - a number PICKED, not derived: Nico asked for "accel past 60 000" and
// named no ceiling, so this is the default and it is said so (spec 17).  The
// reasons it is not larger: the ramp-power guard below allows 204 800 at the
// shipped 25 flaps/s alarm speed, so this covers everything the guard permits at
// any speed worth running; it is ~3x the 82 000 that stalled the drum, which is
// room to find that boundary; and a slider that spans a million cannot select a
// value near the shipped 12 000 (one pixel is ~5 000).  ramp_next_velocity is
// int64-safe far past it, so raising it is a one-line change with no arithmetic
// consequence - what constrains accel is the guard, not this.
inline constexpr int32_t ACCEL_MAX_UNLIMITED = 250000;

// Half a flap = 32 at the 1:1 drive.  Derived from the flap, not typed.
inline constexpr int32_t HALL_TOL_MAX_NORMAL = static_cast<int32_t>(ring_target_usteps(1) / 2);
// ONE FLAP, NOT UNBOUNDED.  Spec 5.4 grades an edge error as silent
// (<= hall_tol), major resync (<= one flap) or fault (> one flap).  A tolerance
// of exactly one flap erases the major band and keeps the fault band; one past
// it silently ACCEPTS errors that 5.4 says must fault.  That is not a wider
// range, it is the slip detector switched off - the failure the dispatcher used
// to allow at 400, where slips of 1.6 to 4.7 flaps raised nothing at all - and
// a protection is not on the list of things this flavour lifts.
inline constexpr int32_t HALL_TOL_MAX_UNLIMITED = static_cast<int32_t>(ring_target_usteps(1));

// ---------------------------------------------------------------------------
// THE RAMP-POWER GUARD
//
// Spec 17 keeps a 2 s ramp floor at show speed "until the bench measures it":
// a drum at 400 flaps/s carries about 2.1 J, and a PD-sourced rail can only
// SOURCE, so regen has nowhere to go.  In a normal image that protection is
// IMPLIED - speeds stop at 40 flaps/s, where the same drum holds 1 % of that
// energy, so no accel in range can hurt.  Lifting the speed range removes the
// implication: 60 000 accel at 400 flaps/s is a 0.43 s ramp, five times under
// the floor, one `motion.params` away.  The range was the protection and
// nobody had written that down.
//
// The floor is a statement about DURATION at ONE speed.  What it protects is
// peak regen POWER, which is proportional to decel x speed (braking torque
// times angular rate), so the guard generalises through the recorded design
// point: the accel the floor allows at the show spin (25 600 / 2 s = 12 800)
// times the show spin (25 600 usteps/s).  Every value a normal image can hold
// sits far inside it - static_asserted below - so it only ever bites here, where
// it REFUSES, never clamps, and names the numbers.
//
// THIS EXTENDS A RECORDED RULE TO SPEEDS IT WAS NOT WRITTEN FOR, BY PROPORTION,
// AND NOTHING HAS MEASURED IT.  It is a hedge with a reason, not a derived limit:
// tighten or relax it from a bench result, not a feeling.
// ---------------------------------------------------------------------------
inline constexpr int32_t RAMP_FLOOR_S = 2;
inline constexpr int32_t SHOW_SPIN_USTEPS_S = flaps_s_to_usteps_s(SHOW_SPIN_FLAPS_S);   // 25 600
inline constexpr int64_t RAMP_ACCEL_AT_SHOW_SPIN = SHOW_SPIN_USTEPS_S / RAMP_FLOOR_S;   // 12 800
inline constexpr int64_t RAMP_POWER_MAX =
    RAMP_ACCEL_AT_SHOW_SPIN * static_cast<int64_t>(SHOW_SPIN_USTEPS_S);                 // 327 680 000

// accel (usteps/s^2) x speed (usteps/s) against the design point.
constexpr bool ramp_power_ok(int64_t accel, int32_t flaps_s) {
    return accel * static_cast<int64_t>(flaps_s_to_usteps_s(flaps_s)) <= RAMP_POWER_MAX;
}

// The refusal as the choke points ask it.  Constant false in a normal image:
// nothing a normal image can be set to violates the guard, and the console's
// `spin` in a normal image is not changed by this flavour existing.
constexpr bool ramp_too_fast(int64_t accel, int32_t flaps_s) {
    return UNLIMITED_BUILD && !ramp_power_ok(accel, flaps_s);
}

// The accel the guard allows at a speed, for the refusal message.
constexpr int64_t ramp_accel_ceiling(int32_t flaps_s) {
    const int32_t v = flaps_s_to_usteps_s(flaps_s);
    return v > 0 ? RAMP_POWER_MAX / v : RAMP_POWER_MAX;
}

// ---------------------------------------------------------------------------
// The relationships that make the above one policy rather than six numbers.
// ---------------------------------------------------------------------------
static_assert(FLAPS_S_MAX_UNLIMITED > FLAPS_S_MAX_NORMAL, "the flavour must widen speed");
static_assert(ACCEL_MAX_UNLIMITED > ACCEL_MAX_NORMAL, "the flavour must widen accel");
static_assert(HALL_TOL_MAX_UNLIMITED > HALL_TOL_MAX_NORMAL, "the flavour must widen hall_tol");
static_assert(HALL_TOL_MAX_NORMAL == 32, "half a flap at 1:1");
static_assert(HALL_TOL_MAX_UNLIMITED == 64, "one flap at 1:1");
static_assert(SHOW_SPIN_USTEPS_S == 25600, "the show spin moved; re-derive the ramp-power guard");
static_assert(RAMP_ACCEL_AT_SHOW_SPIN == 12800, "2 s to the show spin is 12 800 usteps/s^2");
static_assert(RAMP_POWER_MAX == 327680000, "the design point moved; re-derive the guard");

// THE GUARD'S REASON FOR EXISTING, AS ARITHMETIC.  The worst combination a
// NORMAL image can hold - the top of the accel range at the top of the speed
// range - is inside the design point by a factor of two, and the shipped
// default at the show spin is inside it too.  If either stops holding, the
// guard stops being invisible in normal images and this fires first.
static_assert(static_cast<int64_t>(ACCEL_MAX_NORMAL) * flaps_s_to_usteps_s(FLAPS_S_MAX_NORMAL) <=
                  RAMP_POWER_MAX / 2,
              "a normal image can now be set past the ramp-power guard; the range was "
              "the protection (see the block above)");
static_assert(12000 * static_cast<int64_t>(SHOW_SPIN_USTEPS_S) <= RAMP_POWER_MAX,
              "the default accel (12 000) no longer survives the show spin under the guard");

}  // namespace motion
}  // namespace swan
