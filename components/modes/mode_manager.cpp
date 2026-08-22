#include "modes/mode_manager.h"

namespace swan {
namespace {

// Land-on-tick boundaries are scheduled this far ahead of "lead needed" so a
// coarse tick cadence cannot miss the start instant.
constexpr int64_t SCHEDULE_MARGIN_MS = 700;

}  // namespace

const char* mode_name(Mode m) {
    switch (m) {
        case Mode::Clock:     return "clock";
        case Mode::Message:   return "message";
        case Mode::Countdown: return "countdown";
    }
    return "?";
}

const char* cd_phase_name(CdPhase p) {
    switch (p) {
        case CdPhase::Idle:    return "idle";
        case CdPhase::Running: return "running";
        case CdPhase::Zero:    return "zero";
        case CdPhase::Spin:    return "spin";
        case CdPhase::Reveal:  return "reveal";
    }
    return "?";
}

bool ModeManager::numbers_valid(std::string_view s) {
    // The ritual: 4 8 15 16 23 42, any whitespace between, nothing else.
    static constexpr int WANT[6] = {4, 8, 15, 16, 23, 42};
    size_t i = 0;
    for (int expected : WANT) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        int v = 0, digits = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9' && digits < 3) {
            v = v * 10 + (s[i] - '0');
            ++i;
            ++digits;
        }
        if (digits == 0 || v != expected) return false;
    }
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return i == s.size();
}

bool ModeManager::set_tz(std::string_view posix_tz) {
    TimeZone tz;
    if (!TimeZone::parse(posix_tz, tz)) return false;
    tz_ = tz;
    rendered_minute_ = -1;  // re-render with the new zone
    return true;
}

void ModeManager::begin(int64_t utc_ms) {
    CdPersist p;
    if (store_.load(p) && p.phase != CdPhase::Idle && p.target_utc > 0) {
        // A live countdown survives a power cycle (spec 7.3).  Re-derive the
        // phase from the deadline rather than trusting a stale stored phase.
        cd_ = p;
        countdown_arm(p.target_utc, utc_ms);
        enter_mode(Mode::Countdown, utc_ms);
        return;
    }
    enter_mode(Mode::Clock, utc_ms);
}

void ModeManager::enter_mode(Mode m, int64_t utc_ms) {
    if (mode_ != m) prev_mode_ = mode_;
    mode_ = m;
    rendered_minute_ = -1;   // clock re-renders on entry
    cd_shown_ = -1;          // countdown re-renders on entry
    cd_scheduled_land_ = 0;
    tick(utc_ms);
}

void ModeManager::tick(int64_t utc_ms) {
    switch (mode_) {
        case Mode::Clock:     tick_clock(utc_ms); break;
        case Mode::Message:   tick_message(utc_ms); break;
        case Mode::Countdown: tick_countdown(utc_ms); break;
    }
    sched_.tick(utc_ms);
}

// ---------------------------------------------------------------------------
// Clock (spec 7.1)
// ---------------------------------------------------------------------------
void ModeManager::tick_clock(int64_t utc_ms) {
    if (!time_.valid()) {
        if (invalid_since_ms_ < 0) invalid_since_ms_ = utc_ms;
        const bool grace_over =
            utc_ms - invalid_since_ms_ >= static_cast<int64_t>(cfg_.wifi_grace_s) * 1000;
        // All blank after homing; the WiFi glyph on the centre column once the
        // grace expires.  A drop AFTER a successful sync never shows the glyph
        // (time_.valid() is sticky).
        const bool want_glyph = grace_over;
        if (want_glyph != wifi_glyph_ || rendered_minute_ == -1) {
            wifi_glyph_ = want_glyph;
            rendered_minute_ = -2;  // not a real minute; forces re-render on sync
            sched_.show(want_glyph ? render_wifi(ring_) : render_blank(ring_), utc_ms);
        }
        return;
    }
    wifi_glyph_ = false;

    // Render at second 0 of every local minute (offsets are whole minutes, so
    // local second-0 == UTC second-0).
    const int64_t minute = utc_ms / 60000;
    if (minute == rendered_minute_) return;
    rendered_minute_ = minute;

    const LocalTime lt = tz_.to_local(utc_ms / 1000);
    // Moves START on the tick by default (clock.land_on_tick = false).
    if (cfg_.clock_land_on_tick) {
        // Optional: land the NEXT minute's frame on its boundary.
        const int64_t next_boundary = (minute + 1) * 60000;
        const LocalTime nx = tz_.to_local(next_boundary / 1000);
        sched_.show(render_clock(ring_, nx, cfg_.h24), utc_ms, next_boundary);
    } else {
        sched_.show(render_clock(ring_, lt, cfg_.h24), utc_ms);
    }
}

// ---------------------------------------------------------------------------
// Message (spec 7.2)
// ---------------------------------------------------------------------------
void ModeManager::tick_message(int64_t utc_ms) {
    if (!msg_hold_ && utc_ms >= msg_until_ms_) {
        enter_mode(prev_mode_, utc_ms);
    }
}

// ---------------------------------------------------------------------------
// Countdown (spec 7.3) - a deadline, not a timer.
// ---------------------------------------------------------------------------
void ModeManager::countdown_arm(int64_t target_utc, int64_t utc_ms) {
    cd_.target_utc = target_utc;
    ++cd_.seq;
    cd_.phase = (target_utc * 1000 > utc_ms) ? CdPhase::Running : CdPhase::Zero;
    cd_shown_ = -1;
    cd_scheduled_land_ = 0;
    cue_warn4_ = cue_warn1_ = cue_zero_ = false;
    spin_started_ = reveal_shown_ = false;
    // Cues strictly in the future only: resuming with 90 s left must not
    // replay the 4-minute warning.
    const int64_t rem_s = target_utc - utc_ms / 1000;
    if (rem_s <= 240) cue_warn4_ = true;
    if (rem_s <= 60) cue_warn1_ = true;
    persist();
}

Frame ModeManager::reveal_frame() const {
    Frame f = render_blank(ring_);
    for (int i = 0; i < N_COLUMNS; ++i) {
        const int idx = cfg_.reveal[static_cast<size_t>(i)];
        if (idx >= 0 && idx < ring_.col(i).slot_count()) f.idx[static_cast<size_t>(i)] = idx;
    }
    return f;
}

void ModeManager::tick_countdown(int64_t utc_ms) {
    if (cd_.phase == CdPhase::Idle) {
        // Idle countdown: a static 108:00 until started (spec 10.2a mode.set).
        if (cd_shown_ != COUNTDOWN_S) {
            cd_shown_ = COUNTDOWN_S;
            sched_.show(render_countdown(ring_, COUNTDOWN_S), utc_ms);
        }
        return;
    }

    const int64_t target_ms = cd_.target_utc * 1000;
    const int64_t rem_ms = target_ms - utc_ms;

    if (cd_.phase == CdPhase::Running) {
        // Audio cues from the deadline (spec 7.3 / Q4).
        if (!cue_warn4_ && rem_ms <= 240 * 1000) {
            cue_warn4_ = true;
            cues_.on_cue(Cue::Warn4Min);
        }
        if (!cue_warn1_ && rem_ms <= 60 * 1000) {
            cue_warn1_ = true;
            cues_.on_cue(Cue::Warn1Min);
        }
        if (rem_ms <= 0) {
            cd_.phase = CdPhase::Zero;
            persist();  // a reboot after zero must wake into the zero state
        }
    }

    if (cd_.phase == CdPhase::Running) {
        const int shown = static_cast<int>((rem_ms / 1000) / 10) * 10;

        if (cd_shown_ < 0) {
            // Entry / resume: show the current window immediately.
            cd_shown_ = shown;
            sched_.show(render_countdown(ring_, shown), utc_ms);
        } else if (!cfg_.cd_land_on_tick && shown != cd_shown_) {
            cd_shown_ = shown;
            sched_.show(render_countdown(ring_, shown), utc_ms);
        }

        if (cfg_.cd_land_on_tick && shown > 0) {
            // The frame for the next window lands exactly when rem hits
            // `shown` (the terminal screen is the reference - spec 7.3).
            const int64_t land = target_ms - static_cast<int64_t>(shown) * 1000;
            if (cd_scheduled_land_ != land) {
                const Frame next = render_countdown(ring_, shown - 10);
                if (utc_ms + sched_.lead_ms(next) + SCHEDULE_MARGIN_MS >= land) {
                    sched_.show(next, utc_ms, land);
                    cd_scheduled_land_ = land;
                    cd_shown_ = shown - 10;
                }
            }
        }
        return;
    }

    // Zero choreography (spec 7.3 / Q4): 000:00, hold, alarm spin, reveal.
    if (!cue_zero_) {
        cue_zero_ = true;
        cues_.on_cue(Cue::SystemFailure);
        cd_shown_ = 0;
        sched_.show(render_countdown(ring_, 0), utc_ms);  // ensure 000:00
    }

    const int64_t since_zero = utc_ms - target_ms;
    const int64_t hold_ms = static_cast<int64_t>(cfg_.zero_hold_s) * 1000;
    const int64_t spin_ms = static_cast<int64_t>(cfg_.spin_s) * 1000;

    if (cd_.phase == CdPhase::Zero && since_zero >= hold_ms) {
        cd_.phase = CdPhase::Spin;
        if (!spin_started_) {
            spin_started_ = true;
            sched_.spin_all(cfg_.alarm_flaps_s, cfg_.spin_s);
        }
    }
    if (cd_.phase == CdPhase::Spin && since_zero >= hold_ms + spin_ms) {
        cd_.phase = CdPhase::Reveal;
        persist();
    }
    if (cd_.phase == CdPhase::Reveal) {
        if (!reveal_shown_) {
            reveal_shown_ = true;
            sched_.show(reveal_frame(), utc_ms);  // convergence lands it post-spin
        }
        // No auto-return unless configured (spec 7.3).
        if (cfg_.failure_timeout_s > 0 &&
            since_zero >= hold_ms + spin_ms + static_cast<int64_t>(cfg_.failure_timeout_s) * 1000) {
            cd_.phase = CdPhase::Idle;
            persist();
            enter_mode(Mode::Clock, utc_ms);
        }
    }
}

void ModeManager::persist() { store_.save(cd_); }

// ---------------------------------------------------------------------------
// Commands (spec 10.2a)
// ---------------------------------------------------------------------------
ModeManager::Result ModeManager::cmd_mode_set(Mode m, int64_t utc_ms) {
    enter_mode(m, utc_ms);
    return {true, nullptr};
}

ModeManager::Result ModeManager::cmd_message_set(
    const std::array<std::string, N_COLUMNS>& tokens, int dwell_s, bool hold, int64_t utc_ms) {
    Frame f;
    for (int i = 0; i < N_COLUMNS; ++i) {
        const int idx = ring_.col(i).index_for_token(tokens[static_cast<size_t>(i)]);
        if (idx < 0) return {false, "unknown token"};
        f.idx[static_cast<size_t>(i)] = idx;
    }
    msg_frame_ = f;
    msg_hold_ = hold;
    msg_until_ms_ = utc_ms + static_cast<int64_t>(dwell_s > 0 ? dwell_s : cfg_.msg_dwell_s) * 1000;
    enter_mode(Mode::Message, utc_ms);
    sched_.show(msg_frame_, utc_ms);
    return {true, nullptr};
}

ModeManager::Result ModeManager::cmd_countdown_execute(std::string_view numbers,
                                                       int64_t utc_ms) {
    if (!numbers_valid(numbers)) return {false, "rejected"};  // wrong Numbers
    return cmd_countdown_start(utc_ms);
}

ModeManager::Result ModeManager::cmd_countdown_start(int64_t utc_ms) {
    countdown_arm(utc_ms / 1000 + COUNTDOWN_S, utc_ms);
    enter_mode(Mode::Countdown, utc_ms);  // countdown overrides whatever runs
    return {true, nullptr};
}

ModeManager::Result ModeManager::cmd_countdown_cancel(int64_t utc_ms) {
    cd_.phase = CdPhase::Idle;
    cd_shown_ = -1;
    persist();
    if (mode_ == Mode::Countdown) tick(utc_ms);
    return {true, nullptr};
}

ModeManager::Result ModeManager::cmd_countdown_set_target(int64_t target_utc, int64_t utc_ms) {
    if (target_utc <= 0) return {false, "bad epoch"};
    countdown_arm(target_utc, utc_ms);
    enter_mode(Mode::Countdown, utc_ms);
    return {true, nullptr};
}

ModeManager::Result ModeManager::cmd_preset(std::string_view name, int64_t utc_ms) {
    Frame f;
    if (name == "qmarks") f = render_qmarks(ring_);
    else if (name == "blank") f = render_blank(ring_);
    else if (name == "wifi") f = render_wifi(ring_);
    else if (name == "reveal") f = reveal_frame();
    else return {false, "unknown preset"};

    // A preset behaves like a held message: it stays until the mode changes.
    msg_frame_ = f;
    msg_hold_ = true;
    enter_mode(Mode::Message, utc_ms);
    sched_.show(msg_frame_, utc_ms);
    return {true, nullptr};
}

ModeManager::Result ModeManager::cmd_display_frame(const Frame& f, int64_t utc_ms) {
    // Raw frame for props and tests - deliberately does NOT change the mode;
    // the active mode may overwrite it at its next render (spec 10.2a).
    for (int i = 0; i < N_COLUMNS; ++i) {
        if (f.idx[static_cast<size_t>(i)] < 0 ||
            f.idx[static_cast<size_t>(i)] >= ring_.col(i).slot_count()) {
            return {false, "index out of range"};
        }
    }
    sched_.show(f, utc_ms);
    return {true, nullptr};
}

ModeManager::Result ModeManager::cmd_clock_format(bool h24, int64_t utc_ms) {
    cfg_.h24 = h24;
    rendered_minute_ = -1;  // re-render immediately in the new format
    if (mode_ == Mode::Clock) tick(utc_ms);
    return {true, nullptr};
}

}  // namespace swan
