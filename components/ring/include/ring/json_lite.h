// Minimal JSON DOM parser - pure, no IDF, no exceptions, bounded.
//
// Exists because ring.json must parse identically on the host (tests, no IDF
// checkout in CI's host job) and on target; cJSON is an IDF component.  Scope:
// what tools/ringgen.py emits plus tolerance for unknown keys - objects,
// arrays, strings (with escapes), integers, bools, null.  Floats are parsed
// as integers truncated at the dot only if integral; ring.json has none.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace swan {
namespace json {

enum class Type : unsigned char { Null, Bool, Int, Str, Array, Object };

class Value {
public:
    Type type = Type::Null;
    bool boolean = false;
    int64_t number = 0;
    std::string str;
    std::vector<Value> items;                            // Array
    std::vector<std::pair<std::string, Value>> members;  // Object, in order

    bool is_null() const { return type == Type::Null; }

    // Object member lookup; nullptr when absent or not an object.
    const Value* get(std::string_view key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& m : members) {
            if (m.first == key) return &m.second;
        }
        return nullptr;
    }

    // Typed accessors with defaults - the callers' error handling is
    // "fall back to the compiled table", so soft failure is the contract.
    int64_t as_int(int64_t dflt = 0) const { return type == Type::Int ? number : dflt; }
    std::string_view as_str(std::string_view dflt = {}) const {
        return type == Type::Str ? std::string_view(str) : dflt;
    }
    const std::vector<Value>* as_array() const { return type == Type::Array ? &items : nullptr; }
};

// Parses a complete JSON document.  On failure returns false and, if err is
// non-null, a one-line reason with the byte offset.  Limits: depth <= 16,
// input <= 256 KB - ring.json is ~8 KB; anything bigger is not ours.
bool parse(std::string_view text, Value& out, std::string* err = nullptr);

}  // namespace json
}  // namespace swan
