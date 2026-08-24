// The axis control core: FSM, homing, edge verification, command mailbox
// semantics, and the step-ISR per-axis helpers.
//
// Pure: no IDF includes.  The firmware shell (motion.cpp) and the host-side
// simulated-axis suite (test/host/test_axis_sim.cpp) both drive THIS code, so
// what the tests exercise is what runs on the device.  Synchronisation is the
// shell's job; the contract is docs/MOTION_SYNC.md.  The core operates on a
// consistent snapshot of the ISR state and returns the writes to apply -
// taking and holding locks is deliberately impossible from in here.
#pragma once

#include <atomic>
#include <cstdint>

#include "motion/motion_math.h"
#include "motion/fault_policy.h"
#include "motion/motion_types.h"
#include "ring/ring.h"

namespace swan {

inline constexpr int REHOME_RETRIES = 3;
inline constexpr uint32_t HOME_STAGGER_MS = 250;  // spec 5.5, limits inrush
// 1.2 revolutions without an edge is a homing timeout (spec 5.5).
inline constexpr int64_t HOME_LIMIT = (USTEPS_PER_SPOOL_REV_NOMINAL * 6) / 5;

enum class HomePhase : unsigned char { None, Release, Seek, Settle };

// ---------------------------------------------------------------------------
// (1) Step-ISR state.  Plain data; on target it is DRAM_ATTR-placed and every
// task-side access happens under the motion spinlock.  On the host the fake
// ISR and the control tick share one thread, so plain access is exact.
// ---------------------------------------------------------------------------
struct AxisIsr {
    int64_t pos_abs = 0;
    int64_t target_abs = 0;
    int64_t hall_abs = 0;
    int32_t velocity = 0;
    uint32_t accum = 0;
    uint32_t hall_seq = 0;
    uint8_t hall_hist = 0;
    bool hall_active = false;
};

// Per-axis step-ISR helpers.  SWAN_ALWAYS_INLINE so they fold into their caller
// and inherit its placement: the target ISR lives in IRAM and must never call
// flash-resident code (spec 5.2).  The host fake ISR calls the same two
// functions, which is what makes the simulation faithful.
#if defined(__GNUC__)
#define SWAN_ALWAYS_INLINE __attribute__((always_inline)) inline
#else
#define SWAN_ALWAYS_INLINE inline
#endif

// DDA advance.  Returns true when a STEP pulse is due this tick (at most one -
// velocity is clamped to TICK_HZ, which is what lets all five axes share a
// single GPIO bank write).
SWAN_ALWAYS_INLINE bool axis_isr_dda(AxisIsr& a) {
    const int32_t v = a.velocity;
    if (v > 0 && a.pos_abs < a.target_abs) {
        if (dda_tick(a.accum, v)) {
            ++a.pos_abs;
            return true;
        }
    }
    return false;
}

// Hall sample: 2-of-3 majority filter; the operate edge latches pos_abs
// atomically with the step count (spec 5.2).  `raw` is already
// polarity-corrected (true = magnet present).
SWAN_ALWAYS_INLINE void axis_isr_hall(AxisIsr& a, bool raw) {
    const uint8_t h = static_cast<uint8_t>(((a.hall_hist << 1) | (raw ? 1u : 0u)) & 0x7u);
    a.hall_hist = h;
    const unsigned ones = (h & 1u) + ((h >> 1) & 1u) + ((h >> 2) & 1u);
    const bool level = ones >= 2u;
    if (level && !a.hall_active) {
        a.hall_abs = a.pos_abs;
        ++a.hall_seq;
    }
    a.hall_active = level;
}

// ---------------------------------------------------------------------------
// (2) Command mailbox.  One slot per axis, replace-on-write: a newer command
// posted before the drain supersedes the older one, which is exactly the frame
// semantics of spec 6 ("a new frame while moving simply replaces targets").
// A superseded command is replaced whole - it is never partially applied and a
// stale command is never applied after a newer one.
// ---------------------------------------------------------------------------
enum class ReqKind : unsigned char { None, Home, Go, StepOpen, Stop };

struct Request {
    ReqKind kind = ReqKind::None;
    int index = 0;             // Go
    int64_t usteps = 0;        // StepOpen
    int32_t flaps_s = 0;       // StepOpen
    uint32_t delay_ticks = 0;  // Home
};

// ---------------------------------------------------------------------------
// (3) Control-side state.  The atomics are published by the control tick (the
// sole writer of the FSM) and read by anyone.  Each is stored relaxed and is
// independently meaningful; a reader that needs TWO OR MORE of them as a
// consistent pair (state + index, revs + hall_to_hall, ...) must go through
// axis_read_published(), which brackets them with the `seq` seqlock the tick
// maintains - see docs/MOTION_SYNC.md.  The plain fields are tick-private.
// ---------------------------------------------------------------------------
struct AxisCtl {
    // Seqlock: odd while axis_control_tick is writing.  acq_rel on entry keeps
    // the tick's stores after it; release on exit keeps them before it.
    std::atomic<uint32_t> seq{0};

    std::atomic<AxisState> state{AxisState::Unhomed};
    std::atomic<int> index{RING_INVALID};
    std::atomic<int> dest_index{RING_INVALID};
    std::atomic<bool> hall_valid{false};
    std::atomic<uint32_t> revs{0};
    std::atomic<uint32_t> resync_minor{0};
    std::atomic<uint32_t> resync_major{0};
    std::atomic<uint32_t> faults{0};
    std::atomic<int32_t> last_hall_err{0};
    std::atomic<int32_t> hall_to_hall{0};
    // Which automatic re-home attempt is in flight, 0 when not retrying.
    // Published so the UI can say "column 3, attempt 2 of 3" instead of
    // leaving a hunting column indistinguishable from an idle one.
    std::atomic<uint8_t> rehome_attempt{0};
    // Why this axis last faulted.  Published because the two causes want
    // opposite responses and the operator needs to be told which one it is:
    // "sensor or wiring" and "it is jammed" are different call-outs.
    std::atomic<uint8_t> fault_cause{static_cast<uint8_t>(FaultCause::None)};

    // Written by the calibration API from any task, read here.  A single value
    // with no companion invariant; live nudging during a move is intentional
    // (spec 5.6).
    std::atomic<int32_t> cal_offset{0};

    // --- control-tick private ---
    HomePhase home_phase = HomePhase::None;
    int32_t v_max = 0;
    int64_t hall_prev = 0;
    uint32_t seq_seen = 0;
    uint32_t home_delay = 0;  // control ticks until homing starts
    uint8_t rehome_retries = 0;
    // Latched at the Seek->Settle edge and published with `homed`; see
    // TickEvents::recovered_after.
    uint8_t recovered_after = 0;
};

static_assert(std::atomic<int32_t>::is_always_lock_free,
              "32-bit atomics must be lock-free; a libatomic call next to the "
              "step ISR would not be safe");
static_assert(std::atomic<AxisState>::is_always_lock_free, "AxisState must be lock-free");

// Consistent snapshot of one axis's ISR state, taken by the shell under the
// spinlock (or plainly, on the host).
struct IsrSnap {
    int64_t pos;
    int64_t hall;
    int64_t target;
    int32_t velocity;
    uint32_t seq;
    bool hall_active;
};

// Writes the tick decided on.  The shell applies them under one short critical
// section; velocity is always written, the target only when set_target is true.
struct IsrWrite {
    bool set_target = false;
    int64_t target = 0;
    int32_t velocity = 0;
};

// Everything log-worthy that happened during one tick.  The shell turns these
// into ESP_LOGx; the host suite asserts on them.  Numeric context (positions,
// errors) comes from the snapshot and the AxisCtl atomics.
struct TickEvents {
    ReqKind drained = ReqKind::None;  // request taken from the mailbox this tick
    bool req_rejected = false;        // ... and rejected by the FSM
    bool homed = false;
    bool fault = false;
    const char* fault_reason = nullptr;
    FaultCause fault_cause = FaultCause::None;
    bool rehome = false;              // fault -> automatic re-home scheduled
    uint8_t rehome_attempt = 0;
    bool gave_up = false;             // retries exhausted -> latched FAULT
    // Attempts this homing pass cost, published WITH `homed` - because
    // rehome_attempt is cleared at the Seek->Settle edge, several hundred
    // milliseconds before the pass completes, so reading it when `homed`
    // arrives always saw 0 and "column recovered" could never be written.
    uint8_t recovered_after = 0;
    bool resync_major = false;
};

// One control tick for one axis: drain the (already snapshotted) request,
// process a Hall edge if one arrived, run the FSM, compute the velocity.
// The request must have been removed from the mailbox in the SAME critical
// section as the snapshot - see motion.cpp / MOTION_SYNC.md.  Brackets its
// publication with the seqlock.
IsrWrite axis_control_tick(AxisCtl& a, const IsrSnap& in, const Request& req,
                           const MotionParams& p, TickEvents& ev);

// A mutually consistent view of everything the control tick publishes - all
// fields from the same tick.  This is the ONLY sanctioned way to read two or
// more of the AxisCtl atomics together.  Retries while a tick is mid-write
// (rare: the writer runs for microseconds once a millisecond).
struct AxisPublished {
    AxisState state;
    int index;
    int dest_index;
    bool hall_valid;
    uint32_t revs;
    uint32_t resync_minor;
    uint32_t resync_major;
    uint32_t faults;
    int32_t last_hall_err;
    int32_t hall_to_hall;
    int32_t cal_offset;  // written by the cal API, not the tick; single value
    uint8_t rehome_attempt;  // 0 = not retrying, else 1..REHOME_RETRIES
    FaultCause fault_cause;
};
AxisPublished axis_read_published(const AxisCtl& a);

// Calibration offsets are stored normalised into [0, one revolution): index 0
// only has a position modulo one revolution, and the homing settle phase
// relies on the offset pointing forward of the Hall edge.
int32_t normalize_cal(int32_t usteps);

}  // namespace swan
