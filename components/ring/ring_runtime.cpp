#include "ring/ring_runtime.h"

#include <cstdlib>

#include "ring/json_lite.h"

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

}  // namespace

void RingTable::build_caches() {
    digit_.fill(-1);
    for (int i = 0; i < slot_count(); ++i) {
        const Slot& s = slots_[static_cast<size_t>(i)];
        switch (s.cat) {
            case RingCategory::Blank:
                if (blank_ < 0) blank_ = i;
                break;
            case RingCategory::Digit:
                if (s.id.size() == 1 && s.id[0] >= '0' && s.id[0] <= '9') {
                    digit_[static_cast<size_t>(s.id[0] - '0')] = i;
                }
                break;
            case RingCategory::AmPm:
                if (ieq(s.id, "am")) am_ = i;
                if (ieq(s.id, "pm")) pm_ = i;
                break;
            case RingCategory::Wifi:
                if (wifi_ < 0) wifi_ = i;
                break;
            case RingCategory::Glyph:
                break;
        }
        if (ieq(s.id, "qmark")) question_ = i;
    }
}

int RingTable::index_for_role(Role r, int digit) const {
    switch (r) {
        case Role::Blank:    return blank_;
        case Role::Digit:    return (digit >= 0 && digit <= 9) ? digit_[static_cast<size_t>(digit)] : -1;
        case Role::Am:       return am_;
        case Role::Pm:       return pm_;
        case Role::Wifi:     return wifi_;
        case Role::Question: return question_;
    }
    return -1;
}

int RingTable::index_for_token(std::string_view tok) const {
    if (tok.empty()) return -1;
    if (tok == "_") return blank_;
    if (tok[0] == '#') {
        int v = 0;
        if (tok.size() < 2) return -1;
        for (size_t i = 1; i < tok.size(); ++i) {
            if (tok[i] < '0' || tok[i] > '9') return -1;
            v = v * 10 + (tok[i] - '0');
            if (v >= slot_count()) return -1;
        }
        return v;
    }
    for (int i = 0; i < slot_count(); ++i) {
        if (ieq(tok, slots_[static_cast<size_t>(i)].id)) return i;
    }
    return -1;
}

std::shared_ptr<const RingTable> RingTable::compiled() {
    auto t = std::make_shared<RingTable>();
    t->slots_.reserve(RING_SLOT_COUNT);
    for (int i = 0; i < RING_SLOT_COUNT; ++i) {
        t->slots_.push_back(Slot{RING_TABLE[i].char_id, RING_TABLE[i].label,
                                 RING_TABLE[i].category});
    }
    t->build_caches();
    return t;
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
    if (t->blank_ < 0) {
        if (err) *err = "no blank slot";
        return nullptr;
    }
    return t;
}

RingSet RingSet::compiled_fallback() {
    RingSet s;
    s.shared_ = RingTable::compiled();
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
    std::vector<RingTable::Slot> parsed;
    if (!slots_from_json(*slots, parsed, err)) return false;
    auto shared = RingTable::from_slots(std::move(parsed), err);
    if (!shared) return false;

    // Optional per-column overrides: columns[i].slots (spec 4).
    std::array<std::shared_ptr<const RingTable>, N_COLUMNS> per{};
    if (const json::Value* cols = root.get("columns")) {
        const auto* arr = cols->as_array();
        if (arr != nullptr) {
            for (size_t i = 0; i < arr->size() && i < N_COLUMNS; ++i) {
                const json::Value* ov = (*arr)[i].get("slots");
                if (ov == nullptr) continue;
                std::vector<RingTable::Slot> colslots;
                if (!slots_from_json(*ov, colslots, err)) return false;
                per[i] = RingTable::from_slots(std::move(colslots), err);
                if (!per[i]) return false;
            }
        }
    }

    shared_ = std::move(shared);
    per_col_ = std::move(per);
    from_json_ = true;
    return true;
}

int RingSet::index_for_role(int column, Role r, int digit, bool* diag) const {
    const RingTable& t = col(column);
    const int i = t.index_for_role(r, digit);
    if (i >= 0) return i;
    if (diag != nullptr) *diag = true;
    const int blank = t.index_for_role(Role::Blank);
    return blank >= 0 ? blank : 0;
}

}  // namespace swan
