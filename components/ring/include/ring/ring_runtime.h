// The fluid ring (spec 4): runtime tables loaded from ring.json, per-column,
// with the compiled-in tables as fallback.  Pure - no IDF includes; the
// LittleFS loading lives in ring_store.cpp (target only).
//
// Two rings ship: ring A on columns 1-4, ring B on column 5.  Both are
// DESCENDING - one forward flip decrements the displayed digit - and column 5
// carries TWO digit blocks so its 0->9 wrap costs 16 flips instead of 41.
//
// Consequence of the double block: a digit does not have one slot.  Every
// lookup therefore takes the column's CURRENT slot and returns the nearest
// match in the forward direction (rotation is one-way).  Column 5's physical
// position is genuinely not predictable from the displayed digit - see
// docs/BRINGUP.md before calling that a fault on the bench.
//
// No code references a ring index directly: everything goes through a Role or
// a name.  A failed lookup returns the blank slot and raises a diagnostic
// (spec 4), but the roles a column can be asked for are validated at LOAD
// time so that failure surfaces at boot, not mid-render.
#pragma once

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "hal/pins.h"   // N_COLUMNS - pure, cstdint only
#include "ring/ring.h"  // compiled fallback tables + categories

namespace swan {

enum class Role : unsigned char { Blank, Digit, Am, Pm, Wifi, Question };
const char* role_name(Role r);

// "No preference" for the nearest-forward search: returns the lowest matching
// slot, which keeps renders deterministic before anything is displayed.
inline constexpr int FROM_ANY = -1;

class RingTable {
public:
    struct Slot {
        std::string id;     // message token, from the manifest - never invented
        std::string label;  // human-readable, for UI / calibrate walk
        RingCategory cat;
    };

    int slot_count() const { return static_cast<int>(slots_.size()); }
    const Slot& slot(int i) const { return slots_[static_cast<size_t>(i)]; }

    // Role lookup, derived from the slot data itself so a reordered ring keeps
    // working.  `from` is the column's current slot; the nearest match in the
    // FORWARD direction wins, because the drum turns one way.  Returns -1 when
    // the role is absent from this ring.
    int index_for_role(Role r, int digit = 0, int from = FROM_ANY) const;

    // Name lookup: a char_id from the table (case-insensitive), "_" for blank,
    // "#n" for a raw index.  A char_id may appear more than once (column 5's
    // digits do), so this is nearest-forward too.  Returns -1 when unknown.
    int index_for_token(std::string_view tok, int from = FROM_ANY) const;

    // Every digit decrement costs exactly one forward flip.  What "descending"
    // means for a one-way ring, and what the countdown's 1-flip tick needs.
    bool is_descending() const;

    // All slots carrying this role, in slot order (diagnostics, the calibrate
    // walk, and the tests that pin column 5's two blocks).
    const std::vector<int>& slots_for_role(Role r, int digit = 0) const;

    static std::shared_ptr<const RingTable> from_slots(std::vector<Slot> slots,
                                                       std::string* err);
    // Built from one of the generated compile-time tables.
    static std::shared_ptr<const RingTable> compiled(const RingSlot* table, int count);

private:
    std::vector<Slot> slots_;
    // Caches, built once: every slot carrying each role.
    std::vector<int> blank_, am_, pm_, wifi_, question_;
    std::array<std::vector<int>, 10> digit_;
    static const std::vector<int> kEmpty;

    void build_caches();
};

// The per-column view.  Calibration offsets live in NVS, independent of this,
// so a table swap never invalidates calibration.
class RingSet {
public:
    // Ring A on cols 1-4, ring B on col 5, from the generated tables.
    static RingSet compiled_fallback();

    // Parse ring.json.  On ANY failure - malformed, wrong slot count, or a
    // column that cannot render a role it will be asked for - returns false
    // and leaves *this as the compiled fallback: the display must always be
    // able to render.
    bool load_json(std::string_view text, std::string* err);

    const RingTable& col(int column) const {
        const auto& t = per_col_[static_cast<size_t>(column)];
        return t ? *t : *shared_;
    }

    // Convenience carrying the spec 4 failure contract: an absent role yields
    // the column's blank slot and sets *diag.  Load-time validation means this
    // should be unreachable for the roles a column is actually asked for.
    int index_for_role(int column, Role r, int digit, int from, bool* diag = nullptr) const;

    // Every role every column can be asked for must exist in that column's
    // ring: all columns need blank, '?' and digits 0-9; column 1 needs AM/PM;
    // the centre column needs the WiFi glyph.  Fails loudly at load.
    bool validate_roles(std::string* err) const;

    bool loaded_from_json() const { return from_json_; }

    static constexpr int AMPM_COLUMN = 0;              // spec 7.1
    static constexpr int WIFI_COLUMN = N_COLUMNS / 2;  // centre column

private:
    std::shared_ptr<const RingTable> shared_;
    std::array<std::shared_ptr<const RingTable>, N_COLUMNS> per_col_{};
    bool from_json_ = false;
};

}  // namespace swan
