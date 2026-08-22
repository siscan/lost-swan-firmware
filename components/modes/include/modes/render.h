// Frame renderers (spec 7) - pure functions from state to five ring indices.
// Everything resolves through the RingSet by role, per column, so a reordered
// or per-column ring keeps rendering; a failed lookup shows blank on that
// column and sets *diag (spec 4).
#pragma once

#include "frame/frame.h"
#include "ring/ring_runtime.h"
#include "timesvc/tz.h"

namespace swan {

// Clock layout (spec 7.1, Q2): col 1 AM/PM (blank in 24 h), cols 2-3 hours,
// cols 4-5 minutes.  12 h: hours 1-12, tens column blank below 10.  24 h:
// leading zero.
Frame render_clock(const RingSet& ring, const LocalTime& lt, bool h24, bool* diag = nullptr);

Frame render_blank(const RingSet& ring);

// Boot / no-signal: WiFi glyph on the CENTRE column, blanks elsewhere
// (spec 7.1, handoff-DECIDED).
Frame render_wifi(const RingSet& ring, bool* diag = nullptr);

// The ????? state: every column to the question glyph (spec 7.3).
Frame render_qmarks(const RingSet& ring, bool* diag = nullptr);

// Countdown MMM:S0 (spec 7.3): minutes in cols 1-3 with leading zeros,
// tens-of-seconds in col 4, col 5 fixed at digit 0.  shown_s in [0, 6480].
Frame render_countdown(const RingSet& ring, int shown_s, bool* diag = nullptr);

}  // namespace swan
