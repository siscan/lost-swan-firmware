// Frame scheduler (spec 6): duration model vs the real controller, land-on-
// tick lead computation, frame replacement, and resume-after-rehome.
#include <cstring>

#include "check.h"
#include "fake_port.h"
#include "sim_axis.h"

using namespace swan;
using namespace swan::testfakes;

namespace {

// --------------------------------------------------------------------------
// The duration model is pinned against the REAL controller: simulate the
// actual ramp+DDA (SimAxis) and compare, so a controller change that alters
// timing breaks this test instead of silently un-syncing land-on-tick.
// --------------------------------------------------------------------------
void test_duration_vs_simulation() {
    const int32_t accel = 82000;
    for (const int32_t flaps : {15, 25}) {
        for (const int flips : {1, 2, 5, 24, 41, 45, 49}) {
            sim::SimAxis ax;
            ax.ctl.cal_offset.store(40, std::memory_order_relaxed);
            ax.params.flaps_s_normal = flaps;
            ax.post_home();
            if (!ax.run_until_idle()) {
                CHECK(false);
                return;
            }
            const int64_t t0 = ax.ticks;
            ax.post_go(flips % RING_SLOT_COUNT);  // flips forward from index 0
            CHECK(ax.run_until_idle());
            const int64_t sim_ms = (ax.ticks - t0) / (TICK_HZ / 1000);

            const int64_t model_ms = move_duration_ms(flips, flaps, accel);
            const int64_t err = model_ms - sim_ms;
            const int64_t tol = 30 + sim_ms / 20;  // 30 ms + 5%
            if (err > tol || err < -tol) {
                CHECK(false);
                std::printf("  flips=%d flaps=%ld: model %lld ms vs sim %lld ms\n", flips,
                            static_cast<long>(flaps), static_cast<long long>(model_ms),
                            static_cast<long long>(sim_ms));
            }
        }
    }
    CHECK_EQ(move_duration_ms(0, 15, 82000), 0);
}

// --------------------------------------------------------------------------
// Land-on-tick: the frame starts early by the LONGEST column's duration and
// the whirl ends on the boundary (spec 7.3).
// --------------------------------------------------------------------------
void test_land_on_tick() {
    FakePort port;
    FrameScheduler sched(port, {15, 82000});

    // Columns at various indices; worst move below is 45 flips (col 2: 5->0).
    port.cols[0].index = 0;
    port.cols[1].index = 40;  // -> 2: 12 flips
    port.cols[2].index = 5;   // -> 0: 45 flips (longest)
    port.cols[3].index = 10;  // -> 15: 5 flips
    port.cols[4].index = 49;  // -> 49: 0 flips

    Frame f;
    f.idx = {0, 2, 0, 15, 49};
    const int64_t lead = sched.lead_ms(f);
    CHECK_EQ(lead, move_duration_ms(45, 15, 82000));

    const int64_t land = 100000;
    sched.show(f, 0, land);
    CHECK(sched.pending());
    CHECK_EQ(port.gos.size(), 0u);  // nothing starts early

    // Drive time in 10 ms steps; every column must start together at land-lead.
    for (int64_t t = 0; t <= land && sched.pending(); t += 10) {
        port.now_ms = t;
        sched.tick(t);
    }
    CHECK(!sched.pending());
    // Cols 0 (0->0) and 4 (49->49) already show their targets - no go issued.
    CHECK_EQ(port.gos.size(), 3u);
    const int64_t start = port.gos[0].at_ms;
    const int64_t expect = land - lead;
    CHECK(start >= expect && start <= expect + 10);
    for (const auto& g : port.gos) CHECK_EQ(g.at_ms, start);  // simultaneous

    // A lead that no longer fits starts immediately instead of never.
    port.gos.clear();
    port.cols[2].index = 5;
    sched.show(f, land + 5000, land + 5100);  // 100 ms to a 2.3 s move
    CHECK(!sched.pending());
    CHECK(port.gos.size() >= 1u);
}

// --------------------------------------------------------------------------
// A new frame replaces a pending one (spec 6).
// --------------------------------------------------------------------------
void test_replacement() {
    FakePort port;
    FrameScheduler sched(port, {15, 82000});

    Frame a;
    a.idx = {1, 1, 1, 1, 1};
    Frame b;
    b.idx = {2, 2, 2, 2, 2};

    sched.show(a, 0, 60000);  // pending
    CHECK(sched.pending());
    sched.show(b, 100, 0);  // immediate show replaces the scheduled one
    CHECK(!sched.pending());
    CHECK(sched.desired() == b);
    for (int64_t t = 0; t < 70000; t += 1000) sched.tick(t);
    for (const auto& g : port.gos) CHECK_EQ(g.index, 2);  // frame a never issued
}

// --------------------------------------------------------------------------
// Resume after re-home (spec 5.4 / decision log): a column that faulted and
// auto-re-homed sits Idle at index 0; the scheduler must bring it back.
// --------------------------------------------------------------------------
void test_resume_after_rehome() {
    FakePort port;
    FrameScheduler sched(port, {15, 82000});

    Frame f;
    f.idx = {7, 8, 9, 10, 11};
    sched.show(f, 0);
    CHECK(sched.settled());
    port.gos.clear();

    // Column 2 faults and re-homes: Homing first - the scheduler must NOT
    // interfere mid-home.
    port.cols[2].state = AxisState::Homing;
    port.cols[2].index = RING_INVALID;
    for (int64_t t = 100; t < 500; t += 50) sched.tick(t);
    CHECK_EQ(port.gos.size(), 0u);

    // Re-home lands: Idle at index 0 - not the frame's index 9.
    port.cols[2].state = AxisState::Idle;
    port.cols[2].index = 0;
    sched.tick(600);
    CHECK_EQ(port.gos.size(), 1u);
    CHECK_EQ(port.gos[0].col, 2);
    CHECK_EQ(port.gos[0].index, 9);
    CHECK(sched.settled());

    // A rejected go is retried until motion accepts it.
    port.gos.clear();
    port.accept = false;
    port.cols[4].state = AxisState::Idle;
    port.cols[4].index = 0;
    sched.tick(700);
    CHECK_EQ(port.gos.size(), 1u);
    sched.tick(750);
    CHECK_EQ(port.gos.size(), 2u);  // retried
    port.accept = true;
    sched.tick(800);
    CHECK_EQ(port.gos.size(), 3u);
    CHECK(sched.settled());
}

// --------------------------------------------------------------------------
// Post-spin landing: after open-loop spinning the display index is unknown;
// convergence issues the goes and lead_ms budgets a full wrap.
// --------------------------------------------------------------------------
void test_post_spin_convergence() {
    FakePort port;
    FrameScheduler sched(port, {15, 82000});

    Frame f;
    f.idx = {13, 14, 15, 16, 17};
    sched.show(f, 0);
    port.gos.clear();

    port.instant = false;  // a spin takes time; columns stay Moving
    sched.spin_all(25, 6, 0);
    CHECK_EQ(port.spins.size(), static_cast<size_t>(N_COLUMNS));
    CHECK_EQ(port.spins[0].flaps_s, 25);

    // While spinning (Moving), no interference - and the published index
    // stays at the last settled value, like the real axis.
    CHECK_EQ(port.cols[0].index, 13);
    sched.tick(1000);
    CHECK_EQ(port.gos.size(), 0u);
    port.instant = true;

    // Unknown index costs a full wrap in the lead computation.
    CHECK_EQ(sched.lead_ms(f), move_duration_ms(RING_SLOT_COUNT - 1, 15, 82000));

    // Spin ends: columns Idle with unknown index -> convergence re-issues all.
    for (auto& c : port.cols) {
        c.state = AxisState::Idle;
        c.index = RING_INVALID;
    }
    sched.tick(7000);
    CHECK_EQ(port.gos.size(), static_cast<size_t>(N_COLUMNS));
    CHECK(sched.settled());
}

// --------------------------------------------------------------------------
// Non-instant moves: columns sit in Moving for a while, as on hardware.
// Convergence must not spam a Moving column, replacement must re-issue only
// where the destination differs, and lead_ms must measure from a Moving
// column's destination.
// --------------------------------------------------------------------------
void test_non_instant_moves() {
    FakePort port;
    port.instant = false;
    FrameScheduler sched(port, {15, 82000});

    Frame f;
    f.idx = {7, 8, 9, 10, 11};
    sched.show(f, 0);
    CHECK_EQ(port.gos.size(), 5u);  // all columns commanded (all start at 0)
    for (const auto& c : port.cols) CHECK(c.state == AxisState::Moving);

    // No convergence spam while every column is Moving to the right place.
    for (int64_t t = 10; t < 500; t += 10) sched.tick(t);
    CHECK_EQ(port.gos.size(), 5u);
    CHECK(!sched.settled());

    // lead_ms measures a Moving column from its DESTINATION: col 0 is going
    // to 7, so a frame wanting 9 costs 2 flips from there.
    Frame g;
    g.idx = {9, 8, 9, 10, 11};
    CHECK_EQ(sched.lead_ms(g), move_duration_ms(2, 15, 82000));

    // Replacement mid-move: only the column whose destination differs is
    // re-issued; the rest are already Moving to the right index.
    sched.show(g, 600);
    CHECK_EQ(port.gos.size(), 6u);
    CHECK_EQ(port.gos.back().col, 0);
    CHECK_EQ(port.gos.back().index, 9);

    // Moves complete; the scheduler settles without further commands.
    port.finish_moves();
    sched.tick(700);
    CHECK_EQ(port.gos.size(), 6u);
    CHECK(sched.settled());
}

// The phase 3 review's frame finding: within one modes tick the axis still
// reports its pre-command state, because the mailbox is drained by the 1 kHz
// control tick.  A scheduler that re-reads col() in that window reasons from a
// snapshot its own command has already invalidated - and skips a column whose
// new target happens to equal the stale reported index, leaving the
// SUPERSEDED command to execute instead.
void test_mailbox_lag_within_a_tick() {
    FakePort port;
    port.mailbox_lag = true;
    FrameScheduler sched(port, {15, 82000});

    // Column 0 sits Idle at 0 (freshly re-homed); the desired frame still
    // wants 44 there, so convergence posts go(0,44).
    Frame want{{44, 1, 2, 3, 4}};
    sched.show(want, 0);
    port.drain_mailbox();
    CHECK_EQ(port.cols[0].index, 44);

    // Simulate the re-home: the axis lands back on 0 with the frame unchanged.
    port.cols[0].index = 0;
    port.cols[0].dest = 0;
    port.cols[0].state = AxisState::Idle;

    // Now, in ONE tick: convergence re-posts go(0,44), and immediately
    // afterwards a new frame arrives that wants 0 on that column - exactly
    // what cmd_preset("blank") does after enter_mode has already ticked.
    port.gos.clear();
    sched.tick(100);                       // posts go(0,44) into the mailbox
    CHECK_EQ(port.gos_for(0), 1);
    sched.show(Frame{{0, 1, 2, 3, 4}}, 100);

    // The axis STILL reports {Idle, 0} - nothing has drained.  Without the
    // posted-this-tick memory the scheduler concludes column 0 is already
    // showing 0 and posts nothing, so the stale go(0,44) wins and the column
    // walks 44 slots away while the rest of the frame goes blank.
    port.drain_mailbox();
    CHECK_EQ(port.cols[0].index, 0);
}

// A spin must not be overwritten by the convergence pass in the same tick.
void test_spin_survives_convergence() {
    FakePort port;
    port.mailbox_lag = true;
    FrameScheduler sched(port, {15, 82000});

    sched.show(Frame{{10, 11, 12, 13, 14}}, 0);
    port.drain_mailbox();

    // Put the columns somewhere else, then spin and tick in one go - the
    // zero_hold_s = 0 path, where the 000:00 frame, spin_all and convergence
    // all land in a single modes tick.
    for (auto& c : port.cols) {
        c.index = 0;
        c.state = AxisState::Idle;
    }
    port.spins.clear();
    port.gos.clear();
    sched.spin_all(25, 6, 50);
    sched.tick(50);
    port.drain_mailbox();

    CHECK_EQ(port.spins.size(), static_cast<size_t>(N_COLUMNS));
    CHECK_EQ(port.gos.size(), 0u);   // nothing clobbered the spin
}


// A disabled column is a HOLE, not a column that quietly moves anyway
// (spec 5.9).  The frame layer is the only place that knows; renderers keep
// producing five indices and never learn about it.
void test_excluded_column_is_never_commanded() {
    FakePort port;
    FrameScheduler sched(port, {15, 82000});
    for (auto& c : port.cols) {
        c.index = 0;
        c.state = AxisState::Idle;
    }
    sched.set_excluded(0b00101);  // columns 0 and 2 disabled
    CHECK(sched.is_excluded(0));
    CHECK(!sched.is_excluded(1));
    CHECK(sched.is_excluded(2));

    sched.show({10, 11, 12, 13, 14}, 0);
    sched.tick(0);
    port.drain_mailbox();
    for (const auto& g : port.gos) {
        CHECK(g.col != 0 && g.col != 2);
    }
    CHECK_EQ(port.gos.size(), 3u);

    // ... including the alarm spin, which is the one place five columns are
    // commanded at once without a frame behind them.
    port.gos.clear();
    port.spins.clear();
    sched.spin_all(25, 6, 100);
    sched.tick(100);
    port.drain_mailbox();
    CHECK_EQ(port.spins.size(), 3u);
    for (const auto& sp : port.spins) {
        CHECK(sp.col != 0 && sp.col != 2);
    }

    // And the frame is "settled" once the columns that CAN move have arrived:
    // waiting on a column that will never report is a hang, not a safeguard.
    for (int i = 0; i < N_COLUMNS; ++i) {
        if (i == 0 || i == 2) continue;
        port.cols[static_cast<size_t>(i)].index = 10 + i;
        port.cols[static_cast<size_t>(i)].state = AxisState::Idle;
    }
    port.cols[0].state = AxisState::Fault;
    port.cols[2].state = AxisState::Unhomed;
    sched.tick(200);
    CHECK(sched.settled());
}

}  // namespace

void run_tests() {
    test_duration_vs_simulation();
    test_land_on_tick();
    test_replacement();
    test_resume_after_rehome();
    test_post_spin_convergence();
    test_non_instant_moves();
    test_mailbox_lag_within_a_tick();
    test_spin_survives_convergence();
    test_excluded_column_is_never_commanded();
}
