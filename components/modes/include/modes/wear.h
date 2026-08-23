// Flap wear, measured rather than tabulated (spec 7.1, 7.3).
//
// These walk a whole day, or a whole countdown run, through the REAL renderers
// against the REAL ring and count forward flips per column.  That is the only
// way the number in the Settings page can be exact for any input and cannot
// drift from what the display will actually do: change the renderer or upload
// a different ring and the figure changes with it.
//
// Pure - no IDF - so the host tests, the dev server and the firmware share
// one implementation.
#pragma once

#include <array>
#include <cstdint>

#include "modes/render.h"
#include "ring/ring_runtime.h"

namespace swan {

struct WearEstimate {
    std::array<uint32_t, N_COLUMNS> flips{};
    uint32_t total = 0;
    uint32_t renders = 0;  // frames issued; useful for sanity, not for wear
};

// clock.granularity_min must divide 60.  At 7 the flooring steps 56 -> 0
// across the hour boundary, so the last window of every hour is 4 minutes
// long and the display jumps - a defect, not a tuning choice.
bool granularity_valid(int minutes);

// The valid values, ascending: 1 2 3 4 5 6 10 12 15 20 30 60.
const std::array<int, 12>& granularity_choices();

// A nominal 1440-minute day at this granularity, as a cycle: the walk runs
// twice and reports the second lap, so column 5's two-digit-block position has
// settled and the figure is the steady state rather than an artefact of where
// the walk happened to start.  DST days are 23 or 25 hours and differ
// slightly; this is the nominal figure and the UI says so.
WearEstimate clock_wear_per_day(const RingSet& ring, bool h24, int granularity_min);

// One full 108-minute countdown, from the 108:00 face to 000:00.
WearEstimate countdown_wear_per_run(const RingSet& ring, SecondsMode mode, int live_s);

}  // namespace swan
