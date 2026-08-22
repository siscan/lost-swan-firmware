// The fluid ring (spec 4): a runtime table loaded from ring.json, per-column
// capable, with the compiled-in table as fallback.  Pure - no IDF includes;
// the LittleFS loading lives in ring_store.cpp (target only).
//
// No code references a ring index directly: everything goes through a Role or
// a name.  A failed lookup returns the blank slot's index and raises a
// diagnostic flag, per spec 4 ("the column shows blank and a diagnostic is
// raised").
#pragma once

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "hal/pins.h"  // N_COLUMNS - pure, cstdint only
#include "ring/ring.h"   // compiled fallback table + categories

namespace swan {

enum class Role : unsigned char { Blank, Digit, Am, Pm, Wifi, Question };

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
    // working: digits are cat==Digit with id "0".."9", blank is the first
    // cat==Blank, the ? glyph is id "qmark".  Returns -1 when absent.
    int index_for_role(Role r, int digit = 0) const;

    // Name lookup: a char_id from the table (case-insensitive), "_" for blank,
    // "#n" for a raw index.  Returns -1 when unknown.
    int index_for_token(std::string_view tok) const;

    // Built from the generated compile-time table - the fallback, and the
    // baseline the JSON path is tested against.
    static std::shared_ptr<const RingTable> compiled();

    // Builds from parsed slot data; validates categories and non-empty ids.
    static std::shared_ptr<const RingTable> from_slots(std::vector<Slot> slots,
                                                       std::string* err);

private:
    std::vector<Slot> slots_;
    // Caches, derived once in from_slots/compiled.
    int blank_ = -1, am_ = -1, pm_ = -1, wifi_ = -1, question_ = -1;
    std::array<int, 10> digit_{};

    void build_caches();
};

// The per-column view: one shared table, any column may carry its own
// (columns[i].slots in ring.json).  Calibration offsets live in NVS,
// independent of this, so a table swap never invalidates calibration.
class RingSet {
public:
    // Every column on the compiled table.
    static RingSet compiled_fallback();

    // Parse ring.json.  On ANY failure returns false and leaves *this as the
    // compiled fallback - the display must always be able to render.
    bool load_json(std::string_view text, std::string* err);

    const RingTable& col(int column) const {
        const auto& t = per_col_[static_cast<size_t>(column)];
        return t ? *t : *shared_;
    }

    // Convenience that carries the spec 4 failure contract: unknown role ->
    // the column's blank index, and *diag set true.
    int index_for_role(int column, Role r, int digit, bool* diag = nullptr) const;

    bool loaded_from_json() const { return from_json_; }

private:
    std::shared_ptr<const RingTable> shared_;
    std::array<std::shared_ptr<const RingTable>, N_COLUMNS> per_col_{};
    bool from_json_ = false;
};

}  // namespace swan
