#include "ring/json_lite.h"

namespace swan {
namespace json {
namespace {

constexpr size_t MAX_INPUT = 256 * 1024;
constexpr int MAX_DEPTH = 16;

// A node budget, not just a byte budget.  Every Value carries a std::string
// and two vectors whatever it holds - ~64 bytes on RV32, ~88 when it lives in
// an object's member list - while the cheapest token is two bytes ("0,").  A
// document the length check waves through can therefore ask for many times its
// own size, in vectors that must grow contiguously.  With exceptions off and
// no PSRAM an allocation failure is abort(): a reboot, not a rejection.
//
// MEASURED on the board, not guessed.  data/ring.json is 9,361 bytes and
// **535 nodes**.  A flood of 500 nodes parsed in 0.1 s; 2,000 panicked the
// device every time, and the arithmetic is exact: one array growing to a
// capacity of 1024 needs 1024 x 64 = 65,536 bytes while still holding the 512
// x 64 = 32,768 it is copying from, and 98,304 was precisely the largest free
// block the board reported.
//
// So the danger is not the total node count - it is ONE HUGE CONTAINER, whose
// vector must grow contiguously and briefly holds both halves.  Hence two
// limits: a total budget, and a per-container budget that keeps any single
// vector small.  ring.json's largest container is 50 elements, so 256 is five
// times what a legitimate document needs.
//
// The headroom is thin because a DOM is the wrong shape for parsing untrusted
// input on a 128 KB heap; the honest fix is a streaming parse, which is Phase
// 4 work.  Until then these caps and the caller's heap guard are what stand
// between a bad POST and a reboot.
// The value lives in the header so callers can state their own.
constexpr int MAX_NODES = MAX_NODES_UNTRUSTED;
constexpr int MAX_CONTAINER = 256;  // elements in one array or object

struct Parser {
    std::string_view s;
    size_t i = 0;
    std::string* err;
    int nodes = 0;
    int max_nodes = MAX_NODES;

    bool fail(const char* why) {
        if (err != nullptr && err->empty()) {
            *err = why;
            *err += " at byte ";
            *err += std::to_string(i);
        }
        return false;
    }

    void skip_ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }

    bool literal(std::string_view lit) {
        if (s.compare(i, lit.size(), lit) != 0) return fail("bad literal");
        i += lit.size();
        return true;
    }

    bool parse_string(std::string& out) {
        if (i >= s.size() || s[i] != '"') return fail("expected string");
        ++i;
        out.clear();
        while (i < s.size()) {
            const char c = s[i];
            if (c == '"') {
                ++i;
                return true;
            }
            if (c == '\\') {
                if (i + 1 >= s.size()) return fail("truncated escape");
                const char e = s[i + 1];
                i += 2;
                switch (e) {
                    case '"':  out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/'; break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u': {
                        if (i + 4 > s.size()) return fail("truncated \\u");
                        unsigned v = 0;
                        for (int k = 0; k < 4; ++k) {
                            const char h = s[i + k];
                            v <<= 4;
                            if (h >= '0' && h <= '9') v |= static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') v |= static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') v |= static_cast<unsigned>(h - 'A' + 10);
                            else return fail("bad \\u digit");
                        }
                        i += 4;
                        // UTF-8 encode.  A NUL escape is rejected rather than
                        // decoded: it would smuggle a NUL past the control-char
                        // check into C-string consumers.  Surrogate pairs are
                        // rejected too - ringgen emits none.
                        if (v == 0) return fail("NUL escape rejected");
                        if (v >= 0xD800 && v <= 0xDFFF) return fail("surrogates unsupported");
                        if (v < 0x80) {
                            out += static_cast<char>(v);
                        } else if (v < 0x800) {
                            out += static_cast<char>(0xC0 | (v >> 6));
                            out += static_cast<char>(0x80 | (v & 0x3F));
                        } else {
                            out += static_cast<char>(0xE0 | (v >> 12));
                            out += static_cast<char>(0x80 | ((v >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (v & 0x3F));
                        }
                        break;
                    }
                    default: return fail("unknown escape");
                }
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20) return fail("control char in string");
            out += c;
            ++i;
        }
        return fail("unterminated string");
    }

    bool parse_value(Value& v, int depth) {
        if (depth > MAX_DEPTH) return fail("nesting too deep");
        if (++nodes > max_nodes) return fail("document has too many values");
        skip_ws();
        if (i >= s.size()) return fail("unexpected end");
        const char c = s[i];

        if (c == '{') {
            ++i;
            v.type = Type::Object;
            skip_ws();
            if (i < s.size() && s[i] == '}') {
                ++i;
                return true;
            }
            for (;;) {
                skip_ws();
                std::string key;
                if (!parse_string(key)) return false;
                skip_ws();
                if (i >= s.size() || s[i] != ':') return fail("expected ':'");
                ++i;
                Value member;
                if (!parse_value(member, depth + 1)) return false;
                if (static_cast<int>(v.members.size()) >= MAX_CONTAINER) {
                    return fail("object has too many members");
                }
                v.members.emplace_back(std::move(key), std::move(member));
                skip_ws();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < s.size() && s[i] == '}') {
                    ++i;
                    return true;
                }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++i;
            v.type = Type::Array;
            skip_ws();
            if (i < s.size() && s[i] == ']') {
                ++i;
                return true;
            }
            for (;;) {
                Value item;
                if (!parse_value(item, depth + 1)) return false;
                if (static_cast<int>(v.items.size()) >= MAX_CONTAINER) {
                    return fail("array has too many elements");
                }
                v.items.push_back(std::move(item));
                skip_ws();
                if (i < s.size() && s[i] == ',') {
                    ++i;
                    continue;
                }
                if (i < s.size() && s[i] == ']') {
                    ++i;
                    return true;
                }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            v.type = Type::Str;
            return parse_string(v.str);
        }
        if (c == 't') {
            v.type = Type::Bool;
            v.boolean = true;
            return literal("true");
        }
        if (c == 'f') {
            v.type = Type::Bool;
            v.boolean = false;
            return literal("false");
        }
        if (c == 'n') {
            v.type = Type::Null;
            return literal("null");
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            const bool neg = (c == '-');
            if (neg) ++i;
            if (i >= s.size() || s[i] < '0' || s[i] > '9') return fail("bad number");
            int64_t n = 0;
            int digits = 0;
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
                if (++digits > 18) return fail("number too long");
                n = n * 10 + (s[i] - '0');
                ++i;
            }
            if (i < s.size() && (s[i] == '.' || s[i] == 'e' || s[i] == 'E')) {
                return fail("floats unsupported");
            }
            v.type = Type::Int;
            v.number = neg ? -n : n;
            return true;
        }
        return fail("unexpected character");
    }
};

}  // namespace

bool parse(std::string_view text, Value& out, std::string* err, int max_nodes) {
    if (text.size() > MAX_INPUT) {
        if (err != nullptr) *err = "input too large";
        return false;
    }
    Parser p{text, 0, err};
    if (max_nodes > 0) p.max_nodes = max_nodes;
    out = Value{};
    if (!p.parse_value(out, 0)) return false;
    p.skip_ws();
    if (p.i != text.size()) return p.fail("trailing garbage");
    return true;
}

}  // namespace json
}  // namespace swan
