#include "modes/render.h"

namespace swan {
namespace {

// Resolve one column, searching forward from where that column is now.
int at(const RingSet& ring, const Frame& from, int col, Role r, int digit, bool* diag) {
    return ring.index_for_role(col, r, digit, from.idx[static_cast<size_t>(col)], diag);
}

}  // namespace

Frame render_blank(const RingSet& ring, const Frame& from) {
    Frame f;
    for (int i = 0; i < N_COLUMNS; ++i) {
        f.idx[static_cast<size_t>(i)] = at(ring, from, i, Role::Blank, 0, nullptr);
    }
    return f;
}

Frame render_wifi(const RingSet& ring, const Frame& from, bool* diag) {
    Frame f = render_blank(ring, from);
    f.idx[RingSet::WIFI_COLUMN] = at(ring, from, RingSet::WIFI_COLUMN, Role::Wifi, 0, diag);
    return f;
}

Frame render_qmarks(const RingSet& ring, const Frame& from, bool* diag) {
    Frame f;
    for (int i = 0; i < N_COLUMNS; ++i) {
        f.idx[static_cast<size_t>(i)] = at(ring, from, i, Role::Question, 0, diag);
    }
    return f;
}

Frame render_clock(const RingSet& ring, const LocalTime& lt, bool h24, const Frame& from,
                   bool* diag) {
    Frame f;
    if (h24) {
        f.idx[0] = at(ring, from, 0, Role::Blank, 0, diag);
        f.idx[1] = at(ring, from, 1, Role::Digit, lt.hour / 10, diag);
        f.idx[2] = at(ring, from, 2, Role::Digit, lt.hour % 10, diag);
    } else {
        const bool pm = lt.hour >= 12;
        const int h12 = (lt.hour % 12 == 0) ? 12 : lt.hour % 12;
        f.idx[0] = at(ring, from, 0, pm ? Role::Pm : Role::Am, 0, diag);
        f.idx[1] = h12 >= 10 ? at(ring, from, 1, Role::Digit, 1, diag)
                             : at(ring, from, 1, Role::Blank, 0, diag);
        f.idx[2] = at(ring, from, 2, Role::Digit, h12 % 10, diag);
    }
    f.idx[3] = at(ring, from, 3, Role::Digit, lt.minute / 10, diag);
    f.idx[4] = at(ring, from, 4, Role::Digit, lt.minute % 10, diag);
    return f;
}

const char* seconds_mode_name(SecondsMode m) {
    switch (m) {
        case SecondsMode::Minutes: return "minutes";
        case SecondsMode::Tens:    return "tens";
        case SecondsMode::Seconds: return "seconds";
    }
    return "?";
}

bool seconds_mode_from_name(std::string_view s, SecondsMode& out) {
    if (s == "minutes") out = SecondsMode::Minutes;
    else if (s == "tens") out = SecondsMode::Tens;
    else if (s == "seconds") out = SecondsMode::Seconds;
    else return false;
    return true;
}

int countdown_step_s(SecondsMode mode, int remaining_s, int live_s) {
    if (mode == SecondsMode::Minutes) return 60;
    // Floored to a whole minute, and NOT merely as tidiness: with live_s = 250
    // the quiet phase shows floor(251/60)*60 = 240 and the first live value is
    // 250, so the display would count UP - paid for by a 45-flip borrow on
    // column 4 and a 16-flip wrap on column 5, a visible wrong-direction whirl
    // in the middle of the four-minute warning.  Flooring here makes the value
    // monotonic for ANY configured live_s; the API rejects non-multiples too,
    // so a bad setting is refused rather than silently altered, but a stale
    // NVS value still cannot break the invariant.
    if (remaining_s > (live_s / 60) * 60) return 60;
    return mode == SecondsMode::Seconds ? 1 : 10;
}

int countdown_shown_s(SecondsMode mode, int remaining_s, int live_s) {
    if (remaining_s < 0) remaining_s = 0;
    const int step = countdown_step_s(mode, remaining_s, live_s);
    // CEILING, not floor (spec 7.3, corrected 2026-08-24).  The show holds
    // 108:00 for a full minute after the Numbers are entered - 107:55 remaining
    // still reads 108:00 - so a value is displayed for the whole window ABOVE
    // it, not below.  Two things fall out of that and both were wrong before:
    // the first minute of a run showed 107:00 half a second in, and 000:00
    // arrived a full second before the deadline, ahead of its own klaxon.
    return ((remaining_s + step - 1) / step) * step;
}

int countdown_next_shown_s(SecondsMode mode, int shown_s, int live_s) {
    if (shown_s <= 0) return 0;
    // Under ceiling, `shown` is displayed while remaining is in
    // (shown - step, shown], so the next distinct value appears when remaining
    // reaches shown - step.  It cannot be derived from `shown - 1` any more:
    // one second below 300 still ceilings to 300.  Taking the step AT the top
    // of the window keeps the freeze boundary an ordinary step - 300 -> 240
    // while quiet, then 240 -> 239 once seconds are live.
    const int step = countdown_step_s(mode, shown_s, live_s);
    return countdown_shown_s(mode, shown_s - step, live_s);
}

Frame render_countdown(const RingSet& ring, int shown_s, const Frame& from, bool* diag) {
    if (shown_s < 0) shown_s = 0;
    const int minutes = shown_s / 60;
    const int secs = shown_s % 60;

    Frame f;
    f.idx[0] = at(ring, from, 0, Role::Digit, (minutes / 100) % 10, diag);
    f.idx[1] = at(ring, from, 1, Role::Digit, (minutes / 10) % 10, diag);
    f.idx[2] = at(ring, from, 2, Role::Digit, minutes % 10, diag);
    // A quiet-phase value is a whole minute, so both of these resolve to
    // digit 0 - and a column already showing 0 is zero flips away from it.
    // That is what freezes columns 4 and 5, by construction rather than by a
    // special case.
    f.idx[3] = at(ring, from, 3, Role::Digit, secs / 10, diag);
    f.idx[4] = at(ring, from, 4, Role::Digit, secs % 10, diag);
    return f;
}

}  // namespace swan
