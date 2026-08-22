// Ring index arithmetic.  Pure: no IDF includes (CLAUDE.md conventions).
//
// Slot-space only.  The character-level helpers that used to live here
// (index_for_digit, index_for_token, ...) are gone: they assumed ONE ascending
// digit block, and neither is true now - both rings are descending and column
// 5 carries two digit blocks.  Look characters up through RingTable/RingSet,
// which take the column's current slot and search forward (ring_runtime.h).
#pragma once

#include <cstdint>

#include "ring/geometry.h"
#include "ring/ring_table.h"

namespace swan {

inline constexpr int RING_INVALID = -1;

static_assert(RING_SLOT_COUNT == N_RING,
              "generated ring table and geometry.h disagree on the ring size");

constexpr bool ring_index_valid(int i) { return i >= 0 && i < RING_SLOT_COUNT; }

// Every move costs (to - from) mod 50 flips.  Rotation is one direction only,
// so there is no shorter way round and the result is never negative - which is
// what guarantees no move can ever ask for a reverse DIR (spec 4, spec 5.3,
// CLAUDE.md hard constraints).  Direction-agnostic: this is slot arithmetic,
// unchanged by the ring running descending.
constexpr int ring_forward_distance(int from, int to) {
    return ((to - from) % RING_SLOT_COUNT + RING_SLOT_COUNT) % RING_SLOT_COUNT;
}

}  // namespace swan
