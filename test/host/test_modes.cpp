// Modes (spec 7): clock rendering across DST edges and 12/24, the WiFi glyph,
// the deadline countdown with cue timing and zero choreography, persistence
// and resume, presets, and mode arbitration.
#include <cstring>

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

    explicit Rig(bool h24 = false) {
        ModesConfig cfg;
        cfg.h24 = h24;
        mm.set_config(cfg);
        CHECK(mm.set_tz("PST8PDT,M3.2.0,M11.1.0"));
    }

    // Advance in `step_ms` increments, ticking everything consistently.
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

    Frame shown() const {
        Frame f;
        for (int i = 0; i < N_COLUMNS; ++i) f.idx[static_cast<size_t>(i)] = port.cols[i].index;
        return f;
    }
};

// --------------------------------------------------------------------------
// Clock: 12/24 rendering, DST edges, minute-boundary render times.
// --------------------------------------------------------------------------
void test_clock_12h() {
    Rig r;
    // 2026-01-15 17:41 UTC = 09:41 PST.
    r.begin_at(utc_ms(2026, 1, 15, 17, 41, 0));
    CHECK(r.mm.mode() == Mode::Clock);

    Frame f = r.shown();
    CHECK_EQ(f.idx[0], RING_AM_SLOT);              // morning
    CHECK_EQ(f.idx[1], RING_HOME_SLOT);            // blank below 10
    CHECK_EQ(f.idx[2], ring_index_for_digit(9));
    CHECK_EQ(f.idx[3], ring_index_for_digit(4));
    CHECK_EQ(f.idx[4], ring_index_for_digit(1));

    // Noon is PM, midnight is AM, and 12 shows as 12.
    r.run_to(utc_ms(2026, 1, 15, 20, 0, 0));  // 12:00 PST
    f = r.shown();
    CHECK_EQ(f.idx[0], RING_PM_SLOT);
    CHECK_EQ(f.idx[1], ring_index_for_digit(1));
    CHECK_EQ(f.idx[2], ring_index_for_digit(2));

    r.run_to(utc_ms(2026, 1, 16, 8, 0, 0));  // 00:00 PST -> 12 AM
    f = r.shown();
    CHECK_EQ(f.idx[0], RING_AM_SLOT);
    CHECK_EQ(f.idx[1], ring_index_for_digit(1));
    CHECK_EQ(f.idx[2], ring_index_for_digit(2));

    // Renders happen at second 0: between minute boundaries, no new goes.
    const size_t n = r.port.gos.size();
    r.run_to(r.time.utc_ms + 59 * 1000);  // stay inside the minute
    CHECK_EQ(r.port.gos.size(), n);
}

void test_clock_24h() {
    Rig r(true);
    r.begin_at(utc_ms(2026, 1, 15, 17, 5, 0));  // 09:05 PST
    Frame f = r.shown();
    CHECK_EQ(f.idx[0], RING_HOME_SLOT);          // no AM/PM in 24 h
    CHECK_EQ(f.idx[1], ring_index_for_digit(0));  // leading zero
    CHECK_EQ(f.idx[2], ring_index_for_digit(9));
    CHECK_EQ(f.idx[3], ring_index_for_digit(0));
    CHECK_EQ(f.idx[4], ring_index_for_digit(5));

    // Format switch re-renders immediately.
    r.mm.cmd_clock_format(false, r.time.utc_ms);
    f = r.shown();
    CHECK_EQ(f.idx[0], RING_AM_SLOT);
    CHECK_EQ(f.idx[1], RING_HOME_SLOT);
}

void test_clock_dst_edges() {
    Rig r;
    // Spring forward 2026: 01:59 PST -> 03:00 PDT (09:59Z -> 10:00Z Mar 8).
    r.begin_at(utc_ms(2026, 3, 8, 9, 59, 0));
    Frame f = r.shown();
    CHECK_EQ(f.idx[2], ring_index_for_digit(1));  // 1:59
    CHECK_EQ(f.idx[3], ring_index_for_digit(5));
    CHECK_EQ(f.idx[4], ring_index_for_digit(9));

    r.run_to(utc_ms(2026, 3, 8, 10, 0, 0));
    f = r.shown();
    CHECK_EQ(f.idx[2], ring_index_for_digit(3));  // 3:00, 2:xx never exists
    CHECK_EQ(f.idx[3], ring_index_for_digit(0));
    CHECK_EQ(f.idx[4], ring_index_for_digit(0));

    // Fall back 2026: 01:59 PDT -> 01:00 PST (08:59Z -> 09:00Z Nov 1).
    Rig r2;
    r2.begin_at(utc_ms(2026, 11, 1, 8, 59, 0));
    CHECK_EQ(r2.shown().idx[2], ring_index_for_digit(1));
    r2.run_to(utc_ms(2026, 11, 1, 9, 0, 0));
    f = r2.shown();
    CHECK_EQ(f.idx[2], ring_index_for_digit(1));  // 1:00 again
    CHECK_EQ(f.idx[3], ring_index_for_digit(0));
    CHECK_EQ(f.idx[4], ring_index_for_digit(0));
}

void test_wifi_glyph() {
    Rig r;
    r.time.is_valid = false;
    r.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));

    // Blank until the grace expires...
    CHECK(!r.mm.wifi_glyph_shown());
    CHECK_EQ(r.shown().idx[2], RING_HOME_SLOT);

    // ...then the WiFi glyph on the CENTRE column, blanks elsewhere.
    r.run_to(r.time.utc_ms + 16 * 1000);
    CHECK(r.mm.wifi_glyph_shown());
    Frame f = r.shown();
    CHECK_EQ(f.idx[2], RING_WIFI_SLOT);
    CHECK_EQ(f.idx[0], RING_HOME_SLOT);
    CHECK_EQ(f.idx[4], RING_HOME_SLOT);

    // Sync arrives: the clock renders and the glyph never returns (valid is
    // sticky per spec 8 - a later WiFi drop free-runs).
    r.time.is_valid = true;
    r.run_to(r.time.utc_ms + 61 * 1000);
    CHECK(!r.mm.wifi_glyph_shown());
    CHECK(r.shown().idx[2] != RING_WIFI_SLOT);
}

// --------------------------------------------------------------------------
// Countdown: the deadline model.
// --------------------------------------------------------------------------
void test_countdown_start_and_schedule() {
    Rig r;
    r.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));
    const int64_t t0 = r.time.utc_ms;

    // The ritual: wrong Numbers rejected, right Numbers start 108:00.
    CHECK(!r.mm.cmd_countdown_execute("4 8 15 16 23 43", t0).ok);
    CHECK(r.mm.mode() == Mode::Clock);
    CHECK(r.mm.cmd_countdown_execute(" 4  8 15 16 23 42 ", t0).ok);
    CHECK(r.mm.mode() == Mode::Countdown);
    CHECK(r.mm.cd_phase() == CdPhase::Running);
    CHECK_EQ(r.mm.cd_target(), t0 / 1000 + 6480);

    // MMM:S0 floor semantics: within the first seconds the display is already
    // in the [6470, 6480) window -> 107:50 (108:00 is the idle face; running,
    // it shows only for the start instant).
    r.run_to(t0 + 2 * 1000);
    Frame f = r.shown();
    CHECK_EQ(f.idx[0], ring_index_for_digit(1));
    CHECK_EQ(f.idx[1], ring_index_for_digit(0));
    CHECK_EQ(f.idx[2], ring_index_for_digit(7));
    CHECK_EQ(f.idx[3], ring_index_for_digit(5));
    CHECK_EQ(f.idx[4], ring_index_for_digit(0));

    // Run to +35 s: remaining 107:25 -> window [6440, 6450) -> 107:20.
    r.run_to(t0 + 35 * 1000);
    f = r.shown();
    CHECK_EQ(f.idx[0], ring_index_for_digit(1));
    CHECK_EQ(f.idx[1], ring_index_for_digit(0));
    CHECK_EQ(f.idx[2], ring_index_for_digit(7));
    CHECK_EQ(f.idx[3], ring_index_for_digit(2));
    CHECK_EQ(f.idx[4], ring_index_for_digit(0));

    // Land-on-tick: F(6450) = 107:30 lands when remaining hits 6460 (t0+20 s);
    // its tens-column 49-flip wrap starts duration(49) before that (spec 7.3).
    const int64_t target_ms = r.mm.cd_target() * 1000;
    const int64_t boundary = target_ms - 6460 * 1000;
    const int64_t lead = move_duration_ms(49, 15, 82000);
    bool found = false;
    for (const auto& g : r.port.gos) {
        if (g.col == 3 && g.index == ring_index_for_digit(3)) {
            found = true;
            CHECK(g.at_ms >= boundary - lead - 150 && g.at_ms <= boundary - lead + 150);
            break;
        }
    }
    CHECK(found);
}

void test_countdown_cues_and_zero() {
    Rig r;
    ModesConfig cfg;
    cfg.failure_timeout_s = 0;
    r.mm.set_config(cfg);
    r.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));

    // set_target with a short deadline (the terminal-prop path).
    const int64_t target = r.time.utc_ms / 1000 + 300;
    CHECK(r.mm.cmd_countdown_set_target(target, r.time.utc_ms).ok);
    CHECK(r.mm.mode() == Mode::Countdown);

    const int64_t target_ms = target * 1000;
    r.run_to(target_ms + 15 * 1000);

    // Cues at 4:00, 1:00 and zero - each once, on time (±tick).
    CHECK(r.cues.fired(Cue::Warn4Min));
    CHECK(r.cues.fired(Cue::Warn1Min));
    CHECK(r.cues.fired(Cue::SystemFailure));
    CHECK_EQ(r.cues.recs.size(), 3u);
    CHECK(r.cues.at(Cue::Warn4Min) >= target_ms - 240100 &&
          r.cues.at(Cue::Warn4Min) <= target_ms - 239800);
    CHECK(r.cues.at(Cue::Warn1Min) >= target_ms - 60100 &&
          r.cues.at(Cue::Warn1Min) <= target_ms - 59800);
    CHECK(r.cues.at(Cue::SystemFailure) >= target_ms - 100 &&
          r.cues.at(Cue::SystemFailure) <= target_ms + 200);

    // Zero choreography: 000:00 at zero, spin at +hold, reveal after.
    CHECK(r.mm.cd_phase() == CdPhase::Reveal);
    CHECK_EQ(r.port.spins.size(), static_cast<size_t>(N_COLUMNS));
    const int64_t spin_at = r.port.spins[0].at_ms;
    CHECK(spin_at >= target_ms + 3000 - 200 && spin_at <= target_ms + 3000 + 200);
    CHECK_EQ(r.port.spins[0].flaps_s, 25);
    CHECK_EQ(r.port.spins[0].seconds, 6);

    // Reveal unset -> blanks; convergence landed them after the spin.
    Frame f = r.shown();
    for (int i = 0; i < N_COLUMNS; ++i) CHECK_EQ(f.idx[static_cast<size_t>(i)], RING_HOME_SLOT);

    // No auto-return with failure_timeout_s = 0.
    r.run_to(r.time.utc_ms + 120 * 1000);
    CHECK(r.mm.mode() == Mode::Countdown);

    // Re-entering the Numbers from the reveal restarts at 108:00.
    CHECK(r.mm.cmd_countdown_execute(ModeManager::THE_NUMBERS, r.time.utc_ms).ok);
    CHECK(r.mm.cd_phase() == CdPhase::Running);
}

void test_countdown_reveal_and_timeout() {
    Rig r;
    ModesConfig cfg;
    cfg.reveal = {13, 20, 33, 39, 45};  // arbitrary glyph indices
    cfg.failure_timeout_s = 5;
    r.mm.set_config(cfg);
    r.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));

    const int64_t target = r.time.utc_ms / 1000 + 20;
    CHECK(r.mm.cmd_countdown_set_target(target, r.time.utc_ms).ok);
    r.run_to(target * 1000 + (3 + 6 + 1) * 1000);

    CHECK(r.mm.cd_phase() == CdPhase::Reveal);
    Frame f = r.shown();
    CHECK_EQ(f.idx[0], 13);
    CHECK_EQ(f.idx[2], 33);
    CHECK_EQ(f.idx[4], 45);

    // failure_timeout_s = 5 returns to clock.
    r.run_to(target * 1000 + (3 + 6 + 6) * 1000);
    CHECK(r.mm.mode() == Mode::Clock);
    CHECK(r.mm.cd_phase() == CdPhase::Idle);
}

void test_countdown_persistence_and_resume() {
    Rig r;
    r.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));
    const int64_t t0 = r.time.utc_ms;
    CHECK(r.mm.cmd_countdown_start(t0).ok);
    CHECK(r.store.have);
    CHECK(r.store.stored.phase == CdPhase::Running);
    const int64_t target = r.store.stored.target_utc;
    CHECK_EQ(target, t0 / 1000 + 6480);

    // "Reboot" 100 s later with 6380 left: a fresh manager on the same store
    // resumes the countdown from the deadline, not from scratch.
    Rig r2;
    r2.store = r.store;
    ModeManager mm2{r2.ring, r2.sched, r2.time, r2.store, r2.cues};
    mm2.set_config(ModesConfig{});
    CHECK(mm2.set_tz("PST8PDT,M3.2.0,M11.1.0"));
    r2.time.utc_ms = t0 + 100 * 1000;
    r2.port.now_ms = r2.time.utc_ms;
    mm2.begin(r2.time.utc_ms);
    CHECK(mm2.mode() == Mode::Countdown);
    CHECK(mm2.cd_phase() == CdPhase::Running);
    CHECK_EQ(mm2.cd_target(), target);

    // Resuming with 90 s left must NOT replay the 4-minute warning.
    Rig r3;
    r3.store = r.store;
    ModeManager mm3{r3.ring, r3.sched, r3.time, r3.store, r3.cues};
    mm3.set_config(ModesConfig{});
    CHECK(mm3.set_tz("PST8PDT,M3.2.0,M11.1.0"));
    r3.time.utc_ms = (target - 90) * 1000;
    r3.port.now_ms = r3.time.utc_ms;
    mm3.begin(r3.time.utc_ms);
    while (r3.time.utc_ms < (target - 70) * 1000) {
        r3.time.utc_ms += 100;
        r3.port.now_ms = r3.time.utc_ms;
        r3.cues.now_ms = r3.time.utc_ms;
        mm3.tick(r3.time.utc_ms);
    }
    CHECK(!r3.cues.fired(Cue::Warn4Min));  // already in the past at resume
    // ...but the 1-minute warning still fires when its moment comes.
    while (r3.time.utc_ms < (target - 50) * 1000) {
        r3.time.utc_ms += 100;
        r3.port.now_ms = r3.time.utc_ms;
        r3.cues.now_ms = r3.time.utc_ms;
        mm3.tick(r3.time.utc_ms);
    }
    CHECK(r3.cues.fired(Cue::Warn1Min));

    // Cancel persists Idle and shows the static 108:00.
    CHECK(r.mm.cmd_countdown_cancel(r.time.utc_ms).ok);
    CHECK(r.store.stored.phase == CdPhase::Idle);
    CHECK(r.mm.mode() == Mode::Countdown);
    Frame f = r.shown();
    CHECK_EQ(f.idx[2], ring_index_for_digit(8));  // 108:00
}

// --------------------------------------------------------------------------
// Arbitration (spec 7): message dwell/hold, countdown override, presets,
// display.frame neutrality.
// --------------------------------------------------------------------------
void test_arbitration() {
    Rig r;
    r.begin_at(utc_ms(2026, 1, 15, 17, 0, 30));

    // Message overrides clock, dwell returns to it.
    std::array<std::string, N_COLUMNS> toks = {"ankh", "_", "#33", "eye", "4"};
    CHECK(r.mm.cmd_message_set(toks, 2, false, r.time.utc_ms).ok);
    CHECK(r.mm.mode() == Mode::Message);
    Frame f = r.shown();
    CHECK_EQ(f.idx[0], ring_index_for_token("ankh"));
    CHECK_EQ(f.idx[1], RING_HOME_SLOT);
    CHECK_EQ(f.idx[2], 33);
    CHECK_EQ(f.idx[3], ring_index_for_token("eye"));
    CHECK_EQ(f.idx[4], ring_index_for_digit(4));

    r.run_to(r.time.utc_ms + 2500);
    CHECK(r.mm.mode() == Mode::Clock);

    // hold = true stays until the mode changes.
    CHECK(r.mm.cmd_message_set(toks, 2, true, r.time.utc_ms).ok);
    r.run_to(r.time.utc_ms + 10 * 1000);
    CHECK(r.mm.mode() == Mode::Message);

    // A bad token is rejected and changes nothing.
    std::array<std::string, N_COLUMNS> bad = {"nosuch", "_", "_", "_", "_"};
    CHECK(!r.mm.cmd_message_set(bad, 0, false, r.time.utc_ms).ok);
    CHECK(r.mm.mode() == Mode::Message);

    // Countdown overrides message.
    CHECK(r.mm.cmd_countdown_start(r.time.utc_ms).ok);
    CHECK(r.mm.mode() == Mode::Countdown);

    // display.frame changes the display but never the mode.
    Frame raw;
    raw.idx = {13, 14, 15, 16, 17};
    CHECK(r.mm.cmd_display_frame(raw, r.time.utc_ms).ok);
    CHECK(r.mm.mode() == Mode::Countdown);
    Frame bad_raw;
    bad_raw.idx = {0, 0, 0, 0, 99};
    CHECK(!r.mm.cmd_display_frame(bad_raw, r.time.utc_ms).ok);

    // Presets: qmarks puts every column on the question glyph and holds.
    CHECK(r.mm.cmd_preset("qmarks", r.time.utc_ms).ok);
    CHECK(r.mm.mode() == Mode::Message);
    f = r.shown();
    for (int i = 0; i < N_COLUMNS; ++i) CHECK_EQ(f.idx[static_cast<size_t>(i)], RING_QMARK_SLOT);
    CHECK(!r.mm.cmd_preset("nope", r.time.utc_ms).ok);

    // Back to clock on demand.
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

}  // namespace

void run_tests() {
    test_numbers_validation();
    test_clock_12h();
    test_clock_24h();
    test_clock_dst_edges();
    test_wifi_glyph();
    test_countdown_start_and_schedule();
    test_countdown_cues_and_zero();
    test_countdown_reveal_and_timeout();
    test_countdown_persistence_and_resume();
    test_arbitration();
}
