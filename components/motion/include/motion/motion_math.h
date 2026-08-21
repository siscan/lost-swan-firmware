// Motion arithmetic: position model, ramp, DDA, Hall edge verification.
// Pure: no IDF includes, so every line here is exercised by test/host.
// Spec 5.2-5.4.
#pragma once

#include <cstdint>

#include "ring/geometry.h"
#include "ring/ring.h"

namespace swan {

// Step ISR rate (spec 5.2).  One DDA tick per axis per alarm.
inline constexpr int32_t TICK_HZ = 50000;
// Velocity control tick (spec 5.2).  Deliberately NOT in the step ISR.
inline constexpr int32_t CONTROL_HZ = 1000;

// ---------------------------------------------------------------------------
// Position model (spec 5.3)
//
// pos_abs   usteps ever issued, monotonic - rotation is forward-only.
// hall_abs  pos_abs latched at the most recent Hall operate edge.
// cal_offset usteps from that edge to the blank flap hanging correctly (NVS).
//
// Invariant at rest:  pos_abs == hall_abs + cal_offset + T(index)
// ---------------------------------------------------------------------------

constexpr int64_t index_position(int64_t hall_abs, int32_t cal_offset, int64_t i) {
    return hall_abs + cal_offset + ring_target_usteps(i);
}

// Forward-only target for a move to ring index `to`.  If index `to` in this
// revolution already lies behind us, the target is provisionally its position
// one revolution on; the real Hall edge corrects it mid-move (retarget_on_edge).
constexpr int64_t plan_target(int64_t hall_abs, int32_t cal_offset, int64_t pos_abs, int to) {
    int64_t t = index_position(hall_abs, cal_offset, to);
    // A single move is < 1 revolution, so one wrap always suffices; the second
    // is insurance against a caller passing a stale pos_abs.
    for (int wrap = 1; t < pos_abs && wrap <= 2; ++wrap) {
        t = index_position(hall_abs, cal_offset, to + static_cast<int64_t>(RING_SLOT_COUNT) * wrap);
    }
    return t;
}

// The edge arrived mid-move: rebase the target on the freshly latched edge.
// Clamped forward - reverse is mechanically forbidden, so an overshoot stops here.
constexpr int64_t retarget_on_edge(int64_t hall_abs_new, int32_t cal_offset, int64_t pos_abs,
                                   int to) {
    const int64_t t = index_position(hall_abs_new, cal_offset, to);
    return t < pos_abs ? pos_abs : t;
}

// ---------------------------------------------------------------------------
// Hall edge verification (spec 5.4)
// ---------------------------------------------------------------------------

enum class EdgeVerdict : unsigned char {
    Minor = 0,  // within HALL_TOL_SILENT: accept, resync_minor++
    Major,      // <= 1 flap: accept, resync_major++, warn
    Fault,      // > 1 flap: stop and re-home
};

struct EdgeTolerances {
    int32_t silent;  // motion.hall_tol, usteps.  Set from Phase 1 bench step 6.
    int32_t major;   // one flap
};

inline constexpr EdgeTolerances DEFAULT_EDGE_TOLERANCES{
    41,                                          // 1/4 flap [default], spec 5.4
    static_cast<int32_t>(ring_target_usteps(1)),  // 165 usteps
};

constexpr int64_t edge_error(int64_t hall_abs_prev, int64_t hall_abs_now) {
    return (hall_abs_now - hall_abs_prev) - USTEPS_PER_SPOOL_REV_NOMINAL;
}

constexpr EdgeVerdict classify_edge_error(int64_t err, const EdgeTolerances& tol) {
    const int64_t a = err < 0 ? -err : err;
    if (a <= tol.silent) return EdgeVerdict::Minor;
    if (a <= tol.major) return EdgeVerdict::Major;
    return EdgeVerdict::Fault;
}

// No edge within one revolution + one flap of stepping -> FAULT (spec 5.4).
constexpr bool edge_overdue(int64_t pos_abs, int64_t hall_abs) {
    return (pos_abs - hall_abs) > USTEPS_PER_SPOOL_REV_NOMINAL + ring_target_usteps(1);
}

// ---------------------------------------------------------------------------
// Velocity ramp, evaluated on the control tick (spec 5.2).
// Linear accel/decel; short moves come out triangular for free because the
// brake test fires before v_max is reached.
// ---------------------------------------------------------------------------

struct RampParams {
    int32_t v_max;    // usteps/s
    int32_t accel;    // usteps/s^2  (motion.accel, default 82000)
    int32_t tick_hz;  // CONTROL_HZ
};

constexpr int32_t ramp_next_velocity(int64_t remaining, int32_t v, const RampParams& p) {
    if (remaining <= 0) return 0;

    int32_t dv = p.accel / p.tick_hz;
    if (dv < 1) dv = 1;

    // Distance needed to brake from v to rest: v^2 / 2a.
    const int64_t brake = (static_cast<int64_t>(v) * v) / (2 * static_cast<int64_t>(p.accel));

    if (remaining <= brake) {
        v -= dv;
    } else {
        v += dv;
        if (v > p.v_max) v = p.v_max;
    }

    // Never stall while distance remains, and never outrun the DDA (one step
    // per tick maximum - see dda_tick).
    if (v < dv) v = dv;
    if (v > TICK_HZ) v = TICK_HZ;
    return v;
}

// ---------------------------------------------------------------------------
// DDA, evaluated in the step ISR.  v <= TICK_HZ guarantees at most one step per
// tick, which is what lets all five axes share one bank write.
// ---------------------------------------------------------------------------
constexpr bool dda_tick(uint32_t& accum, int32_t v_usteps_s) {
    accum += static_cast<uint32_t>(v_usteps_s);
    if (accum >= static_cast<uint32_t>(TICK_HZ)) {
        accum -= static_cast<uint32_t>(TICK_HZ);
        return true;
    }
    return false;
}

}  // namespace swan
