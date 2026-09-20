// The stand-in bench build's cap and cadence (spec 15 phase 8, BRINGUP 28b/28c).
//
// The cap is a SAFETY CONTRACT on a printed PLA axle, so it is tested in both
// directions: that a bench image refuses the show spin, and that a normal image
// is not quietly crippled by a constant that leaked out of the bench build.
//
// Since 2026-09-20 the cap is a BUILD PARAMETER (-DSWAN_BENCH_CAP) that may only
// lower a compiled-in ceiling, so the tests below check the RULE at every cap in
// range rather than one session's number.
#include "check.h"
#include "motion/bench.h"
#include "motion/bench_policy.h"

using namespace swan;
using namespace swan::motion;

namespace {

// This suite is built WITHOUT SWAN_BENCH (the host suite is not a bench image),
// so BENCH_BUILD is false here and the "normal build" half is the live one.
// The bench half is checked against the constants directly, which is what the
// firmware's own refusal is built from.
void test_normal_build_is_not_capped() {
    CHECK(!BENCH_BUILD);
    CHECK_EQ(bench_speed_cap(), 0);
    // Nothing is refused, at every speed the machine uses.
    CHECK(!bench_speed_refused(8));
    CHECK(!bench_speed_refused(15));
    CHECK(!bench_speed_refused(25));
    CHECK(!bench_speed_refused(SHOW_SPIN_FLAPS_S));
    // Even an absurd one: a normal image's only limit is the step ISR, and a
    // cap constant leaking out of the bench build would cripple the display
    // silently.
    CHECK(!bench_speed_refused(100000));
}

// The CEILING, which no build may exceed: one drum revolution per second,
// which the ring size makes 50 flaps/s.
void test_the_ceiling_is_one_drum_rev_per_second() {
    CHECK_EQ(BENCH_CAP_CEILING, 50);
    CHECK_EQ(BENCH_CAP_CEILING, N_RING);
    // A revolution a second, expressed the other way round, in usteps.
    CHECK_EQ(flaps_s_to_usteps_s(BENCH_CAP_CEILING), 3200);
    CHECK_EQ(flaps_s_to_usteps_s(BENCH_CAP_CEILING), USTEPS_PER_SPOOL_REV_NOMINAL);
    // And it is eight times slower than the show spin, which is the ratio the
    // refusal message quotes.
    CHECK_EQ(SHOW_SPIN_FLAPS_S / BENCH_CAP_CEILING, 8);
}

// THE CAP IS A BUILD PARAMETER AND MAY ONLY GO DOWN (2026-09-20).  This suite
// is not built with SWAN_BENCH_CAP, so what is checkable here is the invariant
// rather than one session's number: whatever the parameter is set to, the
// header pins it into 1..ceiling with a static_assert, and the refusal rule is
// the same function at any cap.
void test_the_cap_may_only_lower_the_ceiling() {
    CHECK(BENCH_MAX_FLAPS_S >= 1);
    CHECK(BENCH_MAX_FLAPS_S <= BENCH_CAP_CEILING);
    // Unset here, so it IS the ceiling - which also pins the default.
    CHECK_EQ(BENCH_MAX_FLAPS_S, BENCH_CAP_CEILING);
}

// REFUSE, NEVER CLAMP.  The rule the whole build parameter rests on, checked
// as a rule rather than against one number: for any cap, everything at or
// below it passes and the first step above it is refused.  bench_clamp_flaps_s
// is deliberately GONE - a silent clamp in the header is a trap still loaded,
// and it is what made a bench image answer `ok` to a speed it did not run.
void test_refusal_is_a_step_at_the_cap() {
    for (int32_t cap = 1; cap <= BENCH_CAP_CEILING; ++cap) {
        // The predicate the firmware uses, evaluated as the firmware evaluates
        // it: cap > 0 && want > cap.  Spelled out here because bench_speed_cap()
        // is 0 in this build and cannot be moved.
        const auto refused = [cap](int32_t want) { return want > cap; };
        CHECK(!refused(cap));
        CHECK(!refused(cap - 1 > 0 ? cap - 1 : 1));
        CHECK(refused(cap + 1));
        CHECK(refused(SHOW_SPIN_FLAPS_S));
    }
}

// ---------------------------------------------------------------------------
// The cadence.  The soak's whole value is that it holds a realistic duty cycle
// for a full hour, so the schedule is arithmetic worth checking without waiting
// one.
// ---------------------------------------------------------------------------
void test_schedule_defaults() {
    BenchSchedule s;
    CHECK_EQ(s.total_s, 3600u);  // one hour
    CHECK_EQ(s.tick_s, 1u);      // one flap a second
    CHECK_EQ(bench_expected_flaps(s), 3601u);
}

void test_flap_cadence() {
    BenchSchedule s;
    s.tick_s = 1;

    // The first flap goes immediately - otherwise a one-tick run does nothing.
    CHECK(bench_flap_due(s, 0, 0, false));
    // Then not again until the tick has elapsed.
    CHECK(!bench_flap_due(s, 0, 0, true));
    CHECK(bench_flap_due(s, 1, 0, true));
    CHECK(bench_flap_due(s, 5, 4, true));
    CHECK(!bench_flap_due(s, 5, 5, true));

    // A slower cadence, which is what a genuinely clock-like duty looks like.
    s.tick_s = 900;  // 15 minutes, the clock's default granularity
    CHECK(!bench_flap_due(s, 800, 0, true));
    CHECK(bench_flap_due(s, 900, 0, true));
    CHECK_EQ(bench_expected_flaps(s), 5u);  // four ticks in an hour, plus t=0

    // tick_s of 0 is not "as fast as possible", it is a stopped cadence.  The
    // alternative reading would drive the drum flat out for an hour, which is
    // the opposite of a clock-cadence heat soak.
    s.tick_s = 0;
    CHECK(!bench_flap_due(s, 0, 0, false));
    CHECK(!bench_flap_due(s, 1000, 0, true));
    CHECK_EQ(bench_expected_flaps(s), 0u);
}

void test_run_end() {
    BenchSchedule s;
    s.total_s = 3600;
    CHECK(!bench_run_over(s, 0));
    CHECK(!bench_run_over(s, 3599));
    CHECK(bench_run_over(s, 3600));
    CHECK(bench_run_over(s, 4000));

    // total_s of 0 means "until stopped", matching soak_start's convention.
    s.total_s = 0;
    CHECK(!bench_run_over(s, 100000));
}

// The duty cycle the heat question actually turns on.  A flap at the default
// normal speed occupies a small fraction of a one-second tick, so the coils
// are holding for essentially the whole run - which is why the tick length is
// not the thermal variable and the comment in bench.h says so.
void test_the_coils_hold_for_almost_all_of_it() {
    // One flap is 64 usteps at 15 flaps/s = 960 usteps/s -> ~67 ms.
    const int32_t v = flaps_s_to_usteps_s(15);
    CHECK_EQ(v, 960);
    const double flap_ms = 1000.0 * USTEPS_PER_FLAP_NUM / v;
    CHECK(flap_ms > 60.0 && flap_ms < 75.0);
    // Against a one-second tick that is well over 90% holding.
    const double holding = 1.0 - flap_ms / 1000.0;
    CHECK(holding > 0.92);
}

// THE BOOT PATH IS THE ONE EXCEPTION, and it must SAY SO.  bench_enforced only
// earns its place if `changed` is true exactly when it moved the value - that
// flag is the whole difference between an announced substitution and the silent
// clamp this replaced.
void test_boot_substitution_reports_itself() {
    // Not a bench build here, so nothing is ever over the cap and nothing is
    // ever reported as changed - which is the half that matters for a display.
    bool changed = false;
    CHECK_EQ(bench_enforced(SHOW_SPIN_FLAPS_S, changed), SHOW_SPIN_FLAPS_S);
    CHECK(!changed);
    CHECK_EQ(bench_enforced(15, changed), 15);
    CHECK(!changed);

    // `changed` ACCUMULATES rather than being reset per call, which is what
    // lets a caller enforce three fields and log once.  Checked because the
    // opposite convention would look identical at one call site and lose the
    // report at the second.
    changed = true;
    CHECK_EQ(bench_enforced(15, changed), 15);
    CHECK(changed);
}

}  // namespace

void run_tests() {
    test_normal_build_is_not_capped();
    test_the_ceiling_is_one_drum_rev_per_second();
    test_the_cap_may_only_lower_the_ceiling();
    test_refusal_is_a_step_at_the_cap();
    test_boot_substitution_reports_itself();
    test_schedule_defaults();
    test_flap_cadence();
    test_run_end();
    test_the_coils_hold_for_almost_all_of_it();
}
