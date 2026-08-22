// Mode arbitration + the three modes (spec 7).  Pure: time comes from
// TimeSource, frames go to the FrameScheduler, persistence goes through
// CountdownStore, audio cues through CueSink - all fakeable on the host.
//
// This class IS the command dispatcher's core (spec 10.2a): every transport
// (CLI now; web/MQTT/HA/button in later phases) funnels into the cmd_*
// methods, so no control path can bypass arbitration.
#pragma once

#include <array>
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

// The persisted countdown deadline (spec 7.3): one NVS write per set, never
// per tick.  seq breaks ties when the terminal prop also sets deadlines.
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
    int wifi_grace_s = 15;             // WIFI_GLYPH_GRACE_S (spec 7.1)
    int msg_dwell_s = 600;             // msg.dwell_s
    bool clock_land_on_tick = false;   // clock.land_on_tick
    bool cd_land_on_tick = true;       // countdown.land_on_tick
    int zero_hold_s = 3;               // countdown.zero_hold_s
    int spin_s = 6;                    // countdown.spin_s
    int failure_timeout_s = 0;         // countdown.failure_timeout_s
    int32_t alarm_flaps_s = 25;        // motion.flaps_s_alarm
    // countdown.reveal[5]: ring indices; -1 = unset -> blank (Nico has not
    // picked the glyphs yet - decision log).
    std::array<int, N_COLUMNS> reveal{-1, -1, -1, -1, -1};
};

class ModeManager {
public:
    static constexpr int COUNTDOWN_S = 6480;  // 108:00
    static constexpr const char* THE_NUMBERS = "4 8 15 16 23 42";

    ModeManager(const RingSet& ring, FrameScheduler& sched, TimeSource& time,
                CountdownStore& store, CueSink& cues)
        : ring_(ring), sched_(sched), time_(time), store_(store), cues_(cues) {}

    void set_config(const ModesConfig& c) { cfg_ = c; }
    const ModesConfig& config() const { return cfg_; }
    bool set_tz(std::string_view posix_tz);  // false = parse rejected, kept old

    // Load the persisted deadline and pick the boot mode: a live countdown
    // resumes (spec 7.3 power-cycle behaviour), otherwise clock.
    void begin(int64_t utc_ms);

    // Drive at >= a few Hz with UTC milliseconds.
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

    // --- status for CLI / web / tests ---
    Mode mode() const { return mode_; }
    CdPhase cd_phase() const { return cd_.phase; }
    int64_t cd_target() const { return cd_.target_utc; }
    int cd_shown() const { return cd_shown_; }
    bool wifi_glyph_shown() const { return wifi_glyph_; }

    // The numbers-string validator, exposed for tests and the future web UI.
    static bool numbers_valid(std::string_view s);

private:
    const RingSet& ring_;
    FrameScheduler& sched_;
    TimeSource& time_;
    CountdownStore& store_;
    CueSink& cues_;
    ModesConfig cfg_;
    TimeZone tz_;  // default-constructed = UTC0 until set_tz

    Mode mode_ = Mode::Clock;
    Mode prev_mode_ = Mode::Clock;

    // Clock state.
    int64_t rendered_minute_ = -1;  // floor(utc/60) of the last rendered frame
    int64_t invalid_since_ms_ = -1;
    bool wifi_glyph_ = false;

    // Message state.
    Frame msg_frame_{};
    bool msg_hold_ = false;
    int64_t msg_until_ms_ = 0;

    // Countdown state.
    CdPersist cd_;
    int cd_shown_ = -1;             // seconds currently displayed (multiple of 10)
    int64_t cd_scheduled_land_ = 0; // land_at_ms of the boundary already scheduled
    bool cue_warn4_ = false, cue_warn1_ = false, cue_zero_ = false;
    bool spin_started_ = false, reveal_shown_ = false;

    void enter_mode(Mode m, int64_t utc_ms);
    void tick_clock(int64_t utc_ms);
    void tick_message(int64_t utc_ms);
    void tick_countdown(int64_t utc_ms);
    void countdown_arm(int64_t target_utc, int64_t utc_ms);
    void persist();
    Frame reveal_frame() const;
};

}  // namespace swan
