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
    if (remaining_s > live_s) return 60;
    return mode == SecondsMode::Seconds ? 1 : 10;
}

int countdown_shown_s(SecondsMode mode, int remaining_s, int live_s) {
    if (remaining_s < 0) remaining_s = 0;
    const int step = countdown_step_s(mode, remaining_s, live_s);
    return (remaining_s / step) * step;
}

int countdown_next_shown_s(SecondsMode mode, int shown_s, int live_s) {
    if (shown_s <= 0) return 0;
    // The next value is whatever one tick below this window floors to.  That
    // makes the freeze boundary fall out of the same rule as every other
    // step: at shown = 300 the next value is 240, and at 240 it is 239.
    return countdown_shown_s(mode, shown_s - 1, live_s);
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
