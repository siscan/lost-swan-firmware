// Modes (spec 7): clock rendering across DST edges, 12/24 and granularity;
// the WiFi glyph; the deadline countdown with cue timing, seconds/tens mode
// and zero choreography; persistence and resume; presets; arbitration.
//
// Assertions read the rendered CHARACTERS, not slot indices - column 5 has two
// slots per digit and which one is used depends on where the column was, so an
// index assertion would be pinning an implementation detail.
#include <cstring>
#include <string>

#include "check.h"
#include "fake_port.h"

using namespace swan;
using namespace swan::testfakes;

namespace {

int64_t utc_ms(int y, int mo, int d, int h, int mi, int s) {
    return (TimeZone::days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s) * 1000;
}

// One bundle of everything a ModeManager needs, host-faked.
struct Rig {
    RingSet ring = RingSet::compiled_fallback();
    FakePort port;
    FrameScheduler sched{port, {15, 82000}};
    FakeTime time;
    FakeStore store;
    FakeCues cues;
    ModeManager mm{ring, sched, time, store, cues};

    Rig() { configure(ModesConfig{}); }

    void configure(ModesConfig c) {
        mm.set_config(c);
        CHECK(mm.set_tz("PST8PDT,M3.2.0,M11.1.0"));
    }

    void run_to(int64_t until_ms, int64_t step_ms = 100) {
        while (time.utc_ms < until_ms) {
            time.utc_ms += step_ms;
            port.now_ms = time.utc_ms;
            cues.now_ms = time.utc_ms;
            mm.tick(time.utc_ms);
        }
    }

    void begin_at(int64_t ms) {
        time.utc_ms = ms;
        port.now_ms = ms;
        cues.now_ms = ms;
        mm.begin(ms);
    }

    // What the drums are showing, as characters: "AM|blank|9|4|1".
    std::string faces() const {
        std::string out;
        for (int i = 0; i < N_COLUMNS; ++i) {
            const int idx = port.cols[static_cast<size_t>(i)].index;
            if (!out.empty()) out += '|';
            out += (idx < 0) ? "?" : ring.col(i).slot(idx).id;
        }
        return out;
    }
};

void expect_faces(const Rig& r, const char* want, int line) {
    const std::string got = r.faces();
    if (got != want) {
        ++g_failures;
        std::printf("FAIL %s:%d  faces \"%s\", expected \"%s\"\n", __FILE__, line, got.c_str(),
                    want);
    }
}
#define FACES(rig, want) expect_faces((rig), (want), __LINE__)

ModesConfig cfg_minutely() {
    ModesConfig c;
    c.granularity_min = 1;  // the clock tests want minute precision
    return c;
}

// --------------------------------------------------------------------------
// Clock
// --------------------------------------------------------------------------
void test_clock_12h() {
    Rig r;
    r.configure(cfg_minutely());
    // 2026-01-15 17:41 UTC = 09:41 PST.
    r.begin_at(utc_ms(2026, 1, 15, 17, 41, 0));
    CHECK(r.mm.mode() == Mode::Clock);
    FACES(r, "AM|blank|9|4|1");

    r.run_to(utc_ms(2026, 1, 15, 20, 0, 0));  // 12:00 PST
    FACES(r, "PM|1|2|0|0");

    r.run_to(utc_ms(2026, 1, 16, 8, 0, 0));   // 00:00 PST -> 12 AM
    FACES(r, "AM|1|2|0|0");

    // Renders happen on the minute: nothing new in between.
    const size_t n = r.port.gos.size();
    r.run_to(r.time.utc_ms + 59 * 1000);
    CHECK_EQ(r.port.gos.size(), n);
}

void test_clock_24h() {
    Rig r;
    ModesConfig c = cfg_minutely();
    c.h24 = true;
    r.configure(c);
    r.begin_at(utc_ms(2026, 1, 15, 17, 5, 0));  // 09:05 PST
    FACES(r, "blank|0|9|0|5");                  // leading zero, no AM/PM

    r.mm.cmd_clock_format(false, r.time.utc_ms);
    FACES(r, "AM|blank|9|0|5");
}

void test_clock_dst_edges() {
    Rig r;
    r.configure(cfg_minutely());
    // Spring forward: 01:59 PST -> 03:00 PDT (09:59Z -> 10:00Z Mar 8).
    r.begin_at(utc_ms(2026, 3, 8, 9, 59, 0));
    FACES(r, "AM|blank|1|5|9");
    r.run_to(utc_ms(2026, 3, 8, 10, 0, 0));
    FACES(r, "AM|blank|3|0|0");  // 2:xx never exists

    // Fall back: 01:59 PDT -> 01:00 PST (08:59Z -> 09:00Z Nov 1).
    Rig r2;
    r2.configure(cfg_minutely());
    r2.begin_at(utc_ms(2026, 11, 1, 8, 59, 0));
    FACES(r2, "AM|blank|1|5|9");
    r2.run_to(utc_ms(2026, 11, 1, 9, 0, 0));
    FACES(r2, "AM|blank|1|0|0");
}

// The descending rings make a clock tick the expensive direction, so the
// display is floored to clock.granularity_min (spec 7.1 wear table).
void test_clock_granularity() {
    Rig r;  // default config: 15 minutes
    CHECK_EQ(r.mm.config().granularity_min, 15);
    r.begin_at(utc_ms(2026, 1, 15, 17, 7, 30));  // 09:07:30 PST
    FACES(r, "AM|blank|9|0|0");                  // floored to :00

    r.run_to(utc_ms(2026, 1, 15, 17, 14, 59));
    FACES(r, "AM|blank|9|0|0");                  // still :00 - no flips at all
    const size_t n = r.port.gos.size();
    r.run_to(utc_ms(2026, 1, 15, 17, 15, 0));
    FACES(r, "AM|blank|9|1|5");                  // :15
    CHECK(r.port.gos.size() > n);

    r.run_to(utc_ms(2026, 1, 15, 17, 45, 0));
    FACES(r, "AM|blank|9|4|5");
    r.run_to(utc_ms(2026, 1, 15, 18, 0, 0));
    FACES(r, "AM|1|0|0|0");                      // 10:00

    // A 30-minute grid never moves column 5 at all: the ones digit is always 0.
    Rig r30;
    ModesConfig c30;
    c30.granularity_min = 30;
    r30.configure(c30);
    r30.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));
    const int col5_start = r30.port.cols[4].index;      // after the entry render
    const int col5_gos = r30.port.gos_for(4);
    r30.run_to(utc_ms(2026, 1, 15, 19, 0, 0));          // two hours
    CHECK_EQ(r30.port.cols[4].index, col5_start);       // never moves again
    CHECK_EQ(r30.port.gos_for(4), col5_gos);
}

void test_wifi_glyph() {
    Rig r;
    r.time.is_valid = false;
    r.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));
    CHECK(!r.mm.wifi_glyph_shown());
    FACES(r, "blank|blank|blank|blank|blank");

    r.run_to(r.time.utc_ms + 16 * 1000);
    CHECK(r.mm.wifi_glyph_shown());
    FACES(r, "blank|blank|wifi|blank|blank");  // CENTRE column only

    // Sync arrives; the glyph never returns (valid is sticky, spec 8).
    r.time.is_valid = true;
    r.run_to(r.time.utc_ms + 61 * 1000);
    CHECK(!r.mm.wifi_glyph_shown());
    CHECK(r.faces().find("wifi") == std::string::npos);
}

// --------------------------------------------------------------------------
// Countdown
// --------------------------------------------------------------------------
void test_countdown_seconds_mode() {
    Rig r;  // default: SecondsMode::Seconds
    r.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));
    const int64_t t0 = r.time.utc_ms;

    CHECK(!r.mm.cmd_countdown_execute("4 8 15 16 23 43", t0).ok);  // wrong Numbers
    CHECK(r.mm.cmd_countdown_execute(" 4  8 15 16 23 42 ", t0).ok);
    CHECK(r.mm.cd_phase() == CdPhase::Running);
    // 108:00 is the idle face; a running countdown holds it only for the start
    // instant before the first window lands (floor semantics, spec 7.3).
    r.run_to(t0 + 500);
    FACES(r, "1|0|7|5|9");

    // Live seconds: one flip per second on column 5.  Sampled mid-window -
    // frames land ON the boundary by design, so a boundary-instant sample sees
    // the incoming value.
    r.run_to(t0 + 1500);
    FACES(r, "1|0|7|5|8");
    r.run_to(t0 + 4500);
    FACES(r, "1|0|7|5|5");

    // Every one-second step costs column 5 exactly one flip - the whole point
    // of the descending ring.
    const int start = r.port.cols[4].index;
    r.run_to(t0 + 8500);
    CHECK_EQ(ring_forward_distance(start, r.port.cols[4].index), 4);

    // The 0->9 wrap uses the second digit block: 16 flips, not the 41 a
    // single-block ring would cost.  Asserted on the flip COST rather than
    // frame timing: 16 flips need ~1.1 s at 15 flaps/s, more than the
    // one-second window, so the scheduler correctly starts that frame early
    // (spec 7.3 timing note) and it lands a little late.
    int at_zero = -1, at_nine = -1;
    for (int64_t t = t0 + 8500; t <= t0 + 13000 && at_nine < 0; t += 100) {
        r.run_to(t);
        const int slot = r.port.cols[4].index;
        const std::string face = r.ring.col(4).slot(slot).id;
        if (face == "0" && at_zero < 0) at_zero = slot;
        if (face == "9" && at_zero >= 0) at_nine = slot;
    }
    CHECK(at_zero >= 0);
    CHECK(at_nine >= 0);
    if (at_zero >= 0 && at_nine >= 0) {
        CHECK_EQ(ring_forward_distance(at_zero, at_nine), 16);
    }
    // The tens column crossed the same boundary with a single flip.
    FACES(r, "1|0|7|4|9");
}

void test_countdown_tens_mode() {
    Rig r;
    ModesConfig c;
    c.seconds_mode = SecondsMode::Tens;
    r.configure(c);
    r.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));
    const int64_t t0 = r.time.utc_ms;
    CHECK(r.mm.cmd_countdown_start(t0).ok);
    // 108:00 holds only for the start instant; the first 10 s window lands
    // immediately after (floor semantics, spec 7.3).
    r.run_to(t0 + 500);
    FACES(r, "1|0|7|5|0");

    // MMM:S0 - column 5 parks on 0 and never moves.
    const int col5 = r.port.cols[4].index;
    r.run_to(t0 + 25 * 1000);   // remaining 6455 -> floor to 6450 = 107:30
    FACES(r, "1|0|7|3|0");
    CHECK_EQ(r.port.cols[4].index, col5);
}

void test_countdown_cues_and_zero() {
    Rig r;
    r.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));
    const int64_t target = r.time.utc_ms / 1000 + 300;
    CHECK(r.mm.cmd_countdown_set_target(target, r.time.utc_ms).ok);

    const int64_t target_ms = target * 1000;
    r.run_to(target_ms + 15 * 1000);

    CHECK_EQ(r.cues.recs.size(), 3u);
    CHECK(r.cues.at(Cue::Warn4Min) >= target_ms - 240100 &&
          r.cues.at(Cue::Warn4Min) <= target_ms - 239800);
    CHECK(r.cues.at(Cue::Warn1Min) >= target_ms - 60100 &&
          r.cues.at(Cue::Warn1Min) <= target_ms - 59800);
    CHECK(r.cues.at(Cue::SystemFailure) >= target_ms - 100 &&
          r.cues.at(Cue::SystemFailure) <= target_ms + 200);

    CHECK(r.mm.cd_phase() == CdPhase::Reveal);
    CHECK_EQ(r.port.spins.size(), static_cast<size_t>(N_COLUMNS));
    const int64_t spin_at = r.port.spins[0].at_ms;
    CHECK(spin_at >= target_ms + 2800 && spin_at <= target_ms + 3200);
    CHECK_EQ(r.port.spins[0].flaps_s, 25);
    FACES(r, "blank|blank|blank|blank|blank");  // reveal unset -> blanks

    r.run_to(r.time.utc_ms + 120 * 1000);
    CHECK(r.mm.mode() == Mode::Countdown);  // no auto-return by default
    CHECK(r.mm.cmd_countdown_execute(ModeManager::THE_NUMBERS, r.time.utc_ms).ok);
    CHECK(r.mm.cd_phase() == CdPhase::Running);
}

void test_countdown_reveal_and_timeout() {
    Rig r;
    ModesConfig c;
    // Reveal glyphs by name, resolved per column - column 5's ring differs.
    const RingSet rs = RingSet::compiled_fallback();
    c.reveal = {rs.col(0).index_for_token("eye"), rs.col(1).index_for_token("ankh"),
                rs.col(2).index_for_token("qmark"), rs.col(3).index_for_token("scarab"),
                rs.col(4).index_for_token("duat")};
    c.failure_timeout_s = 5;
    r.configure(c);
    r.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));

    const int64_t target = r.time.utc_ms / 1000 + 20;
    CHECK(r.mm.cmd_countdown_set_target(target, r.time.utc_ms).ok);
    r.run_to(target * 1000 + (3 + 6 + 1) * 1000);
    CHECK(r.mm.cd_phase() == CdPhase::Reveal);
    FACES(r, "eye|ankh|qmark|scarab|duat");

    r.run_to(target * 1000 + (3 + 6 + 6) * 1000);
    CHECK(r.mm.mode() == Mode::Clock);
    CHECK(r.mm.cd_phase() == CdPhase::Idle);
}

void test_countdown_persistence_and_resume() {
    Rig r;
    r.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));
    const int64_t t0 = r.time.utc_ms;
    CHECK(r.mm.cmd_countdown_start(t0).ok);
    CHECK(r.store.stored.phase == CdPhase::Running);
    const int64_t target = r.store.stored.target_utc;

    // Reboot with 90 s left: resumes Running and does NOT replay the 4-minute
    // warning, but still fires the 1-minute one when its moment comes.
    Rig r3;
    r3.store = r.store;
    ModeManager mm3{r3.ring, r3.sched, r3.time, r3.store, r3.cues};
    mm3.set_config(ModesConfig{});
    CHECK(mm3.set_tz("PST8PDT,M3.2.0,M11.1.0"));
    r3.time.utc_ms = (target - 90) * 1000;
    r3.port.now_ms = r3.time.utc_ms;
    mm3.begin(r3.time.utc_ms);
    CHECK(mm3.cd_phase() == CdPhase::Running);
    while (r3.time.utc_ms < (target - 70) * 1000) {
        r3.time.utc_ms += 100;
        r3.port.now_ms = r3.time.utc_ms;
        r3.cues.now_ms = r3.time.utc_ms;
        mm3.tick(r3.time.utc_ms);
    }
    CHECK(!r3.cues.fired(Cue::Warn4Min));
    while (r3.time.utc_ms < (target - 50) * 1000) {
        r3.time.utc_ms += 100;
        r3.port.now_ms = r3.time.utc_ms;
        r3.cues.now_ms = r3.time.utc_ms;
        mm3.tick(r3.time.utc_ms);
    }
    CHECK(r3.cues.fired(Cue::Warn1Min));

    CHECK(r.mm.cmd_countdown_cancel(r.time.utc_ms).ok);
    CHECK(r.store.stored.phase == CdPhase::Idle);
    FACES(r, "1|0|8|0|0");  // static 108:00
}

// --------------------------------------------------------------------------
// Arbitration
// --------------------------------------------------------------------------
void test_arbitration() {
    Rig r;
    r.begin_at(utc_ms(2026, 1, 15, 17, 0, 30));

    std::array<std::string, N_COLUMNS> toks = {"ankh", "_", "qmark", "eye", "4"};
    CHECK(r.mm.cmd_message_set(toks, 2, false, r.time.utc_ms).ok);
    CHECK(r.mm.mode() == Mode::Message);
    FACES(r, "ankh|blank|qmark|eye|4");

    r.run_to(r.time.utc_ms + 2500);
    CHECK(r.mm.mode() == Mode::Clock);

    CHECK(r.mm.cmd_message_set(toks, 2, true, r.time.utc_ms).ok);
    r.run_to(r.time.utc_ms + 10 * 1000);
    CHECK(r.mm.mode() == Mode::Message);  // hold

    // A token absent from that column's ring is rejected: "cycle" exists on
    // ring A but was dropped from column 5.
    std::array<std::string, N_COLUMNS> bad = {"_", "_", "_", "_", "cycle"};
    CHECK(!r.mm.cmd_message_set(bad, 0, false, r.time.utc_ms).ok);
    CHECK(r.mm.mode() == Mode::Message);

    CHECK(r.mm.cmd_countdown_start(r.time.utc_ms).ok);
    CHECK(r.mm.mode() == Mode::Countdown);

    Frame raw;
    for (int i = 0; i < N_COLUMNS; ++i) raw.idx[static_cast<size_t>(i)] = 13;
    CHECK(r.mm.cmd_display_frame(raw, r.time.utc_ms).ok);
    CHECK(r.mm.mode() == Mode::Countdown);
    Frame bad_raw = raw;
    bad_raw.idx[4] = 99;
    CHECK(!r.mm.cmd_display_frame(bad_raw, r.time.utc_ms).ok);

    CHECK(r.mm.cmd_preset("qmarks", r.time.utc_ms).ok);
    FACES(r, "qmark|qmark|qmark|qmark|qmark");
    CHECK(!r.mm.cmd_preset("nope", r.time.utc_ms).ok);

    CHECK(r.mm.cmd_mode_set(Mode::Clock, r.time.utc_ms).ok);
    CHECK(r.mm.mode() == Mode::Clock);
}

void test_numbers_validation() {
    CHECK(ModeManager::numbers_valid("4 8 15 16 23 42"));
    CHECK(ModeManager::numbers_valid("  4\t8  15 16 23 42  "));
    CHECK(!ModeManager::numbers_valid("4 8 15 16 23"));
    CHECK(!ModeManager::numbers_valid("4 8 15 16 23 42 108"));
    CHECK(!ModeManager::numbers_valid("4 8 15 16 42 23"));
    CHECK(!ModeManager::numbers_valid("48 15 16 23 42"));
    CHECK(!ModeManager::numbers_valid(""));
    CHECK(!ModeManager::numbers_valid("hurley"));
}

// --------------------------------------------------------------------------
// Regressions from the phase 2 adversarial review.
// --------------------------------------------------------------------------
void test_finished_countdown_never_replays() {
    Rig r;
    r.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));
    const int64_t target = r.time.utc_ms / 1000 + 20;
    CHECK(r.mm.cmd_countdown_set_target(target, r.time.utc_ms).ok);
    r.run_to(target * 1000 + 12 * 1000);
    CHECK(r.mm.cd_phase() == CdPhase::Reveal);

    Rig r2;
    r2.store = r.store;
    ModeManager mm2{r2.ring, r2.sched, r2.time, r2.store, r2.cues};
    mm2.set_config(ModesConfig{});
    r2.time.utc_ms = (target + 2 * 86400) * 1000;
    r2.port.now_ms = r2.time.utc_ms;
    mm2.begin(r2.time.utc_ms);
    CHECK(mm2.cd_phase() == CdPhase::Reveal);
    CHECK_EQ(r2.cues.recs.size(), 0u);   // no alarm replay
    CHECK_EQ(r2.port.spins.size(), 0u);  // no spin replay

    CHECK(mm2.cmd_mode_set(Mode::Clock, r2.time.utc_ms).ok);
    CHECK(r2.store.stored.phase == CdPhase::Idle);

    Rig r3;
    r3.store = r2.store;
    ModeManager mm3{r3.ring, r3.sched, r3.time, r3.store, r3.cues};
    mm3.set_config(ModesConfig{});
    r3.time.utc_ms = (target + 3 * 86400) * 1000;
    r3.port.now_ms = r3.time.utc_ms;
    mm3.begin(r3.time.utc_ms);
    CHECK(mm3.mode() == Mode::Clock);
}

void test_time_validity_gating() {
    Rig r;
    r.time.is_valid = false;
    r.begin_at(1000);
    CHECK(!r.mm.cmd_countdown_start(r.time.utc_ms).ok);
    CHECK(!r.mm.cmd_countdown_execute(ModeManager::THE_NUMBERS, r.time.utc_ms).ok);
    CHECK(!r.mm.cmd_countdown_set_target(1787000000, r.time.utc_ms).ok);
    CHECK(r.mm.mode() == Mode::Clock);

    Rig r2;
    r2.store.have = true;
    r2.store.stored = {CdPhase::Running, utc_ms(2026, 1, 15, 18, 0, 0) / 1000 + 300, 1};
    r2.time.is_valid = false;
    r2.begin_at(1000);
    CHECK(r2.mm.mode() == Mode::Clock);  // deferred, not mis-derived

    r2.time.is_valid = true;
    r2.time.utc_ms = utc_ms(2026, 1, 15, 18, 0, 0);
    r2.port.now_ms = r2.time.utc_ms;
    r2.mm.tick(r2.time.utc_ms);
    CHECK(r2.mm.mode() == Mode::Countdown);
    CHECK(r2.mm.cd_phase() == CdPhase::Running);
    CHECK_EQ(r2.cues.recs.size(), 0u);
}

void test_time_step_preserves_message_dwell() {
    Rig r;
    r.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));
    std::array<std::string, N_COLUMNS> toks = {"ankh", "_", "_", "_", "eye"};
    CHECK(r.mm.cmd_message_set(toks, 60, false, r.time.utc_ms).ok);
    r.run_to(r.time.utc_ms + 10 * 1000);
    CHECK(r.mm.mode() == Mode::Message);

    r.time.utc_ms += 2 * 3600 * 1000;  // a two-hour resync step
    r.port.now_ms = r.time.utc_ms;
    r.mm.tick(r.time.utc_ms);
    CHECK(r.mm.mode() == Mode::Message);
    r.run_to(r.time.utc_ms + 40 * 1000);
    CHECK(r.mm.mode() == Mode::Message);  // 50 of 60 s remain
    r.run_to(r.time.utc_ms + 15 * 1000);
    CHECK(r.mm.mode() == Mode::Clock);
}

void test_mode_set_message_requires_message() {
    Rig r;
    r.begin_at(utc_ms(2026, 1, 15, 17, 0, 30));
    CHECK(!r.mm.cmd_mode_set(Mode::Message, r.time.utc_ms).ok);
    CHECK(r.mm.mode() == Mode::Clock);

    std::array<std::string, N_COLUMNS> toks = {"eye", "_", "_", "_", "_"};
    CHECK(r.mm.cmd_message_set(toks, 0, true, r.time.utc_ms).ok);
    CHECK(r.mm.cmd_mode_set(Mode::Clock, r.time.utc_ms).ok);
    CHECK(r.mm.cmd_mode_set(Mode::Message, r.time.utc_ms).ok);
    FACES(r, "eye|blank|blank|blank|blank");
}

}  // namespace

void run_tests() {
    test_numbers_validation();
    test_clock_12h();
    test_clock_24h();
    test_clock_dst_edges();
    test_clock_granularity();
    test_wifi_glyph();
    test_countdown_seconds_mode();
    test_countdown_tens_mode();
    test_countdown_cues_and_zero();
    test_countdown_reveal_and_timeout();
    test_countdown_persistence_and_resume();
    test_arbitration();
    test_finished_countdown_never_replays();
    test_time_validity_gating();
    test_time_step_preserves_message_dwell();
    test_mode_set_message_requires_message();
}
