// GENERATED FILE - DO NOT EDIT.
// Source:     docs/ref/manifest.json
// Regenerate: python tools/gen_ring_table.py
//
// Ring frozen: 2026-08-20 — Column 5 production flaps generated from this order
// Integrity:   50/50 flap fronts match ring order
#pragma once

#include "ring/ring_category.h"

namespace swan {

inline constexpr int RING_SLOT_COUNT  = 50;
inline constexpr int RING_HOME_SLOT   = 0;
inline constexpr int RING_DIGIT_FIRST = 1;
inline constexpr int RING_DIGIT_LAST  = 10;
inline constexpr int RING_AM_SLOT     = 11;
inline constexpr int RING_PM_SLOT     = 12;
inline constexpr int RING_WIFI_SLOT   = 49;
inline constexpr int RING_QMARK_SLOT  = 33;

struct RingSlot {
    const char*  char_id;   // token accepted by the message parser
    const char*  label;     // human-readable, for the UI
    RingCategory category;
};

inline constexpr RingSlot RING_TABLE[RING_SLOT_COUNT] = {
    /*  0 */ { "blank", "blank", RingCategory::Blank },
    /*  1 */ { "0", "digit 0", RingCategory::Digit },
    /*  2 */ { "1", "digit 1", RingCategory::Digit },
    /*  3 */ { "2", "digit 2", RingCategory::Digit },
    /*  4 */ { "3", "digit 3", RingCategory::Digit },
    /*  5 */ { "4", "digit 4", RingCategory::Digit },
    /*  6 */ { "5", "digit 5", RingCategory::Digit },
    /*  7 */ { "6", "digit 6", RingCategory::Digit },
    /*  8 */ { "7", "digit 7", RingCategory::Digit },
    /*  9 */ { "8", "digit 8", RingCategory::Digit },
    /* 10 */ { "9", "digit 9", RingCategory::Digit },
    /* 11 */ { "AM", "AM", RingCategory::AmPm },
    /* 12 */ { "PM", "PM", RingCategory::AmPm },
    /* 13 */ { "staff", "hooked staff", RingCategory::Glyph },
    /* 14 */ { "spiral", "spiral coil", RingCategory::Glyph },
    /* 15 */ { "obelisk", "loop/obelisk", RingCategory::Glyph },
    /* 16 */ { "bird", "bird", RingCategory::Glyph },
    /* 17 */ { "branch", "bent branch", RingCategory::Glyph },
    /* 18 */ { "gate", "gate", RingCategory::Glyph },
    /* 19 */ { "seated", "seated figure", RingCategory::Glyph },
    /* 20 */ { "hand", "hand/arm", RingCategory::Glyph },
    /* 21 */ { "stripedisc", "striped disc", RingCategory::Glyph },
    /* 22 */ { "waves", "ocean waves", RingCategory::Glyph },
    /* 23 */ { "jellyfish", "umbrella/jellyfish", RingCategory::Glyph },
    /* 24 */ { "halfdisc", "half-disc", RingCategory::Glyph },
    /* 25 */ { "wave", "breaking wave", RingCategory::Glyph },
    /* 26 */ { "hook", "curved hook", RingCategory::Glyph },
    /* 27 */ { "fork", "forked branch", RingCategory::Glyph },
    /* 28 */ { "vloop", "vertical loop", RingCategory::Glyph },
    /* 29 */ { "arch", "arch/tunnel", RingCategory::Glyph },
    /* 30 */ { "zig", "zig", RingCategory::Glyph },
    /* 31 */ { "sun", "sun/day", RingCategory::Glyph },
    /* 32 */ { "moon", "moon/month", RingCategory::Glyph },
    /* 33 */ { "qmark", "? (unknown)", RingCategory::Glyph },
    /* 34 */ { "palm", "palm/year", RingCategory::Glyph },
    /* 35 */ { "sunrise", "sunrise/horizon", RingCategory::Glyph },
    /* 36 */ { "duat", "duat", RingCategory::Glyph },
    /* 37 */ { "eye", "eye of Horus", RingCategory::Glyph },
    /* 38 */ { "djed", "djed pillar", RingCategory::Glyph },
    /* 39 */ { "ankh", "ankh", RingCategory::Glyph },
    /* 40 */ { "armoffer", "arm/offering", RingCategory::Glyph },
    /* 41 */ { "lotus", "lotus", RingCategory::Glyph },
    /* 42 */ { "jackal", "jackal", RingCategory::Glyph },
    /* 43 */ { "boundloop", "bound loop", RingCategory::Glyph },
    /* 44 */ { "serpent", "serpent", RingCategory::Glyph },
    /* 45 */ { "scarab", "scarab", RingCategory::Glyph },
    /* 46 */ { "boat", "boat", RingCategory::Glyph },
    /* 47 */ { "hourglass", "hourglass", RingCategory::Glyph },
    /* 48 */ { "cycle", "circle/cycle", RingCategory::Glyph },
    /* 49 */ { "wifi", "wifi glyph (boot/no-signal state)", RingCategory::Wifi },
};

}  // namespace swan
