// The stand-in bench session (BRINGUP §28b gate 3, spec §15 phase 8).
//
// ONE REAL COLUMN, on a real TMC2209, on a printed PLA axle, before anything is
// bought.  It answers the question the direct drive created and nothing else in
// this repo can answer: a NEMA 17 is now sealed inside a PLA drum that softens
// at 55-60 °C, and the clock holds position 99% of the day.  Does it cook?
//
// The verdict is a HAND ON THE MOTOR CASE.  There is no temperature sensor in
// this build and adding one would be answering a different question with worse
// equipment - so the firmware's job is to run the duty cycle honestly for an
// hour, report exactly what the drum did while it ran, and then stop and ask.
//
// Two modes, and one thing that is absent on purpose:
//
//   soak   one flap per tick at clock cadence for a set time.  The heat test.
//          Hall edges, resyncs and elapsed time are logged, because a thermal
//          run that also proves the drum kept its registration is two answers
//          for one hour of bench time.
//   spin   slow continuous rotation for runout and wire-routing checks, hard
//          capped at one drum revolution per second (bench_policy.h).
//   ----   NO SHOW SPIN.  Not slower, not shorter: absent.  See bench_policy.h.
//
// Pure schedule logic lives here and is host-tested; bench.cpp is the IDF shell
// that owns the task and talks to motion::.
#pragma once

#include <cstdint>

namespace swan {
namespace motion {

// ---------------------------------------------------------------------------
// The schedule.  Pure: given how long the run has been going and when the last
// flap was issued, say what to do now.  Separated from the shell so the cadence
// is testable without a drum, a timer or an hour.
// ---------------------------------------------------------------------------
struct BenchSchedule {
    uint32_t total_s = 3600;  // one hour
    uint32_t tick_s = 1;      // one flap per tick

    // THE TICK IS NOT THE THERMAL VARIABLE, and it is worth being clear about
    // why it defaults to a second rather than to the clock's 15 minutes.
    //
    // The heat comes almost entirely from HOLDING current: a flap at 15 flaps/s
    // occupies ~67 ms, so even at one flap a second the coils are holding for
    // >93% of the hour, and at the clock's real cadence it is >99.99%.  Those
    // are the same thermal question to within the accuracy of a hand on a case.
    //
    // What the tick actually buys is MOTION DATA - hall edges to classify,
    // resyncs to count, a chance for a marginal magnet or a slipping axle to
    // show itself.  At one flap per second an hour gives 3600 flaps and 72 drum
    // revolutions; at one per 15 minutes it gives four flaps and no revolutions
    // at all, and the hour would answer only the thermal half.
    //
    // So: a second by default, settable, and the reason recorded rather than
    // the number looking arbitrary.
};

struct BenchStats {
    bool running = false;
    int column = 0;
    uint32_t elapsed_s = 0;
    uint32_t total_s = 0;
    uint32_t flaps = 0;
    uint32_t edges = 0;         // hall edges seen during the run
    uint32_t resync_minor = 0;  // deltas since the run started
    uint32_t resync_major = 0;
    uint32_t faults = 0;
    int32_t h2h_min = 0;
    int32_t h2h_max = 0;
    int32_t err_abs_max = 0;
    uint32_t heap_start = 0;
    uint32_t heap_now = 0;
    uint32_t heap_min = 0;
    const char* stopped_because = "";
    // Set when the run reaches its full duration, which is the only state in
    // which the hand-on-the-case verdict means anything.  A soak that was cut
    // short is not a shorter answer, it is no answer.
    bool completed = false;
};

// Whether a flap is due now.  `elapsed_s` is seconds since the run began and
// `last_flap_s` the elapsed time at which the previous flap was issued.
constexpr bool bench_flap_due(const BenchSchedule& s, uint32_t elapsed_s,
                              uint32_t last_flap_s, bool any_flap_yet) {
    if (s.tick_s == 0) return false;
    if (!any_flap_yet) return true;                 // the first one goes at once
    return elapsed_s - last_flap_s >= s.tick_s;
}

constexpr bool bench_run_over(const BenchSchedule& s, uint32_t elapsed_s) {
    return s.total_s != 0 && elapsed_s >= s.total_s;
}

// How many flaps a schedule should produce over its whole run.  Used by the
// report to say whether the drum actually did what it was asked.
constexpr uint32_t bench_expected_flaps(const BenchSchedule& s) {
    if (s.tick_s == 0 || s.total_s == 0) return 0;
    return s.total_s / s.tick_s + 1;  // the first flap is at t = 0
}

// ---------------------------------------------------------------------------
// The shell (bench.cpp).  Absent from a non-bench build entirely.
// ---------------------------------------------------------------------------

// Start the clock-cadence heat soak on one column.  False if a run is already
// going, the column cannot be driven, or this is not a bench build.
bool bench_soak_start(int column, const BenchSchedule& s);

// The slow inspection spin.  `flaps_s` above the cap is REFUSED, not clamped.
bool bench_spin_start(int column, int32_t flaps_s, int seconds);

void bench_stop(const char* why);
bool bench_running();
BenchStats bench_report();

}  // namespace motion
}  // namespace swan
