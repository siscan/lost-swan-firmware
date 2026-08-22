#include "modes/render.h"

namespace swan {
namespace {

int role_or_blank(const RingSet& ring, int col, Role r, int digit, bool* diag) {
    return ring.index_for_role(col, r, digit, diag);
}

}  // namespace

Frame render_blank(const RingSet& ring) {
    Frame f;
    for (int i = 0; i < N_COLUMNS; ++i) {
        f.idx[static_cast<size_t>(i)] = ring.index_for_role(i, Role::Blank, 0, nullptr);
    }
    return f;
}

Frame render_wifi(const RingSet& ring, bool* diag) {
    Frame f = render_blank(ring);
    constexpr int CENTRE = N_COLUMNS / 2;  // col 3 of 5
    f.idx[CENTRE] = role_or_blank(ring, CENTRE, Role::Wifi, 0, diag);
    return f;
}

Frame render_qmarks(const RingSet& ring, bool* diag) {
    Frame f;
    for (int i = 0; i < N_COLUMNS; ++i) {
        f.idx[static_cast<size_t>(i)] = role_or_blank(ring, i, Role::Question, 0, diag);
    }
    return f;
}

Frame render_clock(const RingSet& ring, const LocalTime& lt, bool h24, bool* diag) {
    Frame f;
    if (h24) {
        f.idx[0] = role_or_blank(ring, 0, Role::Blank, 0, diag);
        f.idx[1] = role_or_blank(ring, 1, Role::Digit, lt.hour / 10, diag);
        f.idx[2] = role_or_blank(ring, 2, Role::Digit, lt.hour % 10, diag);
    } else {
        const bool pm = lt.hour >= 12;
        const int h12 = (lt.hour % 12 == 0) ? 12 : lt.hour % 12;
        f.idx[0] = role_or_blank(ring, 0, pm ? Role::Pm : Role::Am, 0, diag);
        f.idx[1] = h12 >= 10 ? role_or_blank(ring, 1, Role::Digit, 1, diag)
                             : role_or_blank(ring, 1, Role::Blank, 0, diag);
        f.idx[2] = role_or_blank(ring, 2, Role::Digit, h12 % 10, diag);
    }
    f.idx[3] = role_or_blank(ring, 3, Role::Digit, lt.minute / 10, diag);
    f.idx[4] = role_or_blank(ring, 4, Role::Digit, lt.minute % 10, diag);
    return f;
}

Frame render_countdown(const RingSet& ring, int shown_s, bool* diag) {
    if (shown_s < 0) shown_s = 0;
    const int minutes = shown_s / 60;
    const int tens = (shown_s % 60) / 10;

    Frame f;
    f.idx[0] = role_or_blank(ring, 0, Role::Digit, (minutes / 100) % 10, diag);
    f.idx[1] = role_or_blank(ring, 1, Role::Digit, (minutes / 10) % 10, diag);
    f.idx[2] = role_or_blank(ring, 2, Role::Digit, minutes % 10, diag);
    f.idx[3] = role_or_blank(ring, 3, Role::Digit, tens, diag);
    f.idx[4] = role_or_blank(ring, 4, Role::Digit, 0, diag);
    return f;
}

}  // namespace swan
