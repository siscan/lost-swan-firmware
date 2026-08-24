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

#include <string_view>

#include "frame/frame.h"
#include "ring/ring_runtime.h"
#include "timesvc/tz.h"

namespace swan {

// Countdown seconds resolution (spec 7.3, countdown.seconds_mode).
//
// Every mode runs MMM:00 for the bulk of the run - the seconds columns are
// FROZEN on 00 and cost nothing - and only comes alive in the last
// `seconds_live_s` (default 240, which is also when the 4-minute cue fires):
//
//   Minutes - never shows seconds.  MMM:00 for the whole run.
//   Tens    - MMM:00, then MMM:S0 inside the live window.
//   Seconds - MMM:00, then MMM:SS inside the live window.  Default, and what
//             the show does.
//
// The mode selects nothing but the display STEP; the renderer draws whatever
// value it is handed.  A step of 60 puts zeros in both seconds columns by
// construction, which is why the quiet phase provably cannot move them.
enum class SecondsMode : unsigned char { Minutes, Tens, Seconds };

const char* seconds_mode_name(SecondsMode m);
bool seconds_mode_from_name(std::string_view s, SecondsMode& out);

// Seconds per display window at this point in the run (spec 7.3).  60 during
// the quiet phase; 10 or 1 once `remaining_s <= live_s`, per the mode.
int countdown_step_s(SecondsMode mode, int remaining_s, int live_s);

// THE RENDERING CONTRACT (spec 7.3): displayed = ceil(remaining / step) * step.
//
// CEILING, corrected 2026-08-24.  The show holds 108:00 until a full minute has
// elapsed, so a value owns the window ABOVE it: 107:55 remaining still reads
// 108:00, and 000:00 lands exactly at remaining = 0, with the klaxon rather
// than a second ahead of it.
//
// This is a CROSS-REPO contract.  The presentation readout and the separate
// terminal prop derive their displays from the same deadline and must use the
// same formula, or two screens showing the same countdown disagree by a whole
// step.
int countdown_shown_s(SecondsMode mode, int remaining_s, int live_s);

// The next distinct display value below `shown` - what the land-on-tick
// scheduler must pre-render, and the value that lands when remaining reaches
// it.  Crossing the freeze boundary is just an ordinary step: 300 -> 240
// (quiet), then 240 -> 239 (live).
int countdown_next_shown_s(SecondsMode mode, int shown_s, int live_s);

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

// Countdown (spec 7.3): MMM:SS of the value it is given.  Pass a value from
// countdown_shown_s() - the resolution lives entirely in that.
Frame render_countdown(const RingSet& ring, int shown_s, const Frame& from,
                       bool* diag = nullptr);

}  // namespace swan
