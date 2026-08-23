#include "modes/wear.h"

#include "ring/ring.h"

namespace swan {
namespace {

constexpr std::array<int, 12> kGranularities = {1, 2, 3, 4, 5, 6, 10, 12, 15, 20, 30, 60};

// Cost of moving to `to` from `from`, per column, accumulated.  Forward-only,
// so this is the real flip count and never negative (ring.h).
void accumulate(WearEstimate& w, const Frame& from, const Frame& to) {
    bool moved = false;
    for (int i = 0; i < N_COLUMNS; ++i) {
        const int d = ring_forward_distance(from.idx[static_cast<size_t>(i)],
                                            to.idx[static_cast<size_t>(i)]);
        if (d != 0) moved = true;
        w.flips[static_cast<size_t>(i)] += static_cast<uint32_t>(d);
        w.total += static_cast<uint32_t>(d);
    }
    if (moved) ++w.renders;
}

LocalTime at_minute(int minute_of_day) {
    LocalTime lt{};
    lt.year = 2026;
    lt.month = 1;
    lt.day = 1;
    lt.hour = minute_of_day / 60;
    lt.minute = minute_of_day % 60;
    lt.second = 0;
    lt.weekday = 4;
    lt.dst = false;
    return lt;
}

}  // namespace

bool granularity_valid(int minutes) {
    for (const int g : kGranularities) {
        if (g == minutes) return true;
    }
    return false;
}

const std::array<int, 12>& granularity_choices() { return kGranularities; }

WearEstimate clock_wear_per_day(const RingSet& ring, bool h24, int granularity_min) {
    WearEstimate w;
    if (!granularity_valid(granularity_min)) return w;

    // Only the floored values ever render, so walk those rather than all 1440
    // minutes - same answer, a fraction of the work on the device.
    Frame cur = render_clock(ring, at_minute(0), h24, Frame{{RING_HOME_SLOT, RING_HOME_SLOT,
                                                            RING_HOME_SLOT, RING_HOME_SLOT,
                                                            RING_HOME_SLOT}});
    // Lap 1 settles column 5's block; lap 2 is the steady-state figure.
    for (int lap = 0; lap < 2; ++lap) {
        WearEstimate lap_w;
        for (int m = granularity_min; m < 1440; m += granularity_min) {
            const Frame next = render_clock(ring, at_minute(m), h24, cur);
            accumulate(lap_w, cur, next);
            cur = next;
        }
        // Close the cycle: midnight follows 23:xx, and that rollover is part
        // of a day's cost.
        const Frame midnight = render_clock(ring, at_minute(0), h24, cur);
        accumulate(lap_w, cur, midnight);
        cur = midnight;
        w = lap_w;
    }
    return w;
}

WearEstimate countdown_wear_per_run(const RingSet& ring, SecondsMode mode, int live_s) {
    WearEstimate w;
    if (live_s < 0) live_s = 0;

    // The run starts from the 108:00 face already displayed - reaching it from
    // whatever was up before is the cost of entering the mode, not of the run.
    constexpr int kTotal = 6480;
    Frame cur = render_countdown(ring, kTotal, Frame{{RING_HOME_SLOT, RING_HOME_SLOT,
                                                      RING_HOME_SLOT, RING_HOME_SLOT,
                                                      RING_HOME_SLOT}});

    int shown = kTotal;
    while (shown > 0) {
        const int next_shown = countdown_next_shown_s(mode, shown, live_s);
        const Frame next = render_countdown(ring, next_shown, cur);
        accumulate(w, cur, next);
        cur = next;
        shown = next_shown;
    }
    return w;
}

}  // namespace swan
