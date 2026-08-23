#include "ring/ring_runtime.h"

#include "ring/json_lite.h"
#include "ring/json_write.h"

namespace swan {
namespace {

char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

bool ieq(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

bool category_from(std::string_view s, RingCategory& out) {
    if (s == "blank") out = RingCategory::Blank;
    else if (s == "digit") out = RingCategory::Digit;
    else if (s == "ampm") out = RingCategory::AmPm;
    else if (s == "glyph") out = RingCategory::Glyph;
    else if (s == "wifi") out = RingCategory::Wifi;
    else return false;
    return true;
}

// The drum turns one way, so "nearest" means nearest going forward.
int nearest_forward(const std::vector<int>& slots, int from, int count) {
    if (slots.empty()) return -1;
    if (from < 0 || count <= 0) return slots.front();
    int best = -1, best_cost = count + 1;
    for (const int s : slots) {
        const int cost = ((s - from) % count + count) % count;
        if (cost < best_cost) {
            best_cost = cost;
            best = s;
        }
    }
    return best;
}

// Parses one "slots" array into RingTable::Slot records.
bool slots_from_json(const json::Value& arr, std::vector<RingTable::Slot>& out,
                     std::string* err) {
    const auto* items = arr.as_array();
    if (items == nullptr) {
        if (err) *err = "slots is not an array";
        return false;
    }
    out.clear();
    out.reserve(items->size());
    for (size_t i = 0; i < items->size(); ++i) {
        const json::Value& s = (*items)[i];
        const json::Value* idx = s.get("i");
        const json::Value* id = s.get("id");
        const json::Value* cat = s.get("cat");
        if (idx == nullptr || id == nullptr || cat == nullptr ||
            idx->as_int(-1) != static_cast<int64_t>(i)) {
            if (err) *err = "slot " + std::to_string(i) + " malformed or out of order";
            return false;
        }
        RingTable::Slot slot;
        slot.id = std::string(id->as_str());
        const json::Value* label = s.get("label");
        slot.label = label ? std::string(label->as_str()) : slot.id;
        if (!category_from(cat->as_str(), slot.cat)) {
            if (err) *err = "slot " + std::to_string(i) + " has unknown category";
            return false;
        }
        out.push_back(std::move(slot));
    }
    return true;
}

// Parses one table and enforces the physical size: the drums carry exactly
// RING_SLOT_COUNT flaps and the motion targets T(i) are compiled for that
// geometry, so any other size would command positions that do not exist.
std::shared_ptr<const RingTable> table_from_json(const json::Value& arr, const char* what,
                                                 std::string* err) {
    std::vector<RingTable::Slot> parsed;
    if (!slots_from_json(arr, parsed, err)) return nullptr;
    if (parsed.size() != static_cast<size_t>(RING_SLOT_COUNT)) {
        if (err) {
            *err = std::string(what) + " has " + std::to_string(parsed.size()) +
                   " slots; the drum has " + std::to_string(RING_SLOT_COUNT);
        }
        return nullptr;
    }
    return RingTable::from_slots(std::move(parsed), err);
}

}  // namespace

const std::vector<int> RingTable::kEmpty{};

const char* role_name(Role r) {
    switch (r) {
        case Role::Blank:    return "blank";
        case Role::Digit:    return "digit";
        case Role::Am:       return "AM";
        case Role::Pm:       return "PM";
        case Role::Wifi:     return "wifi";
        case Role::Question: return "question";
    }
    return "?";
}

void RingTable::build_caches() {
    blank_.clear();
    am_.clear();
    pm_.clear();
    wifi_.clear();
    question_.clear();
    for (auto& v : digit_) v.clear();

    for (int i = 0; i < slot_count(); ++i) {
        const Slot& s = slots_[static_cast<size_t>(i)];
        switch (s.cat) {
            case RingCategory::Blank:
                blank_.push_back(i);
                break;
            case RingCategory::Digit:
                if (s.id.size() == 1 && s.id[0] >= '0' && s.id[0] <= '9') {
                    digit_[static_cast<size_t>(s.id[0] - '0')].push_back(i);
                }
                break;
            case RingCategory::AmPm:
                if (ieq(s.id, "am")) am_.push_back(i);
                if (ieq(s.id, "pm")) pm_.push_back(i);
                break;
            case RingCategory::Wifi:
                wifi_.push_back(i);
                break;
            case RingCategory::Glyph:
                break;
        }
        if (ieq(s.id, "qmark")) question_.push_back(i);
    }
}

const std::vector<int>& RingTable::slots_for_role(Role r, int digit) const {
    switch (r) {
        case Role::Blank:    return blank_;
        case Role::Am:       return am_;
        case Role::Pm:       return pm_;
        case Role::Wifi:     return wifi_;
        case Role::Question: return question_;
        case Role::Digit:
            if (digit >= 0 && digit <= 9) return digit_[static_cast<size_t>(digit)];
            return kEmpty;
    }
    return kEmpty;
}

int RingTable::index_for_role(Role r, int digit, int from) const {
    return nearest_forward(slots_for_role(r, digit), from, slot_count());
}

int RingTable::index_for_token(std::string_view tok, int from) const {
    if (tok.empty()) return -1;
    if (tok == "_") return index_for_role(Role::Blank, 0, from);
    if (tok[0] == '#') {
        // A raw index is exact by definition - no nearest-forward search.
        int v = 0;
        if (tok.size() < 2) return -1;
        for (size_t i = 1; i < tok.size(); ++i) {
            if (tok[i] < '0' || tok[i] > '9') return -1;
            v = v * 10 + (tok[i] - '0');
            if (v >= slot_count()) return -1;
        }
        return v;
    }
    // A char_id can appear more than once (column 5's digits), so collect all
    // matches and take the nearest forward.
    std::vector<int> hits;
    for (int i = 0; i < slot_count(); ++i) {
        if (ieq(tok, slots_[static_cast<size_t>(i)].id)) hits.push_back(i);
    }
    return nearest_forward(hits, from, slot_count());
}

bool RingTable::is_descending() const {
    for (int d = 1; d <= 9; ++d) {
        const std::vector<int>& src = slots_for_role(Role::Digit, d);
        if (src.empty() || slots_for_role(Role::Digit, d - 1).empty()) return false;
        for (const int s : src) {
            if (index_for_role(Role::Digit, d - 1, s) != (s + 1) % slot_count()) return false;
        }
    }
    return true;
}

std::shared_ptr<const RingTable> RingTable::from_slots(std::vector<Slot> slots,
                                                       std::string* err) {
    if (slots.size() < 2) {
        if (err) *err = "too few slots";
        return nullptr;
    }
    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i].id.empty()) {
            if (err) *err = "slot " + std::to_string(i) + " has an empty id";
            return nullptr;
        }
    }
    auto t = std::make_shared<RingTable>();
    t->slots_ = std::move(slots);
    t->build_caches();
    if (t->blank_.empty()) {
        if (err) *err = "no blank slot";
        return nullptr;
    }
    return t;
}

std::shared_ptr<const RingTable> RingTable::compiled(const RingSlot* table, int count) {
    auto t = std::make_shared<RingTable>();
    t->slots_.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        t->slots_.push_back(Slot{table[i].char_id, table[i].label, table[i].category});
    }
    t->build_caches();
    return t;
}

RingSet RingSet::compiled_fallback() {
    static_assert(RING_COLUMN_COUNT == N_COLUMNS,
                  "generated ring tables and the pin map disagree on the column count");
    RingSet s;
    s.shared_ = RingTable::compiled(RING_TABLE_FOR_COLUMN[0], RING_SLOT_COUNT);
    for (int i = 0; i < N_COLUMNS; ++i) {
        // Only columns whose ring differs from the shared one need their own.
        if (RING_TABLE_FOR_COLUMN[i] != RING_TABLE_FOR_COLUMN[0]) {
            s.per_col_[static_cast<size_t>(i)] =
                RingTable::compiled(RING_TABLE_FOR_COLUMN[i], RING_SLOT_COUNT);
        }
    }
    s.schemes_json_ = RING_SCHEMES_JSON;
    for (int i = 0; i < N_COLUMNS; ++i) {
        s.scheme_[static_cast<size_t>(i)] = RING_COLUMN_SCHEME[i];
    }
    s.from_json_ = false;
    return s;
}

bool RingSet::load_json(std::string_view text, std::string* err) {
    *this = compiled_fallback();  // any failure below leaves a working table

    json::Value root;
    if (!json::parse(text, root, err)) return false;

    const json::Value* slots = root.get("slots");
    if (slots == nullptr) {
        if (err) *err = "no slots array";
        return false;
    }
    auto shared = table_from_json(*slots, "shared table", err);
    if (!shared) return false;

    // Per-column overrides: columns[i].ring (spec 4); "slots" is accepted as
    // an alias for symmetry with the top-level key.
    std::array<std::shared_ptr<const RingTable>, N_COLUMNS> per{};
    if (const json::Value* cols = root.get("columns")) {
        const auto* arr = cols->as_array();
        if (arr != nullptr) {
            for (size_t i = 0; i < arr->size() && i < N_COLUMNS; ++i) {
                const json::Value* ov = (*arr)[i].get("ring");
                if (ov == nullptr) ov = (*arr)[i].get("slots");
                if (ov == nullptr) continue;
                const std::string what = "column " + std::to_string(i + 1) + " table";
                per[i] = table_from_json(*ov, what.c_str(), err);
                if (!per[i]) return false;
            }
        }
    }

    RingSet candidate = compiled_fallback();  // presentation defaults come along
    candidate.shared_ = std::move(shared);
    candidate.per_col_ = std::move(per);
    candidate.from_json_ = true;

    // Presentation, carried through verbatim and never interpreted here.  A
    // malformed or missing block is not an error: the drums still turn, they
    // just render in the fallback colours.
    if (const json::Value* sch = root.get("schemes")) {
        if (sch->type == json::Type::Object) candidate.schemes_json_ = json::serialize(*sch);
    }
    if (const json::Value* cols = root.get("columns")) {
        if (const auto* arr = cols->as_array()) {
            for (size_t i = 0; i < arr->size() && i < N_COLUMNS; ++i) {
                const json::Value* n = (*arr)[i].get("scheme");
                if (n != nullptr && n->type == json::Type::Str) {
                    candidate.scheme_[i] = std::string(n->as_str());
                }
            }
        }
    }

    // Validate BEFORE publishing: a table that cannot render a role the column
    // will be asked for must fail here, not at render time (spec 4).
    if (!candidate.validate_roles(err)) return false;

    *this = std::move(candidate);
    return true;
}

int RingSet::index_for_role(int column, Role r, int digit, int from, bool* diag) const {
    const RingTable& t = col(column);
    const int i = t.index_for_role(r, digit, from);
    if (i >= 0) return i;
    if (diag != nullptr) *diag = true;
    const int blank = t.index_for_role(Role::Blank, 0, from);
    return blank >= 0 ? blank : 0;
}

bool RingSet::validate_roles(std::string* err) const {
    for (int c = 0; c < N_COLUMNS; ++c) {
        const RingTable& t = col(c);

        auto need = [&](Role r, int digit) {
            if (!t.slots_for_role(r, digit).empty()) return true;
            if (err) {
                *err = "column " + std::to_string(c + 1) + " cannot render role '" +
                       role_name(r) + (r == Role::Digit ? " " + std::to_string(digit) : "") +
                       "'";
            }
            return false;
        };

        // Every column renders blanks, the ????? preset, and countdown digits.
        if (!need(Role::Blank, 0)) return false;
        if (!need(Role::Question, 0)) return false;
        for (int d = 0; d <= 9; ++d) {
            if (!need(Role::Digit, d)) return false;
        }
        // Column 1 carries AM/PM; the centre column carries the WiFi glyph.
        // Column 5's ring has neither, by design - and is asked for neither.
        if (c == AMPM_COLUMN) {
            if (!need(Role::Am, 0) || !need(Role::Pm, 0)) return false;
        }
        if (c == WIFI_COLUMN) {
            if (!need(Role::Wifi, 0)) return false;
        }
    }
    return true;
}

}  // namespace swan
