// The ranges an image accepts, and the one flavour that widens them
// (motion/limits_policy.h, spec 17 2026-10-04).
//
// COMPILED TWICE, from this one file: once as a normal image and once with
// -DSWAN_UNLIMITED=1 (test_limits_policy_unlimited).  Everything that must be
// true in BOTH is asserted unconditionally - above all, that what may be
// PERSISTED is the normal image's range whichever flavour is running.  Everything
// the flavour changes branches on kUnlimited, which is read from the same macro
// the header reads, so a test that agrees with a header that is wrong about its
// own flavour cannot happen: the macro is the only input.
#include <string>

#include "check.h"
#include "motion/limits_policy.h"
#include "motion/motion_math.h"   // TICK_HZ: the ISR's own ceiling
#include "motion/motion_types.h"

using namespace swan;
using namespace swan::motion;

namespace {

#if defined(SWAN_UNLIMITED) && SWAN_UNLIMITED
constexpr bool kUnlimited = true;
#else
constexpr bool kUnlimited = false;
#endif

void test_flavour_is_what_the_build_said() {
    CHECK_EQ(UNLIMITED_BUILD, kUnlimited);
    // The two flavours it must never share an image with.  The pair itself is a
    // static_assert, proved to fire by the negative-compile tests in CMake; here
    // is the half that can run.
    CHECK(!(UNLIMITED_BUILD && BENCH_BUILD));
}

// ---- speed -----------------------------------------------------------------
void test_speed_range() {
    CHECK_EQ(FLAPS_S_MIN, 1);
    CHECK_EQ(FLAPS_S_MAX_NORMAL, 40);
    CHECK_EQ(FLAPS_S_MAX_UNLIMITED, SHOW_SPIN_FLAPS_S);   // 400, the spec's show spin
    CHECK_EQ(FLAPS_S_MAX, kUnlimited ? 400 : 40);

    // The live range is a step at its ceiling, in both flavours.
    CHECK(!flaps_s_plausible(0));
    CHECK(!flaps_s_plausible(-1));
    CHECK(flaps_s_plausible(1));
    CHECK(flaps_s_plausible(FLAPS_S_MAX));
    CHECK(!flaps_s_plausible(FLAPS_S_MAX + 1));
    CHECK(flaps_s_plausible(25));   // the shipped alarm speed

    // What NVS may hold is the NORMAL range, whatever is running.
    CHECK(!flaps_s_persistable(0));
    CHECK(flaps_s_persistable(1));
    CHECK(flaps_s_persistable(40));
    CHECK(!flaps_s_persistable(41));
    CHECK(!flaps_s_persistable(400));
    CHECK(!flaps_s_persistable(1000));

    // The unlimited ceiling is the show spin and not the ISR's maximum: the
    // architecture was validated at 25 600 usteps/s, and 50 000 is the wall.
    CHECK(flaps_s_to_usteps_s(FLAPS_S_MAX_UNLIMITED) <= TICK_HZ);
    CHECK_EQ(flaps_s_to_usteps_s(FLAPS_S_MAX_UNLIMITED), 25600);
}

// ---- accel and hall_tol ----------------------------------------------------
void test_accel_range() {
    CHECK_EQ(ACCEL_MAX_NORMAL, 60000);
    CHECK_EQ(ACCEL_MAX, kUnlimited ? ACCEL_MAX_UNLIMITED : ACCEL_MAX_NORMAL);
    CHECK(!accel_plausible(0));          // zero divides by zero in the ramp
    CHECK(!accel_plausible(ACCEL_MIN - 1));
    CHECK(accel_plausible(ACCEL_MIN));
    CHECK(accel_plausible(ACCEL_MAX));
    CHECK(!accel_plausible(ACCEL_MAX + 1));
    CHECK(accel_plausible(60000));
    // 60001 is the line between the two flavours.
    CHECK_EQ(accel_plausible(60001), kUnlimited);

    // Persistence is flavour-independent and stays below the 82000 that stalled.
    CHECK(accel_persistable(ACCEL_MIN));
    CHECK(accel_persistable(60000));
    CHECK(!accel_persistable(60001));
    CHECK(!accel_persistable(82000));
    CHECK(!accel_persistable(1000000));
    CHECK(!accel_persistable(0));
}

void test_hall_tol_range() {
    CHECK_EQ(HALL_TOL_MAX_NORMAL, 32);     // half a flap, derived
    CHECK_EQ(HALL_TOL_MAX_UNLIMITED, 64);  // one flap: the fault band survives
    CHECK_EQ(HALL_TOL_MAX, kUnlimited ? 64 : 32);
    CHECK(!hall_tol_plausible(0));
    CHECK(hall_tol_plausible(1));
    CHECK(hall_tol_plausible(32));
    CHECK_EQ(hall_tol_plausible(33), kUnlimited);
    CHECK_EQ(hall_tol_plausible(64), kUnlimited);
    CHECK(!hall_tol_plausible(65));
    CHECK(!hall_tol_plausible(400));   // the old dispatcher bound: slip detection off

    CHECK(hall_tol_persistable(32));
    CHECK(!hall_tol_persistable(33));
    CHECK(!hall_tol_persistable(64));

    // THE BOOT RULE DOES NOT FOLLOW THE FLAVOUR.  41 was a quarter of the rim
    // gear's flap and is 64 % of this one; an unlimited image accepts it LIVE,
    // and must still discard it when NVS hands it back at boot.
    CHECK_EQ(hall_tol_migrated(41), 16);
    CHECK_EQ(hall_tol_migrated(64), 16);
    CHECK_EQ(hall_tol_migrated(33), 16);
    CHECK_EQ(hall_tol_migrated(32), 32);
    CHECK_EQ(hall_tol_migrated(16), 16);
    CHECK_EQ(hall_tol_migrated(0), 16);
}

// ---- what may be written to NVS --------------------------------------------
void test_persist_refusal_is_the_normal_range_in_every_flavour() {
    MotionParams p;
    CHECK(persist_refusal(p) == nullptr);    // the spec defaults always save

    // The edges of the normal range save.
    p.flaps_s_normal = 40; p.flaps_s_alarm = 40; p.flaps_s_home = 1;
    p.accel = ACCEL_MIN;
    p.hall_tol = 32;
    CHECK(persist_refusal(p) == nullptr);
    p.accel = 60000;
    CHECK(persist_refusal(p) == nullptr);

    // Each field names itself when it is past it - whether or not THIS image
    // would have accepted the value live.  That is the entire rule.
    const auto refused = [](MotionParams q) {
        const char* w = persist_refusal(q);
        return w == nullptr ? std::string() : std::string(w);
    };
    MotionParams q;
    q.flaps_s_normal = 41;  CHECK(refused(q) == "flaps_s_normal");
    q = MotionParams{}; q.flaps_s_alarm = 400;  CHECK(refused(q) == "flaps_s_alarm");
    q = MotionParams{}; q.flaps_s_home = 0;     CHECK(refused(q) == "flaps_s_home");
    q = MotionParams{}; q.accel = 60001;        CHECK(refused(q) == "accel");
    q = MotionParams{}; q.accel = 0;            CHECK(refused(q) == "accel");
    q = MotionParams{}; q.hall_tol = 33;        CHECK(refused(q) == "hall_tol");
    q = MotionParams{}; q.hall_tol = 64;        CHECK(refused(q) == "hall_tol");
}

// ---- the ramp-power guard --------------------------------------------------
void test_ramp_power_guard_constants() {
    CHECK_EQ(RAMP_FLOOR_S, 2);
    CHECK_EQ(SHOW_SPIN_USTEPS_S, 25600);
    CHECK_EQ(RAMP_ACCEL_AT_SHOW_SPIN, 12800);     // 25 600 usteps/s in 2 s
    CHECK_EQ(RAMP_POWER_MAX, 327680000LL);
    // The spec's own derivation (motion_types.h, accel): 12000 -> 2.13 s to the
    // show spin, 14000 -> 1.83 s and FAILS.  The guard must agree with both.
    CHECK(ramp_power_ok(12000, 400));
    CHECK(ramp_power_ok(12800, 400));
    CHECK(!ramp_power_ok(14000, 400));
}

// A normal image can never reach the guard: the extremes of everything it can
// hold are inside it.  The static_asserts say so at the corners; this walks
// the whole range, so a change to ANY range constant that opens a gap fails here
// with a speed in the message and not as a template error.
void test_a_normal_image_is_always_inside_the_guard() {
    for (int32_t v = FLAPS_S_MIN; v <= FLAPS_S_MAX_NORMAL; ++v) {
        CHECK(ramp_power_ok(ACCEL_MAX_NORMAL, v));
        CHECK(ramp_power_ok(ACCEL_MIN, v));
    }
    // And the shipped defaults at the show spin, which is what the 2 s floor is about.
    const MotionParams d;
    CHECK(ramp_power_ok(d.accel, SHOW_SPIN_FLAPS_S));
}

// The guard only EVER refuses in the unlimited flavour.  In a normal image the
// console's `spin` must not change because this flavour exists.
void test_the_guard_refuses_only_in_the_unlimited_flavour() {
    CHECK_EQ(ramp_too_fast(60000, 400), kUnlimited);
    CHECK_EQ(ramp_too_fast(14000, 400), kUnlimited);
    CHECK_EQ(ramp_too_fast(1000000, 400), kUnlimited);
    CHECK(!ramp_too_fast(12000, 400));         // the shipped default passes everywhere
    CHECK(!ramp_too_fast(12800, 400));         // the boundary itself is allowed
    CHECK_EQ(ramp_too_fast(12801, 400), kUnlimited);
    CHECK(!ramp_too_fast(60000, 40));          // a normal image's own corner
}

// The guard is a proportion, so it is exact at every speed, not just at 400.
void test_the_guard_is_exact_at_every_speed() {
    for (int32_t v = 1; v <= FLAPS_S_MAX_UNLIMITED; ++v) {
        const int64_t c = ramp_accel_ceiling(v);
        CHECK(c >= 1);
        CHECK(ramp_power_ok(c, v));            // the ceiling itself passes...
        CHECK(!ramp_power_ok(c + 1, v));       // ...and one past it does not
    }
    CHECK_EQ(ramp_accel_ceiling(400), 12800);
    CHECK_EQ(ramp_accel_ceiling(25), 204800);   // 25 flaps/s = 1600 usteps/s
    CHECK_EQ(ramp_accel_ceiling(40), 128000);   // what a normal image tops out at
    // "accel past 60 000" is real in the unlimited flavour - at low speed.
    CHECK(ACCEL_MAX_UNLIMITED > ACCEL_MAX_NORMAL);
    CHECK(ramp_accel_ceiling(25) > ACCEL_MAX_NORMAL);
    CHECK(ramp_accel_ceiling(85) >= ACCEL_MAX_NORMAL);       // 5440 usteps/s: 60 000 just fits
    CHECK(ramp_accel_ceiling(86) < ACCEL_MAX_NORMAL);        // and one flap/s faster does not
}

}  // namespace

void run_tests() {
    test_flavour_is_what_the_build_said();
    test_speed_range();
    test_accel_range();
    test_hall_tol_range();
    test_persist_refusal_is_the_normal_range_in_every_flavour();
    test_ramp_power_guard_constants();
    test_a_normal_image_is_always_inside_the_guard();
    test_the_guard_refuses_only_in_the_unlimited_flavour();
    test_the_guard_is_exact_at_every_speed();
}
