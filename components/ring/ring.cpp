#include "ring/ring.h"

#include <cstdlib>
#include <cstring>

namespace swan {
namespace {

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool ieq(const char* a, const char* b) {
    while (*a && *b) {
        if (lower(*a) != lower(*b)) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

}  // namespace

int ring_index_for_token(const char* tok) {
    if (tok == nullptr || tok[0] == '\0') return RING_INVALID;

    if (std::strcmp(tok, "_") == 0) return RING_HOME_SLOT;  // blank

    if (tok[0] == '#') {
        char* end = nullptr;
        long v = std::strtol(tok + 1, &end, 10);
        if (end == tok + 1 || *end != '\0') return RING_INVALID;
        return ring_index_valid(static_cast<int>(v)) ? static_cast<int>(v) : RING_INVALID;
    }

    for (int i = 0; i < RING_SLOT_COUNT; ++i) {
        if (ieq(tok, RING_TABLE[i].char_id)) return i;
    }
    return RING_INVALID;
}

const char* ring_char_id(int i) { return ring_index_valid(i) ? RING_TABLE[i].char_id : "?"; }
const char* ring_label(int i) { return ring_index_valid(i) ? RING_TABLE[i].label : "invalid"; }

RingCategory ring_category(int i) {
    return ring_index_valid(i) ? RING_TABLE[i].category : RingCategory::Blank;
}

}  // namespace swan
