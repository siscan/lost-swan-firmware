// Mode arbitration + the three modes (spec 7).  Pure: time comes from
// TimeSource, frames go to the FrameScheduler, persistence goes through
// CountdownStore, audio cues through CueSink - all fakeable on the host.
//
// This class IS the command dispatcher's core (spec 10.2a): every transport
// (CLI now; web/MQTT/HA/button in later phases) funnels into the cmd_*
// methods, so no control path can bypass arbitration.  All public methods
// serialize on an internal mutex - commands arrive from the console (later:
// httpd, MQTT) while the 20 Hz mode task ticks, and none of this state is
// atomic.  On ESP-IDF the std::mutex maps to a FreeRTOS mutex with priority
// inheritance.
#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

#include "frame/frame.h"
#include "modes/render.h"
#include "ring/ring_runtime.h"
#include "timesvc/time_source.h"
#include "timesvc/tz.h"

namespace swan {

enum class Mode : unsigned char { Clock, Message, Countdown };
enum class CdPhase : unsigned char { Idle, Running, Zero, Spin, Reveal };
enum class Cue : unsigned char { Warn4Min, Warn1Min, SystemFailure };

const char* mode_name(Mode m);
const char* cd_phase_name(CdPhase p);

// The persisted countdown deadline (spec 7.3): one NVS write per set or
// phase milestone, never per tick.  seq breaks ties when the terminal prop
// also sets deadlines.
struct CdPersist {
    CdPhase phase = CdPhase::Idle;
    int64_t target_utc = 0;
    uint32_t seq = 0;
};

class CountdownStore {
public:
    virtual ~CountdownStore() = default;
    virtual bool load(CdPersist& out) = 0;
    virtual void save(const CdPersist& s) = 0;
};

class CueSink {
public:
    virtual ~CueSink() = default;
    virtual void on_cue(Cue c) = 0;
};

struct ModesConfig {
    bool h24 = false;                  // clock.h24
    // clock.granularity_min (spec 7.1).  The rings are descending, so a clock
    // tick is the expensive direction; at 1 minute the display costs ~39,500
    // flips/day against ~5,900 at 15.  Clamped to 1..60.
    int granularity_min = 15;
    // countdown.seconds_mode (spec 7.3).  Live seconds are affordable now
    // that a decrement is 1 flip and column 5's wrap is 16 - but only in the
    // last stretch; the rest of the run holds MMM:00.
    SecondsMode seconds_mode = SecondsMode::Seconds;
    // countdown.seconds_live_s (spec 7.3).  Seconds are frozen on 00 above
    // this; below it they tick.  240 is the show's own 4-minute cue, so the
    // display coming alive and the warning sound are the same moment.
    int seconds_live_s = 240;
    int wifi_grace_s = 15;             // WIFI_GLYPH_GRACE_S (spec 7.1)
    int msg_dwell_s = 600;             // msg.dwell_s
    bool clock_land_on_tick = false;   // clock.land_on_tick
    bool cd_land_on_tick = true;       // countdown.land_on_tick
    int zero_hold_s = 3;               // countdown.zero_hold_s
    int spin_s = 6;                    // countdown.spin_s
    int failure_timeout_s = 0;         // countdown.failure_timeout_s
    int32_t alarm_flaps_s = 25;        // motion.flaps_s_alarm
    // countdown.reveal[5]: ring indices; -1 = unset -> blank (Nico has not
    // picked the glyphs yet - decision log).  NOTE: an index means a
    // different character on column 5, whose ring differs - see spec 11.
    std::array<int, N_COLUMNS> reveal{-1, -1, -1, -1, -1};
};

class ModeManager {
public:
    static constexpr int COUNTDOWN_S = 6480;  // 108:00
    // The furthest ahead a deadline may be set.  Guards against a millisecond
    // epoch, which truncates to a negative int remaining and parks the display
    // on 000:00 while claiming to run.
    static constexpr int64_t MAX_TARGET_AHEAD_S = 86400;
    static constexpr const char* THE_NUMBERS = "4 8 15 16 23 42";

    ModeManager(const RingSet& ring, FrameScheduler& sched, TimeSource& time,
                CountdownStore& store, CueSink& cues)
        : ring_(ring), sched_(sched), time_(time), store_(store), cues_(cues) {}

    void set_config(const ModesConfig& c);
    ModesConfig config() const;
    bool set_tz(std::string_view posix_tz);  // false = parse rejected, kept old

    // Load the persisted deadline and pick the boot mode.  A live countdown
    // resumes (spec 7.3 power-cycle behaviour) - but only once the time
    // source is valid: before the first SNTP sync the clock reads 1970 and
    // every deadline comparison would be garbage, so the resume is deferred
    // and performed by tick() when validity arrives.
    void begin(int64_t utc_ms);

    // Drive at >= a few Hz with UTC milliseconds.  Detects SNTP time steps
    // (any jump a 20 Hz cadence cannot produce) and re-arms wall-clock-
    // relative state: the message dwell keeps its remaining time, the clock
    // re-renders, scheduled land-on-tick boundaries are re-derived.  The
    // countdown needs nothing - its target is an absolute epoch.
    void tick(int64_t utc_ms);

    struct Result {
        bool ok;
        const char* err;  // static string, nullptr when ok
    };

    // --- the command set (spec 10.2a subset for phase 2) ---
    Result cmd_mode_set(Mode m, int64_t utc_ms);
    Result cmd_message_set(const std::array<std::string, N_COLUMNS>& tokens, int dwell_s,
                           bool hold, int64_t utc_ms);
    Result cmd_countdown_execute(std::string_view numbers, int64_t utc_ms);
    Result cmd_countdown_start(int64_t utc_ms);
    Result cmd_countdown_reset(int64_t utc_ms) { return cmd_countdown_start(utc_ms); }
    Result cmd_countdown_cancel(int64_t utc_ms);
    Result cmd_countdown_set_target(int64_t target_utc, int64_t utc_ms);
    Result cmd_preset(std::string_view name, int64_t utc_ms);
    Result cmd_display_frame(const Frame& f, int64_t utc_ms);  // no mode change
    Result cmd_clock_format(bool h24, int64_t utc_ms);

    // Adopt an uploaded ring table.  MODES TASK ONLY.  `swap` is invoked with
    // the mode lock held and returns true when it actually swapped: that is
    // what keeps a command arriving from the CLI or the HTTP task from reading
    // the table mid-assignment, since every other entry point takes the same
    // lock.  On a real swap the render markers are invalidated and the frame
    // re-issued - a software table change moves no drums, so without this the
    // columns stay parked on slots chosen from the OLD table until whatever
    // renders next, which in message, preset, reveal or countdown-idle is
    // never.
    Result cmd_ring_swap(const std::function<bool()>& swap, int64_t utc_ms);

    // Maintenance (spec 5.9).  Suspends the whole display: no rendering, no
    // frame scheduling, nothing moves on its own.  Manual commands keep
    // working - that is the point of it - and the motion layer separately
    // stops automatic re-homing.  Leaving re-arms and re-renders; the caller
    // re-homes.
    Result cmd_maintenance(bool on, int64_t utc_ms);
    bool maintenance() const;

    // Columns the frame layer must skip (bit i = column i).  Set when a
    // column is disabled.  Renderers are untouched: they still produce a full
    // frame and the hole is simply never issued.
    Result cmd_set_excluded(uint8_t mask, int64_t utc_ms);
    uint8_t excluded() const;

    // The active POSIX TZ string, read under the lock.  ModeManager owns it;
    // a mirrored copy elsewhere would be written by one task and read by
    // another with nothing between them.
    std::string tz_string() const;

    // Calibration walk (spec 5.6, Calibrate page): step `col` forward from
    // index `from` towards `to` in increments of `step`, dwelling `dwell_s` at
    // each stop.  Forward-only like every other move.  While a ramp runs it
    // owns the display - mode rendering is suspended and resumes when the ramp
    // finishes or is stopped, so the clock cannot fight the walk.
    Result cmd_cal_ramp(int col, int from, int to, int step, int dwell_s, int64_t utc_ms);
    Result cmd_cal_ramp_stop(int64_t utc_ms);
    bool cal_ramp_active() const;
    int cal_ramp_column() const;

    // --- status for CLI / web / tests ---
    Mode mode() const;
    CdPhase cd_phase() const;
    int64_t cd_target() const;
    bool wifi_glyph_shown() const;
    bool time_valid() const;

    // The numbers-string validator, exposed for tests and the web UI.
    static bool numbers_valid(std::string_view s);

    // Test hook.  Every public entry point bumps this inside the lock, so a
    // second thread getting in concurrently would push it above 1.  That is
    // how test_api proves the HTTP task never reaches mode state unlocked.
    int max_concurrent() const { return max_concurrent_.load(std::memory_order_relaxed); }

private:
    const RingSet& ring_;
    FrameScheduler& sched_;
    TimeSource& time_;
    CountdownStore& store_;
    CueSink& cues_;
    ModesConfig cfg_;
    TimeZone tz_;  // default-constructed = UTC0 until set_tz
    std::string tz_str_;  // the string it was parsed from, for reporting

    mutable std::mutex mu_;  // serializes every public entry point

    // Concurrency witness - see max_concurrent().
    mutable std::atomic<int> in_critical_{0};
    mutable std::atomic<int> max_concurrent_{0};
    struct Enter {
        const ModeManager& m;
        explicit Enter(const ModeManager& mm) : m(mm) {
            const int n = m.in_critical_.fetch_add(1, std::memory_order_relaxed) + 1;
            int prev = m.max_concurrent_.load(std::memory_order_relaxed);
            while (n > prev &&
                   !m.max_concurrent_.compare_exchange_weak(prev, n,
                                                            std::memory_order_relaxed)) {
            }
        }
        ~Enter() { m.in_critical_.fetch_sub(1, std::memory_order_relaxed); }
    };

    Mode mode_ = Mode::Clock;
    Mode prev_mode_ = Mode::Clock;

    // The frame currently displayed (or being moved to).  Renderers need it:
    // the rings are one-way and column 5 has two slots per digit, so the slot
    // that renders a character depends on where the column is now.  Starts on
    // the home slot, which is where homing leaves every column.
    Frame last_frame_ = Frame{{RING_HOME_SLOT, RING_HOME_SLOT, RING_HOME_SLOT,
                               RING_HOME_SLOT, RING_HOME_SLOT}};
    void issue(const Frame& f, int64_t utc_ms, int64_t land_at_ms = 0);

    // Clock state.  The key is the floored LOCAL time actually displayed, so
    // granularity and sub-hour UTC offsets both behave.
    int64_t rendered_key_ = -1;
    static constexpr int64_t RENDER_NONE = -1;
    static constexpr int64_t RENDER_SPECIAL = -2;  // blank/wifi frame is up
    int64_t invalid_since_ms_ = -1;
    bool wifi_glyph_ = false;

    // Message state.
    Frame msg_frame_{};
    bool msg_live_ = false;
    bool msg_hold_ = false;
    int64_t msg_until_ms_ = 0;

    // Countdown state.  cd_shown_ doubles as the render marker: >= 0 is the
    // shown countdown value, SHOWN_NONE forces a render, SHOWN_REVEAL means
    // the reveal frame is up.
    static constexpr int SHOWN_NONE = -1;
    static constexpr int SHOWN_REVEAL = -2;
    CdPersist cd_;
    int cd_shown_ = SHOWN_NONE;
    // Seconds per display window at the CURRENT point in the run - 60 in the
    // quiet phase, then 10 or 1.  Recomputed each tick; not a fixed property
    // of the mode any more (spec 7.3).
    int cd_step_s_ = 60;
    int64_t cd_scheduled_land_ = 0;
    bool cue_warn4_ = false, cue_warn1_ = false, cue_zero_ = false;
    bool spin_started_ = false;
    bool pending_resume_ = false;  // countdown resume deferred until time_valid
    bool maintenance_ = false;
    int64_t last_tick_ms_ = INT64_MIN;

    // Calibration walk state (control-side only).
    struct CalRamp {
        bool active = false;
        int col = 0;
        int to = 0;
        int step = 1;
        int64_t dwell_ms = 1000;
        int next_index = 0;
        int64_t due_ms = 0;
        bool last_stop = false;
    } ramp_;
    void tick_ramp(int64_t utc_ms);

    void enter_mode(Mode m, int64_t utc_ms);
    void tick_locked(int64_t utc_ms);
    void render_current(int64_t utc_ms);
    void tick_clock(int64_t utc_ms);
    void tick_message(int64_t utc_ms);
    void tick_countdown(int64_t utc_ms);
    void tick_countdown_offscreen(int64_t utc_ms);
    // A fresh set (execute/start/set_target): bumps seq and persists.  A past
    // target lands in Reveal SILENTLY - the choreography belongs to the
    // moment of zero, which already happened.
    void countdown_arm(int64_t target_utc, int64_t utc_ms);
    // Boot-time resume of a persisted deadline: no seq bump, no re-persist,
    // and never a replayed cue or spin.
    void countdown_resume(int64_t utc_ms);
    void enter_reveal_silently();
    void handle_time_step(int64_t delta_ms);
    void persist();
    Frame reveal_frame() const;
};

}  // namespace swan
