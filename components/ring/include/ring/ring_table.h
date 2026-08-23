// GENERATED FILE - DO NOT EDIT.
// Source:     docs/ref/manifest_cols1234.json, docs/ref/manifest_col5.json
// Regenerate: python tools/ringgen.py
//
// Ring version: v3 — descending  (frozen 2026-08-22)
// Direction:    descending - one forward flip DECREMENTS the digit,
//               so a countdown tick is 1 flip and a clock tick is the
//               expensive direction (spec 4, spec 7.1 wear table).
//
// This is the compiled FALLBACK.  The live table is data/ring.json in
// LittleFS; no code may reference a slot index directly - look roles up
// through RingTable/RingSet, which handle column 5's two digit blocks.
#pragma once

#include "ring/ring_category.h"

namespace swan {

inline constexpr int RING_SLOT_COUNT   = 50;
inline constexpr int RING_HOME_SLOT    = 0;
inline constexpr int RING_COLUMN_COUNT = 5;

struct RingSlot {
    const char*  char_id;   // token accepted by the message parser
    const char*  label;     // human-readable, for the UI
    RingCategory category;
};

// Ring A - columns 1-4.  columns 1, 2, 3, 4
inline constexpr RingSlot RING_TABLE_A[RING_SLOT_COUNT] = {
    /*  0 */ { "blank", "blank", RingCategory::Blank },
    /*  1 */ { "wifi", "wifi glyph (boot/no-signal state)", RingCategory::Wifi },
    /*  2 */ { "cycle", "circle/cycle", RingCategory::Glyph },
    /*  3 */ { "hourglass", "hourglass", RingCategory::Glyph },
    /*  4 */ { "boat", "boat", RingCategory::Glyph },
    /*  5 */ { "scarab", "scarab", RingCategory::Glyph },
    /*  6 */ { "serpent", "serpent", RingCategory::Glyph },
    /*  7 */ { "boundloop", "bound loop", RingCategory::Glyph },
    /*  8 */ { "jackal", "jackal", RingCategory::Glyph },
    /*  9 */ { "lotus", "lotus", RingCategory::Glyph },
    /* 10 */ { "armoffer", "arm/offering", RingCategory::Glyph },
    /* 11 */ { "ankh", "ankh", RingCategory::Glyph },
    /* 12 */ { "djed", "djed pillar", RingCategory::Glyph },
    /* 13 */ { "eye", "eye of Horus", RingCategory::Glyph },
    /* 14 */ { "duat", "duat", RingCategory::Glyph },
    /* 15 */ { "sunrise", "sunrise/horizon", RingCategory::Glyph },
    /* 16 */ { "palm", "palm/year", RingCategory::Glyph },
    /* 17 */ { "qmark", "? (unknown)", RingCategory::Glyph },
    /* 18 */ { "moon", "moon/month", RingCategory::Glyph },
    /* 19 */ { "sun", "sun/day", RingCategory::Glyph },
    /* 20 */ { "zig", "zig", RingCategory::Glyph },
    /* 21 */ { "arch", "arch/tunnel", RingCategory::Glyph },
    /* 22 */ { "vloop", "vertical loop", RingCategory::Glyph },
    /* 23 */ { "fork", "forked branch", RingCategory::Glyph },
    /* 24 */ { "hook", "curved hook", RingCategory::Glyph },
    /* 25 */ { "wave", "breaking wave", RingCategory::Glyph },
    /* 26 */ { "halfdisc", "half-disc", RingCategory::Glyph },
    /* 27 */ { "jellyfish", "umbrella/jellyfish", RingCategory::Glyph },
    /* 28 */ { "waves", "ocean waves", RingCategory::Glyph },
    /* 29 */ { "stripedisc", "striped disc", RingCategory::Glyph },
    /* 30 */ { "hand", "hand/arm", RingCategory::Glyph },
    /* 31 */ { "seated", "seated figure", RingCategory::Glyph },
    /* 32 */ { "gate", "gate", RingCategory::Glyph },
    /* 33 */ { "branch", "bent branch", RingCategory::Glyph },
    /* 34 */ { "bird", "bird", RingCategory::Glyph },
    /* 35 */ { "obelisk", "loop/obelisk", RingCategory::Glyph },
    /* 36 */ { "spiral", "spiral coil", RingCategory::Glyph },
    /* 37 */ { "staff", "hooked staff", RingCategory::Glyph },
    /* 38 */ { "PM", "PM", RingCategory::AmPm },
    /* 39 */ { "AM", "AM", RingCategory::AmPm },
    /* 40 */ { "9", "digit 9", RingCategory::Digit },
    /* 41 */ { "8", "digit 8", RingCategory::Digit },
    /* 42 */ { "7", "digit 7", RingCategory::Digit },
    /* 43 */ { "6", "digit 6", RingCategory::Digit },
    /* 44 */ { "5", "digit 5", RingCategory::Digit },
    /* 45 */ { "4", "digit 4", RingCategory::Digit },
    /* 46 */ { "3", "digit 3", RingCategory::Digit },
    /* 47 */ { "2", "digit 2", RingCategory::Digit },
    /* 48 */ { "1", "digit 1", RingCategory::Digit },
    /* 49 */ { "0", "digit 0", RingCategory::Digit },
};

// Ring B - column 5.  column 5 only — distinct part number
inline constexpr RingSlot RING_TABLE_B[RING_SLOT_COUNT] = {
    /*  0 */ { "blank", "blank", RingCategory::Blank },
    /*  1 */ { "hourglass", "hourglass", RingCategory::Glyph },
    /*  2 */ { "boat", "boat", RingCategory::Glyph },
    /*  3 */ { "scarab", "scarab", RingCategory::Glyph },
    /*  4 */ { "serpent", "serpent", RingCategory::Glyph },
    /*  5 */ { "boundloop", "bound loop", RingCategory::Glyph },
    /*  6 */ { "jackal", "jackal", RingCategory::Glyph },
    /*  7 */ { "lotus", "lotus", RingCategory::Glyph },
    /*  8 */ { "armoffer", "arm/offering", RingCategory::Glyph },
    /*  9 */ { "ankh", "ankh", RingCategory::Glyph },
    /* 10 */ { "djed", "djed pillar", RingCategory::Glyph },
    /* 11 */ { "eye", "eye of Horus", RingCategory::Glyph },
    /* 12 */ { "duat", "duat", RingCategory::Glyph },
    /* 13 */ { "sunrise", "sunrise/horizon", RingCategory::Glyph },
    /* 14 */ { "palm", "palm/year", RingCategory::Glyph },
    /* 15 */ { "9", "digit 9", RingCategory::Digit },
    /* 16 */ { "8", "digit 8", RingCategory::Digit },
    /* 17 */ { "7", "digit 7", RingCategory::Digit },
    /* 18 */ { "6", "digit 6", RingCategory::Digit },
    /* 19 */ { "5", "digit 5", RingCategory::Digit },
    /* 20 */ { "4", "digit 4", RingCategory::Digit },
    /* 21 */ { "3", "digit 3", RingCategory::Digit },
    /* 22 */ { "2", "digit 2", RingCategory::Digit },
    /* 23 */ { "1", "digit 1", RingCategory::Digit },
    /* 24 */ { "0", "digit 0", RingCategory::Digit },
    /* 25 */ { "qmark", "? (unknown)", RingCategory::Glyph },
    /* 26 */ { "moon", "moon/month", RingCategory::Glyph },
    /* 27 */ { "sun", "sun/day", RingCategory::Glyph },
    /* 28 */ { "wave", "breaking wave", RingCategory::Glyph },
    /* 29 */ { "jellyfish", "umbrella/jellyfish", RingCategory::Glyph },
    /* 30 */ { "waves", "ocean waves", RingCategory::Glyph },
    /* 31 */ { "stripedisc", "striped disc", RingCategory::Glyph },
    /* 32 */ { "hand", "hand/arm", RingCategory::Glyph },
    /* 33 */ { "seated", "seated figure", RingCategory::Glyph },
    /* 34 */ { "gate", "gate", RingCategory::Glyph },
    /* 35 */ { "branch", "bent branch", RingCategory::Glyph },
    /* 36 */ { "bird", "bird", RingCategory::Glyph },
    /* 37 */ { "obelisk", "loop/obelisk", RingCategory::Glyph },
    /* 38 */ { "spiral", "spiral coil", RingCategory::Glyph },
    /* 39 */ { "staff", "hooked staff", RingCategory::Glyph },
    /* 40 */ { "9", "digit 9", RingCategory::Digit },
    /* 41 */ { "8", "digit 8", RingCategory::Digit },
    /* 42 */ { "7", "digit 7", RingCategory::Digit },
    /* 43 */ { "6", "digit 6", RingCategory::Digit },
    /* 44 */ { "5", "digit 5", RingCategory::Digit },
    /* 45 */ { "4", "digit 4", RingCategory::Digit },
    /* 46 */ { "3", "digit 3", RingCategory::Digit },
    /* 47 */ { "2", "digit 2", RingCategory::Digit },
    /* 48 */ { "1", "digit 1", RingCategory::Digit },
    /* 49 */ { "0", "digit 0", RingCategory::Digit },
};

// Which compiled table each column falls back to.
inline constexpr const RingSlot* RING_TABLE_FOR_COLUMN[RING_COLUMN_COUNT] = {
    RING_TABLE_A, RING_TABLE_A, RING_TABLE_A, RING_TABLE_A, RING_TABLE_B,
};

// Presentation only - the drums' colour schemes, so the web UI and the
// simulator mirror what the wall actually looks like even on a board that
// has no ring.json yet.  Firmware behaviour never reads these.
inline constexpr const char* RING_SCHEMES_JSON =
    "{\"minutes\":{\"card\":{\"default\":\"#181818\"},\"ink\":{\"default\":\"#e8e4da\",\"glyph\":\"#b03a2e\"}},\"seconds\":{\"card\":{\"default\":\"#e8e4da\"},\"ink\":{\"default\":\"#181818\",\"glyph\":\"#b03a2e\"}}}";
inline constexpr const char* RING_COLUMN_SCHEME[RING_COLUMN_COUNT] = {
    "minutes", "minutes", "minutes", "seconds", "seconds",
};

}  // namespace swan
