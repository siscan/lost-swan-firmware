// Ring index arithmetic.  Pure: no IDF includes (CLAUDE.md conventions).
#pragma once

#include <cstdint>

#include "ring/geometry.h"
#include "ring/ring_table.h"

namespace swan {

inline constexpr int RING_INVALID = -1;

static_assert(RING_SLOT_COUNT == N_RING,
              "generated ring table and geometry.h disagree on the ring size");

constexpr bool ring_index_valid(int i) { return i >= 0 && i < RING_SLOT_COUNT; }

// Every move costs (to - from) mod 50 flips.  Rotation is one direction only;
// there is no shorter way round (spec 4, CLAUDE.md hard constraints).
constexpr int ring_forward_distance(int from, int to) {
    return ((to - from) % RING_SLOT_COUNT + RING_SLOT_COUNT) % RING_SLOT_COUNT;
}

// Digits occupy a contiguous block, so a clock tick is one flip (handoff 2).
constexpr int ring_index_for_digit(int d) {
    return (d >= 0 && d <= 9) ? RING_DIGIT_FIRST + d : RING_INVALID;
}

constexpr int ring_digit_for_index(int i) {
    return (i >= RING_DIGIT_FIRST && i <= RING_DIGIT_LAST) ? i - RING_DIGIT_FIRST : RING_INVALID;
}

// Message tokens: a char_id from the manifest, "_" for blank, or "#n" for a raw
// index (spec 4).  Case-insensitive so "am" and "AM" both work.
int ring_index_for_token(const char* tok);

const char* ring_char_id(int i);
const char* ring_label(int i);
RingCategory ring_category(int i);

}  // namespace swan
