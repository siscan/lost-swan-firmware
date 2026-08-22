// Axis control core - pure, host-testable.  See axis_control.h for the
// synchronisation contract.  This file is a 1:1 transcription of the FSM that
// used to live inside motion.cpp; behaviour changes belong here AND in the
// simulated-axis suite, never in the shell.

#include "motion/axis_control.h"

namespace swan {
namespace {

constexpr auto RLX = std::memory_order_relaxed;

struct TickCtx {
    AxisCtl& a;
    const IsrSnap& in;
    const MotionParams& p;
    TickEvents& ev;
    int64_t tgt;       // running view of the target; starts at the snapshot's
    bool tgt_written = false;

    void set_target(int64_t t) {
        tgt = t;
        tgt_written = true;
    }
};

void enter_fault(TickCtx& c, const char* why) {
    AxisCtl& a = c.a;
    a.faults.fetch_add(1, RLX);
    c.ev.fault = true;
    c.ev.fault_reason = why;

    if (a.rehome_retries < REHOME_RETRIES) {
        ++a.rehome_retries;
        c.ev.rehome = true;
        c.ev.rehome_attempt = a.rehome_retries;
        a.home_delay = 1;
        a.state.store(AxisState::Unhomed, RLX);
        // A fault raised while processing the Hall edge reaches the Unhomed
        // case below in this same tick, so that re-home starts immediately;
        // faults raised inside the FSM switch start it one tick later.
        // Velocity is deliberately NOT forced to zero here: the ramp converges
        // onto the homing speed instead, so a fault->re-home is a smooth
        // forward transition rather than an instantaneous stop (forward-only
        // rotation makes that safe; the old shell zeroed velocity, which was a
        // step discontinuity, not a braked stop, so nothing gentler was lost).
    } else {
        // Phase 6 applies the fault display policy (Q5: park on blank, others
        // continue).  `home` clears the latch.
        c.ev.gave_up = true;
        a.state.store(AxisState::Fault, RLX);
    }
}

void begin_home(TickCtx& c) {
    AxisCtl& a = c.a;
    a.dest_index.store(RING_HOME_SLOT, RLX);
    a.index.store(RING_INVALID, RLX);
    a.hall_valid.store(false, RLX);  // first edge of a pass has no predecessor
    a.v_max = flaps_s_to_usteps_s(c.p.flaps_s_home);
    // If we are sitting inside the magnet zone, step out of it first (spec 5.5).
    a.home_phase = c.in.hall_active ? HomePhase::Release : HomePhase::Seek;
    c.set_target(c.in.pos + HOME_LIMIT);
    a.state.store(AxisState::Homing, RLX);
}

void on_hall_edge(TickCtx& c) {
    AxisCtl& a = c.a;

    if (a.hall_valid.load(RLX)) {
        const int64_t err = edge_error(a.hall_prev, c.in.hall);
        a.hall_to_hall.store(static_cast<int32_t>(c.in.hall - a.hall_prev), RLX);
        a.last_hall_err.store(static_cast<int32_t>(err), RLX);
        a.revs.fetch_add(1, RLX);

        const EdgeTolerances tol{c.p.hall_tol, static_cast<int32_t>(ring_target_usteps(1))};
        switch (classify_edge_error(err, tol)) {
            case EdgeVerdict::Minor:
                a.resync_minor.fetch_add(1, RLX);
                break;
            case EdgeVerdict::Major:
                a.resync_major.fetch_add(1, RLX);
                c.ev.resync_major = true;
                break;
            case EdgeVerdict::Fault:
                enter_fault(c, "hall edge off by more than one flap");
                return;
        }
    }

    a.hall_prev = c.in.hall;
    a.hall_valid.store(true, RLX);

    const int32_t cal = a.cal_offset.load(RLX);
    const AxisState st = a.state.load(RLX);

    if (st == AxisState::Homing && a.home_phase == HomePhase::Seek) {
        a.home_phase = HomePhase::Settle;
        a.rehome_retries = 0;
        // Forward-clamped, NOT plan_target.  By the time the control tick sees
        // the edge, pos has already crept a ustep or two past hall, so with the
        // common cal_offset of 0 plan_target would wrap a whole extra
        // revolution and leave pos referenced to the previous edge.
        c.set_target(retarget_on_edge(c.in.hall, cal, c.in.pos, RING_HOME_SLOT));
        return;
    }

    // A move that crossed home: rebase on the edge that just landed, so the
    // 0.42 ustep/rev residue is absorbed instead of accumulating (spec 5.3).
    const int dest = a.dest_index.load(RLX);
    if (st == AxisState::Moving && ring_index_valid(dest)) {
        c.set_target(retarget_on_edge(c.in.hall, cal, c.in.pos, dest));
    }
}

// Runs on the control tick, which is the only writer of the FSM, so the
// validation here is authoritative - the checks in the shell's public API are
// advisory, for immediate CLI feedback only.
void apply_request(TickCtx& c, const Request& r) {
    AxisCtl& a = c.a;
    c.ev.drained = r.kind;

    switch (r.kind) {
        case ReqKind::Home:
            a.rehome_retries = 0;
            // delay 0 would never fire the countdown and park the axis in
            // Unhomed silently; the minimum is "this tick".
            a.home_delay = r.delay_ticks > 0 ? r.delay_ticks : 1;
            a.state.store(AxisState::Unhomed, RLX);
            break;

        case ReqKind::Go: {
            const AxisState s = a.state.load(RLX);
            if ((s != AxisState::Idle && s != AxisState::Moving) || !a.hall_valid.load(RLX)) {
                c.ev.req_rejected = true;
                break;
            }
            a.dest_index.store(r.index, RLX);
            a.v_max = flaps_s_to_usteps_s(c.p.flaps_s_normal);
            c.set_target(plan_target(c.in.hall, a.cal_offset.load(RLX), c.in.pos, r.index));
            a.state.store(AxisState::Moving, RLX);
            break;
        }

        case ReqKind::StepOpen: {
            if (a.state.load(RLX) == AxisState::Homing) {
                c.ev.req_rejected = true;
                break;
            }
            // Open loop: the drum stays position-tracked, but what is on the
            // front is unknown until the next home.  Deliberately allowed from
            // FAULT - it is the bench tool for un-jamming a column (spec 13);
            // `go` from FAULT stays rejected, so nothing false is displayed
            // (decision log 17).
            a.dest_index.store(RING_INVALID, RLX);
            a.v_max = flaps_s_to_usteps_s(r.flaps_s);
            c.set_target(c.in.pos + r.usteps);
            a.state.store(AxisState::Moving, RLX);
            break;
        }

        case ReqKind::Stop: {
            c.set_target(c.in.pos);
            const AxisState s = a.state.load(RLX);
            if (s == AxisState::Moving) {
                a.state.store(AxisState::Idle, RLX);
            } else if (s == AxisState::Homing) {
                // Abort the homing pass honestly: without this, the truncated
                // target made the phase checks fire - Settle "completed" at the
                // wrong position and Seek raised a spurious FAULT.  Stopped
                // mid-home means unhomed, full stop.
                a.home_phase = HomePhase::None;
                a.index.store(RING_INVALID, RLX);
                a.home_delay = 0;
                a.state.store(AxisState::Unhomed, RLX);
            } else if (s == AxisState::Unhomed) {
                a.home_delay = 0;  // cancel a pending (staggered) home
            }
            break;
        }

        case ReqKind::None:
            break;
    }
}

}  // namespace

// RAII seqlock bracket for the tick's publication window.
struct PublishGuard {
    AxisCtl& a;
    explicit PublishGuard(AxisCtl& ax) : a(ax) { a.seq.fetch_add(1, std::memory_order_acq_rel); }
    ~PublishGuard() { a.seq.fetch_add(1, std::memory_order_release); }
};

IsrWrite axis_control_tick(AxisCtl& a, const IsrSnap& in, const Request& req,
                           const MotionParams& p, TickEvents& ev) {
    const PublishGuard guard(a);
    TickCtx c{a, in, p, ev, in.target, false};

    if (req.kind != ReqKind::None) apply_request(c, req);

    if (in.seq != a.seq_seen) {
        a.seq_seen = in.seq;
        on_hall_edge(c);
    }

    switch (a.state.load(RLX)) {
        case AxisState::Unhomed:
            if (a.home_delay > 0 && --a.home_delay == 0) begin_home(c);
            break;

        case AxisState::Homing:
            if (a.home_phase == HomePhase::Release) {
                if (!in.hall_active) {
                    a.home_phase = HomePhase::Seek;
                    c.set_target(in.pos + HOME_LIMIT);
                } else if (in.pos >= c.tgt) {
                    enter_fault(c, "hall never released");
                }
            } else if (a.home_phase == HomePhase::Seek) {
                if (in.pos >= c.tgt) enter_fault(c, "no hall edge in 1.2 revolutions");
            } else if (a.home_phase == HomePhase::Settle) {
                if (in.pos >= c.tgt) {
                    a.home_phase = HomePhase::None;
                    a.index.store(RING_HOME_SLOT, RLX);
                    a.rehome_retries = 0;
                    a.state.store(AxisState::Idle, RLX);
                    ev.homed = true;
                }
            }
            break;

        case AxisState::Moving:
            if (in.pos >= c.tgt) {
                // RING_INVALID after open-loop stepping.
                a.index.store(a.dest_index.load(RLX), RLX);
                a.state.store(AxisState::Idle, RLX);
            } else if (a.hall_valid.load(RLX) && edge_overdue(in.pos, in.hall)) {
                enter_fault(c, "missed hall edge while moving");
            }
            break;

        case AxisState::Idle:
        case AxisState::Fault:
            break;
    }

    IsrWrite w;
    w.set_target = c.tgt_written;
    w.target = c.tgt;

    const AxisState now = a.state.load(RLX);
    const bool running = (now == AxisState::Moving || now == AxisState::Homing);
    if (!running) {
        w.velocity = 0;
    } else {
        const RampParams rp{a.v_max, p.accel, CONTROL_HZ};
        w.velocity = ramp_next_velocity(c.tgt - in.pos, in.velocity, rp);
    }
    return w;
}

AxisPublished axis_read_published(const AxisCtl& a) {
    AxisPublished out;
    for (;;) {
        const uint32_t s1 = a.seq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;  // tick in progress
        out.state = a.state.load(RLX);
        out.index = a.index.load(RLX);
        out.dest_index = a.dest_index.load(RLX);
        out.hall_valid = a.hall_valid.load(RLX);
        out.revs = a.revs.load(RLX);
        out.resync_minor = a.resync_minor.load(RLX);
        out.resync_major = a.resync_major.load(RLX);
        out.faults = a.faults.load(RLX);
        out.last_hall_err = a.last_hall_err.load(RLX);
        out.hall_to_hall = a.hall_to_hall.load(RLX);
        out.cal_offset = a.cal_offset.load(RLX);
        std::atomic_thread_fence(std::memory_order_acquire);
        const uint32_t s2 = a.seq.load(std::memory_order_relaxed);
        if (s1 == s2) return out;
    }
}

int32_t normalize_cal(int32_t usteps) {
    const int32_t rev = static_cast<int32_t>(USTEPS_PER_SPOOL_REV_NOMINAL);
    return ((usteps % rev) + rev) % rev;
}

const char* axis_state_name(AxisState s) {
    switch (s) {
        case AxisState::Unhomed: return "UNHOMED";
        case AxisState::Homing:  return "HOMING";
        case AxisState::Idle:    return "IDLE";
        case AxisState::Moving:  return "MOVING";
        case AxisState::Fault:   return "FAULT";
    }
    return "?";
}

}  // namespace swan
