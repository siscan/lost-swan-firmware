#include "frame/frame.h"

#include <cstddef>

#include "ring/ring.h"

namespace swan {
namespace {

// Forward flip distance from a (possibly unknown) display position.
int flips_from(const MotionPort::Col& c, int target) {
    int from = RING_INVALID;
    if (c.state == AxisState::Moving) {
        // A closed-loop move is measured from its destination (a replacement
        // continues from there).  An open-loop move (spin) has no destination
        // and the stale index is a lie - the position is unknowable until it
        // settles, so budget the full wrap.
        from = ring_index_valid(c.dest_index) ? c.dest_index : RING_INVALID;
    } else if (ring_index_valid(c.index)) {
        from = c.index;
    }
    if (from == RING_INVALID) return RING_SLOT_COUNT - 1;  // unknown: full wrap
    return ring_forward_distance(from, target);
}

}  // namespace

int64_t move_duration_ms(int flips, int32_t flaps_s, int32_t accel) {
    if (flips <= 0) return 0;
    const int64_t d = ring_target_usteps(flips);          // usteps
    const int64_t v = flaps_s_to_usteps_s(flaps_s);       // usteps/s
    if (v <= 0 || accel <= 0) return 0;

    // Trapezoid: accel v/a covering v^2/2a, same to brake; cruise the rest.
    const int64_t ramp_d = (v * v) / accel;  // both ramps together
    if (d >= ramp_d) {
        // t = d/v + v/a, in ms without floating point.
        return (d * 1000) / v + (v * 1000) / accel;
    }
    // Triangular: t = 2*sqrt(d/a).  Integer sqrt on d*4e6/a.
    const int64_t x = (d * 4000000) / accel;  // (2000*sqrt(d/a))^2 = 4e6*d/a
    int64_t lo = 0, hi = 200000;              // up to 200 s
    while (lo < hi) {
        const int64_t mid = (lo + hi + 1) / 2;
        if (mid * mid <= x) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

int64_t FrameScheduler::lead_ms(const Frame& f) {
    int64_t worst = 0;
    for (int i = 0; i < N_COLUMNS; ++i) {
        const int flips = flips_from(port_.col(i), f.idx[static_cast<size_t>(i)]);
        const int64_t d = move_duration_ms(flips, timing_.flaps_s, timing_.accel);
        if (d > worst) worst = d;
    }
    return worst;
}

void FrameScheduler::issue(const Frame& f) {
    for (int i = 0; i < N_COLUMNS; ++i) {
        const size_t k = static_cast<size_t>(i);
        const MotionPort::Col c = port_.col(i);
        const int want = f.idx[k];
        // What the axis reports lags what we have already posted: the mailbox
        // is drained by the 1 kHz control tick, so a command issued earlier in
        // THIS same modes tick is still invisible here.  posted_ is that
        // missing knowledge.  Without it a column whose new target happens to
        // equal its stale reported index was skipped, leaving a superseded
        // command in the mailbox to execute instead - e.g. a freshly re-homed
        // column flipping to a clock digit while the other four went blank.
        if (posted_[k] == want) continue;   // already commanded exactly there
        // A command posted earlier this tick to a DIFFERENT target is simply
        // superseded - replace-on-write, newest wins - so fall through.
        if (posted_[k] == kNotPosted) {
            const bool moving_there = c.state == AxisState::Moving && c.dest_index == want;
            const bool showing = c.state == AxisState::Idle && c.index == want;
            if (moving_there || showing) continue;
        }
        if (port_.go(i, want)) posted_[k] = want;
    }
}

void FrameScheduler::note_tick(int64_t now_ms) {
    if (now_ms == posted_tick_ms_) return;
    posted_tick_ms_ = now_ms;
    posted_.fill(kNotPosted);
}

void FrameScheduler::show(const Frame& f, int64_t now_ms, int64_t land_at_ms) {
    note_tick(now_ms);
    if (land_at_ms > now_ms) {
        const int64_t start = land_at_ms - lead_ms(f);
        if (start > now_ms) {
            pending_ = f;
            have_pending_ = true;
            start_at_ms_ = start;
            return;
        }
        // The lead no longer fits - land late rather than never.
    }
    have_pending_ = false;
    desired_ = f;
    have_desired_ = true;
    issue(f);
}

void FrameScheduler::spin_all(int32_t flaps_s, int seconds, int64_t now_ms) {
    note_tick(now_ms);
    have_pending_ = false;  // a spin supersedes a scheduled frame
    for (int i = 0; i < N_COLUMNS; ++i) {
        // RING_INVALID is what an open-loop move lands on, so recording it
        // stops the convergence pass later in this same tick from posting a
        // `go` over the spin it just started.  With zero_hold_s = 0 that was
        // deterministic: the columns that had not yet moved to 000:00 never
        // whirled at all.
        if (port_.spin(i, flaps_s, seconds)) posted_[static_cast<size_t>(i)] = RING_INVALID;
    }
}

void FrameScheduler::tick(int64_t now_ms) {
    note_tick(now_ms);
    if (have_pending_ && now_ms >= start_at_ms_) {
        have_pending_ = false;
        desired_ = pending_;
        have_desired_ = true;
        issue(desired_);
    }
    if (!have_desired_) return;

    // Convergence: an Idle column that is not showing its desired index gets
    // the frame re-issued.  This is the spec 5.4 / decision-log obligation -
    // after an automatic re-home the column sits Idle at index 0 and this
    // brings it back - and it also lands the post-spin choreography (index
    // unknown after open-loop) and retries anything the mailbox rejected.
    for (int i = 0; i < N_COLUMNS; ++i) {
        const size_t k = static_cast<size_t>(i);
        const MotionPort::Col c = port_.col(i);
        const int want = desired_.idx[k];
        // Anything commanded during this tick - a frame, or a spin - stands.
        // The axis has not reported it yet, so "Idle at the wrong index" here
        // is a stale observation, and acting on it would post a `go` over the
        // command that was just issued.  With zero_hold_s = 0 that reliably
        // killed the alarm spin on any column still on its old digit.
        if (posted_[k] != kNotPosted) continue;
        if (c.state == AxisState::Idle && c.index != want) {
            if (port_.go(i, want)) posted_[k] = want;
        }
    }
}

bool FrameScheduler::settled() {
    if (!have_desired_ || have_pending_) return false;
    for (int i = 0; i < N_COLUMNS; ++i) {
        const MotionPort::Col c = port_.col(i);
        if (c.state != AxisState::Idle || c.index != desired_.idx[static_cast<size_t>(i)]) {
            return false;
        }
    }
    return true;
}

}  // namespace swan
