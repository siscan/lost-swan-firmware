// Trace generator for the browser simulator (spec 14): runs the REAL
// ModeManager + FrameScheduler through scripted scenarios and records every
// motion command, cue and mode change with timestamps.  The simulator page
// replays these - so what the page shows is what the firmware logic actually
// did, not a JavaScript re-implementation of it.
//
// Usage: gen_traces <path/to/ring.json> <path/to/output/traces.js>
// The output embeds ring.json verbatim (the page opens from file://, so no
// fetch).  Committed artifact; regenerate after mode/frame changes:
//     .\test-host.ps1  (builds it)   then   build\host\gen_traces data\ring.json web\sim\traces.js
#include <cstdio>
#include <string>
#include <vector>

#include "fake_port.h"

using namespace swan;
using namespace swan::testfakes;

namespace {

struct Event {
    int64_t t;  // ms since scenario start
    std::string json;
};

struct Recorder {
    std::vector<Event> events;
    int64_t t0 = 0;

    void add(int64_t at, const std::string& j) { events.push_back({at - t0, j}); }
};

// A port that records into the trace while keeping FakePort's instant-settle
// behaviour so the mode logic advances.
struct TracePort final : MotionPort {
    FakePort inner;
    Recorder* rec = nullptr;
    int32_t flaps = 15;

    Col col(int i) override { return inner.col(i); }

    bool go(int i, int index) override {
        char buf[96];
        std::snprintf(buf, sizeof buf, "{\"e\":\"go\",\"col\":%d,\"idx\":%d,\"flaps\":%ld}", i,
                      index, static_cast<long>(flaps));
        rec->add(inner.now_ms, buf);
        return inner.go(i, index);
    }

    bool spin(int i, int32_t flaps_s, int seconds) override {
        char buf[96];
        std::snprintf(buf, sizeof buf, "{\"e\":\"spin\",\"col\":%d,\"flaps\":%ld,\"secs\":%d}", i,
                      static_cast<long>(flaps_s), seconds);
        rec->add(inner.now_ms, buf);
        return inner.spin(i, flaps_s, seconds);
    }
};

struct TraceCues final : CueSink {
    Recorder* rec = nullptr;
    const FakePort* port = nullptr;
    void on_cue(Cue c) override {
        const char* n = c == Cue::Warn4Min ? "warn_4min"
                        : c == Cue::Warn1Min ? "warn_1min"
                                             : "system_failure";
        rec->add(port->now_ms, std::string("{\"e\":\"cue\",\"name\":\"") + n + "\"}");
    }
};

struct Rig {
    RingSet ring = RingSet::compiled_fallback();
    TracePort port;
    FrameScheduler sched{port, {15, 82000}};
    FakeTime time;
    FakeStore store;
    TraceCues cues;
    ModeManager mm{ring, sched, time, store, cues};
    Recorder rec;
    Mode last_mode = Mode::Clock;

    Rig() {
        port.rec = &rec;
        cues.rec = &rec;
        cues.port = &port.inner;
        mm.set_config(ModesConfig{});
        mm.set_tz("PST8PDT,M3.2.0,M11.1.0");
    }

    void begin(int64_t utc_ms) {
        time.utc_ms = utc_ms;
        port.inner.now_ms = utc_ms;
        rec.t0 = utc_ms;
        mm.begin(utc_ms);
        // Always record the starting mode - note_mode()'s empty-events check
        // is defeated by the go events begin() itself records.
        last_mode = mm.mode();
        rec.add(port.inner.now_ms,
                std::string("{\"e\":\"mode\",\"name\":\"") + mode_name(last_mode) + "\"}");
    }

    void note_mode() {
        if (rec.events.empty() || mm.mode() != last_mode) {
            last_mode = mm.mode();
            rec.add(port.inner.now_ms,
                    std::string("{\"e\":\"mode\",\"name\":\"") + mode_name(last_mode) + "\"}");
        }
    }

    void run_for(int64_t ms, int64_t step = 100) {
        const int64_t until = time.utc_ms + ms;
        while (time.utc_ms < until) {
            time.utc_ms += step;
            port.inner.now_ms = time.utc_ms;
            mm.tick(time.utc_ms);
            note_mode();
        }
    }
};

std::string read_all(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return {};
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

int64_t base_utc() {
    // 2026-01-15 18:57:55Z = 10:57:55 PST - the 11:00 quarter boundary is two
    // minutes away, and 11:15 follows inside the clock scenario's window.
    return (TimeZone::days_from_civil(2026, 1, 15) * 86400 + 18 * 3600 + 57 * 60 + 55) * 1000;
}

void emit(std::FILE* out, const char* name, const char* desc, const Recorder& rec) {
    std::fprintf(out, "  \"%s\": {\n    \"desc\": \"%s\",\n    \"events\": [\n", name, desc);
    for (size_t i = 0; i < rec.events.size(); ++i) {
        std::fprintf(out, "      {\"t\": %lld, %s%s\n",
                     static_cast<long long>(rec.events[i].t), rec.events[i].json.c_str() + 1,
                     i + 1 < rec.events.size() ? "," : "");
    }
    std::fprintf(out, "    ]\n  }");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: gen_traces <ring.json> <traces.js>\n");
        return 2;
    }
    const std::string ring_json = read_all(argv[1]);
    if (ring_json.empty()) {
        std::fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }

    std::FILE* out = std::fopen(argv[2], "wb");
    if (out == nullptr) {
        std::fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }

    std::fprintf(out,
                 "// GENERATED by test/host/gen_traces.cpp - DO NOT EDIT.\n"
                 "// Traces recorded from the REAL ModeManager/FrameScheduler.\n"
                 "window.SWAN_RING = %s;\n\nwindow.SWAN_TRACES = {\n",
                 ring_json.c_str());

    {  // Clock at the default 15-minute granularity: the hour rollover and
       // the following quarter.  The rings descend, so a clock tick is the
       // expensive direction - hence the granularity (spec 7.1 wear table).
        Rig r;
        r.begin(base_utc());
        r.run_for(19 * 60 * 1000);
        emit(out, "clock", "Clock at 15-min granularity: 11:00 then 11:15", r.rec);
        std::fprintf(out, ",\n");
    }
    {  // Countdown in seconds mode: live one-flip ticks on column 5 and the
       // 16-flip 0->9 wrap onto its second digit block.
        Rig r;
        r.begin(base_utc());
        r.mm.cmd_countdown_execute(ModeManager::THE_NUMBERS, r.time.utc_ms);
        r.note_mode();
        r.run_for(25 * 1000);
        emit(out, "countdown", "MMM:SS live seconds, incl. the 16-flip 0->9 wrap", r.rec);
        std::fprintf(out, ",\n");
    }
    {  // The same countdown in the original MMM:S0 scheme, for comparison.
        Rig r;
        ModesConfig cfg;
        cfg.seconds_mode = SecondsMode::Tens;
        r.mm.set_config(cfg);
        r.begin(base_utc());
        r.mm.cmd_countdown_start(r.time.utc_ms);
        r.note_mode();
        r.run_for(35 * 1000);
        emit(out, "tens", "Countdown MMM:S0 - column 5 parked, 10 s windows", r.rec);
        std::fprintf(out, ",\n");
    }
    {  // Zero choreography with a named-glyph reveal.
        Rig r;
        ModesConfig cfg;
        const RingTable& t = r.ring.col(0);
        cfg.reveal = {t.index_for_token("eye"), t.index_for_token("ankh"),
                      t.index_for_token("qmark"), t.index_for_token("scarab"),
                      t.index_for_token("duat")};
        r.mm.set_config(cfg);
        r.begin(base_utc());
        r.mm.cmd_countdown_set_target(r.time.utc_ms / 1000 + 15, r.time.utc_ms);
        r.note_mode();
        r.run_for(28 * 1000);
        emit(out, "zero", "Last 15 s, zero, alarm spin, hieroglyph reveal", r.rec);
        std::fprintf(out, ",\n");
    }
    {  // Message + the ????? preset.
        Rig r;
        r.begin(base_utc());
        std::array<std::string, N_COLUMNS> toks = {"eye", "_", "ankh", "_", "scarab"};
        r.mm.cmd_message_set(toks, 8, false, r.time.utc_ms);
        r.note_mode();
        r.run_for(10 * 1000);  // dwell expires back to clock
        r.mm.cmd_preset("qmarks", r.time.utc_ms);
        r.note_mode();
        r.run_for(3 * 1000);
        emit(out, "message", "Glyph message, dwell back to clock, ????? preset", r.rec);
        std::fprintf(out, ",\n");
    }
    {  // Boot without WiFi: blank, then the glyph on the centre column.
        Rig r;
        r.time.is_valid = false;
        r.begin(base_utc());
        r.run_for(20 * 1000);
        r.time.is_valid = true;  // sync lands; clock takes over
        r.run_for(5 * 1000);
        emit(out, "wifi", "No-signal boot: blank, WiFi glyph at 15 s, then sync", r.rec);
        std::fprintf(out, "\n");
    }

    std::fprintf(out, "};\n");
    std::fclose(out);
    std::printf("wrote %s\n", argv[2]);
    return 0;
}
