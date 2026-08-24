// The log ring and the journal record format (spec 12, and the persistent
// event journal).  Both are pure; the queue, the task and the file are the IDF
// shell and are exercised on the board.
#include <string>
#include <vector>

#include "check.h"
#include "journal/journal.h"
#include "journal/log_ring.h"

using namespace swan;
using namespace swan::journal;

namespace {

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char c : s) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

void test_ring_keeps_whole_lines() {
    char store[256];
    LogRing r(store, sizeof store);
    for (int i = 0; i < 40; ++i) {
        const std::string line = "line " + std::to_string(i) + " of the log";
        r.push(line.data(), line.size());
    }
    const std::vector<std::string> lines = split(r.read());
    CHECK(!lines.empty());
    // Never a partial line, however many wraps it took.
    for (const std::string& l : lines) {
        CHECK(l.rfind("line ", 0) == 0);
        CHECK(l.find(" of the log") != std::string::npos);
    }
    // The NEWEST survive; the oldest are the ones evicted.
    CHECK(lines.back() == "line 39 of the log");
    CHECK(r.dropped() > 0);
    CHECK(r.used() <= r.capacity());
}

void test_ring_read_returns_the_newest_when_capped() {
    char store[512];
    LogRing r(store, sizeof store);
    for (int i = 0; i < 10; ++i) {
        const std::string line = "entry-" + std::to_string(i);
        r.push(line.data(), line.size());
    }
    // A capped read is a log VIEW: it must show the end of the log, not the
    // beginning.  Returning the oldest bytes and calling it "the log" would be
    // the wrong half, and would look plausible.
    const std::vector<std::string> tail = split(r.read(30));
    CHECK(!tail.empty());
    CHECK(tail.back() == "entry-9");
    CHECK(tail.front() != "entry-0");
    // Uncapped is everything, oldest first.
    const std::vector<std::string> all = split(r.read());
    CHECK_EQ(all.size(), 10u);
    CHECK(all.front() == "entry-0");
    CHECK(all.back() == "entry-9");
}

void test_ring_truncates_rather_than_drops_a_long_line() {
    char store[1024];
    LogRing r(store, sizeof store);
    const std::string huge(LOG_LINE_MAX * 2, 'x');
    r.push(huge.data(), huge.size());
    const std::vector<std::string> lines = split(r.read());
    CHECK_EQ(lines.size(), 1u);
    // A mangled backtrace is worth more than no backtrace.
    CHECK(lines[0].size() <= LOG_LINE_MAX);
    CHECK(lines[0].find("...") != std::string::npos);
}

void test_ring_ignores_empty_and_trailing_newlines() {
    char store[256];
    LogRing r(store, sizeof store);
    r.push("", 0);
    r.push("\n", 1);
    CHECK_EQ(r.lines(), 0u);
    r.push("with newline\n", 13);
    const std::vector<std::string> lines = split(r.read());
    CHECK_EQ(lines.size(), 1u);
    CHECK(lines[0] == "with newline");
}

void test_record_round_trip() {
    Event e{};
    e.kind = Event::Kind::CountdownExecute;
    e.utc_s = 1787541319;
    e.uptime_s = 412;
    e.seq = 7;
    std::snprintf(e.who, sizeof e.who, "ui");
    std::snprintf(e.detail, sizeof e.detail, "4 8 15 16 23 42");

    const std::string line = encode(e);
    CHECK(line.back() == '\n');
    Event back{};
    CHECK(decode(line, back));
    CHECK(back.kind == Event::Kind::CountdownExecute);
    CHECK_EQ(back.utc_s, 1787541319);
    CHECK_EQ(back.uptime_s, 412u);
    CHECK_EQ(back.seq, 7u);
    CHECK_STREQ(back.who, "ui");
    CHECK_STREQ(back.detail, "4 8 15 16 23 42");

    // Every kind names itself, and every name resolves back.
    for (const Event::Kind k : {Event::Kind::Boot, Event::Kind::CountdownExecute,
                                Event::Kind::CountdownStart, Event::Kind::CountdownReset,
                                Event::Kind::CountdownCancel, Event::Kind::CountdownZero,
                                Event::Kind::Fault, Event::Kind::Recover,
                                Event::Kind::ModeChange, Event::Kind::Maintenance,
                                Event::Kind::ColumnMode}) {
        Event::Kind got{};
        CHECK(kind_from_name(kind_name(k), got));
        CHECK(got == k);
    }
}

void test_a_truncated_last_line_is_dropped_not_fatal() {
    // What a power cut mid-append leaves behind.  The format is one
    // self-contained object per line precisely so this costs one entry.
    Event e{};
    CHECK(!decode("{\"t\":1787541319,\"e\":\"exec", e));
    CHECK(!decode("", e));
    CHECK(!decode("{}", e));                       // no kind
    CHECK(!decode("{\"e\":\"nonsense\"}", e));     // unknown kind
    CHECK(decode("{\"t\":5,\"e\":\"zero\"}", e));  // minimal but valid
    CHECK(e.kind == Event::Kind::CountdownZero);
}

void test_rotation_keeps_the_newest() {
    RotationPolicy p;
    p.max_entries = 10;
    p.keep_entries = 4;
    p.max_bytes = 1000;

    CHECK(!needs_rotation(p, 9, 100));
    CHECK(needs_rotation(p, 10, 100));
    CHECK(needs_rotation(p, 3, 1000));   // the byte cap bites independently

    std::vector<std::string> lines;
    for (int i = 0; i < 10; ++i) lines.push_back("{\"e\":\"mode\",\"d\":\"" + std::to_string(i) + "\"}");
    const std::vector<std::string> kept = split(compact(p, lines));
    CHECK_EQ(kept.size(), 4u);
    CHECK(kept.front().find("\"6\"") != std::string::npos);
    CHECK(kept.back().find("\"9\"") != std::string::npos);

    // Fewer entries than the keep target is not an error.
    const std::vector<std::string> few = {lines[0], lines[1]};
    CHECK_EQ(split(compact(p, few)).size(), 2u);
    CHECK(compact(p, {}).empty());
}

void test_an_event_is_small_enough_to_be_worth_keeping() {
    // Every entry lands on a 2 MB partition shared with the web assets and the
    // audio, and 400 of them have to be affordable.
    Event e{};
    e.kind = Event::Kind::CountdownExecute;
    e.utc_s = 1787541319;
    e.uptime_s = 99999;
    e.seq = 42;
    std::snprintf(e.who, sizeof e.who, "mqtt");
    std::snprintf(e.detail, sizeof e.detail, "4 8 15 16 23 42");
    const std::size_t n = encode(e).size();
    std::printf("  a full execute entry is %u bytes; 400 of them is %u KB\n",
                static_cast<unsigned>(n), static_cast<unsigned>(n * 400 / 1024));
    CHECK(n < 110);
    CHECK(n * 400 < 48 * 1024);   // fits inside the byte cap with room to spare
}

}  // namespace

void run_tests() {
    test_ring_keeps_whole_lines();
    test_ring_read_returns_the_newest_when_capped();
    test_ring_truncates_rather_than_drops_a_long_line();
    test_ring_ignores_empty_and_trailing_newlines();
    test_record_round_trip();
    test_a_truncated_last_line_is_dropped_not_fatal();
    test_rotation_keeps_the_newest();
    test_an_event_is_small_enough_to_be_worth_keeping();
}
