// Simulated axis: a modeled drum + the REAL step-ISR helpers + the REAL
// control tick, run in lockstep in one thread.  Phase 1.5 (spec 15).
//
// The only fake parts are the drum mechanics and the Hall sensor model; every
// line of control logic executed here is the same axis_control.cpp the
// firmware links.
#pragma once

#include <cstdint>
#include <vector>

#include "motion/axis_control.h"

namespace swan {
namespace sim {

// Floor division for possibly-negative numerators.
inline int64_t floordiv(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

// ---------------------------------------------------------------------------
// The drum.  One revolution is REV_NUM/REV_DEN microsteps - NOT an integer -
// so mechanics are computed in integer units of 1/REV_DEN microstep and stay
// exact over any number of revolutions.
//
// Default: the 1:1 direct drive, rev = 3200 usteps exactly (rev_num=3200,
// rev_den=1).  The fractional form is kept so a wrong drum can be modelled -
// the 85T/33T rim gear is rev_num=272000, rev_den=33, which is what
// test_wrong_drum_faults_immediately drives.
//
// The Hall operate window starts at each edge position and extends
// window_usteps forward.  Optional per-edge jitter shifts each edge by a
// deterministic pseudo-random amount in [-jitter, +jitter] usteps (no
// rand(): reproducibility is the point of a simulation).
// ---------------------------------------------------------------------------
struct SimDrum {
    int64_t rev_num = USTEPS_PER_SPOOL_REV_NUM;  // 3200
    int64_t rev_den = USTEPS_PER_SPOOL_REV_DEN;  // 1
    int64_t start_angle_usteps = 0;  // mechanical angle at pos_abs == 0
    int64_t window_usteps = 60;      // operate window width
    int32_t jitter_usteps = 0;       // max +- per-edge jitter
    int64_t slip_usteps = 0;         // accumulated mechanical slip (drum behind motor)

    int64_t mech_units(int64_t pos_abs) const {  // in 1/rev_den usteps
        return (start_angle_usteps + pos_abs - slip_usteps) * rev_den;
    }

    int64_t jitter_for_edge(int64_t k) const {
        if (jitter_usteps == 0) return 0;
        const int64_t span = 2 * jitter_usteps + 1;
        int64_t j = ((k * 7919 + 13) % span + span) % span - jitter_usteps;
        return j * rev_den;
    }

    // Is the Hall asserted at this motor position?  Edge k sits at mechanical
    // k*rev_num (+jitter); the window extends window_usteps forward from it.
    bool hall_at(int64_t pos_abs) const {
        const int64_t m = mech_units(pos_abs);
        const int64_t w = window_usteps * rev_den;
        // The window belongs to edge k where k*rev is at or just below m; with
        // jitter the previous edge's window can also still cover m.
        for (int64_t k = floordiv(m, rev_num) + 1; k >= floordiv(m, rev_num) - 1; --k) {
            const int64_t e = k * rev_num + jitter_for_edge(k);
            if (m >= e && m < e + w) return true;
        }
        return false;
    }

    // Microsteps travelled since the most recent operate edge at or before this
    // position - the independent oracle for "which flap is showing".
    // Returns a value in usteps (rounded down).
    int64_t offset_past_edge(int64_t pos_abs) const {
        const int64_t m = mech_units(pos_abs);
        for (int64_t k = floordiv(m, rev_num) + 1;; --k) {
            const int64_t e = k * rev_num + jitter_for_edge(k);
            if (e <= m) return (m - e) / rev_den;
        }
    }
};

// ---------------------------------------------------------------------------
// One simulated axis.  tick() advances 20 us of simulated time: the control
// tick runs every TICK_HZ/CONTROL_HZ-th call (1 kHz), then the fake ISR (the
// real per-axis helpers against the drum model).
// ---------------------------------------------------------------------------
struct SimAxis {
    AxisIsr isr;
    AxisCtl ctl;
    SimDrum drum;
    MotionParams params;
    Request mailbox;  // single slot, replace-on-write - same semantics as the shell

    int64_t ticks = 0;
    static constexpr int CONTROL_DIV = TICK_HZ / CONTROL_HZ;  // 50

    // Event log, for the mailbox-ordering assertions.
    struct Drained {
        ReqKind kind;
        int dest;  // dest_index after the tick that drained it
        bool rejected;
    };
    std::vector<Drained> drained;
    TickEvents last_events;
    uint32_t homed_count = 0;
    uint32_t fault_events = 0;
    uint32_t rehome_events = 0;
    uint32_t gave_up_events = 0;

    void post(const Request& r) { mailbox = r; }

    void post_home(uint32_t delay = 1) {
        Request r;
        r.kind = ReqKind::Home;
        r.delay_ticks = delay;
        post(r);
    }

    void post_go(int index) {
        Request r;
        r.kind = ReqKind::Go;
        r.index = index;
        post(r);
    }

    void post_step_open(int64_t usteps, int32_t flaps_s) {
        Request r;
        r.kind = ReqKind::StepOpen;
        r.usteps = usteps;
        r.flaps_s = flaps_s;
        post(r);
    }

    void control_once() {
        const IsrSnap in{isr.pos_abs, isr.hall_abs,   isr.target_abs,
                         isr.velocity, isr.hall_seq, isr.hall_active};
        const Request req = mailbox;
        mailbox.kind = ReqKind::None;

        TickEvents ev;
        const IsrWrite w = axis_control_tick(ctl, in, req, params, ev);
        if (w.set_target) isr.target_abs = w.target;
        isr.velocity = w.velocity;

        last_events = ev;
        if (ev.drained != ReqKind::None) {
            drained.push_back({ev.drained, ctl.dest_index.load(std::memory_order_relaxed),
                               ev.req_rejected});
        }
        if (ev.homed) ++homed_count;
        if (ev.fault) ++fault_events;
        if (ev.rehome) ++rehome_events;
        if (ev.gave_up) ++gave_up_events;
    }

    // ISR first, control at the end of every 50th tick - mirroring hardware,
    // where the 50 kHz ISR has run (and primed the Hall filter) for many
    // periods before any given 1 kHz control tick.
    void tick() {
        if (axis_isr_dda(isr)) { /* step pulse - nothing extra to model */ }
        axis_isr_hall(isr, drum.hall_at(isr.pos_abs));
        ++ticks;
        if (ticks % CONTROL_DIV == 0) control_once();
    }

    void run(int64_t n) {
        for (int64_t i = 0; i < n; ++i) tick();
    }

    AxisState state() const { return ctl.state.load(std::memory_order_relaxed); }
    int index() const { return ctl.index.load(std::memory_order_relaxed); }

    // Run until the axis settles in `want` (default Idle).  A command still
    // sitting in the mailbox is waited out first - otherwise "post go, wait
    // for Idle" would return before the command was ever drained, since the
    // axis is Idle at the moment of posting.  Returns false on timeout or if
    // it latches FAULT while waiting for something else.
    bool run_until(AxisState want, int64_t max_ticks) {
        bool pending = (mailbox.kind != ReqKind::None);
        for (int64_t i = 0; i < max_ticks; ++i) {
            tick();
            if (pending) {
                if (mailbox.kind != ReqKind::None) continue;  // not drained yet
                pending = false;
            }
            const AxisState s = state();
            if (s == want) return true;
            if (s == AxisState::Fault && want != AxisState::Fault) return false;
        }
        return false;
    }

    bool run_until_idle(int64_t max_ticks = 2'000'000) {
        return run_until(AxisState::Idle, max_ticks);
    }

    // The oracle: how many usteps past the operate edge the drum is showing.
    int64_t shown_offset() const { return drum.offset_past_edge(isr.pos_abs); }
};

}  // namespace sim
}  // namespace swan
