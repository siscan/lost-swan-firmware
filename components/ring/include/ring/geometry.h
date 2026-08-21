// Drive geometry — the single source of truth for every motion constant.
// Pure: no IDF includes, compiles on the host (CLAUDE.md conventions).
#pragma once

#include <cstdint>

namespace swan {

// ---------------------------------------------------------------------------
// Spec 3 / handoff 1.  These four numbers define everything below them.
//
// Status: BOM.md and FIRMWARE_HANDOFF.md 1 both state 85T/33T at module 1.5 and
// MS1=MS2=high -> 1/16 ("DECIDED").  MECHANICAL_README.md:67 still carries stale
// 68T/26T prose from the module-2 revision, and the parts table marks the pinion
// "33T *if regenerated*".  Phase 1 bench step 4 (`revs 0 10`) settles it:
//
//     85/33 -> hall_to_hall ~= 8242      68/26 -> hall_to_hall ~= 8369
//
// If the bench disagrees with the spec, the spec is wrong (CLAUDE.md).  Change
// only these four constants and rebuild; nothing else hard-codes a ratio.
// ---------------------------------------------------------------------------
inline constexpr int STEPS_PER_MOTOR_REV = 200;  // 1.8 deg/step        VERIFY(motors)
inline constexpr int MICROSTEPS          = 16;   // TMC2209 MS1=MS2=high
inline constexpr int GEAR_DRIVEN_TEETH   = 85;   // rim gear cut into the spool disc
inline constexpr int GEAR_DRIVE_TEETH    = 33;   // pinion on the motor shaft
inline constexpr int N_RING              = 50;   // flaps per drum

inline constexpr int64_t USTEPS_PER_MOTOR_REV =
    static_cast<int64_t>(STEPS_PER_MOTOR_REV) * MICROSTEPS;  // 3200

// usteps per flap = 3200 * 85 / (33 * 50) = 5440/33 = 164.8484...  NOT an integer.
static_assert(USTEPS_PER_MOTOR_REV * GEAR_DRIVEN_TEETH % N_RING == 0,
              "usteps/motor-rev * driven teeth must divide the ring size exactly, "
              "otherwise USTEPS_PER_FLAP_NUM below silently truncates");

inline constexpr int64_t USTEPS_PER_FLAP_NUM =
    USTEPS_PER_MOTOR_REV * GEAR_DRIVEN_TEETH / N_RING;       // 5440
inline constexpr int64_t USTEPS_PER_FLAP_DEN = GEAR_DRIVE_TEETH;  // 33

// usteps per full spool revolution = 272000/33 = 8242.4242...  Also not an integer.
inline constexpr int64_t USTEPS_PER_SPOOL_REV_NUM = USTEPS_PER_MOTOR_REV * GEAR_DRIVEN_TEETH;
inline constexpr int64_t USTEPS_PER_SPOOL_REV_DEN = GEAR_DRIVE_TEETH;

// Rounded nominal, used only as the expected edge-to-edge distance (spec 5.4).
// The 0.42 ustep residue is absorbed at every Hall edge and never accumulates,
// so a +-1 error here is irrelevant against HALL_TOL_SILENT (~41 usteps).
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
// directly, never by accumulating per-flap increments.
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

// Sanity: these are the numbers printed in spec 3.  If a teeth count above
// changes, these fire and force the spec table to be updated with the bench
// result rather than silently drifting.
static_assert(USTEPS_PER_FLAP_NUM == 5440 && USTEPS_PER_FLAP_DEN == 33,
              "usteps/flap is no longer 5440/33 - update spec 3 and the decision log");
static_assert(USTEPS_PER_SPOOL_REV_NOMINAL == 8242, "spool rev != 8242 - update spec 3");
static_assert(ring_target_usteps(0) == 0, "T(0) must be 0");
static_assert(ring_target_usteps(1) == 165, "T(1) = round(164.8485) = 165");
static_assert(ring_target_usteps(50) == 8242, "T(50) = round(8242.4242) = 8242");

}  // namespace swan
