// Fault classification, escalation and the per-column state machine
// (spec 5.4, 5.8, 5.9).  Pure policy, so it is all host-testable and none of
// it needs a drum.
#include <string>

#include "check.h"
#include "motion/column_mode.h"
#include "motion/fault_policy.h"

using namespace swan;

namespace {

// --------------------------------------------------------------------------
// Which causes may be retried
// --------------------------------------------------------------------------
void test_retry_rule() {
    // A cold home that never saw an edge: the drum is almost certainly free,
    // so another pass costs nothing but 7.5 s.
    CHECK(fault_retry_allowed(FaultCause::NoHallEver));
    // The edge ARRIVED, in the wrong place.  The drum turned; it lost
    // registration.  Re-homing is the recovery, and Phase 1.5's slip suite
    // depends on it happening.
    CHECK(fault_retry_allowed(FaultCause::Slip));
    // The edge never came at all while the motor kept stepping.  The drum has
    // stopped.  Retrying drives a stepper into the obstruction three more
    // times, 7.5 s each, against printed gear teeth.
    CHECK(!fault_retry_allowed(FaultCause::Jam));
    // Not a fault; nothing to retry.
    CHECK(!fault_retry_allowed(FaultCause::None));
}

// --------------------------------------------------------------------------
// Escalation
// --------------------------------------------------------------------------
void test_escalation() {
    // One column, sensor signature: park it, the rest of the display carries
    // on.  A wall clock missing one digit still tells you more than a dark one.
    CHECK(escalate_fault(FaultCause::NoHallEver, 1, false) == Escalation::ParkColumn);
    CHECK(escalate_fault(FaultCause::Slip, 1, false) == Escalation::ParkColumn);

    // One column, jam signature: stop THAT column at once.  Still not the
    // whole display - four working columns are not endangered by one stuck one.
    CHECK(escalate_fault(FaultCause::Jam, 1, false) == Escalation::StopColumn);

    // Two at once is not two coincidences.  Power, loom, or frame - drop EN.
    CHECK(escalate_fault(FaultCause::NoHallEver, 2, false) == Escalation::DropEnable);
    CHECK(escalate_fault(FaultCause::Slip, 3, false) == Escalation::DropEnable);
    CHECK(escalate_fault(FaultCause::Jam, 2, false) == Escalation::DropEnable);

    // Any fault during the alarm spin drops EN whatever the cause and however
    // few columns: five drums at 25 flaps/s is the worst moment to keep
    // driving into a mechanical problem.
    CHECK(escalate_fault(FaultCause::NoHallEver, 1, true) == Escalation::DropEnable);
    CHECK(escalate_fault(FaultCause::Slip, 1, true) == Escalation::DropEnable);
    CHECK(escalate_fault(FaultCause::Jam, 1, true) == Escalation::DropEnable);

    // Escalation is monotonic in the column count: adding a faulted column
    // never produces a GENTLER response.  Guards against a future reordering
    // of the branches.
    auto rank = [](Escalation e) {
        switch (e) {
            case Escalation::ParkColumn: return 0;
            case Escalation::StopColumn: return 1;
            case Escalation::DropEnable: return 2;
        }
        return -1;
    };
    for (FaultCause c : {FaultCause::NoHallEver, FaultCause::Slip, FaultCause::Jam}) {
        for (int n = 1; n < N_COLUMNS; ++n) {
            CHECK(rank(escalate_fault(c, n + 1, false)) >= rank(escalate_fault(c, n, false)));
            CHECK(rank(escalate_fault(c, n, true)) >= rank(escalate_fault(c, n, false)));
        }
    }
}

void test_names_round_trip() {
    CHECK(std::string(fault_cause_name(FaultCause::None)) == "none");
    CHECK(std::string(fault_cause_name(FaultCause::NoHallEver)) == "no_hall");
    CHECK(std::string(fault_cause_name(FaultCause::Slip)) == "slip");
    CHECK(std::string(fault_cause_name(FaultCause::Jam)) == "jam");
    CHECK(std::string(escalation_name(Escalation::ParkColumn)) == "park_column");
    CHECK(std::string(escalation_name(Escalation::StopColumn)) == "stop_column");
    CHECK(std::string(escalation_name(Escalation::DropEnable)) == "drop_enable");

    for (ColumnMode m : {ColumnMode::Real, ColumnMode::Sim, ColumnMode::Disabled}) {
        ColumnMode back{};
        CHECK(column_mode_from_name(column_mode_name(m), back));
        CHECK(back == m);
    }
    ColumnMode m{};
    CHECK(column_mode_from_name("off", m));  // console shorthand
    CHECK(m == ColumnMode::Disabled);
    CHECK(!column_mode_from_name("", m));
    CHECK(!column_mode_from_name("REAL", m));       // case is not guessed at
    CHECK(!column_mode_from_name("simulated", m));  // nor prefixes
}

// --------------------------------------------------------------------------
// The per-column state machine
// --------------------------------------------------------------------------
void test_column_config_defaults() {
    ColumnConfig c;
    // The single most important property in this file: a fresh NVS boots
    // REAL, not simulated, and not in maintenance.  Also asserted at compile
    // time in column_mode.h; asserted again here so a reader sees it.
    for (int i = 0; i < N_COLUMNS; ++i) CHECK(c.mode[static_cast<size_t>(i)] == ColumnMode::Real);
    CHECK(!c.maintenance);
    CHECK(!c.any(ColumnMode::Sim));
    CHECK(!c.any(ColumnMode::Disabled));
    CHECK_EQ(c.count(ColumnMode::Real), N_COLUMNS);
    CHECK_EQ(static_cast<int>(c.excluded_mask()), 0);
}

void test_excluded_mask() {
    ColumnConfig c;
    c.mode[0] = ColumnMode::Disabled;
    c.mode[4] = ColumnMode::Disabled;
    CHECK_EQ(static_cast<int>(c.excluded_mask()), 0b10001);
    CHECK_EQ(c.count(ColumnMode::Disabled), 2);

    // A SIMULATED column is not excluded.  It renders, it homes, it moves -
    // that is the whole point of the capability.  Only "disabled" is a hole.
    c.mode[1] = ColumnMode::Sim;
    c.mode[2] = ColumnMode::Sim;
    CHECK_EQ(static_cast<int>(c.excluded_mask()), 0b10001);
    CHECK_EQ(c.count(ColumnMode::Sim), 2);
    CHECK_EQ(c.count(ColumnMode::Real), 1);

    // The two compose, which is the point: one real column and four simulated
    // during build-out; one disabled and four real during a repair.
    ColumnConfig build_out;
    build_out.mode[0] = ColumnMode::Real;
    for (int i = 1; i < N_COLUMNS; ++i) build_out.mode[static_cast<size_t>(i)] = ColumnMode::Sim;
    CHECK_EQ(static_cast<int>(build_out.excluded_mask()), 0);
    CHECK(build_out.any(ColumnMode::Sim));

    ColumnConfig repair;
    repair.mode[2] = ColumnMode::Disabled;
    CHECK_EQ(static_cast<int>(repair.excluded_mask()), 0b00100);
    CHECK(!repair.any(ColumnMode::Sim));
}

void test_disabled_is_never_inferred() {
    // There is no path in the policy layer that turns a fault into Disabled -
    // the only way a column becomes disabled is an explicit assignment.  What
    // is assertable here is that escalation's vocabulary does not contain
    // "disable the column": its three outcomes are all about stepping, not
    // configuration, so no fault can quietly rewrite the persisted state.
    for (FaultCause c : {FaultCause::NoHallEver, FaultCause::Slip, FaultCause::Jam}) {
        for (int n = 1; n <= N_COLUMNS; ++n) {
            for (bool spin : {false, true}) {
                const Escalation e = escalate_fault(c, n, spin);
                CHECK(e == Escalation::ParkColumn || e == Escalation::StopColumn ||
                      e == Escalation::DropEnable);
            }
        }
    }
}

}  // namespace

void run_tests() {
    test_retry_rule();
    test_escalation();
    test_names_round_trip();
    test_column_config_defaults();
    test_excluded_mask();
    test_disabled_is_never_inferred();
}
