// A chunk-fed JSON tokenizer: events out, no document in.
//
// WHY, precisely.  json_lite builds a DOM, and on this device that is the last
// path where a bad upload can still take the board down: a `json::Value` is
// ~64 bytes against a 2-byte minimum token, so the node cap (700, against a
// real ring.json's 535) is the only thing between an upload and an allocation
// failure - and with exceptions off, an allocation failure is abort(), which is
// a reboot.  700 was already measured as the point where the arithmetic works
// out; there is no margin left to give.
//
// This parser holds a token buffer, a depth stack and nothing else, so its
// memory does not depend on the document at all.  The caller feeds it whatever
// arrives from the socket and builds its own result as the events land - for
// the ring, that is the table itself, which has a fixed size by definition.
//
// Deliberately NOT a general replacement for json_lite: commands are small,
// random-access and easier to read as a tree.  This is for the one path where
// the input is large, untrusted and structurally known.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace swan {
namespace json {

// A token longer than this is an error rather than a reallocation: the longest
// legitimate string in a ring document is a glyph label.
inline constexpr std::size_t STREAM_TOKEN_MAX = 192;
// Deeper than any legitimate document here, and the bound on the state stack.
inline constexpr std::size_t STREAM_DEPTH_MAX = 12;

class StreamHandler {
public:
    virtual ~StreamHandler() = default;
    // Returning false stops the parse with the handler's own error - a wrong
    // slot count should not have to be discovered after the whole document.
    virtual bool on_object_begin() { return true; }
    virtual bool on_object_end() { return true; }
    virtual bool on_array_begin() { return true; }
    virtual bool on_array_end() { return true; }
    virtual bool on_key(std::string_view k) { (void)k; return true; }
    virtual bool on_string(std::string_view v) { (void)v; return true; }
    virtual bool on_number(double v) { (void)v; return true; }
    virtual bool on_bool(bool v) { (void)v; return true; }
    virtual bool on_null() { return true; }
    // Set by a handler that failed, so the caller can report WHY rather than
    // "parse error".
    std::string err;
};

class StreamParser {
public:
    explicit StreamParser(StreamHandler& h) : h_(h) {}

    // Feed any number of bytes, in any split - a chunk boundary may fall in the
    // middle of a string, a number, or an escape sequence.
    bool feed(std::string_view chunk);
    // No more input.  Fails if the document is truncated.
    bool finish();

    const std::string& error() const { return err_; }
    std::size_t depth() const { return depth_; }

private:
    enum class St : uint8_t {
        Value,        // expecting a value
        InString,     // inside a string
        InEscape,     // just saw a backslash
        InUnicode,    // inside \uXXXX
        InNumber,
        InLiteral,    // true / false / null
        AfterValue,   // expecting , or ] or }
        Key,          // expecting a key string or }
        AfterKey,     // expecting :
    };
    enum class Ctx : uint8_t { Object, Array };

    bool fail(const char* why);
    bool push_ctx(Ctx c);
    bool pop_ctx(Ctx expect);
    bool emit_scalar();
    bool step(char c);

    StreamHandler& h_;
    St st_ = St::Value;
    Ctx stack_[STREAM_DEPTH_MAX] = {};
    std::size_t depth_ = 0;
    std::string tok_;
    std::string lit_;
    bool tok_is_key_ = false;
    bool seen_root_ = false;
    bool done_ = false;
    int uni_ = 0;
    uint32_t uni_val_ = 0;
    std::string err_;
};

}  // namespace json
}  // namespace swan
