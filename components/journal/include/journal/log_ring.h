// The log ring buffer (spec 12): the last few kilobytes of ESP_LOGx output,
// readable from the UI without a serial cable.
//
// Pure - no IDF includes - so the eviction and framing logic is host-tested.
// The IDF shell (journal.cpp) installs it behind esp_log_set_vprintf.
//
// SIZE, and why it is not "200 lines": spec 12 asked for ~200 lines, which at a
// realistic 90 characters is 18 KB of internal RAM.  PSRAM is deliberately off
// (sdkconfig.defaults), and the board reports ~90 KB free after boot, so 18 KB
// is a fifth of the headroom for a diagnostic.  This keeps a byte budget
// instead and reports how many lines fit - typically 80-110.  The spec is
// corrected rather than the number quietly missed.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace swan {
namespace journal {

// One line longer than this is truncated with a marker rather than dropped: a
// stack backtrace is worth having in mangled form.
inline constexpr std::size_t LOG_LINE_MAX = 160;

// A byte ring holding whole lines.  Writing a line that does not fit evicts
// whole lines from the oldest end until it does, so a read never returns a
// partial line.
class LogRing {
public:
    LogRing(char* storage, std::size_t capacity) : buf_(storage), cap_(capacity) {}

    // Append one line.  A trailing newline is normalised away; embedded ones
    // are kept, because an ESP_LOG line can carry a multi-line payload and
    // splitting it would lose the association.
    void push(const char* data, std::size_t len);

    // Oldest first, newline-separated.  `max_bytes` of 0 means everything.
    std::string read(std::size_t max_bytes = 0) const;

    std::size_t lines() const { return lines_; }
    std::size_t used() const { return used_; }
    std::size_t capacity() const { return cap_; }
    // Lines evicted since boot - so "the log starts here" is a fact rather than
    // an assumption.
    uint32_t dropped() const { return dropped_; }
    void clear();

private:
    void pop_oldest();

    char* buf_;
    std::size_t cap_;
    std::size_t head_ = 0;    // where the next byte is written
    std::size_t tail_ = 0;    // the oldest byte
    std::size_t used_ = 0;
    std::size_t lines_ = 0;
    uint32_t dropped_ = 0;
};

}  // namespace journal
}  // namespace swan
