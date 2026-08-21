// Ring arithmetic and the generated ring table.  Spec 4.
#include <cstring>

#include "check.h"
#include "ring/ring.h"

using namespace swan;

namespace {

void test_table_shape() {
    CHECK_EQ(RING_SLOT_COUNT, 50);
    CHECK_EQ(RING_HOME_SLOT, 0);
    CHECK_EQ(RING_DIGIT_FIRST, 1);
    CHECK_EQ(RING_DIGIT_LAST, 10);
    CHECK_EQ(RING_AM_SLOT, 11);
    CHECK_EQ(RING_PM_SLOT, 12);
    CHECK_EQ(RING_WIFI_SLOT, 49);
    CHECK_EQ(RING_QMARK_SLOT, 33);  // the ????? state (handoff 2)

    CHECK(RING_TABLE[0].category == RingCategory::Blank);
    CHECK(RING_TABLE[RING_WIFI_SLOT].category == RingCategory::Wifi);
    CHECK(RING_TABLE[RING_AM_SLOT].category == RingCategory::AmPm);
    CHECK(RING_TABLE[RING_PM_SLOT].category == RingCategory::AmPm);
    CHECK_STREQ(RING_TABLE[RING_QMARK_SLOT].char_id, "qmark");

    // Digits are contiguous and in order, which is what makes a clock tick one
    // flip (handoff 2).  If the manifest ever reorders them, this fires.
    for (int d = 0; d <= 9; ++d) {
        const int i = RING_DIGIT_FIRST + d;
        CHECK(RING_TABLE[i].category == RingCategory::Digit);
        const char expect[2] = {static_cast<char>('0' + d), '\0'};
        CHECK_STREQ(RING_TABLE[i].char_id, expect);
    }

    // Everything between AM/PM and the wifi glyph is a hieroglyph: 36 of them.
    int glyphs = 0;
    for (int i = 0; i < RING_SLOT_COUNT; ++i) {
        if (RING_TABLE[i].category == RingCategory::Glyph) ++glyphs;
    }
    CHECK_EQ(glyphs, 36);

    // char_ids must be unique or token lookup is ambiguous.
    for (int i = 0; i < RING_SLOT_COUNT; ++i) {
        for (int j = i + 1; j < RING_SLOT_COUNT; ++j) {
            CHECK(std::strcmp(RING_TABLE[i].char_id, RING_TABLE[j].char_id) != 0);
        }
    }
}

void test_forward_distance() {
    CHECK_EQ(ring_forward_distance(0, 0), 0);
    CHECK_EQ(ring_forward_distance(0, 1), 1);
    CHECK_EQ(ring_forward_distance(49, 0), 1);  // wraps through home

    // The wrap-cost table in spec 3, recomputed from the ring rather than copied.
    const int d9 = ring_index_for_digit(9);
    const int d0 = ring_index_for_digit(0);
    const int d5 = ring_index_for_digit(5);
    const int d4 = ring_index_for_digit(4);
    CHECK_EQ(ring_forward_distance(d9, d0), 41);   // clock minute-units 9 -> 0
    CHECK_EQ(ring_forward_distance(d5, d0), 45);   // clock minute-tens 5 -> 0
    CHECK_EQ(ring_forward_distance(d5, d4), 49);   // countdown decrement
    CHECK_EQ(ring_forward_distance(0, RING_WIFI_SLOT), 49);  // blank -> wifi glyph

    // Ascending ring: cost up + cost down always sums to a full revolution.
    for (int a = 0; a < RING_SLOT_COUNT; ++a) {
        for (int b = 0; b < RING_SLOT_COUNT; ++b) {
            if (a == b) {
                CHECK_EQ(ring_forward_distance(a, b), 0);
            } else {
                CHECK_EQ(ring_forward_distance(a, b) + ring_forward_distance(b, a),
                         RING_SLOT_COUNT);
            }
        }
    }

    // One flip advances the displayed slot by exactly +1 (manifest constant).
    for (int i = 0; i < RING_SLOT_COUNT; ++i) {
        CHECK_EQ(ring_forward_distance(i, (i + 1) % RING_SLOT_COUNT), 1);
    }
}

void test_digit_mapping() {
    for (int d = 0; d <= 9; ++d) {
        const int i = ring_index_for_digit(d);
        CHECK_EQ(i, d + 1);
        CHECK_EQ(ring_digit_for_index(i), d);
        // Counting up one digit is one flip; counting down is 49.
        if (d < 9) CHECK_EQ(ring_forward_distance(i, ring_index_for_digit(d + 1)), 1);
        if (d > 0) CHECK_EQ(ring_forward_distance(i, ring_index_for_digit(d - 1)), 49);
    }
    CHECK_EQ(ring_index_for_digit(-1), RING_INVALID);
    CHECK_EQ(ring_index_for_digit(10), RING_INVALID);
    CHECK_EQ(ring_digit_for_index(0), RING_INVALID);   // blank is not a digit
    CHECK_EQ(ring_digit_for_index(11), RING_INVALID);  // AM is not a digit
}

void test_tokens() {
    CHECK_EQ(ring_index_for_token("_"), RING_HOME_SLOT);
    CHECK_EQ(ring_index_for_token("blank"), 0);
    CHECK_EQ(ring_index_for_token("0"), 1);   // digit 0 lives at slot 1
    CHECK_EQ(ring_index_for_token("9"), 10);
    CHECK_EQ(ring_index_for_token("AM"), RING_AM_SLOT);
    CHECK_EQ(ring_index_for_token("am"), RING_AM_SLOT);  // case-insensitive
    CHECK_EQ(ring_index_for_token("pm"), RING_PM_SLOT);
    CHECK_EQ(ring_index_for_token("ankh"), 39);
    CHECK_EQ(ring_index_for_token("wifi"), RING_WIFI_SLOT);
    CHECK_EQ(ring_index_for_token("qmark"), RING_QMARK_SLOT);

    CHECK_EQ(ring_index_for_token("#0"), 0);
    CHECK_EQ(ring_index_for_token("#33"), 33);
    CHECK_EQ(ring_index_for_token("#49"), 49);

    CHECK_EQ(ring_index_for_token("#50"), RING_INVALID);  // off the ring
    CHECK_EQ(ring_index_for_token("#-1"), RING_INVALID);
    CHECK_EQ(ring_index_for_token("#12x"), RING_INVALID);
    CHECK_EQ(ring_index_for_token("#"), RING_INVALID);
    CHECK_EQ(ring_index_for_token("nosuchglyph"), RING_INVALID);
    CHECK_EQ(ring_index_for_token(""), RING_INVALID);
    CHECK_EQ(ring_index_for_token(nullptr), RING_INVALID);

    // Every slot is reachable by its own char_id.
    for (int i = 0; i < RING_SLOT_COUNT; ++i) {
        CHECK_EQ(ring_index_for_token(RING_TABLE[i].char_id), i);
    }
}

}  // namespace

void run_tests() {
    test_table_shape();
    test_forward_distance();
    test_digit_mapping();
    test_tokens();
}
