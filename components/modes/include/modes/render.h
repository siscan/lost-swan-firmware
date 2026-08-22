// Frame renderers (spec 7) - pure functions from state to five ring indices.
// Everything resolves through the RingSet by role, per column, so a reordered
// or per-column ring keeps rendering; a failed lookup shows blank on that
// column and sets *diag (spec 4).
//
// Every renderer takes `from`, the frame currently displayed (or being moved
// to).  It is not decoration: the rings are one-way and column 5 carries two
// slots per digit, so "which slot renders this digit" depends on where that
// column is now - the nearest one going forward wins.
#pragma once

#include "frame/frame.h"
#include "ring/ring_runtime.h"
#include "timesvc/tz.h"

namespace swan {

// Countdown seconds resolution (spec 7.3, countdown.seconds_mode).
//   Seconds - MMM:SS, live one-second ticks.  Possible because the descending
//             ring makes a decrement 1 flip and column 5's double block makes
//             its 0->9 wrap 16 flips rather than 41.
//   Tens    - MMM:S0, the original 10-second scheme; column 5 parks on 0.
enum class SecondsMode : unsigned char { Seconds, Tens };

// Clock layout (spec 7.1, Q2): col 1 AM/PM (blank in 24 h), cols 2-3 hours,
// cols 4-5 minutes.  12 h: hours 1-12, tens column blank below 10.  24 h:
// leading zero.  `lt` is expected already floored to clock.granularity_min.
Frame render_clock(const RingSet& ring, const LocalTime& lt, bool h24, const Frame& from,
                   bool* diag = nullptr);

Frame render_blank(const RingSet& ring, const Frame& from);

// Boot / no-signal: WiFi glyph on the CENTRE column, blanks elsewhere
// (spec 7.1, handoff-DECIDED).  Column 5's ring has no WiFi glyph and is
// never asked for one.
Frame render_wifi(const RingSet& ring, const Frame& from, bool* diag = nullptr);

// The ????? state: every column to the question glyph (spec 7.3).
Frame render_qmarks(const RingSet& ring, const Frame& from, bool* diag = nullptr);

// Countdown (spec 7.3).  Seconds mode renders MMM:SS from the exact remaining
// second; Tens mode renders MMM:S0 and parks column 5 on digit 0.
Frame render_countdown(const RingSet& ring, int remaining_s, SecondsMode mode,
                       const Frame& from, bool* diag = nullptr);

}  // namespace swan
