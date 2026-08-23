// Minimal JSON writer - pure, no IDF, no allocation beyond the output string.
//
// Companion to json_lite.h (the parser).  Exists for the same reason: the /ws
// and /api payloads must be built identically on the host dev server and on
// target, and cJSON is IDF-only.  Scope is what the web UI needs; it does not
// pretty-print and it does not validate nesting beyond an assert-free
// discipline of matching begin/end calls.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>

#include "ring/json_lite.h"

namespace swan {
namespace json {

class Writer {
public:
    // --- containers ---
    Writer& obj() { return open('{'); }
    Writer& end_obj() { return close('}'); }
    Writer& arr() { return open('['); }
    Writer& end_arr() { return close(']'); }

    // --- object members ---
    Writer& key(std::string_view k) {
        comma();
        quote(k);
        out_ += ':';
        first_ = true;  // the value that follows is not preceded by a comma
        return *this;
    }

    // --- values (also usable as array elements) ---
    Writer& str(std::string_view v) {
        comma();
        quote(v);
        return *this;
    }
    Writer& num(int64_t v) {
        comma();
        out_ += std::to_string(v);
        return *this;
    }
    Writer& boolean(bool v) {
        comma();
        out_ += v ? "true" : "false";
        return *this;
    }
    Writer& null() {
        comma();
        out_ += "null";
        return *this;
    }
    // Emits an already-encoded fragment (a nested document built elsewhere).
    Writer& raw(std::string_view v) {
        comma();
        out_ += v;
        return *this;
    }

    // --- key + value shorthands, which is most of the call sites ---
    Writer& kv(std::string_view k, std::string_view v) { return key(k).str(v); }
    Writer& kv(std::string_view k, const char* v) { return key(k).str(v); }
    // One template rather than int/int64_t overloads: on riscv32 int32_t is
    // `long`, which matches neither exactly and made every kv(k, int32_t)
    // call ambiguous.
    template <typename T, typename = std::enable_if_t<std::is_integral_v<T> &&
                                                      !std::is_same_v<T, bool>>>
    Writer& kv(std::string_view k, T v) {
        return key(k).num(static_cast<int64_t>(v));
    }
    Writer& kv(std::string_view k, bool v) { return key(k).boolean(v); }
    Writer& kv_null(std::string_view k) { return key(k).null(); }
    Writer& kv_raw(std::string_view k, std::string_view v) { return key(k).raw(v); }

    const std::string& str() const { return out_; }
    std::string take() { return std::move(out_); }

private:
    std::string out_;
    bool first_ = true;

    Writer& open(char c) {
        comma();
        out_ += c;
        first_ = true;
        return *this;
    }
    Writer& close(char c) {
        out_ += c;
        first_ = false;
        return *this;
    }
    void comma() {
        if (!first_) out_ += ',';
        first_ = false;
    }
    void quote(std::string_view v) {
        out_ += '"';
        for (const char c : v) {
            switch (c) {
                case '"':  out_ += "\\\""; break;
                case '\\': out_ += "\\\\"; break;
                case '\n': out_ += "\\n"; break;
                case '\r': out_ += "\\r"; break;
                case '\t': out_ += "\\t"; break;
                case '\b': out_ += "\\b"; break;
                case '\f': out_ += "\\f"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        // Control characters must be escaped; our own parser
                        // rejects a NUL escape, so never emit one.
                        static const char* kHex = "0123456789abcdef";
                        out_ += "\\u00";
                        out_ += kHex[(static_cast<unsigned char>(c) >> 4) & 0xF];
                        out_ += kHex[static_cast<unsigned char>(c) & 0xF];
                    } else {
                        out_ += c;  // UTF-8 passes through byte for byte
                    }
            }
        }
        out_ += '"';
    }
};

// Re-emits a parsed document.  Lets an opaque sub-object be carried through a
// component that does not understand it - the ring's colour schemes travel
// from ring.json to the browser this way, and firmware never interprets them.
inline void serialize_into(const Value& v, Writer& w) {
    switch (v.type) {
        case Type::Null:  w.null(); break;
        case Type::Bool:  w.boolean(v.boolean); break;
        case Type::Int:   w.num(v.number); break;
        case Type::Str:   w.str(v.str); break;
        case Type::Array:
            w.arr();
            for (const Value& e : v.items) serialize_into(e, w);
            w.end_arr();
            break;
        case Type::Object:
            w.obj();
            for (const auto& m : v.members) {
                w.key(m.first);
                serialize_into(m.second, w);
            }
            w.end_obj();
            break;
    }
}

inline std::string serialize(const Value& v) {
    Writer w;
    serialize_into(v, w);
    return w.take();
}

}  // namespace json
}  // namespace swan
