// Building a RingSet straight from tokenizer events, with no document in
// between.  See json_stream.h for why this path exists at all.
//
// The structure is known, so the handler is a small state machine over the
// paths that matter and a deliberate skip over everything else:
//
//   { "slots": [ {i,id,label,cat}, ... ],
//     "schemes": { ...opaque, carried verbatim... },
//     "columns": [ { "ring"|"slots": [ ... ], "scheme": "..." }, ... ] }
//
// Anything unrecognised is skipped by depth, not by parsing it into memory, so
// a document that carries fields we do not know about costs nothing.
#include <array>
#include <string>
#include <vector>

#include "ring/json_stream.h"
#include "ring/ring_runtime.h"

namespace swan {

namespace {

// The bit of "schemes" that has to survive: it is presentation, passed through
// to the browser verbatim and never interpreted here.  Re-serialising it from
// events is the one place the streaming design costs something, so it is
// captured as raw text with a hard cap rather than parsed.
constexpr std::size_t SCHEMES_MAX = 2048;

class RingHandler final : public json::StreamHandler {
public:
    bool on_object_begin() override {
        ++depth_;
        if (!in_schemes_ && pending_ == Key::Schemes) {
            // Presentation, carried through verbatim: re-serialised from events
            // rather than parsed, because nothing here interprets it.  Without
            // this the colours were silently dropped on every upload.
            in_schemes_ = true;
            schemes_depth_ = depth_;
            raw_.clear();
            first_.clear();
            pending_ = Key::None;
        }
        if (in_schemes_) { raw_.push_back('{'); first_.push_back(true); return true; }
        // A column's index is its POSITION in the array, counted here - not
        // "the first key I saw inside it".  columns:[{},{},{},{},{ring:...}]
        // is legal and normal (only column 5 carries its own ring), and
        // counting keys put that ring on column 1.
        if (state_ == St::Columns && depth_ == columns_depth_ + 1) ++col_index_;
        if (collecting() && depth_ == slots_depth_ + 1) {
            slot_ = RingTable::Slot{};
            slot_index_seen_ = -1;
            has_id_ = has_cat_ = false;
        }
        return true;
    }

    bool on_object_end() override {
        if (in_schemes_) {
            raw_.push_back('}');
            first_.pop_back();
            if (--depth_ < schemes_depth_) {
                in_schemes_ = false;
                if (raw_.size() <= SCHEMES_MAX) schemes_ = raw_;
                raw_.clear();
            }
            return true;
        }
        if (collecting() && depth_ == slots_depth_ + 1) {
            if (!has_id_ || !has_cat_) return reject("a slot is missing id or cat");
            if (slot_index_seen_ != static_cast<int>(slots_.size())) {
                return reject("slot " + std::to_string(slots_.size()) +
                              " is malformed or out of order");
            }
            if (slot_.label.empty()) slot_.label = slot_.id;
            if (slots_.size() >= static_cast<std::size_t>(RING_SLOT_COUNT) + 1) {
                // Refuse EARLY: a 100k-slot array must not be read to the end
                // just to be told the count is wrong at the finish.
                return reject("more slots than the drum has");
            }
            slots_.push_back(std::move(slot_));
        }
        --depth_;
        return true;
    }

    bool on_array_begin() override {
        ++depth_;
        if (in_schemes_) { raw_.push_back('['); first_.push_back(true); return true; }
        if (pending_ == Key::Slots && state_ == St::Idle) {
            state_ = St::SlotsArray;
            slots_depth_ = depth_;
            slots_.clear();
        } else if (pending_ == Key::ColumnRing && state_ == St::Columns) {
            state_ = St::ColumnSlots;
            slots_depth_ = depth_;
            slots_.clear();
        } else if (pending_ == Key::Columns) {
            state_ = St::Columns;
            columns_depth_ = depth_;
        }
        pending_ = Key::None;
        return true;
    }

    bool on_array_end() override {
        if (in_schemes_) {
            raw_.push_back(']');
            first_.pop_back();
            --depth_;
            return true;
        }
        if (state_ == St::SlotsArray && depth_ == slots_depth_) {
            if (!finish_table(shared_, "shared table")) return false;
            state_ = St::Idle;
        } else if (state_ == St::ColumnSlots && depth_ == slots_depth_) {
            if (col_index_ >= 0 && col_index_ < N_COLUMNS) {
                if (!finish_table(per_[static_cast<std::size_t>(col_index_)],
                                  "column " + std::to_string(col_index_ + 1) + " table")) {
                    return false;
                }
            }
            state_ = St::Columns;
        } else if (state_ == St::Columns && depth_ == columns_depth_) {
            state_ = St::Idle;
        }
        --depth_;
        return true;
    }

    bool on_key(std::string_view k) override {
        if (in_schemes_) {
            sep();
            raw_.push_back('"');
            raw_.append(k);
            raw_.append("\":");
            first_.back() = false;
            return true;
        }
        if (k == "slots" && depth_ == 1) pending_ = Key::Slots;
        else if (k == "columns" && depth_ == 1) pending_ = Key::Columns;
        else if (k == "schemes" && depth_ == 1) pending_ = Key::Schemes;
        else if ((k == "ring" || k == "slots") && state_ == St::Columns) pending_ = Key::ColumnRing;
        else if (k == "scheme" && state_ == St::Columns) pending_ = Key::ColumnScheme;
        else if (k == "i") pending_ = Key::SlotI;
        else if (k == "id") pending_ = Key::SlotId;
        else if (k == "label") pending_ = Key::SlotLabel;
        else if (k == "cat") pending_ = Key::SlotCat;
        else pending_ = Key::None;
        return true;
    }

    bool on_string(std::string_view v) override {
        if (in_schemes_) {
            sep();
            raw_.push_back('"');
            raw_.append(v);
            raw_.push_back('"');
            first_.back() = false;
            return true;
        }
        switch (pending_) {
            case Key::SlotId:    slot_.id.assign(v); has_id_ = true; break;
            case Key::SlotLabel: slot_.label.assign(v); break;
            case Key::SlotCat:
                if (!category_from(v, slot_.cat)) {
                    return reject("slot " + std::to_string(slots_.size()) +
                                  " has unknown category");
                }
                has_cat_ = true;
                break;
            case Key::ColumnScheme:
                if (col_index_ >= 0 && col_index_ < N_COLUMNS) {
                    scheme_[static_cast<std::size_t>(col_index_)] = std::string(v);
                }
                break;
            default: break;
        }
        pending_ = Key::None;
        return true;
    }

    bool on_number(double v) override {
        if (in_schemes_) {
            sep();
            char b[32];
            std::snprintf(b, sizeof b, "%g", v);
            raw_.append(b);
            first_.back() = false;
            return true;
        }
        if (pending_ == Key::SlotI) slot_index_seen_ = static_cast<int>(v);
        pending_ = Key::None;
        return true;
    }

    bool on_bool(bool v) override {
        if (in_schemes_) {
            sep();
            raw_.append(v ? "true" : "false");
            first_.back() = false;
        }
        pending_ = Key::None;
        return true;
    }

    bool on_null() override {
        if (in_schemes_) {
            sep();
            raw_.append("null");
            first_.back() = false;
        }
        pending_ = Key::None;
        return true;
    }

    bool build(RingSet& out, std::string* err);

private:
    enum class St : uint8_t { Idle, SlotsArray, Columns, ColumnSlots };
    // The shared table and a per-column override are collected identically;
    // only where the finished table lands differs.
    bool collecting() const { return state_ == St::SlotsArray || state_ == St::ColumnSlots; }
    enum class Key : uint8_t {
        None, Slots, Columns, Schemes, ColumnRing, ColumnScheme,
        SlotI, SlotId, SlotLabel, SlotCat,
    };

    bool reject(std::string why) {
        err = std::move(why);
        return false;
    }

    void sep() {
        if (!first_.empty() && !first_.back()) raw_.push_back(',');
        if (!first_.empty()) first_.back() = false;
        if (raw_.size() > SCHEMES_MAX) raw_.resize(SCHEMES_MAX);   // capped, never grows unbounded
    }

    bool finish_table(std::shared_ptr<const RingTable>& dest, const std::string& what) {
        if (slots_.size() != static_cast<std::size_t>(RING_SLOT_COUNT)) {
            return reject(what + " has " + std::to_string(slots_.size()) +
                          " slots; the drum has " + std::to_string(RING_SLOT_COUNT));
        }
        std::string e;
        dest = RingTable::from_slots(std::move(slots_), &e);
        slots_.clear();
        if (!dest) return reject(e.empty() ? (what + " rejected") : e);
        return true;
    }

    friend class RingStreamLoader;
    int depth_ = 0;
    St state_ = St::Idle;
    Key pending_ = Key::None;
    int slots_depth_ = 0;
    int columns_depth_ = 0;
    int col_index_ = -1;

    std::vector<RingTable::Slot> slots_;
    RingTable::Slot slot_;
    int slot_index_seen_ = -1;
    bool has_id_ = false;
    bool has_cat_ = false;

    std::shared_ptr<const RingTable> shared_;
    std::array<std::shared_ptr<const RingTable>, N_COLUMNS> per_{};
    std::array<std::string, N_COLUMNS> scheme_{};

    bool in_schemes_ = false;
    int schemes_depth_ = 0;
    std::string raw_;
    std::vector<bool> first_;
    std::string schemes_;
};

bool RingHandler::build(RingSet& out, std::string* err) {
    if (!shared_) {
        if (err) *err = "no slots array";
        return false;
    }
    RingSet candidate = RingSet::compiled_fallback();
    candidate.adopt_streamed(shared_, per_, schemes_, scheme_);
    std::string e;
    if (!candidate.validate_roles(&e)) {
        if (err) *err = e;
        return false;
    }
    out = std::move(candidate);
    return true;
}

}  // namespace

bool RingSet::load_json_streaming(std::string_view text, std::string* err) {
    // FIRST, exactly as load_json does: any failure below must leave a working
    // table behind.  Without this a rejected upload left the set holding a null
    // shared table, and the next col() dereferenced it - which is a crash on
    // the failure path, i.e. on the path a hostile upload takes.
    *this = compiled_fallback();

    RingHandler h;
    json::StreamParser p(h);
    // Fed in chunks on purpose, even though the caller has the whole body: it
    // is the same path the HTTP route uses straight off the socket, so the
    // tests exercise the real one rather than a convenience wrapper.
    constexpr std::size_t CHUNK = 512;
    for (std::size_t off = 0; off < text.size(); off += CHUNK) {
        if (!p.feed(text.substr(off, CHUNK))) {
            if (err) *err = h.err.empty() ? p.error() : h.err;
            return false;
        }
    }
    if (!p.finish()) {
        if (err) *err = h.err.empty() ? p.error() : h.err;
        return false;
    }
    return h.build(*this, err);
}

}  // namespace swan
