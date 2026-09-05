// The stand-in bench build's cap and cadence (spec 15 phase 8, BRINGUP 28b).
//
// The cap is a SAFETY CONTRACT on a printed PLA axle, so it is tested in both
// directions: that a bench image refuses the show spin, and that a normal image
// is not quietly crippled by a constant that leaked out of the bench build.
#include "check.h"
#include "motion/bench.h"
#include "motion/bench_policy.h"

using namespace swan;
using namespace swan::motion;

namespace {

// This suite is built WITHOUT SWAN_BENCH (the host suite is not a bench image),
// so BENCH_BUILD is false here and the "normal build" half is the live one.
// The bench half is checked against the constants directly, which is what the
// firmware's own clamp is built from.
void test_normal_build_is_not_capped() {
    CHECK(!BENCH_BUILD);
    CHECK_EQ(bench_speed_cap(), 0);
    // The identity, at every speed the machine uses.
    CHECK_EQ(bench_clamp_flaps_s(8), 8);
    CHECK_EQ(bench_clamp_flaps_s(15), 15);
    CHECK_EQ(bench_clamp_flaps_s(25), 25);
    CHECK_EQ(bench_clamp_flaps_s(SHOW_SPIN_FLAPS_S), SHOW_SPIN_FLAPS_S);
    CHECK(!bench_speed_refused(SHOW_SPIN_FLAPS_S));
}

// The cap itself, independent of which flavour this suite was built as: one
// drum revolution per second, which the ring size makes 50 flaps/s.
void test_the_cap_is_one_drum_rev_per_second() {
    CHECK_EQ(BENCH_MAX_FLAPS_S, 50);
    CHECK_EQ(BENCH_MAX_FLAPS_S, N_RING);
    // A revolution a second, expressed the other way round, in usteps.
    CHECK_EQ(flaps_s_to_usteps_s(BENCH_MAX_FLAPS_S), 3200);
    CHECK_EQ(flaps_s_to_usteps_s(BENCH_MAX_FLAPS_S), USTEPS_PER_SPOOL_REV_NOMINAL);
    // And it is eight times slower than the show spin, which is the ratio the
    // refusal message quotes.
    CHECK_EQ(SHOW_SPIN_FLAPS_S / BENCH_MAX_FLAPS_S, 8);
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

}  // namespace

void run_tests() {
    test_normal_build_is_not_capped();
    test_the_cap_is_one_drum_rev_per_second();
    test_schedule_defaults();
    test_flap_cadence();
    test_run_end();
    test_the_coils_hold_for_almost_all_of_it();
}
