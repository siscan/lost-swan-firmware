#include "ring/json_lite.h"

namespace swan {
namespace json {
namespace {

constexpr size_t MAX_INPUT = 256 * 1024;
constexpr int MAX_DEPTH = 16;

struct Parser {
    std::string_view s;
    size_t i = 0;
    std::string* err;

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
                        // UTF-8 encode (surrogate pairs unsupported; ringgen
                        // emits none - reject rather than mangle).
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

bool parse(std::string_view text, Value& out, std::string* err) {
    if (text.size() > MAX_INPUT) {
        if (err != nullptr) *err = "input too large";
        return false;
    }
    Parser p{text, 0, err};
    out = Value{};
    if (!p.parse_value(out, 0)) return false;
    p.skip_ws();
    if (p.i != text.size()) return p.fail("trailing garbage");
    return true;
}

}  // namespace json
}  // namespace swan
