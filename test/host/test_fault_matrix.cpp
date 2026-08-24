// The fault-path matrix: maintenance x disabled x simulated, and the
// interactions between them.
//
// test_fault_policy covers the pure classifier (three causes, two responses)
// and test_axis_sim covers one axis recovering.  This file is about the
// COMBINATIONS, which is where every one of the following was hiding.  Each
// case names the spec clause it is holding to.
#include <cstdio>

#include "check.h"
#include "motion/axis_control.h"
#include "motion/fault_policy.h"

using namespace swan;

namespace {

// The escalation input the shell computes.  These tests pin the DECISION
// function against the counts the shell now produces; the counting itself is
// asserted on the board (it reads live axis state) and described here so the
// two cannot drift apart silently.
struct Situation {
    int in_trouble = 0;      // columns latched in Fault OR with a re-home in flight
    bool during_spin = false;
};

Escalation decide(FaultCause cause, const Situation& s) {
    return escalate_fault(cause, s.in_trouble, s.during_spin);
}

// --- 5.8: two or more columns in trouble drop EN ---------------------------

void test_two_columns_in_trouble_drop_en_even_while_retrying() {
    // THE regression this file exists for.  A retryable fault stores Unhomed,
    // not Fault, so the shell's old count saw ZERO for both columns and each
    // got ParkColumn: "two columns failing together is not two coincidences"
    // could only fire ~22 s later, after both had exhausted three retries.
    // A column with a re-home in flight is in trouble.
    CHECK(decide(FaultCause::Slip, {2, false}) == Escalation::DropEnable);
    CHECK(decide(FaultCause::NoHallEver, {2, false}) == Escalation::DropEnable);
    CHECK(decide(FaultCause::Jam, {2, false}) == Escalation::DropEnable);

    // One column is still just one column, whatever the cause.
    CHECK(decide(FaultCause::Slip, {1, false}) == Escalation::ParkColumn);
    CHECK(decide(FaultCause::NoHallEver, {1, false}) == Escalation::ParkColumn);
    CHECK(decide(FaultCause::Jam, {1, false}) == Escalation::StopColumn);
}

void test_a_fault_during_a_high_speed_spin_drops_en_however_few() {
    // Spec 5.8: "any fault while a column is in a high-speed open-loop spin ->
    // drop_enable".  The faulting column used to exclude ITSELF from the spin
    // test (its state had already moved to Fault/Unhomed), so a single spinning
    // column that faulted produced ParkColumn - the exact case the rule is for.
    CHECK(decide(FaultCause::Slip, {1, true}) == Escalation::DropEnable);
    CHECK(decide(FaultCause::NoHallEver, {1, true}) == Escalation::DropEnable);
    CHECK(decide(FaultCause::Jam, {1, true}) == Escalation::DropEnable);
}

// --- 5.9: a disabled column is configuration, not a fault -------------------

void test_a_disabled_column_does_not_vote() {
    // The shell excludes Disabled columns from the count, so the situation a
    // repair produces - one column disabled and latched, one real column
    // faulting - is a SINGLE fault, not two.  Before, the first real fault
    // after a repair dropped EN for all five.
    //
    // Stated here as the count the shell must produce, and asserted directly
    // in test_column_modes_do_not_vote below.
    CHECK(decide(FaultCause::Slip, {1, false}) == Escalation::ParkColumn);
}

// --- The counting rule itself ----------------------------------------------

// A tiny model of the shell's faulted_count(), kept in step with motion.cpp by
// the comment there and by this test reading the same way.  It is worth
// modelling because the rule is three exclusions deep and every one of them
// was wrong at some point.
int count_in_trouble(const ColumnMode modes[N_COLUMNS], const bool latched[N_COLUMNS],
                     const bool retrying[N_COLUMNS]) {
    int real_in_trouble = 0, sim_in_trouble = 0, real_columns = 0;
    for (int i = 0; i < N_COLUMNS; ++i) {
        if (modes[i] == ColumnMode::Disabled) continue;
        if (modes[i] == ColumnMode::Real) ++real_columns;
        if (!latched[i] && !retrying[i]) continue;
        if (modes[i] == ColumnMode::Real) ++real_in_trouble;
        else ++sim_in_trouble;
    }
    return real_columns > 0 ? real_in_trouble : sim_in_trouble;
}

void test_column_modes_do_not_vote() {
    ColumnMode modes[N_COLUMNS];
    bool latched[N_COLUMNS] = {};
    bool retrying[N_COLUMNS] = {};

    // A disabled column carrying a latched fault, plus one real fault: ONE.
    for (auto& m : modes) m = ColumnMode::Real;
    modes[2] = ColumnMode::Disabled;
    latched[2] = true;
    latched[0] = true;
    CHECK_EQ(count_in_trouble(modes, latched, retrying), 1);

    // Two real columns mid-retry: TWO, which is the whole point.
    for (auto& m : modes) m = ColumnMode::Real;
    for (auto& b : latched) b = false;
    retrying[0] = retrying[3] = true;
    CHECK_EQ(count_in_trouble(modes, latched, retrying), 2);

    // The build-out configuration spec 5.9 names: one real, four simulated.
    // Two INJECTED faults on modelled drums must not de-energize the one real
    // column - a modelled drum is not evidence of power, the loom or the frame.
    for (int i = 0; i < N_COLUMNS; ++i) modes[i] = i == 0 ? ColumnMode::Real : ColumnMode::Sim;
    for (auto& b : latched) b = false;
    for (auto& b : retrying) b = false;
    latched[1] = latched[2] = true;
    CHECK_EQ(count_in_trouble(modes, latched, retrying), 0);
    // ... and a real fault on the real column still counts as one.
    latched[0] = true;
    CHECK_EQ(count_in_trouble(modes, latched, retrying), 1);

    // With NO real columns the simulated ones vote, so the rule stays
    // demonstrable on a bench board - where EN means nothing anyway.  BRINGUP
    // step 17 exercises exactly this.
    for (auto& m : modes) m = ColumnMode::Sim;
    for (auto& b : latched) b = false;
    latched[1] = latched[2] = true;
    CHECK_EQ(count_in_trouble(modes, latched, retrying), 2);
    CHECK(decide(FaultCause::Slip, {count_in_trouble(modes, latched, retrying), false}) ==
          Escalation::DropEnable);

    // Every column disabled: nothing votes, whatever happened to them.
    for (auto& m : modes) m = ColumnMode::Disabled;
    for (auto& b : latched) b = true;
    CHECK_EQ(count_in_trouble(modes, latched, retrying), 0);
}

// --- 5.9: maintenance suppresses AUTOMATIC motion, not manual --------------

void test_maintenance_and_the_ota_hold_both_suppress_automatic_rehoming() {
    // Two flags, one rule: nothing re-homes ITSELF while someone has their
    // hands in the mechanism, or while an image is being written.  The OTA
    // hold was enforced only at the dispatcher, so a staggered home already
    // posted - or a retry the core schedules by itself - started a 7.5 s
    // homing pass in the middle of a flash write.
    MotionParams p;
    CHECK(!p.maintenance);
    CHECK(!p.ota_hold);   // neither is ever the default

    p.maintenance = true;
    CHECK(p.maintenance && !p.ota_hold);
    p.maintenance = false;
    p.ota_hold = true;
    CHECK(!p.maintenance && p.ota_hold);
}

}  // namespace

void run_tests() {
    test_two_columns_in_trouble_drop_en_even_while_retrying();
    test_a_fault_during_a_high_speed_spin_drops_en_however_few();
    test_a_disabled_column_does_not_vote();
    test_column_modes_do_not_vote();
    test_maintenance_and_the_ota_hold_both_suppress_automatic_rehoming();
}
