// Phase 1.5: the simulated-axis suite (spec 15).  Drives the REAL control tick
// (axis_control.cpp) and the REAL step-ISR helpers against a modeled drum with
// a configurable Hall operate window, jitter, wrong gearing, and injected slip.
#include <cstring>

#include "check.h"
#include "sim_axis.h"

using namespace swan;
using namespace swan::sim;

namespace {

constexpr auto RLX = std::memory_order_relaxed;

// AxisCtl holds atomics, so SimAxis is neither copyable nor movable - set up in place.
void setup_axis(SimAxis& ax, int32_t cal, int64_t start_angle, int32_t jitter = 0) {
    ax.drum.start_angle_usteps = start_angle;
    ax.drum.jitter_usteps = jitter;
    ax.ctl.cal_offset.store(normalize_cal(cal), RLX);  // what motion::init does
}

// Latch delay (2-of-3 filter) + one control period of drift bounds the gap
// between the true mechanical edge and where the firmware believes it is.
constexpr int64_t ORACLE_TOL = 4;

bool offset_close(const SimAxis& ax, int64_t expect, int64_t tol = ORACLE_TOL) {
    const int64_t off = ax.shown_offset();
    return off >= expect && off <= expect + tol;
}

// ---------------------------------------------------------------------------
// Homing from any start angle, including inside the magnet window, ends at
// index 0 (with the drum physically cal usteps past the operate edge).
// ---------------------------------------------------------------------------
void test_homing_from_any_angle() {
    const int32_t CAL = 40;
    bool covered_inside_window = false;

    for (int k = 0; k < 100; ++k) {
        const int64_t angle = k * 82;  // 0..8118, dense across the revolution
        SimAxis ax;
        setup_axis(ax, CAL, angle);
        if (ax.drum.hall_at(0)) covered_inside_window = true;  // Release path

        ax.post_home();
        if (!ax.run_until_idle()) {
            CHECK(false);
            std::printf("  homing failed from angle %lld (state %s)\n",
                        static_cast<long long>(angle), axis_state_name(ax.state()));
            continue;
        }
        CHECK_EQ(ax.index(), RING_HOME_SLOT);
        CHECK_EQ(ax.homed_count, 1);
        CHECK_EQ(ax.fault_events, 0);
        if (!offset_close(ax, CAL)) {
            CHECK(false);
            std::printf("  angle %lld: offset %lld, expected ~%d\n",
                        static_cast<long long>(angle),
                        static_cast<long long>(ax.shown_offset()), CAL);
        }
    }
    CHECK(covered_inside_window);  // the sweep must exercise the Release phase

    // Explicitly: dead centre of the magnet window.
    SimAxis ax;
    setup_axis(ax, CAL, 30);
    CHECK(ax.drum.hall_at(0));
    ax.post_home();
    CHECK(ax.run_until_idle());
    CHECK_EQ(ax.index(), RING_HOME_SLOT);
    CHECK(offset_close(ax, CAL));
}

// ---------------------------------------------------------------------------
// go from every index to every index, including wraps across the edge with
// mid-move re-basing.  The oracle is the DRUM's angle, not the firmware's own
// bookkeeping, so accumulated rounding would show up here.
// ---------------------------------------------------------------------------
void test_go_full_matrix() {
    const int32_t CAL = 40;
    SimAxis ax;
    setup_axis(ax, CAL, 1234);
    ax.params.flaps_s_normal = 25;  // keep the 5000-move matrix fast
    ax.post_home();
    CHECK(ax.run_until_idle());

    int failures = 0;
    for (int from = 0; from < RING_SLOT_COUNT && failures < 5; ++from) {
        ax.post_go(from);
        CHECK(ax.run_until_idle());
        for (int to = 0; to < RING_SLOT_COUNT && failures < 5; ++to) {
            ax.post_go(to);
            if (!ax.run_until_idle()) {
                CHECK(false);
                ++failures;
                std::printf("  go %d -> %d never became Idle (state %s)\n", from, to,
                            axis_state_name(ax.state()));
                break;
            }
            if (ax.index() != to || !offset_close(ax, CAL + ring_target_usteps(to))) {
                CHECK(false);
                ++failures;
                std::printf("  go %d -> %d: index %d, offset %lld, expected ~%lld\n", from, to,
                            ax.index(), static_cast<long long>(ax.shown_offset()),
                            static_cast<long long>(CAL + ring_target_usteps(to)));
            }
            if (to != from) {
                ax.post_go(from);
                CHECK(ax.run_until_idle());
            }
        }
    }
    CHECK_EQ(failures, 0);
    CHECK_EQ(ax.fault_events, 0);
    // Every wrap re-based on a real edge; the error counters must stay silent.
    CHECK_EQ(ax.ctl.resync_major.load(RLX), 0u);
}

// ---------------------------------------------------------------------------
// Injected slip: 30 usteps -> minor, 100 -> major, 200 -> fault followed by a
// successful automatic re-home.  (Tolerances: silent <= 41, major <= 165.)
// ---------------------------------------------------------------------------
struct SlipResult {
    uint32_t minor, major, faults, rehomes, gave_up;
    int32_t err_at_slip;
    bool recovered_idle;
    int final_index;
};

SlipResult run_slip(int64_t slip) {
    const int32_t CAL = 40;
    SimAxis ax;
    setup_axis(ax, CAL, 500);
    ax.post_home();
    if (!ax.run_until_idle()) return {0, 0, 99, 0, 0, 0, false, -1};

    // Spin several revolutions open-loop; edge verification stays live.
    ax.post_step_open(6 * USTEPS_PER_SPOOL_REV_NOMINAL, 20);
    ax.run(5000);  // well inside the first revolution

    const uint32_t revs_before = ax.ctl.revs.load(RLX);
    const uint32_t minor_before = ax.ctl.resync_minor.load(RLX);
    ax.drum.slip_usteps += slip;  // the drum falls behind the motor, once

    // Run until the slipped edge has been classified (or a fault intervenes).
    int32_t err = 0;
    for (int64_t i = 0; i < 3'000'000; ++i) {
        ax.tick();
        if (ax.ctl.revs.load(RLX) != revs_before || ax.fault_events > 0) {
            err = ax.ctl.last_hall_err.load(RLX);
            break;
        }
    }

    // Let the rest of the spin (or the automatic re-home) finish.
    ax.run_until_idle(6'000'000);

    return {ax.ctl.resync_minor.load(RLX) - minor_before,
            ax.ctl.resync_major.load(RLX),
            ax.ctl.faults.load(RLX),
            ax.rehome_events,
            ax.gave_up_events,
            err,
            ax.state() == AxisState::Idle,
            ax.index()};
}

void test_slip_classification() {
    // 30 usteps: within HALL_TOL_SILENT -> minor, nothing else.
    SlipResult r30 = run_slip(30);
    CHECK(r30.minor >= 1);
    CHECK_EQ(r30.major, 0);
    CHECK_EQ(r30.faults, 0);
    CHECK(r30.err_at_slip >= 28 && r30.err_at_slip <= 32);
    CHECK(r30.recovered_idle);

    // 100 usteps: major resync, accepted, no fault.
    SlipResult r100 = run_slip(100);
    CHECK_EQ(r100.major, 1);
    CHECK_EQ(r100.faults, 0);
    CHECK(r100.err_at_slip >= 98 && r100.err_at_slip <= 102);
    CHECK(r100.recovered_idle);

    // 200 usteps: > one flap -> FAULT, then the automatic re-home succeeds.
    SlipResult r200 = run_slip(200);
    CHECK_EQ(r200.faults, 1);
    CHECK(r200.rehomes >= 1);
    CHECK_EQ(r200.gave_up, 0);
    CHECK(r200.recovered_idle);
    CHECK_EQ(r200.final_index, RING_HOME_SLOT);  // re-home landed on index 0
}

// ---------------------------------------------------------------------------
// Calibration offsets normalise into [0, one revolution).
// ---------------------------------------------------------------------------
void test_cal_normalisation() {
    CHECK_EQ(normalize_cal(0), 0);
    CHECK_EQ(normalize_cal(100), 100);
    CHECK_EQ(normalize_cal(-100), 8142);            // negative
    CHECK_EQ(normalize_cal(8242), 0);               // exactly one rev
    CHECK_EQ(normalize_cal(8242 * 2 + 5), 5);       // > 1 rev
    CHECK_EQ(normalize_cal(-8242 - 3), 8239);       // < -1 rev
    CHECK_EQ(normalize_cal(8241), 8241);

    // Behavioural: homing with a normalised negative offset lands 8142 usteps
    // past the edge - most of a revolution forward, never a reverse move.
    SimAxis ax;
    setup_axis(ax, -100, 2000);
    CHECK_EQ(ax.ctl.cal_offset.load(RLX), 8142);
    ax.post_home();
    CHECK(ax.run_until_idle());
    CHECK_EQ(ax.index(), RING_HOME_SLOT);
    CHECK(offset_close(ax, 8142));
}

// ---------------------------------------------------------------------------
// A 68T/26T drum (8369.23 usteps/rev) against firmware built for 85/33: a
// major resync every revolution, never a fault.  This is the exact signature
// bench step 4 discriminates on.
// ---------------------------------------------------------------------------
void test_wrong_gearing_signature() {
    SimAxis ax;
    setup_axis(ax, 40, 700);
    ax.drum.rev_num = 108800;  // 217600/26 reduced
    ax.drum.rev_den = 13;

    ax.post_home();
    CHECK(ax.run_until_idle());
    CHECK_EQ(ax.index(), RING_HOME_SLOT);

    // 20 revolutions open loop.
    ax.post_step_open(20 * 8370, 25);
    CHECK(ax.run_until_idle(20'000'000));

    const uint32_t revs = ax.ctl.revs.load(RLX);
    const uint32_t major = ax.ctl.resync_major.load(RLX);
    CHECK(revs >= 19 && revs <= 21);
    CHECK(major >= revs - 1 && major <= revs);  // every classified rev is major
    CHECK_EQ(ax.ctl.faults.load(RLX), 0);
    CHECK_EQ(ax.gave_up_events, 0);
    // h2h reads the real drum, not the assumption:
    const int32_t h2h = ax.ctl.hall_to_hall.load(RLX);
    CHECK(h2h >= 8368 && h2h <= 8371);
}

// ---------------------------------------------------------------------------
// Hall jitter: edges wobble +-3 usteps; everything still homes and lands
// within the widened tolerance, and every rev classifies as minor.
// ---------------------------------------------------------------------------
void test_edge_jitter() {
    const int32_t CAL = 40;
    SimAxis ax;
    setup_axis(ax, CAL, 3000, 3);
    ax.post_home();
    CHECK(ax.run_until_idle());
    CHECK_EQ(ax.index(), RING_HOME_SLOT);
    CHECK(offset_close(ax, CAL - 3, ORACLE_TOL + 6));

    ax.post_step_open(5 * USTEPS_PER_SPOOL_REV_NOMINAL, 20);
    CHECK(ax.run_until_idle(5'000'000));
    CHECK_EQ(ax.ctl.resync_major.load(RLX), 0u);
    CHECK_EQ(ax.ctl.faults.load(RLX), 0u);
}

// ---------------------------------------------------------------------------
// No magnet at all: homing exhausts its retries and latches FAULT.
// ---------------------------------------------------------------------------
void test_no_magnet_faults() {
    SimAxis ax;
    setup_axis(ax, 0, 0);
    ax.drum.window_usteps = 0;  // sensor never asserts
    ax.post_home();
    CHECK(ax.run_until(AxisState::Fault, 10'000'000));
    CHECK_EQ(ax.gave_up_events, 1);
    CHECK_EQ(ax.ctl.faults.load(RLX), 1u + REHOME_RETRIES);
}

// ---------------------------------------------------------------------------
// Mailbox semantics: back-to-back commands are never lost and never reordered.
// The slot is replace-on-write: two posts inside one control period apply
// exactly the NEWER one (spec 6 - a new frame replaces targets); a stale
// command is never applied after a newer one, and nothing is half-applied.
//
// Scope, stated honestly: the single-slot replace itself is one assignment,
// replicated here by the harness; on target it happens under the shell's
// spinlock (motion.cpp), which cannot be host-compiled.  What IS the real
// firmware code here is everything downstream of the drain: drain-then-apply
// ordering, FSM validation, and that a drained command is applied whole.
// ---------------------------------------------------------------------------
void test_mailbox_ordering() {
    SimAxis ax;
    setup_axis(ax, 0, 400);
    ax.post_home();
    CHECK(ax.run_until_idle());
    ax.drained.clear();

    // (a) Commands in separate control periods: all applied, in order.
    const int seq[] = {5, 9, 3, 49, 0};
    for (int ix : seq) {
        ax.post_go(ix);
        CHECK(ax.run_until_idle());
        CHECK_EQ(ax.index(), ix);
    }
    CHECK_EQ(ax.drained.size(), 5u);
    for (size_t i = 0; i < 5; ++i) {
        CHECK(ax.drained[i].kind == ReqKind::Go);
        CHECK_EQ(ax.drained[i].dest, seq[i]);
        CHECK(!ax.drained[i].rejected);
    }

    // (b) Two posts inside one control period: exactly one drain, the newer
    // command; the older is superseded whole - dest 3 must never appear.
    ax.drained.clear();
    ax.post_go(3);
    ax.post_go(7);
    CHECK(ax.run_until_idle());
    CHECK_EQ(ax.drained.size(), 1u);
    CHECK_EQ(ax.drained[0].dest, 7);
    CHECK_EQ(ax.index(), 7);

    // (c) Retarget mid-move: both commands apply, in order, no rejection.
    ax.drained.clear();
    ax.post_go(30);
    ax.run(2000);  // mid-move
    CHECK(ax.state() == AxisState::Moving);
    ax.post_go(45);
    CHECK(ax.run_until_idle());
    CHECK_EQ(ax.drained.size(), 2u);
    CHECK_EQ(ax.drained[0].dest, 30);
    CHECK_EQ(ax.drained[1].dest, 45);
    CHECK(!ax.drained[0].rejected && !ax.drained[1].rejected);
    CHECK_EQ(ax.index(), 45);

    // (d) Home posted after go in the same period supersedes it.
    ax.drained.clear();
    ax.post_go(10);
    ax.post_home();
    CHECK(ax.run_until_idle());
    CHECK_EQ(ax.drained.size(), 1u);
    CHECK(ax.drained[0].kind == ReqKind::Home);
    CHECK_EQ(ax.index(), RING_HOME_SLOT);

    // (e) Stop freezes the axis; no further steps are issued.
    ax.post_go(30);
    ax.run(2000);
    Request st;
    st.kind = ReqKind::Stop;
    ax.post(st);
    ax.run(SimAxis::CONTROL_DIV + 1);  // let the drain happen
    CHECK(ax.state() == AxisState::Idle);
    const int64_t frozen = ax.isr.pos_abs;
    ax.run(5000);
    CHECK_EQ(ax.isr.pos_abs, frozen);
}

// ---------------------------------------------------------------------------
// Regression: go() must terminate for EVERY calibration offset.  Before the
// edge_anchor_offset reduction, any cal with cal + T(dest) > one revolution
// made the mid-move rebase push the target past the next edge, which rebased
// it again - the target receded one revolution per edge, forever.  cal 8142 is
// the suite's own normalize_cal(-100); cal > 164 used to make index 49 (the
// boot wifi glyph) unreachable.  Found by adversarial review.
// ---------------------------------------------------------------------------
void test_go_terminates_for_any_cal() {
    const int32_t cals[] = {200, 4000, 8142, 8241};
    for (const int32_t cal : cals) {
        SimAxis ax;
        setup_axis(ax, cal, 900);
        ax.params.flaps_s_normal = 25;
        ax.post_home();
        CHECK(ax.run_until_idle());

        const int dests[] = {1, 26, 30, 49, 0, 25};
        for (const int to : dests) {
            const int from = ax.index();
            ax.post_go(to);
            if (!ax.run_until_idle(3'000'000)) {
                CHECK(false);
                std::printf("  cal %ld: go %d -> %d never became Idle (state %s, pos %lld)\n",
                            static_cast<long>(cal), from, to, axis_state_name(ax.state()),
                            static_cast<long long>(ax.isr.pos_abs));
                return;
            }
            CHECK_EQ(ax.index(), to);
            // The oracle: the drum must physically show `to`, i.e. sit at the
            // reduced anchor offset past the most recent edge.
            int64_t expect = cal + ring_target_usteps(to);
            while (expect >= USTEPS_PER_SPOOL_REV_NOMINAL) expect -= USTEPS_PER_SPOOL_REV_NOMINAL;
            if (!offset_close(ax, expect - 1, ORACLE_TOL + 2)) {
                CHECK(false);
                std::printf("  cal %ld: go -> %d landed at offset %lld, expected ~%lld\n",
                            static_cast<long>(cal), to,
                            static_cast<long long>(ax.shown_offset()),
                            static_cast<long long>(expect));
            }
        }
        CHECK_EQ(ax.ctl.faults.load(RLX), 0u);
    }
}

// ---------------------------------------------------------------------------
// Closed-loop disturbance: slip injected DURING a go() move.  The mid-move
// edge rebase must absorb the slip so the landing is correct relative to the
// drum, with a minor resync and nothing else.  (The open-loop slip tests
// above never exercise the rebase, since StepOpen has no destination.)
// ---------------------------------------------------------------------------
void test_slip_during_go_is_absorbed() {
    const int32_t CAL = 40;
    SimAxis ax;
    setup_axis(ax, CAL, 600);
    ax.post_home();
    CHECK(ax.run_until_idle());

    // A wrap move (10 -> 5 costs 45 flips) guarantees an edge crossing.
    ax.post_go(10);
    CHECK(ax.run_until_idle());
    ax.post_go(5);
    ax.run(20'000);  // mid-move, before the edge
    CHECK(ax.state() == AxisState::Moving);
    ax.drum.slip_usteps += 30;

    CHECK(ax.run_until_idle());
    CHECK_EQ(ax.index(), 5);
    CHECK_EQ(ax.ctl.resync_major.load(RLX), 0u);
    CHECK_EQ(ax.ctl.faults.load(RLX), 0u);
    // Judged against the DRUM: the rebase re-anchored on the slipped edge, so
    // the landing must be correct relative to where the drum actually is.
    CHECK(offset_close(ax, CAL + ring_target_usteps(5)));
}

// ---------------------------------------------------------------------------
// The 200-ustep POSITIVE slip faults via the missed-edge timeout (the edge
// arrives 200 late, past the overdue threshold).  A NEGATIVE slip makes the
// edge arrive early, so the fault must come from the EdgeVerdict::Fault
// classification branch instead - this is the only test that exercises it.
// ---------------------------------------------------------------------------
void test_early_edge_classifies_as_fault() {
    SimAxis ax;
    setup_axis(ax, 40, 500);
    ax.post_home();
    CHECK(ax.run_until_idle());

    ax.post_step_open(6 * USTEPS_PER_SPOOL_REV_NOMINAL, 20);
    ax.run(5000);
    const uint32_t revs_before = ax.ctl.revs.load(RLX);
    ax.drum.slip_usteps -= 200;  // drum jumps AHEAD: next edge arrives 200 early

    for (int64_t i = 0; i < 3'000'000; ++i) {
        ax.tick();
        if (ax.ctl.revs.load(RLX) != revs_before || ax.fault_events > 0) break;
    }
    CHECK_EQ(ax.fault_events, 1);
    const int32_t err = ax.ctl.last_hall_err.load(RLX);
    CHECK(err <= -198 && err >= -202);  // classified, not timed out
    CHECK(ax.run_until_idle(6'000'000));  // automatic re-home succeeds
    CHECK_EQ(ax.index(), RING_HOME_SLOT);
    CHECK_EQ(ax.gave_up_events, 0);
}

// ---------------------------------------------------------------------------
// Stop drained during a homing pass aborts it honestly: state Unhomed, no
// spurious fault, no false "homed", frozen position; a later home works.
// ---------------------------------------------------------------------------
void test_stop_during_homing_aborts_cleanly() {
    SimAxis ax;
    setup_axis(ax, 4000, 3000);
    ax.post_home();
    ax.run(20'000);  // mid-seek
    CHECK(ax.state() == AxisState::Homing);

    Request st;
    st.kind = ReqKind::Stop;
    ax.post(st);
    ax.run(2 * SimAxis::CONTROL_DIV);
    CHECK(ax.state() == AxisState::Unhomed);
    CHECK_EQ(ax.ctl.faults.load(RLX), 0u);   // no spurious "no hall edge" fault
    CHECK_EQ(ax.homed_count, 0u);            // no false completion
    CHECK_EQ(ax.index(), RING_INVALID);

    const int64_t frozen = ax.isr.pos_abs;
    ax.run(5000);
    CHECK_EQ(ax.isr.pos_abs, frozen);

    ax.post_home();
    CHECK(ax.run_until_idle());
    CHECK_EQ(ax.index(), RING_HOME_SLOT);
    CHECK_EQ(ax.homed_count, 1u);
}

}  // namespace

void run_tests() {
    test_cal_normalisation();
    test_homing_from_any_angle();
    test_slip_classification();
    test_early_edge_classifies_as_fault();
    test_slip_during_go_is_absorbed();
    test_wrong_gearing_signature();
    test_edge_jitter();
    test_no_magnet_faults();
    test_stop_during_homing_aborts_cleanly();
    test_mailbox_ordering();
    test_go_terminates_for_any_cal();
    test_go_full_matrix();
}
