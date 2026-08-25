// Modes (spec 7): clock rendering across DST edges, 12/24 and granularity;
// the WiFi glyph; the deadline countdown with cue timing, seconds/tens mode
// and zero choreography; persistence and resume; presets; arbitration.
//
// Assertions read the rendered CHARACTERS, not slot indices - column 5 has two
// slots per digit and which one is used depends on where the column was, so an
// index assertion would be pinning an implementation detail.
#include <cstdio>
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
// The seconds freeze (spec 7.3).  Every mode holds MMM:00 until the run has
// `seconds_live_s` left; only then does the resolution change.  The boundary
// coincides with the 4-minute cue, so the display coming alive and the warning
// sound are one moment.
void test_countdown_step_rules() {
    // Pure rules first - the driven cases below inherit whatever these say.
    const int live = 240;

    // Quiet phase: whole minutes, whatever the mode.
    for (const SecondsMode m : {SecondsMode::Minutes, SecondsMode::Tens, SecondsMode::Seconds}) {
        CHECK_EQ(countdown_step_s(m, 6480, live), 60);
        CHECK_EQ(countdown_step_s(m, 241, live), 60);
        CHECK_EQ(countdown_shown_s(m, 6480, live), 6480);
        // CEILING (spec 7.3): a value owns the window ABOVE it, so 108:00 is
        // held for the whole first minute rather than rolling off at once.
        CHECK_EQ(countdown_shown_s(m, 6479, live), 6480);
        CHECK_EQ(countdown_shown_s(m, 6421, live), 6480);
        CHECK_EQ(countdown_shown_s(m, 6420, live), 6420);   // and only now 107:00
        CHECK_EQ(countdown_shown_s(m, 299, live), 300);
        CHECK_EQ(countdown_shown_s(m, 241, live), 300);
        // A quiet-phase value is always a whole minute, which is what puts
        // zeros in both seconds columns.
        CHECK_EQ(countdown_shown_s(m, 6479, live) % 60, 0);
    }

    // At the boundary the modes part company.
    CHECK_EQ(countdown_step_s(SecondsMode::Minutes, 240, live), 60);
    CHECK_EQ(countdown_step_s(SecondsMode::Tens, 240, live), 10);
    CHECK_EQ(countdown_step_s(SecondsMode::Seconds, 240, live), 1);

    // 240 itself is already live, and ceilings to itself in every mode - so the
    // frame at the boundary is 004:00 in all three and nothing jumps.  Under
    // ceiling the value ABOVE it is 005:00, held for the whole preceding
    // minute, so 004:00 appears exactly as the seconds go live and the
    // 4-minute cue fires.  They are the same instant.
    CHECK_EQ(countdown_shown_s(SecondsMode::Minutes, 240, live), 240);
    CHECK_EQ(countdown_shown_s(SecondsMode::Tens, 240, live), 240);
    CHECK_EQ(countdown_shown_s(SecondsMode::Seconds, 240, live), 240);

    CHECK_EQ(countdown_shown_s(SecondsMode::Seconds, 241, live), 300);
    CHECK_EQ(countdown_shown_s(SecondsMode::Tens, 241, live), 300);

    // One tick below the boundary: 004:00 -> 003:59 in seconds.  In tens and
    // minutes the coarser window still covers 239, so the face does not change
    // yet - which is the ceiling rule doing exactly what it says.
    CHECK_EQ(countdown_shown_s(SecondsMode::Seconds, 239, live), 239);
    CHECK_EQ(countdown_shown_s(SecondsMode::Tens, 239, live), 240);
    CHECK_EQ(countdown_shown_s(SecondsMode::Minutes, 239, live), 240);
    CHECK_EQ(countdown_shown_s(SecondsMode::Tens, 231, live), 240);
    CHECK_EQ(countdown_shown_s(SecondsMode::Tens, 230, live), 230);

    // ZERO LANDS ON ZERO.  Under the old floor rule 000:00 appeared at
    // remaining = 1, a full second before the deadline and ahead of its own
    // klaxon.
    CHECK_EQ(countdown_shown_s(SecondsMode::Seconds, 1, live), 1);
    CHECK_EQ(countdown_shown_s(SecondsMode::Seconds, 0, live), 0);

    // The scheduler's "next value" must cross the boundary without a special
    // case: 300 -> 240 while quiet, then 240 -> 239 once live.
    CHECK_EQ(countdown_next_shown_s(SecondsMode::Seconds, 300, live), 240);
    CHECK_EQ(countdown_next_shown_s(SecondsMode::Seconds, 240, live), 239);
    CHECK_EQ(countdown_next_shown_s(SecondsMode::Tens, 240, live), 230);
    CHECK_EQ(countdown_next_shown_s(SecondsMode::Minutes, 240, live), 180);
    // 108:00 -> 107:00, the step the first minute of a run now actually takes.
    CHECK_EQ(countdown_next_shown_s(SecondsMode::Seconds, 6480, live), 6420);
    CHECK_EQ(countdown_next_shown_s(SecondsMode::Seconds, 1, live), 0);
    CHECK_EQ(countdown_next_shown_s(SecondsMode::Seconds, 0, live), 0);

    // live_s = 0 is a legitimate "never show seconds" setting for any mode.
    CHECK_EQ(countdown_step_s(SecondsMode::Seconds, 1, 0), 60);
    CHECK_EQ(countdown_shown_s(SecondsMode::Seconds, 59, 0), 60);
    CHECK_EQ(countdown_shown_s(SecondsMode::Seconds, 0, 0), 0);
}

// The three consequences the ceiling correction exists for (spec 7.3,
// 2026-08-24).  Each one was wrong under the old floor rule, and each is a
// thing somebody watching the display would actually notice.
void test_countdown_ceiling_consequences() {
    const int live = 240;
    const SecondsMode m = SecondsMode::Seconds;

    // (1) 108:00 IS HELD FOR A FULL MINUTE.  The show does not roll off the
    // start face until a minute has elapsed; the old rule dropped to 107:00
    // half a second in.
    CHECK_EQ(countdown_shown_s(m, 6480, live), 6480);
    for (int r = 6479; r > 6420; --r) CHECK_EQ(countdown_shown_s(m, r, live), 6480);
    CHECK_EQ(countdown_shown_s(m, 6420, live), 6420);

    // (2) THE 4:00 TRANSITION IS SEAMLESS.  005:00 owns the minute above the
    // boundary and 004:00 appears exactly as remaining reaches 240 - the same
    // instant the seconds go live and the 4-minute cue fires.  Nothing repeats
    // and nothing jumps.
    CHECK_EQ(countdown_shown_s(m, 300, live), 300);
    CHECK_EQ(countdown_shown_s(m, 241, live), 300);
    CHECK_EQ(countdown_shown_s(m, 240, live), 240);
    CHECK_EQ(countdown_shown_s(m, 239, live), 239);
    CHECK_EQ(countdown_next_shown_s(m, 300, live), 240);

    // (3) 000:00 LANDS AT REMAINING = 0, with the klaxon rather than a second
    // ahead of it.  The land instant is target - next_shown * 1000, so
    // next_shown == 0 is exactly the statement "the zero frame lands on the
    // deadline"; under floor it was 1, i.e. a second early.
    CHECK_EQ(countdown_shown_s(m, 1, live), 1);
    CHECK_EQ(countdown_shown_s(m, 0, live), 0);
    CHECK_EQ(countdown_next_shown_s(m, 1, live), 0);

    // The display never claims less time than remains, in any mode, at any
    // setting.  That single property is what all three of the above are.
    for (const SecondsMode mm : {SecondsMode::Minutes, SecondsMode::Tens,
                                 SecondsMode::Seconds}) {
        for (int r = 0; r <= 6480; ++r) CHECK(countdown_shown_s(mm, r, live) >= r);
    }
}

void test_countdown_quiet_phase() {
    Rig r;  // default: SecondsMode::Seconds, seconds_live_s = 240
    r.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));
    const int64_t t0 = r.time.utc_ms;

    CHECK(!r.mm.cmd_countdown_execute("4 8 15 16 23 43", t0).ok);  // wrong Numbers
    CHECK(r.mm.cmd_countdown_execute(" 4  8 15 16 23 42 ", t0).ok);
    CHECK(r.mm.cd_phase() == CdPhase::Running);

    // 108:00 IS HELD FOR THE FIRST FULL MINUTE (ceiling, spec 7.3) - the show
    // does not roll off it until a minute has actually elapsed, so 107:55
    // remaining still reads 108:00.  Half a second in it is still 108:00; it
    // becomes 107:00 when remaining reaches 6420 and not a moment before.
    r.run_to(t0 + 500);
    FACES(r, "1|0|8|0|0");
    r.run_to(t0 + 59 * 1000);
    FACES(r, "1|0|8|0|0");     // 59 s in: still 108:00

    // From here on, only the minutes columns may move.  This is the assertion
    // the whole redesign exists for.
    r.port.gos.clear();
    r.run_to(t0 + 600 * 1000);  // ten minutes in -> 5880 left -> 098:00
    FACES(r, "0|9|8|0|0");
    CHECK_EQ(r.port.gos_for(3), 0);
    CHECK_EQ(r.port.gos_for(4), 0);
    // ...and the minutes columns did move: ten ones-digit steps and a borrow.
    CHECK(r.port.gos_for(2) >= 10);
}

void test_countdown_freeze_boundary() {
    Rig r;
    r.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));
    const int64_t t0 = r.time.utc_ms;
    // 305 s out, so the boundary at 240 arrives 65 s in.
    CHECK(r.mm.cmd_countdown_set_target(t0 / 1000 + 305, t0).ok);

    // Sampled mid-window throughout: frames land ON the boundary by design, so
    // a boundary-instant sample would see the incoming value.
    r.run_to(t0 + 500);
    FACES(r, "0|0|6|0|0");     // 304.5 s left -> 006:00
    r.run_to(t0 + 10 * 1000);
    FACES(r, "0|0|5|0|0");     // 295 left -> 005:00, and it holds a full minute

    const int col4 = r.port.cols[3].index;
    const int col5 = r.port.cols[4].index;

    // Still frozen most of the way to the boundary.  Sampled at 55 s, not 64:
    // land-on-tick COMMITS the waking frame a lead-time early (col 4's 45-flip
    // borrow needs ~3 s at 15 flaps/s) so it arrives exactly on the boundary,
    // and FakePort settles a move the instant it is issued.  On the wall the
    // columns are in flight over that stretch; in the fake they teleport.
    r.run_to(t0 + 55 * 1000);
    FACES(r, "0|0|5|0|0");     // 250 left: still the 005:00 window
    CHECK_EQ(r.port.cols[3].index, col4);
    CHECK_EQ(r.port.cols[4].index, col5);

    // THE BOUNDARY IS SEAMLESS.  004:00 appears exactly as remaining reaches
    // 240 - the same instant the seconds go live and the 4-minute cue fires -
    // and then ticks one second at a time.  Under the old floor rule 004:00 had
    // already been up for a minute by then.
    r.run_to(t0 + 65500);
    FACES(r, "0|0|3|5|9");
    r.run_to(t0 + 66500);
    FACES(r, "0|0|3|5|9");

    // Waking the seconds costs column 4 the 0->5 borrow (45 flips on ring A)
    // and column 5 the 0->9 wrap (16 on ring B, not the 41 a single digit
    // block would cost).  The wear is unchanged by the ceiling correction - the
    // sequence of displayed values is identical, only its timing shifted by one
    // window - which is why test_wear's totals did not move.
    CHECK_EQ(ring_forward_distance(col4, r.port.cols[3].index), 45);
    CHECK_EQ(ring_forward_distance(col5, r.port.cols[4].index), 16);
}

void test_countdown_modes() {
    // minutes: the seconds columns never move, even inside the live window.
    {
        Rig r;
        ModesConfig c;
        c.seconds_mode = SecondsMode::Minutes;
        r.configure(c);
        r.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));
        const int64_t t0 = r.time.utc_ms;
        CHECK(r.mm.cmd_countdown_set_target(t0 / 1000 + 125, t0).ok);
        r.run_to(t0 + 500);
        FACES(r, "0|0|3|0|0");      // 124.5 left -> 003:00
        r.port.gos.clear();
        r.run_to(t0 + 70 * 1000);   // 55 s left: deep inside the live window
        // 001:00, not 000:00.  Ceiling will not claim the run is over while
        // nearly a minute of it remains - which is the same defect, in
        // miniature, that made 000:00 arrive a second before the klaxon.
        FACES(r, "0|0|1|0|0");
        CHECK_EQ(r.port.gos_for(3), 0);
        CHECK_EQ(r.port.gos_for(4), 0);
    }
    // tens: MMM:S0 inside the window, column 5 parked on 0 throughout.
    {
        Rig r;
        ModesConfig c;
        c.seconds_mode = SecondsMode::Tens;
        r.configure(c);
        r.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));
        const int64_t t0 = r.time.utc_ms;
        CHECK(r.mm.cmd_countdown_set_target(t0 / 1000 + 245, t0).ok);
        r.run_to(t0 + 500);
        FACES(r, "0|0|5|0|0");      // 244.5 left, still quiet -> 005:00
        const int col5 = r.port.cols[4].index;
        r.run_to(t0 + 20500);       // 224.5 s left -> ceil to 230 = 003:50
        FACES(r, "0|0|3|5|0");
        CHECK_EQ(r.port.cols[4].index, col5);   // never moved
    }
    // seconds: live one-second ticks, one flip each on column 5.
    {
        Rig r;
        r.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));
        const int64_t t0 = r.time.utc_ms;
        CHECK(r.mm.cmd_countdown_set_target(t0 / 1000 + 100, t0).ok);
        r.run_to(t0 + 500);
        FACES(r, "0|0|1|3|9");      // 99 s -> 001:39
        const int start = r.port.cols[4].index;
        r.run_to(t0 + 4500);
        FACES(r, "0|0|1|3|6");      // 95.5 left -> ceil to 96 = 001:36
        CHECK_EQ(ring_forward_distance(start, r.port.cols[4].index), 3);
    }
}

// The phase 3 review's third critical: enter_mode keeps a Running deadline
// alive across a mode switch, but only the countdown MODE used to advance it.
// Switching to the clock mid-run therefore skipped the cues and the zero
// entirely - and then fired all of them at once, with the alarm spin, whenever
// countdown mode was next entered.  A run started before bed and left on the
// clock face detonated the next time anyone opened the Modes page.
void test_countdown_runs_offscreen() {
    Rig r;
    r.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));
    const int64_t t0 = r.time.utc_ms;
    CHECK(r.mm.cmd_countdown_set_target(t0 / 1000 + 300, t0).ok);

    // Ten seconds in, walk away to the clock.
    r.run_to(t0 + 10 * 1000);
    CHECK(r.mm.cmd_mode_set(Mode::Clock, r.time.utc_ms).ok);
    CHECK(r.mm.cd_phase() == CdPhase::Running);  // the deadline survives

    // The cues fire at the real moments, on the clock face.
    r.run_to(t0 + 65 * 1000);   // 235 s left
    CHECK(r.cues.fired(Cue::Warn4Min));
    CHECK(!r.cues.fired(Cue::Warn1Min));
    r.run_to(t0 + 245 * 1000);  // 55 s left
    CHECK(r.cues.fired(Cue::Warn1Min));

    // Zero happens whether or not anyone is looking at it.
    const int64_t clock_gos = r.port.gos.size();
    r.run_to(t0 + 310 * 1000);
    CHECK(r.cues.fired(Cue::SystemFailure));
    CHECK_EQ(r.cues.recs.size(), 3u);
    CHECK(r.mm.cd_phase() == CdPhase::Reveal);
    CHECK(r.mm.mode() == Mode::Clock);   // it did not seize the display
    CHECK_EQ(r.port.spins.size(), 0u);   // and did not spin columns it does not own
    CHECK(r.port.gos.size() >= static_cast<size_t>(clock_gos));  // clock kept ticking

    // Entering countdown mode later shows the reveal.  It must NOT replay.
    const size_t cues_before = r.cues.recs.size();
    CHECK(r.mm.cmd_mode_set(Mode::Countdown, r.time.utc_ms).ok);
    r.run_to(r.time.utc_ms + 20 * 1000);
    CHECK_EQ(r.cues.recs.size(), cues_before);
    CHECK_EQ(r.port.spins.size(), 0u);
    CHECK(r.mm.cd_phase() == CdPhase::Reveal);
}

// A deadline in milliseconds is the obvious integration mistake, and it used
// to park the display on 000:00 while reporting `running`: rem_ms/1000
// truncates to int and wraps negative.
void test_countdown_target_bounds() {
    Rig r;
    r.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));
    const int64_t now_s = r.time.utc_ms / 1000;

    CHECK(!r.mm.cmd_countdown_set_target(0, r.time.utc_ms).ok);
    CHECK(!r.mm.cmd_countdown_set_target(-1, r.time.utc_ms).ok);
    CHECK(!r.mm.cmd_countdown_set_target(now_s, r.time.utc_ms).ok);       // already past
    CHECK(!r.mm.cmd_countdown_set_target(now_s * 1000, r.time.utc_ms).ok);  // ms epoch
    CHECK(!r.mm.cmd_countdown_set_target(now_s + 86401, r.time.utc_ms).ok);
    CHECK(r.mm.cmd_countdown_set_target(now_s + 1, r.time.utc_ms).ok);
    CHECK(r.mm.cmd_countdown_set_target(now_s + 86400, r.time.utc_ms).ok);
}

// seconds_live_s that is not a whole minute used to make the display count UP
// at the boundary - a 45-flip borrow on column 4 and a 16-flip wrap on column
// 5, in the wrong direction, mid-warning.  The core floors defensively even if
// a stale NVS value gets through, so the shown value is monotonic for ANY
// setting.
void test_countdown_shown_is_monotonic() {
    for (const SecondsMode m : {SecondsMode::Minutes, SecondsMode::Tens, SecondsMode::Seconds}) {
        for (const int live : {0, 1, 30, 59, 60, 61, 100, 239, 240, 241, 250, 299, 600, 6480}) {
            int prev = -1;
            for (int rem = 0; rem <= 6480; ++rem) {
                const int shown = countdown_shown_s(m, rem, live);
                if (shown < prev) {
                    std::printf("FAIL non-monotonic: mode=%d live=%d rem=%d shown=%d prev=%d\n",
                                static_cast<int>(m), live, rem, shown, prev);
                    ++g_failures;
                    break;
                }
                if (shown < rem) {
                    std::printf("FAIL shown BEHIND remaining: live=%d rem=%d shown=%d\n",
                                live, rem, shown);
                    ++g_failures;
                    break;
                }
                // ... and never more than one window ahead, which is what
                // stops "round up" becoming "round up to anything".
                const int step = countdown_step_s(m, rem, live);
                if (shown - rem >= step || shown % step != 0) {
                    std::printf("FAIL bad window: live=%d rem=%d shown=%d step=%d\n",
                                live, rem, shown, step);
                    ++g_failures;
                    break;
                }
                prev = shown;
            }
        }
    }
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


// --------------------------------------------------------------------------
// Maintenance mode (spec 5.9): a deliberate override, never inferred
// --------------------------------------------------------------------------
void test_maintenance_suspends_everything() {
    Rig r;
    r.configure(cfg_minutely());
    r.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));
    r.run_to(r.time.utc_ms + 2000);
    const size_t gos_before = r.port.gos.size();
    CHECK(gos_before > 0);  // the clock was rendering

    CHECK(r.mm.cmd_maintenance(true, r.time.utc_ms).ok);
    CHECK(r.mm.maintenance());

    // Nothing auto-moves for a full simulated hour: no clock render, no
    // land-on-tick boundary, no scheduler convergence.  Someone has their
    // hands in the mechanism.
    r.port.gos.clear();
    r.run_to(r.time.utc_ms + 3600 * 1000);
    CHECK_EQ(r.port.gos.size(), 0u);

    // ... and time keeps passing, so leaving does not replay an hour of ticks
    // as a burst.  The clock renders the CURRENT minute, once.
    CHECK(r.mm.cmd_maintenance(false, r.time.utc_ms).ok);
    CHECK(!r.mm.maintenance());
    r.run_to(r.time.utc_ms + 500);
    CHECK(r.port.gos.size() > 0);
    CHECK(r.port.gos.size() <= static_cast<size_t>(N_COLUMNS));
}

// Escalation drops EN for all five (spec 5.8) and stops every axis - and then
// the frame layer put them straight back to work.  The Stop is one-shot; the
// convergence pass runs every tick, so "stop everything" lasted 50 ms and the
// axes carried on stepping into dead drivers, completing moves and publishing
// faces the drums never reached.
// THE SHIPPED DEFAULT (spec 11).  The canon five from the show's screen at
// zero, resolved BY NAME against whichever ring is loaded - so it follows a ring
// upload instead of pointing at whatever now sits at a hard-coded slot.  This
// runs once, at the first boot of a board with empty NVS, which is precisely the
// path nobody ever exercises again.
void test_the_canon_reveal_resolves_on_both_rings() {
    Rig r;
    const RingSet& ring = r.ring;
    const std::array<int, N_COLUMNS> slots = canon_reveal_slots(ring);

    // Every column resolves - column 5's reduced ring carries `branch`, which is
    // the one that had to be true.
    for (int i = 0; i < N_COLUMNS; ++i) {
        if (slots[static_cast<size_t>(i)] < 0) {
            std::printf("  column %d cannot render %s\n", i + 1,
                        ModesConfig::REVEAL_CANON[static_cast<size_t>(i)]);
        }
        CHECK(slots[static_cast<size_t>(i)] >= 0);
        // ... and the slot it picked really is that glyph.
        CHECK(ring.col(i).slot(slots[static_cast<size_t>(i)]).id ==
              std::string(ModesConfig::REVEAL_CANON[static_cast<size_t>(i)]));
    }

    // The order is the show's, columns 1 to 5.
    const char* want[] = {"staff", "spiral", "obelisk", "bird", "branch"};
    for (int i = 0; i < N_COLUMNS; ++i) {
        CHECK(std::string(ModesConfig::REVEAL_CANON[static_cast<size_t>(i)]) == want[i]);
    }

    // Column 5 is on a DIFFERENT ring, so its index must differ from column 4's
    // for the same-named glyph would be a coincidence - the point of resolving
    // by name rather than copying an index across.
    const int c4_bird = ring.col(3).index_for_token("bird");
    const int c5_bird = ring.col(4).index_for_token("bird");
    CHECK(c4_bird >= 0 && c5_bird >= 0);
    CHECK(c4_bird != c5_bird);

    // The near-siblings the manifests carry are NOT what we picked.
    CHECK(std::string(ModesConfig::REVEAL_CANON[4]) != "fork");
    CHECK(std::string(ModesConfig::REVEAL_CANON[0]) != "hook");
}

// THE CRITICAL the scoped re-sweep found in my own escalation fix.  The EN gate
// freezes the countdown, so a deadline that passes while the display is held
// leaves the phase on Running with the cue unfired.  Without care, the first
// unheld tick fires the 60 s system-failure klaxon AND starts a six-second
// open-loop spin at alarm speed - and that spin destroys the recovery re-home
// posted alongside it, leaving the display powered, spinning, and with no idea
// where its drums are.
//
// It is the Phase 3 review's "a run started before bed detonated the next time
// anyone opened the Modes page", reintroduced through a different gate, and the
// same rule fixes it: the cues and the spin belong to the real zero moment and
// never replay (spec 17).
void test_a_deadline_that_passes_while_held_wakes_silently() {
    for (const int which : {0, 1, 2}) {   // EN down, maintenance, OTA hold
        Rig r;
        r.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));
        const int64_t t0 = r.time.utc_ms;
        CHECK(r.mm.cmd_countdown_set_target(t0 / 1000 + 20, t0).ok);
        r.run_to(t0 + 2000);

        // Hold the display, by each of the three routes in turn.
        if (which == 0) r.mm.set_drivers_enabled(false);
        else if (which == 1) CHECK(r.mm.cmd_maintenance(true, r.time.utc_ms).ok);
        else r.mm.cmd_ota_hold(true, r.time.utc_ms);

        r.cues.recs.clear();
        r.port.gos.clear();
        r.port.spins.clear();

        // Sail past the deadline and well past hold + spin.
        r.run_to(t0 + 60 * 1000);

        // Release it.
        if (which == 0) r.mm.set_drivers_enabled(true);
        else if (which == 1) CHECK(r.mm.cmd_maintenance(false, r.time.utc_ms).ok);
        else r.mm.cmd_ota_hold(false, r.time.utc_ms);
        r.run_to(r.time.utc_ms + 3000);

        // NO KLAXON and NO SPIN.  Those belong to the zero moment, which passed
        // while nobody could see or hear it.
        // at() returns -1 for "never fired", which is the whole point.
        if (r.cues.at(Cue::SystemFailure) >= 0) {
            std::printf("  route %d replayed the system-failure cue\n", which);
        }
        CHECK(r.cues.at(Cue::SystemFailure) < 0);
        if (!r.port.spins.empty()) std::printf("  route %d replayed the alarm spin\n", which);
        CHECK(r.port.spins.empty());

        // ... and it is in the reveal, not stuck on Running for ever.
        CHECK(r.mm.cd_phase() == CdPhase::Reveal);
    }
}

void test_en_down_stops_the_frame_layer() {
    Rig r;
    r.configure(cfg_minutely());
    r.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));
    r.run_to(r.time.utc_ms + 2000);
    CHECK(r.port.gos.size() > 0);

    r.mm.set_drivers_enabled(false);
    r.port.gos.clear();
    r.run_to(r.time.utc_ms + 3600 * 1000);
    CHECK_EQ(r.port.gos.size(), 0u);

    // Energizing resumes it - and the motion layer separately re-homes,
    // because the drums have been sitting unpowered.
    r.mm.set_drivers_enabled(true);
    r.run_to(r.time.utc_ms + 500);
    CHECK(r.port.gos.size() > 0);
}

void test_maintenance_survives_and_blocks_countdown() {
    Rig r;
    r.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));
    // A countdown running when maintenance is entered keeps its deadline -
    // the deadline is absolute and nothing about a repair changes when it
    // expires - but nothing is DRIVEN while the repair is happening.
    CHECK(r.mm.cmd_countdown_start(r.time.utc_ms).ok);
    CHECK(r.mm.cd_phase() == CdPhase::Running);
    const int64_t target = r.mm.cd_target();

    CHECK(r.mm.cmd_maintenance(true, r.time.utc_ms).ok);
    r.port.gos.clear();
    r.run_to(r.time.utc_ms + 120 * 1000);
    CHECK_EQ(r.port.gos.size(), 0u);
    CHECK_EQ(r.mm.cd_target(), target);  // untouched

    CHECK(r.mm.cmd_maintenance(false, r.time.utc_ms).ok);
    r.run_to(r.time.utc_ms + 1000);
    CHECK(r.port.gos.size() > 0);
}

void test_excluded_columns_leave_a_hole() {
    Rig r;
    r.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));
    // Column 3 disabled: the mode keeps running, the renderer keeps producing
    // five indices, and exactly one column is never commanded.  The preference
    // Nico asked for, made testable.  A preset is the probe because it moves
    // ALL FIVE columns - a clock tick would only move the ones whose digit
    // changed, and would pass whether the hole worked or not.
    r.mm.cmd_set_excluded(0b01000, r.time.utc_ms);
    r.port.gos.clear();
    CHECK(r.mm.cmd_preset("qmarks", r.time.utc_ms).ok);
    r.run_to(r.time.utc_ms + 200);
    CHECK_EQ(r.port.gos.size(), static_cast<size_t>(N_COLUMNS - 1));
    for (const auto& g : r.port.gos) CHECK(g.col != 3);

    // The mode is untouched: a hole does not halt the display.  (The clock
    // moves only the columns whose digit changed, so the count here is not
    // fixed - the invariant that matters is that column 3 is never among them.)
    CHECK(r.mm.cmd_mode_set(Mode::Clock, r.time.utc_ms).ok);
    CHECK(r.mm.mode() == Mode::Clock);
    r.port.gos.clear();
    r.run_to(r.time.utc_ms + 61 * 1000);
    for (const auto& g : r.port.gos) CHECK(g.col != 3);

    // Re-enabling brings it straight back - no reboot, no re-home dance.
    r.mm.cmd_set_excluded(0, r.time.utc_ms);
    r.port.gos.clear();
    CHECK(r.mm.cmd_preset("qmarks", r.time.utc_ms).ok);
    r.run_to(r.time.utc_ms + 200);
    CHECK_EQ(r.port.gos.size(), static_cast<size_t>(N_COLUMNS));
}

// The calibration walk is a MANUAL command and runs in maintenance, which is
// the mode you enter in order to work on a drum (spec 5.9).  It did not: the
// walk sat below the maintenance gate in tick_locked, so motion.ramp returned
// ok, cal_ramp_active() reported true, and the column never moved.  Measured on
// the board before the fix - IDLE at one index for five seconds against a
// one-second dwell.
//
// Asserted on port.gos, not on the return value: the return value was true the
// whole time it was broken.
void test_calibration_walk_runs_in_maintenance() {
    Rig r;
    r.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));
    CHECK(r.mm.cmd_maintenance(true, r.time.utc_ms).ok);

    r.port.gos.clear();
    CHECK(r.mm.cmd_cal_ramp(2, 0, 6, 2, 1, r.time.utc_ms).ok);
    CHECK(r.mm.cal_ramp_active());
    r.run_to(r.time.utc_ms + 6000);

    CHECK(r.port.gos.size() >= 4u);                  // stops at 0, 2, 4, 6
    for (const auto& g : r.port.gos) CHECK_EQ(g.col, 2);   // and NOTHING else moved
    CHECK(!r.mm.cal_ramp_active());                  // it finished, it did not hang

    // The rest of maintenance is unchanged: with no walk running, nothing moves.
    r.port.gos.clear();
    r.run_to(r.time.utc_ms + 3600 * 1000);
    CHECK_EQ(r.port.gos.size(), 0u);
    CHECK(r.mm.cmd_maintenance(false, r.time.utc_ms).ok);
}

// An excluded column is dropped by the frame scheduler, so a walk on one would
// report itself active and move nothing - the same shape of lie.  Refused.
void test_calibration_walk_refuses_a_disabled_column() {
    Rig r;
    r.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));
    r.mm.cmd_set_excluded(0b00100, r.time.utc_ms);

    const auto res = r.mm.cmd_cal_ramp(2, 0, 6, 1, 1, r.time.utc_ms);
    CHECK(!res.ok);
    CHECK(!r.mm.cal_ramp_active());
    r.port.gos.clear();
    r.run_to(r.time.utc_ms + 3000);
    for (const auto& g : r.port.gos) CHECK(g.col != 2);
}

// The class, not the instances.  Every command that a person presses expecting
// a drum to move must either move one or report that it did not - so each case
// here asserts that the port RECORDED something, and the negative cases assert
// the command was refused rather than silently accepted.
void test_commands_that_promise_motion_deliver_it() {
    struct Case {
        const char* what;
        bool (*run)(Rig&);
    };
    static const Case cases[] = {
        {"preset qmarks", [](Rig& r) { return r.mm.cmd_preset("qmarks", r.time.utc_ms).ok; }},
        {"preset blank", [](Rig& r) { return r.mm.cmd_preset("blank", r.time.utc_ms).ok; }},
        {"mode.set clock", [](Rig& r) { return r.mm.cmd_mode_set(Mode::Clock, r.time.utc_ms).ok; }},
        {"countdown.start",
         [](Rig& r) { return r.mm.cmd_countdown_start(r.time.utc_ms).ok; }},
        {"cal ramp", [](Rig& r) { return r.mm.cmd_cal_ramp(0, 0, 3, 1, 0, r.time.utc_ms).ok; }},
        {"display.frame",
         [](Rig& r) {
             Frame f{};
             for (int i = 0; i < N_COLUMNS; ++i) f.idx[static_cast<size_t>(i)] = 11;
             return r.mm.cmd_display_frame(f, r.time.utc_ms).ok;
         }},
    };
    for (const Case& c : cases) {
        Rig r;
        r.begin_at(utc_ms(2026, 1, 15, 17, 0, 0));
        // Park everything somewhere the command has to move away from.
        Frame park{};
        for (int i = 0; i < N_COLUMNS; ++i) park.idx[static_cast<size_t>(i)] = 30;
        CHECK(r.mm.cmd_display_frame(park, r.time.utc_ms).ok);
        r.run_to(r.time.utc_ms + 500);
        r.port.gos.clear();

        const bool accepted = c.run(r);
        r.run_to(r.time.utc_ms + 1500);
        if (accepted) {
            if (r.port.gos.empty()) {
                std::printf("  %s: accepted and NOTHING moved\n", c.what);
            }
            CHECK(!r.port.gos.empty());
        }
    }
}

}  // namespace

void run_tests() {
    test_numbers_validation();
    test_clock_12h();
    test_clock_24h();
    test_clock_dst_edges();
    test_clock_granularity();
    test_wifi_glyph();
    test_countdown_step_rules();
    test_countdown_ceiling_consequences();
    test_countdown_quiet_phase();
    test_countdown_freeze_boundary();
    test_countdown_modes();
    test_countdown_runs_offscreen();
    test_countdown_target_bounds();
    test_countdown_shown_is_monotonic();
    test_countdown_cues_and_zero();
    test_countdown_reveal_and_timeout();
    test_countdown_persistence_and_resume();
    test_arbitration();
    test_finished_countdown_never_replays();
    test_time_validity_gating();
    test_time_step_preserves_message_dwell();
    test_mode_set_message_requires_message();
    test_maintenance_suspends_everything();
    test_the_canon_reveal_resolves_on_both_rings();
    test_a_deadline_that_passes_while_held_wakes_silently();
    test_en_down_stops_the_frame_layer();
    test_maintenance_survives_and_blocks_countdown();
    test_excluded_columns_leave_a_hole();
    test_calibration_walk_runs_in_maintenance();
    test_calibration_walk_refuses_a_disabled_column();
    test_commands_that_promise_motion_deliver_it();
}
