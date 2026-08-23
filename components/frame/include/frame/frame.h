// The frame layer (spec 6): five ring indices shown together.  Pure - the
// scheduler talks to motion through MotionPort, which the host tests fake and
// motion_port.cpp implements on target.
//
// Responsibilities: simultaneous starts, land-on-tick lead scheduling
// (spec 7.3), frame replacement while moving (mailbox semantics downstream),
// and the decision-log obligation: after a column's automatic re-home, the
// current frame is re-issued so the column converges back to what it should
// be showing (spec 5.4 "resume the current frame").
#pragma once

#include <array>
#include <climits>
#include <cstdint>

#include "hal/pins.h"
#include "motion/motion_types.h"
#include "ring/geometry.h"
#include "ring/ring.h"

namespace swan {

struct Frame {
    std::array<int, N_COLUMNS> idx{};

    bool operator==(const Frame& o) const { return idx == o.idx; }
    bool operator!=(const Frame& o) const { return !(*this == o); }
};

// What the scheduler needs from motion - a consistent per-column snapshot
// (the seqlock on target) and the two commands it issues.
class MotionPort {
public:
    virtual ~MotionPort() = default;

    struct Col {
        AxisState state;
        int index;       // displayed; RING_INVALID after open-loop stepping
        int dest_index;  // current move's destination
    };
    virtual Col col(int i) = 0;
    virtual bool go(int i, int index) = 0;
    virtual bool spin(int i, int32_t flaps_s, int seconds) = 0;  // open loop
};

// Duration of a move of `flips` forward flips at cruise speed `flaps_s` with
// ramp `accel` (usteps/s^2).  Mirrors the target ramp: trapezoidal, or
// triangular when the distance is too short to reach cruise.  The simulated-
// axis suite pins this against the real controller.
int64_t move_duration_ms(int flips, int32_t flaps_s, int32_t accel);

class FrameScheduler {
public:
    struct Timing {
        int32_t flaps_s = 15;   // motion.flaps_s_normal
        int32_t accel = 82000;  // motion.accel
    };

    explicit FrameScheduler(MotionPort& port) : port_(port) { posted_.fill(kNotPosted); }
    FrameScheduler(MotionPort& port, Timing t)
        : port_(port), timing_(t) { posted_.fill(kNotPosted); }

    void set_timing(Timing t) { timing_ = t; }

    // The lead the longest column needs to land frame `f` from what is
    // displayed now.  Moving columns are measured from their destination;
    // columns with an unknown index (post-spin) count the full wrap.
    int64_t lead_ms(const Frame& f);

    // Show a frame.  land_at_ms == 0: start every column now (clock mode -
    // moves START on the tick).  land_at_ms > now_ms: delay the simultaneous
    // start so the longest column finishes at land_at_ms (countdown mode -
    // moves LAND on the tick); if the lead no longer fits, start immediately.
    // A new show() replaces any pending one (spec 6).
    void show(const Frame& f, int64_t now_ms, int64_t land_at_ms = 0);

    // The zero-choreography spin: every column open-loop at `flaps_s` for
    // `seconds`.  The desired frame is left untouched, so convergence lands
    // the columns back on it (or on a reveal frame shown right after).
    // `now_ms` is the tick this belongs to, for the same reason show() takes
    // one: the spin must be recorded as commanded-this-tick, or the
    // convergence pass that follows it in the same tick posts a `go` over it.
    void spin_all(int32_t flaps_s, int seconds, int64_t now_ms);

    // Drops a scheduled-but-not-started frame.  Used when the timebase steps
    // (SNTP resync): the pending start instant lived in the old timebase and
    // the mode re-derives its schedule.
    void cancel_pending() { have_pending_ = false; }

    // Call frequently (>= a few Hz).  Starts a pending frame whose time has
    // come and converges columns: any Idle column not displaying its desired
    // index is re-issued - this is what resumes the frame after an automatic
    // re-home, finishes the post-spin landing, and retries a rejected go.
    void tick(int64_t now_ms);

    bool settled();                       // every column Idle on its desired index
    const Frame& desired() const { return desired_; }
    bool has_desired() const { return have_desired_; }
    bool pending() const { return have_pending_; }

private:
    MotionPort& port_;
    Timing timing_;

    Frame desired_{};
    bool have_desired_ = false;
    Frame pending_{};
    bool have_pending_ = false;
    int64_t start_at_ms_ = 0;

    // Per column, what was commanded during THIS tick: kNotPosted for nothing,
    // an index for a go, RING_INVALID for a spin (which lands nowhere
    // knowable).  The axis cannot report any of it yet - the mailbox is
    // drained by the 1 kHz control tick - so without this the scheduler
    // reasons from a snapshot its own command has already invalidated.
    static constexpr int kNotPosted = -2;
    std::array<int, N_COLUMNS> posted_{};
    // The tick `posted_` belongs to.  show() and tick() both stamp it, and
    // whichever runs first at a new timestamp clears the record - so a frame
    // issued after the convergence pass in the SAME tick still sees what that
    // pass commanded, which is exactly the case the record exists for.
    int64_t posted_tick_ms_ = INT64_MIN;
    void note_tick(int64_t now_ms);

    void issue(const Frame& f);
};

}  // namespace swan
