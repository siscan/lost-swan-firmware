// The two shipped rings (spec 4): descending order, role coverage, column 5's
// double digit block, and the forward-only slot arithmetic every move rests on.
#include <cstring>
#include <set>

#include "check.h"
#include "ring/ring_runtime.h"

using namespace swan;

namespace {

const RingSet& rings() {
    static const RingSet s = RingSet::compiled_fallback();
    return s;
}

// --------------------------------------------------------------------------
// Slot arithmetic.  Direction-agnostic, and never negative - which is what
// guarantees no move can ask for a reverse DIR (spec 5.3).
// --------------------------------------------------------------------------
void test_forward_distance() {
    CHECK_EQ(ring_forward_distance(0, 0), 0);
    CHECK_EQ(ring_forward_distance(0, 1), 1);
    CHECK_EQ(ring_forward_distance(49, 0), 1);  // wraps through home

    for (int a = 0; a < RING_SLOT_COUNT; ++a) {
        for (int b = 0; b < RING_SLOT_COUNT; ++b) {
            const int d = ring_forward_distance(a, b);
            CHECK(d >= 0 && d < RING_SLOT_COUNT);  // never reverse, never > a rev
            if (a == b) {
                CHECK_EQ(d, 0);
            } else {
                CHECK_EQ(d + ring_forward_distance(b, a), RING_SLOT_COUNT);
            }
        }
    }
}

// --------------------------------------------------------------------------
// Both rings descend: one forward flip decrements the digit.  This is what
// makes a countdown tick 1 flip, and it is the invariant the whole redesign
// rests on - assert it on every column, both compiled tables.
// --------------------------------------------------------------------------
void test_all_columns_descending() {
    for (int c = 0; c < N_COLUMNS; ++c) {
        const RingTable& t = rings().col(c);
        CHECK_EQ(t.slot_count(), RING_SLOT_COUNT);
        if (!t.is_descending()) {
            CHECK(false);
            std::printf("  column %d is not descending\n", c + 1);
            continue;
        }
        // Spelled out, not just via the helper: from EVERY slot carrying digit
        // d, the nearest d-1 going forward is exactly the next slot.
        for (int d = 1; d <= 9; ++d) {
            for (const int s : t.slots_for_role(Role::Digit, d)) {
                CHECK_EQ(t.index_for_role(Role::Digit, d - 1, s), (s + 1) % RING_SLOT_COUNT);
                CHECK_EQ(ring_forward_distance(s, t.index_for_role(Role::Digit, d - 1, s)), 1);
            }
        }
    }
}

// --------------------------------------------------------------------------
// Ring A (cols 1-4): one digit block, AM/PM, wifi, 36 glyphs.
// Ring B (col 5):    two digit blocks, no AM/PM, no wifi, 29 glyphs.
// --------------------------------------------------------------------------
void test_ring_shapes() {
    const RingTable& a = rings().col(0);
    const RingTable& b = rings().col(4);

    // Cols 1-4 share one table; col 5 differs.
    for (int c = 0; c < 4; ++c) {
        CHECK_EQ(rings().col(c).index_for_role(Role::Wifi), a.index_for_role(Role::Wifi));
    }

    // Ring A: exactly one slot per digit; slot = 49 - digit.
    for (int d = 0; d <= 9; ++d) {
        CHECK_EQ(a.slots_for_role(Role::Digit, d).size(), 1u);
        CHECK_EQ(a.slots_for_role(Role::Digit, d)[0], 49 - d);
    }
    CHECK(a.index_for_role(Role::Am) >= 0);
    CHECK(a.index_for_role(Role::Pm) >= 0);
    CHECK(a.index_for_role(Role::Wifi) >= 0);
    CHECK(a.index_for_role(Role::Question) >= 0);
    CHECK_EQ(a.index_for_role(Role::Blank), RING_HOME_SLOT);

    // Ring B: two slots per digit, 25 apart; no AM/PM, no wifi; '?' survives.
    for (int d = 0; d <= 9; ++d) {
        const auto& s = b.slots_for_role(Role::Digit, d);
        CHECK_EQ(s.size(), 2u);
        if (s.size() == 2) {
            CHECK_EQ(s[0], 24 - d);
            CHECK_EQ(s[1], 49 - d);
            CHECK_EQ(s[1] - s[0], 25);
        }
    }
    CHECK_EQ(b.index_for_role(Role::Am), -1);
    CHECK_EQ(b.index_for_role(Role::Pm), -1);
    CHECK_EQ(b.index_for_role(Role::Wifi), -1);
    CHECK(b.index_for_role(Role::Question) >= 0);
    CHECK_EQ(b.index_for_role(Role::Blank), RING_HOME_SLOT);

    int glyphs_a = 0, glyphs_b = 0;
    for (int i = 0; i < RING_SLOT_COUNT; ++i) {
        if (a.slot(i).cat == RingCategory::Glyph) ++glyphs_a;
        if (b.slot(i).cat == RingCategory::Glyph) ++glyphs_b;
    }
    CHECK_EQ(glyphs_a, 36);
    CHECK_EQ(glyphs_b, 29);
}

// --------------------------------------------------------------------------
// The nearest-forward rule, including the wrap.  Column 5's physical slot is
// genuinely not a function of the displayed digit - that is by design and
// documented in BRINGUP.md so it is not diagnosed as a fault.
// --------------------------------------------------------------------------
void test_nearest_forward_selection() {
    const RingTable& b = rings().col(4);

    // From either 0, the next 9 is 16 flips - the whole point of the second
    // block (41 flips on a single descending ring).
    for (const int from0 : {24, 49}) {
        const int to9 = b.index_for_role(Role::Digit, 9, from0);
        CHECK_EQ(ring_forward_distance(from0, to9), 16);
    }
    // ...and the single-block ring pays the full 41 for the same wrap.
    const RingTable& a = rings().col(0);
    CHECK_EQ(ring_forward_distance(49, a.index_for_role(Role::Digit, 9, 49)), 41);

    // Every (from, digit) pair picks the cheaper of the two blocks, and the
    // cost never exceeds the 25-slot block spacing.
    for (int from = 0; from < RING_SLOT_COUNT; ++from) {
        for (int d = 0; d <= 9; ++d) {
            const int got = b.index_for_role(Role::Digit, d, from);
            const auto& cands = b.slots_for_role(Role::Digit, d);
            int best = RING_SLOT_COUNT;
            for (const int c : cands) best = std::min(best, ring_forward_distance(from, c));
            CHECK_EQ(ring_forward_distance(from, got), best);
            CHECK(best <= 25);
        }
    }

    // A clock increment on ring B costs 24, not 49 - the second block helps
    // the expensive direction too (spec 7.1 wear table).
    for (int d = 0; d <= 8; ++d) {
        const int from = b.slots_for_role(Role::Digit, d)[0];
        CHECK_EQ(ring_forward_distance(from, b.index_for_role(Role::Digit, d + 1, from)), 24);
    }

    // FROM_ANY is deterministic: the lowest matching slot.
    CHECK_EQ(b.index_for_role(Role::Digit, 0, FROM_ANY), 24);
    CHECK_EQ(a.index_for_role(Role::Digit, 0, FROM_ANY), 49);
}

// --------------------------------------------------------------------------
// Tokens resolve nearest-forward too - a char_id is not unique on ring B.
// --------------------------------------------------------------------------
void test_tokens() {
    const RingTable& a = rings().col(0);
    const RingTable& b = rings().col(4);

    CHECK_EQ(a.index_for_token("_"), RING_HOME_SLOT);
    CHECK_EQ(a.index_for_token("blank"), RING_HOME_SLOT);
    CHECK_EQ(a.index_for_token("#33"), 33);
    CHECK_EQ(a.index_for_token("AM"), a.index_for_role(Role::Am));
    CHECK_EQ(a.index_for_token("am"), a.index_for_role(Role::Am));  // case-insensitive
    CHECK_EQ(a.index_for_token("#50"), -1);
    CHECK_EQ(a.index_for_token("nosuchglyph"), -1);
    CHECK_EQ(a.index_for_token(""), -1);

    // "5" appears twice on ring B; the nearest forward wins, and a raw #n is
    // always exact.
    CHECK_EQ(b.index_for_token("5", 0), 19);
    CHECK_EQ(b.index_for_token("5", 20), 44);
    CHECK_EQ(b.index_for_token("#44", 0), 44);

    // Every slot is reachable by its own char_id from that slot.
    for (int c = 0; c < N_COLUMNS; ++c) {
        const RingTable& t = rings().col(c);
        for (int i = 0; i < RING_SLOT_COUNT; ++i) {
            CHECK_EQ(t.index_for_token(t.slot(i).id, i), i);
        }
    }

    // Glyph names are unique within each ring, so a name is unambiguous.
    for (int c : {0, 4}) {
        const RingTable& t = rings().col(c);
        std::set<std::string> ids;
        int dupes = 0;
        for (int i = 0; i < RING_SLOT_COUNT; ++i) {
            if (t.slot(i).cat == RingCategory::Glyph && !ids.insert(t.slot(i).id).second) {
                ++dupes;
            }
        }
        CHECK_EQ(dupes, 0);
    }
}

// --------------------------------------------------------------------------
// Role coverage is validated at LOAD time, so a column can never be asked at
// render time for something its ring does not carry (spec 4).
// --------------------------------------------------------------------------
void test_role_validation() {
    std::string err;
    CHECK(rings().validate_roles(&err));
    CHECK(err.empty());

    // Column 5 has no AM/PM or wifi - and is never asked for them, so the
    // shipped set validates.  The columns that ARE asked do carry them.
    CHECK(rings().col(RingSet::AMPM_COLUMN).index_for_role(Role::Am) >= 0);
    CHECK(rings().col(RingSet::WIFI_COLUMN).index_for_role(Role::Wifi) >= 0);
    CHECK_EQ(RingSet::WIFI_COLUMN, 2);  // the centre column (spec 7.1)
    CHECK_EQ(RingSet::AMPM_COLUMN, 0);
}

}  // namespace

void run_tests() {
    test_forward_distance();
    test_all_columns_descending();
    test_ring_shapes();
    test_nearest_forward_selection();
    test_tokens();
    test_role_validation();
}
