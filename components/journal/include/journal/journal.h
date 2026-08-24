// The persistent event journal: what this display has actually DONE, kept
// across reboots so it can be read back as the device's history.
//
// Significant events only - countdowns started, reset, cancelled and reaching
// zero; faults and their recoveries; mode changes; boots.  Not general logging:
// that is the in-RAM LogRing next door, which is large, noisy and forgotten at
// power-off.  This one is small, quiet and permanent, and Phase 7 renders it as
// the Pearl station's printout, so every line has to be a real event rather
// than decoration.
//
// TWO RULES, both load-bearing:
//   1. A journal write must NEVER block the modes task.  Producers push a POD
//      into a queue with a zero timeout and move on; a low-priority writer task
//      batches them to LittleFS.  A full queue drops the event and counts it -
//      a stalled filesystem must not stop the display being a display.
//   2. It is bounded and wear-aware.  Appends are batched, the file is capped,
//      and rotation is a single compacting rewrite that keeps the newest half.
//
// The record format and the rotation policy are pure and host-tested; only the
// queue, the task and the file live in the IDF shell.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace swan {
namespace journal {

// Kept small and POD: it is copied by value into a FreeRTOS queue from the
// modes task, so it must not allocate and must not carry a std::string.
struct Event {
    enum class Kind : uint8_t {
        Boot,
        CountdownExecute,   // the Numbers were entered and accepted
        CountdownStart,     // started without the ritual (HA, automation)
        CountdownReset,
        CountdownCancel,
        CountdownZero,
        Fault,
        Recover,
        ModeChange,
        Maintenance,
        ColumnMode,
    };

    Kind kind = Kind::Boot;
    int64_t utc_s = 0;      // 0 = the clock had not synced; rendered as uptime
    uint32_t uptime_s = 0;
    int8_t column = -1;     // -1 = not about one column
    uint32_t seq = 0;       // the countdown's seq, where one applies
    char who[12] = {};      // set_by: ui | mqtt | cli | button | ha | unknown
    char detail[40] = {};   // numbers, fault cause, mode name, column modes
};

const char* kind_name(Event::Kind k);
bool kind_from_name(const char* s, Event::Kind& out);

// One JSONL line, newline included.  Deliberately compact - this file lives on
// a 2 MB partition shared with the web assets and the audio.
std::string encode(const Event& e);
// Tolerant of a truncated last line (a power cut mid-append) - that line is
// dropped rather than failing the whole file.
bool decode(const std::string& line, Event& out);

// Rotation policy, pure so the arithmetic is testable without a filesystem.
struct RotationPolicy {
    std::size_t max_entries = 400;    // hard cap on what the file holds
    std::size_t keep_entries = 200;   // what survives a rotation
    std::size_t max_bytes = 48 * 1024;
};

// Given the current entry count and byte size, does the file need compacting?
bool needs_rotation(const RotationPolicy& p, std::size_t entries, std::size_t bytes);

// Keep the newest `keep_entries` lines of `all`, in order.  Returns the text to
// write.  Splitting this out is what makes rotation testable: the shell only
// has to do temp-write-then-rename around it.
std::string compact(const RotationPolicy& p, const std::vector<std::string>& lines);

// ---------------------------------------------------------------------------
// The IDF shell (journal.cpp).  No-ops on the host.
// ---------------------------------------------------------------------------

// Mounts nothing itself - ring_store owns the LittleFS mount - and starts the
// writer task.  Safe to call before the clock is valid.
void init();

// Called from anywhere, including the modes task under its own lock.  Never
// blocks; returns false if the queue was full, which is counted and reported.
bool record(const Event& e);

// Convenience for the common shapes, so a caller never has to remember which
// fields belong together.
bool note_countdown(Event::Kind k, int64_t utc_s, uint32_t seq, const char* who,
                    const char* numbers);
bool note_fault(int64_t utc_s, int column, const char* cause);
bool note_recover(int64_t utc_s, int column, int attempts);
bool note_mode(int64_t utc_s, const char* mode);

// Newest last, at most `max_lines` (0 = all).  Reads the file, so it belongs on
// the HTTP task, not the modes task.
std::string read(std::size_t max_lines = 0);

struct Stats {
    std::size_t entries = 0;
    std::size_t bytes = 0;
    uint32_t dropped = 0;     // events the queue could not take
    uint32_t rotations = 0;
    bool writable = false;
};
Stats stats();

// The in-RAM log ring (spec 12), installed behind esp_log_set_vprintf.
void log_capture_start();
std::string log_read(std::size_t max_bytes = 0);
void log_clear();
struct LogStats {
    std::size_t lines = 0;
    std::size_t used = 0;
    std::size_t capacity = 0;
    uint32_t dropped = 0;
};
LogStats log_stats();

}  // namespace journal
}  // namespace swan
