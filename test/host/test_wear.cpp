// Flap wear (spec 7.1 wear table, 7.3), measured by walking the real
// renderers over a real ring rather than tabulated by hand.
//
// This suite is also where the spec's numbers come from: it prints the tables
// it asserts, so a figure quoted in the spec can be regenerated in one command
// instead of being trusted.
#include <cstdio>
#include <string>

#include "check.h"
#include "modes/wear.h"
#include "ring/ring.h"

using namespace swan;

namespace {

void print_row(const char* label, const WearEstimate& w) {
    std::printf("  %-10s %7u  |", label, w.total);
    for (int i = 0; i < N_COLUMNS; ++i) {
        std::printf(" %6u", w.flips[static_cast<size_t>(i)]);
    }
    std::printf("  | %u renders\n", w.renders);
}

// --------------------------------------------------------------------------
void test_granularity_domain() {
    // Divisors of 60 only.  At 7 the flooring steps 56 -> 0 across the hour,
    // so the last window of every hour would be four minutes long.
    const int good[] = {1, 2, 3, 4, 5, 6, 10, 12, 15, 20, 30, 60};
    for (const int g : good) CHECK(granularity_valid(g));
    const int bad[] = {0, -1, 7, 8, 9, 11, 13, 14, 16, 25, 45, 59, 61, 120};
    for (const int g : bad) CHECK(!granularity_valid(g));
    CHECK_EQ(granularity_choices().size(), 12u);

    // An invalid granularity yields an empty estimate rather than a wrong one.
    const RingSet ring = RingSet::compiled_fallback();
    CHECK_EQ(clock_wear_per_day(ring, false, 7).total, 0u);
}

void test_clock_wear() {
    const RingSet ring = RingSet::compiled_fallback();

    std::printf("\nclock wear, flips per day (12 h)\n");
    std::printf("  %-10s %7s  | %6s %6s %6s %6s %6s\n", "gran", "total", "col1", "col2", "col3",
                "col4", "col5");
    WearEstimate prev;
    bool first = true;
    for (const int g : granularity_choices()) {
        const WearEstimate w = clock_wear_per_day(ring, false, g);
        print_row((std::to_string(g) + " min").c_str(), w);

        // Every column total is the sum, and nothing is negative by
        // construction (forward-only).
        uint32_t sum = 0;
        for (int i = 0; i < N_COLUMNS; ++i) sum += w.flips[static_cast<size_t>(i)];
        CHECK_EQ(sum, w.total);

        (void)prev;
        prev = w;
        first = false;
    }

    // Coarser is NOT always cheaper, and the exact figures are the only way to
    // see it: at 10 minutes the ones-of-minutes digit is always 0 so column 5
    // never moves, while 12 minutes walks it through 0,2,4,6,8 every hour.  12
    // is worse than 10 AND worse than 15.  A UI interpolating a lookup table
    // would hide that; this is why the figure is computed, not tabulated.
    CHECK(clock_wear_per_day(ring, false, 12).total >
          clock_wear_per_day(ring, false, 10).total);
    CHECK(clock_wear_per_day(ring, false, 12).total >
          clock_wear_per_day(ring, false, 15).total);
    CHECK_EQ(clock_wear_per_day(ring, false, 10).flips[4], 0u);
    CHECK(clock_wear_per_day(ring, false, 12).flips[4] > 0u);

    // The facts the spec 7.1 table rests on, pinned so a renderer change that
    // moves them fails here rather than silently rewriting the guidance.
    const WearEstimate g1 = clock_wear_per_day(ring, false, 1);
    const WearEstimate g15 = clock_wear_per_day(ring, false, 15);
    const WearEstimate g30 = clock_wear_per_day(ring, false, 30);
    const WearEstimate g60 = clock_wear_per_day(ring, false, 60);

    // Column 1 is AM/PM: exactly two changes a day at any granularity.
    for (int i = 0; i < N_COLUMNS; ++i) {
        CHECK(g60.flips[static_cast<size_t>(i)] <= g1.flips[static_cast<size_t>(i)]);
    }
    // At 30 minutes the ones-of-minutes digit is always 0, so column 5 is
    // still; at 60 both minute columns are.
    CHECK_EQ(g30.flips[4], 0u);
    CHECK_EQ(g60.flips[4], 0u);
    CHECK_EQ(g60.flips[3], 0u);

    // Column 4 outweighs column 5 at the default granularity - the deliberate
    // asymmetry recorded in the spec 17 log (ring A pays the full increment,
    // ring B has the cheap double block).
    CHECK(g15.flips[3] > g15.flips[4]);

    // 24 h drops AM/PM and gives the hour-tens column a third digit.
    std::printf("\nclock wear, flips per day (24 h)\n");
    std::printf("  %-10s %7s  | %6s %6s %6s %6s %6s\n", "gran", "total", "col1", "col2",
                "col3", "col4", "col5");
    for (const int g : {1, 15, 60}) {
        print_row((std::to_string(g) + " min").c_str(), clock_wear_per_day(ring, true, g));
    }
    const WearEstimate h24 = clock_wear_per_day(ring, true, 15);
    // The minute columns do the same work either way; only the left three
    // differ, and column 1 is the one that stops moving.
    CHECK_EQ(h24.flips[3], g15.flips[3]);
    CHECK_EQ(h24.flips[4], g15.flips[4]);
    CHECK_EQ(h24.flips[0], 0u);          // blank all day in 24 h
    CHECK(g15.flips[0] > 0u);            // AM/PM in 12 h
}

void test_countdown_wear() {
    const RingSet ring = RingSet::compiled_fallback();

    std::printf("\ncountdown wear, flips per 108-minute run (seconds_live_s = 240)\n");
    std::printf("  %-10s %7s  | %6s %6s %6s %6s %6s\n", "mode", "total", "col1", "col2", "col3",
                "col4", "col5");
    const WearEstimate mins = countdown_wear_per_run(ring, SecondsMode::Minutes, 240);
    const WearEstimate tens = countdown_wear_per_run(ring, SecondsMode::Tens, 240);
    const WearEstimate secs = countdown_wear_per_run(ring, SecondsMode::Seconds, 240);
    print_row("minutes", mins);
    print_row("tens", tens);
    print_row("seconds", secs);

    // What the freeze buys, against the same modes run live for the whole 108
    // minutes (live_s = 6480) - the designs this replaced.
    const WearEstimate tens_all = countdown_wear_per_run(ring, SecondsMode::Tens, 6480);
    const WearEstimate secs_all = countdown_wear_per_run(ring, SecondsMode::Seconds, 6480);
    std::printf("\n  live for the whole run (the pre-freeze designs)\n");
    print_row("tens", tens_all);
    print_row("seconds", secs_all);

    // The quiet phase is identical in every mode - only the last window
    // differs - so minutes is a lower bound and the others build on it.
    CHECK(mins.total < tens.total);
    CHECK(tens.total < secs.total);

    // Minutes mode never touches the seconds columns, all run.
    CHECK_EQ(mins.flips[3], 0u);
    CHECK_EQ(mins.flips[4], 0u);
    // Tens parks column 5.
    CHECK_EQ(tens.flips[4], 0u);

    // The freeze is worth an order of magnitude on the seconds columns.
    CHECK(secs.flips[4] * 10 < secs_all.flips[4]);
    CHECK(secs.total * 5 < secs_all.total);

    // The two figures the spec quotes for the live window, from the manifests:
    // column 5 pays 216 single flips and 24 wraps of 16; column 4 pays 20
    // single flips and 4 borrows of 45.
    CHECK_EQ(secs.flips[4], 216u + 24u * 16u);
    CHECK_EQ(secs.flips[3], 20u + 4u * 45u);

    // seconds_live_s = 0 collapses every mode onto the minutes cost.
    CHECK_EQ(countdown_wear_per_run(ring, SecondsMode::Seconds, 0).total, mins.total);
    CHECK_EQ(countdown_wear_per_run(ring, SecondsMode::Tens, 0).total, mins.total);
}

}  // namespace

void run_tests() {
    test_granularity_domain();
    test_clock_wear();
    test_countdown_wear();
}
