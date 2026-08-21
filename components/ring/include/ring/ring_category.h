// Ring slot categories.  Hand-written; the table itself is generated.
#pragma once

namespace swan {

enum class RingCategory : unsigned char {
    Blank = 0,
    Digit,
    AmPm,
    Glyph,
    Wifi,
};

inline const char* ring_category_name(RingCategory c) {
    switch (c) {
        case RingCategory::Blank: return "blank";
        case RingCategory::Digit: return "digit";
        case RingCategory::AmPm:  return "ampm";
        case RingCategory::Glyph: return "glyph";
        case RingCategory::Wifi:  return "wifi";
    }
    return "?";
}

}  // namespace swan
