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

Frame render_countdown(const RingSet& ring, int remaining_s, SecondsMode mode,
                       const Frame& from, bool* diag) {
    if (remaining_s < 0) remaining_s = 0;
    const int minutes = remaining_s / 60;
    const int secs = remaining_s % 60;

    Frame f;
    f.idx[0] = at(ring, from, 0, Role::Digit, (minutes / 100) % 10, diag);
    f.idx[1] = at(ring, from, 1, Role::Digit, (minutes / 10) % 10, diag);
    f.idx[2] = at(ring, from, 2, Role::Digit, minutes % 10, diag);
    f.idx[3] = at(ring, from, 3, Role::Digit, secs / 10, diag);
    // MMM:S0 parks the ones column on 0; MMM:SS runs it live.
    f.idx[4] = at(ring, from, 4, Role::Digit,
                  mode == SecondsMode::Seconds ? secs % 10 : 0, diag);
    return f;
}

}  // namespace swan
