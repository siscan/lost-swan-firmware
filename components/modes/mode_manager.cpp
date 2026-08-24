#include "modes/mode_manager.h"

#include <cstdint>

namespace swan {
namespace {

// Land-on-tick boundaries are scheduled this far ahead of "lead needed" so a
// coarse tick cadence cannot miss the start instant.
constexpr int64_t SCHEDULE_MARGIN_MS = 700;

// A tick-to-tick jump outside this window cannot come from the 20 Hz cadence;
// it is an SNTP step (first sync jumping years, or a resync correction).
constexpr int64_t STEP_FORWARD_MS = 5000;
constexpr int64_t STEP_BACKWARD_MS = -1000;

int clamp_granularity(int g) { return g < 1 ? 1 : (g > 60 ? 60 : g); }

// The displayed time: minute floored to the granularity, seconds dropped.
LocalTime floor_to(const LocalTime& lt, int g) {
    LocalTime out = lt;
    out.minute = (lt.minute / g) * g;
    out.second = 0;
    return out;
}

// Identifies the displayed window.  Built from the LOCAL floored time, so it
// is correct for zones whose offset is not a whole multiple of the
// granularity (India's +5:30 against a 15-minute grid, say).
int64_t local_key(const LocalTime& lt) {
    return (((static_cast<int64_t>(lt.year) * 12 + lt.month) * 31 + lt.day) * 24 + lt.hour) *
               60 + lt.minute;
}

// UTC ms of the next granularity boundary after now.
int64_t next_boundary_ms(int64_t utc_ms, const LocalTime& now, int g) {
    const int into = now.minute % g;
    const int64_t ahead_s = static_cast<int64_t>(g - into) * 60 - now.second;
    return ((utc_ms / 1000) + ahead_s) * 1000;
}

}  // namespace

const char* mode_name(Mode m) {
    switch (m) {
        case Mode::Clock:     return "clock";
        case Mode::Message:   return "message";
        case Mode::Countdown: return "countdown";
    }
    return "?";
}

const char* origin_name(Origin o) {
    switch (o) {
        case Origin::Unknown: return "unknown";
        case Origin::Ui:      return "ui";
        case Origin::Mqtt:    return "mqtt";
        case Origin::Cli:     return "cli";
        case Origin::Button:  return "button";
        case Origin::Ha:      return "ha";
    }
    return "unknown";
}

bool origin_from_name(std::string_view s, Origin& out) {
    if (s == "ui") out = Origin::Ui;
    else if (s == "mqtt") out = Origin::Mqtt;
    else if (s == "cli") out = Origin::Cli;
    else if (s == "button") out = Origin::Button;
    else if (s == "ha") out = Origin::Ha;
    else if (s == "unknown" || s.empty()) out = Origin::Unknown;
    else return false;
    return true;
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

void ModeManager::set_config(const ModesConfig& c) {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    cfg_ = c;
}

ModesConfig ModeManager::config() const {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    return cfg_;
}

bool ModeManager::set_tz(std::string_view posix_tz) {
    TimeZone tz;
    if (!TimeZone::parse(posix_tz, tz)) return false;
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    tz_ = tz;
    tz_str_.assign(posix_tz);
    rendered_key_ = RENDER_NONE;  // re-render with the new zone
    return true;
}

Mode ModeManager::mode() const {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    return mode_;
}
CdPhase ModeManager::cd_phase() const {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    return cd_.phase;
}
int64_t ModeManager::cd_target() const {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    return cd_.target_utc;
}
std::string ModeManager::message_tokens() const {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    if (!msg_live_) return {};
    std::string out;
    for (const auto& t : msg_tokens_) {
        if (!out.empty()) out += ' ';
        out += t.empty() ? "_" : t;
    }
    return out;
}

LocalTime ModeManager::local_now() const {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    return tz_.to_local(time_.now_utc());
}

uint32_t ModeManager::cd_seq() const {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    return cd_.seq;
}
Origin ModeManager::cd_set_by() const {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    return cd_.set_by;
}
bool ModeManager::time_valid() const {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    return time_.valid();
}
bool ModeManager::wifi_glyph_shown() const {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    return wifi_glyph_;
}

void ModeManager::begin(int64_t utc_ms) {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);

    CdPersist p;
    if (store_.load(p) && p.phase != CdPhase::Idle && p.target_utc > 0) {
        cd_ = p;
        if (time_.valid()) {
            countdown_resume(utc_ms);
            enter_mode(Mode::Countdown, utc_ms);
            return;
        }
        // Before the first SNTP sync the clock reads 1970; comparing the
        // stored 2026 deadline against it would mis-derive everything.  Boot
        // as a clock (blank / WiFi glyph) and resume when validity arrives.
        pending_resume_ = true;
    }
    enter_mode(Mode::Clock, utc_ms);
}

void ModeManager::enter_mode(Mode m, int64_t utc_ms) {
    // Leaving the countdown after zero ends the run: the reveal holds only
    // while the mode does (spec 7.3 "until the mode is changed"), and a
    // finished run left in NVS would replay the failure choreography on the
    // next boot - possibly days later.  A RUNNING deadline survives the mode
    // switch: it is the shared clock the terminal prop renders too.
    if (mode_ == Mode::Countdown && m != Mode::Countdown &&
        (cd_.phase == CdPhase::Zero || cd_.phase == CdPhase::Spin ||
         cd_.phase == CdPhase::Reveal)) {
        cd_.phase = CdPhase::Idle;
        persist();
    }

    if (mode_ != m) {
        prev_mode_ = mode_;
        note_locked(journal::Event::Kind::ModeChange, mode_name(m));
    }
    mode_ = m;
    rendered_key_ = RENDER_NONE;  // clock re-renders on entry
    cd_shown_ = SHOWN_NONE;          // countdown re-renders on entry
    cd_scheduled_land_ = 0;
    tick_locked(utc_ms);
}

void ModeManager::tick(int64_t utc_ms) {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    tick_locked(utc_ms);
}

void ModeManager::render_current(int64_t utc_ms) {
    switch (mode_) {
        case Mode::Clock:     tick_clock(utc_ms); break;
        case Mode::Message:   tick_message(utc_ms); break;
        case Mode::Countdown: tick_countdown(utc_ms); break;
    }
}

// The calibration walk owns the display while it runs (spec 5.6).
void ModeManager::tick_ramp(int64_t utc_ms) {
    if (utc_ms < ramp_.due_ms) return;

    Frame f = last_frame_;
    f.idx[static_cast<size_t>(ramp_.col)] = ramp_.next_index;
    issue(f, utc_ms);
    ramp_.due_ms = utc_ms + ramp_.dwell_ms;

    if (ramp_.last_stop) {
        ramp_.active = false;
        rendered_key_ = RENDER_NONE;  // the mode re-renders on the next tick
        cd_shown_ = SHOWN_NONE;
        return;
    }
    const int remaining = ring_forward_distance(ramp_.next_index, ramp_.to);
    if (remaining == 0) {
        ramp_.last_stop = true;
    } else if (remaining <= ramp_.step) {
        ramp_.next_index = ramp_.to;   // final partial step lands exactly on `to`
        ramp_.last_stop = true;
    } else {
        ramp_.next_index =
            (ramp_.next_index + ramp_.step) % ring_.col(ramp_.col).slot_count();
    }
}

ModeManager::Result ModeManager::cmd_maintenance(bool on, int64_t utc_ms) {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    if (maintenance_ == on) return {true, nullptr};
    maintenance_ = on;
    note_locked(journal::Event::Kind::Maintenance, on ? "on" : "off");
    if (on) {
        // Drop anything scheduled.  Nothing may move on its own from here.
        sched_.cancel_pending();
        return {true, nullptr};
    }
    // Leaving re-arms: re-render from scratch, since the drums have been moved
    // by hand and last_frame_ is the only record of where they were told to be.
    rendered_key_ = RENDER_NONE;
    cd_shown_ = SHOWN_NONE;
    cd_scheduled_land_ = 0;
    tick_locked(utc_ms);
    return {true, nullptr};
}

ModeManager::Result ModeManager::cmd_ota_hold(bool on, int64_t utc_ms) {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    if (ota_hold_ == on) return {true, nullptr};
    ota_hold_ = on;
    if (on) {
        sched_.cancel_pending();
    } else {
        // Re-render at once so the display is not left on whatever frame the
        // upload interrupted.
        tick_locked(utc_ms);
    }
    return {true, nullptr};
}

bool ModeManager::ota_hold() const {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    return ota_hold_;
}

bool ModeManager::maintenance() const {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    return maintenance_;
}

ModeManager::Result ModeManager::cmd_set_excluded(uint8_t mask, int64_t utc_ms) {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    if (sched_.excluded() == mask) return {true, nullptr};
    sched_.set_excluded(mask);
    // A column joining or leaving the frame changes what should be showing.
    rendered_key_ = RENDER_NONE;
    cd_shown_ = SHOWN_NONE;
    if (!maintenance_) tick_locked(utc_ms);
    return {true, nullptr};
}

uint8_t ModeManager::excluded() const {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    return sched_.excluded();
}

void ModeManager::tick_locked(int64_t utc_ms) {
    if (last_tick_ms_ != INT64_MIN) {
        const int64_t delta = utc_ms - last_tick_ms_;
        if (delta > STEP_FORWARD_MS || delta < STEP_BACKWARD_MS) handle_time_step(delta);
    }
    last_tick_ms_ = utc_ms;

    // Maintenance: the deadline keeps running (it is absolute, and a repair
    // should not silently cancel a countdown) but NOTHING is rendered or
    // scheduled.  Cues are held too - a system-failure alarm going off while
    // someone has their hands in the mechanism is exactly wrong.
    //
    // ONE exception, and it is the point of the mode: the calibration walk is a
    // MANUAL command, and spec 5.9 says manual commands work in maintenance
    // because "a suspect column is driven by hand from the Calibrate page".
    // The walk used to sit below this gate, so in maintenance motion.ramp
    // returned ok, the page reported "walking column N", cal.ramp_active stayed
    // true - and the column never moved.  Measured on the board: IDLE at the
    // same index for five seconds against a one-second dwell.
    //
    // An OTA hold is different and keeps suppressing it: motion is held for the
    // duration by design, and the dispatcher already refuses motion.ramp there.
    if (maintenance_ || ota_hold_) {
        if (ramp_.active && !ota_hold_) tick_ramp(utc_ms);
        return;
    }

    if (pending_resume_ && time_.valid()) {
        pending_resume_ = false;
        countdown_resume(utc_ms);
        enter_mode(Mode::Countdown, utc_ms);
        return;  // enter_mode already ticked
    }

    // The deadline runs whatever is on the display.  enter_mode deliberately
    // keeps a Running countdown alive across a mode switch, so if only the
    // countdown MODE advanced it, switching to the clock mid-run would skip
    // the cues and the zero entirely - and then fire all of them at once
    // whenever countdown mode was next entered, possibly days later.  Cues and
    // phase are unconditional; only the rendering is mode-gated.
    if (mode_ != Mode::Countdown) tick_countdown_offscreen(utc_ms);

    if (ramp_.active) {
        tick_ramp(utc_ms);
    } else {
        render_current(utc_ms);
    }
    sched_.tick(utc_ms);
}

void ModeManager::handle_time_step(int64_t delta_ms) {
    // Wall-clock-relative state shifts with the step; deadline state (the
    // countdown target) is absolute and needs nothing.
    msg_until_ms_ += delta_ms;
    if (invalid_since_ms_ >= 0) invalid_since_ms_ += delta_ms;
    rendered_key_ = RENDER_NONE;
    cd_scheduled_land_ = 0;
    sched_.cancel_pending();  // its start time lived in the old timebase
    if (cd_.phase == CdPhase::Running) cd_shown_ = SHOWN_NONE;  // re-render now
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
        if (grace_over != wifi_glyph_ || rendered_key_ == RENDER_NONE) {
            wifi_glyph_ = grace_over;
            rendered_key_ = RENDER_SPECIAL;
            issue(grace_over ? render_wifi(ring_, last_frame_) : render_blank(ring_, last_frame_),
                  utc_ms);
        }
        return;
    }
    wifi_glyph_ = false;

    // The rings are descending, so a clock tick is the expensive direction.
    // clock.granularity_min floors the displayed minute; at 1 minute the
    // display costs ~39,500 flips/day, at 15 about ~5,900 (spec 7.1).
    const int g = clamp_granularity(cfg_.granularity_min);
    const LocalTime now = tz_.to_local(utc_ms / 1000);
    const LocalTime shown = floor_to(now, g);
    const int64_t key = local_key(shown);
    if (key == rendered_key_) return;
    const bool fresh_entry = (rendered_key_ < 0);
    rendered_key_ = key;

    if (!cfg_.clock_land_on_tick || fresh_entry) {
        // Show the current window now - on entry even in land-on-tick mode,
        // otherwise the display would sit stale until the next boundary.
        issue(render_clock(ring_, shown, cfg_.h24, last_frame_), utc_ms);
    }
    if (cfg_.clock_land_on_tick) {
        // Land the NEXT window's frame on its boundary.
        const int64_t next_ms = next_boundary_ms(utc_ms, now, g);
        const LocalTime nx = floor_to(tz_.to_local(next_ms / 1000), g);
        issue(render_clock(ring_, nx, cfg_.h24, last_frame_), utc_ms, next_ms);
    }
}

// ---------------------------------------------------------------------------
// Message (spec 7.2)
// ---------------------------------------------------------------------------
void ModeManager::tick_message(int64_t utc_ms) {
    if (!msg_hold_ && utc_ms >= msg_until_ms_) {
        msg_live_ = false;
        enter_mode(prev_mode_, utc_ms);
    }
}

// ---------------------------------------------------------------------------
// Countdown (spec 7.3) - a deadline, not a timer.
// ---------------------------------------------------------------------------
void ModeManager::countdown_arm(int64_t target_utc, int64_t utc_ms, Origin by) {
    cd_.target_utc = target_utc;
    cd_.set_by = by;
    ++cd_.seq;
    cd_shown_ = SHOWN_NONE;
    cd_scheduled_land_ = 0;
    cue_warn4_ = cue_warn1_ = cue_zero_ = false;
    spin_started_ = false;
    const int64_t rem_s = target_utc - utc_ms / 1000;
    if (rem_s > 0) {
        cd_.phase = CdPhase::Running;
        // Cues strictly in the future only.
        if (rem_s <= 240) cue_warn4_ = true;
        if (rem_s <= 60) cue_warn1_ = true;
    } else {
        // A deadline already in the past (terminal prop peer, or clock steps):
        // the zero moment happened without us - land on the reveal without
        // re-running the alarm or the spin.
        enter_reveal_silently();
    }
    persist();
}

void ModeManager::countdown_resume(int64_t utc_ms) {
    // cd_ already holds the persisted record.  No seq bump, no re-persist.
    cd_shown_ = SHOWN_NONE;
    cd_scheduled_land_ = 0;
    cue_warn4_ = cue_warn1_ = cue_zero_ = false;
    spin_started_ = false;

    const int64_t rem_s = cd_.target_utc - utc_ms / 1000;
    if (rem_s > 0) {
        cd_.phase = CdPhase::Running;
        if (rem_s <= 240) cue_warn4_ = true;  // never replay past cues
        if (rem_s <= 60) cue_warn1_ = true;
    } else {
        // The zero moment is in the past - whether we died during Running,
        // Zero, Spin or Reveal, the choreography belongs to that moment and
        // must not replay.  Wake straight into the reveal.
        enter_reveal_silently();
    }
}

void ModeManager::enter_reveal_silently() {
    cd_.phase = CdPhase::Reveal;
    cue_zero_ = true;      // the cue fired (or should have) at the real zero
    spin_started_ = true;  // ditto the spin
    cd_shown_ = SHOWN_NONE;  // forces the reveal frame to render
}

Frame ModeManager::reveal_frame() const {
    Frame f = render_blank(ring_, last_frame_);
    for (int i = 0; i < N_COLUMNS; ++i) {
        const int idx = cfg_.reveal[static_cast<size_t>(i)];
        if (idx >= 0 && idx < ring_.col(i).slot_count()) f.idx[static_cast<size_t>(i)] = idx;
    }
    return f;
}

// The cue and phase half of tick_countdown, run while some other mode owns the
// display.  Deliberately issues no frames: the clock (or a message) is what
// should be showing.  Audio still plays - spec 7.3 puts the cues on the
// deadline, not on the visible mode - and the phase advances so that entering
// countdown mode later shows the reveal, not a replay of the alarm.
void ModeManager::tick_countdown_offscreen(int64_t utc_ms) {
    if (cd_.phase != CdPhase::Running) return;
    const int64_t rem_ms = cd_.target_utc * 1000 - utc_ms;

    if (!cue_warn4_ && rem_ms <= 240 * 1000) {
        cue_warn4_ = true;
        cues_.on_cue(Cue::Warn4Min);
    }
    if (!cue_warn1_ && rem_ms <= 60 * 1000) {
        cue_warn1_ = true;
        cues_.on_cue(Cue::Warn1Min);
    }
    if (rem_ms > 0) return;

    if (!cue_zero_) {
        cue_zero_ = true;
        cues_.on_cue(Cue::SystemFailure);
    }
    // No spin and no 000:00 - the columns are showing something else, and
    // seizing them would be a countdown overriding a mode it does not own.
    // Land straight in Reveal so a later mode.set countdown shows the reveal
    // rather than replaying the choreography.
    cd_.phase = CdPhase::Reveal;
    spin_started_ = true;
    cd_shown_ = SHOWN_NONE;
    persist();
}

void ModeManager::tick_countdown(int64_t utc_ms) {
    const SecondsMode mode = cfg_.seconds_mode;
    const int live_s = cfg_.seconds_live_s;

    if (cd_.phase == CdPhase::Idle) {
        // Idle countdown: a static 108:00 until started (spec 10.2a mode.set).
        if (cd_shown_ != COUNTDOWN_S) {
            cd_shown_ = COUNTDOWN_S;
            issue(render_countdown(ring_, COUNTDOWN_S, last_frame_), utc_ms);
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
            persist();  // a reboot after zero must wake into the reveal
            // The event the whole machine exists for.
            note_locked(journal::Event::Kind::CountdownZero, nullptr, cd_.seq, cd_.set_by);
        }
    }

    if (cd_.phase == CdPhase::Running) {
        const int rem_s = static_cast<int>(rem_ms / 1000);
        const int shown = countdown_shown_s(mode, rem_s, live_s);
        cd_step_s_ = countdown_step_s(mode, rem_s, live_s);

        if (cd_shown_ < 0) {
            // Entry / resume: show the current window immediately.
            cd_shown_ = shown;
            issue(render_countdown(ring_, shown, last_frame_), utc_ms);
        } else if (!cfg_.cd_land_on_tick && shown != cd_shown_) {
            cd_shown_ = shown;
            issue(render_countdown(ring_, shown, last_frame_), utc_ms);
        }

        if (cfg_.cd_land_on_tick && shown > 0) {
            // The frame for the next window lands exactly when rem hits
            // `shown` (the terminal screen is the reference - spec 7.3).  In
            // seconds mode a wrap can need longer than the one-second window;
            // FrameScheduler::show then starts it immediately and it lands a
            // little late rather than never, catching up on the next tick
            // because forward-only moves just extend (spec 7.3 timing note).
            //
            // The next value comes from countdown_next_shown_s, not from
            // `shown - step`: the window below this one may be a different
            // size, which is exactly what happens at the freeze boundary
            // (300 -> 240 while quiet, then 240 -> 239 once seconds are live).
            const int64_t land = target_ms - static_cast<int64_t>(shown) * 1000;
            if (cd_scheduled_land_ != land) {
                const int next_shown = countdown_next_shown_s(mode, shown, live_s);
                const Frame next = render_countdown(ring_, next_shown, last_frame_);
                if (utc_ms + sched_.lead_ms(next) + SCHEDULE_MARGIN_MS >= land) {
                    issue(next, utc_ms, land);
                    cd_scheduled_land_ = land;
                    cd_shown_ = next_shown;
                }
            }
        }
        return;
    }

    // Zero choreography (spec 7.3 / Q4): 000:00, hold, alarm spin, reveal.
    // Rendering keys off cd_shown_, so re-entering the mode mid-choreography
    // re-renders instead of leaving the previous mode's frame up.
    if (!cue_zero_) {
        cue_zero_ = true;
        cues_.on_cue(Cue::SystemFailure);
    }
    if (cd_.phase == CdPhase::Zero && cd_shown_ != 0) {
        cd_shown_ = 0;
        issue(render_countdown(ring_, 0, last_frame_), utc_ms);  // 000:00
    }

    const int64_t since_zero = utc_ms - target_ms;
    const int64_t hold_ms = static_cast<int64_t>(cfg_.zero_hold_s) * 1000;
    const int64_t spin_ms = static_cast<int64_t>(cfg_.spin_s) * 1000;

    if (cd_.phase == CdPhase::Zero && since_zero >= hold_ms) {
        cd_.phase = CdPhase::Spin;
        if (!spin_started_) {
            spin_started_ = true;
            sched_.spin_all(cfg_.alarm_flaps_s, cfg_.spin_s, utc_ms);
        }
    }
    if (cd_.phase == CdPhase::Spin && since_zero >= hold_ms + spin_ms) {
        cd_.phase = CdPhase::Reveal;
        cd_shown_ = SHOWN_NONE;
        persist();
    }
    if (cd_.phase == CdPhase::Reveal) {
        if (cd_shown_ != SHOWN_REVEAL) {
            cd_shown_ = SHOWN_REVEAL;
            issue(reveal_frame(), utc_ms);  // convergence lands it post-spin
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

void ModeManager::issue(const Frame& f, int64_t utc_ms, int64_t land_at_ms) {
    // Record what the columns will be showing BEFORE handing it over: the next
    // render searches forward from here to pick column 5's slot.
    last_frame_ = f;
    sched_.show(f, utc_ms, land_at_ms);
}

// ---------------------------------------------------------------------------
// Commands (spec 10.2a)
// ---------------------------------------------------------------------------
ModeManager::Result ModeManager::cmd_mode_set(Mode m, int64_t utc_ms) {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    if (m == Mode::Message) {
        // Message mode without a live message would show nothing (or bounce
        // straight back on an expired dwell).
        if (!msg_live_ || (!msg_hold_ && utc_ms >= msg_until_ms_)) {
            return {false, "no message set"};
        }
        enter_mode(Mode::Message, utc_ms);
        issue(msg_frame_, utc_ms);
        return {true, nullptr};
    }
    enter_mode(m, utc_ms);
    return {true, nullptr};
}

ModeManager::Result ModeManager::cmd_message_set(
    const std::array<std::string, N_COLUMNS>& tokens, int dwell_s, bool hold, int64_t utc_ms) {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    Frame f;
    for (int i = 0; i < N_COLUMNS; ++i) {
        const int idx = ring_.col(i).index_for_token(tokens[static_cast<size_t>(i)],
                                                     last_frame_.idx[static_cast<size_t>(i)]);
        if (idx < 0) return {false, "unknown token"};
        f.idx[static_cast<size_t>(i)] = idx;
    }
    msg_frame_ = f;
    msg_tokens_ = tokens;
    msg_live_ = true;
    msg_hold_ = hold;
    msg_until_ms_ = utc_ms + static_cast<int64_t>(dwell_s > 0 ? dwell_s : cfg_.msg_dwell_s) * 1000;
    enter_mode(Mode::Message, utc_ms);
    issue(msg_frame_, utc_ms);
    return {true, nullptr};
}

// One body, so the ritual and an automation start cannot drift apart, and so
// exactly ONE journal entry is written for either - phase 7 prints these, and
// "start" immediately after "execute" for the same act would be a lie about
// what happened.
ModeManager::Result ModeManager::start_locked(int64_t utc_ms, Origin by,
                                              journal::Event::Kind kind, const char* detail) {
    // "now + 6480" is meaningless before the first sync; a start issued on
    // the 1970 clock would detonate the alarm the moment SNTP steps time.
    if (!time_.valid()) return {false, "time not synced"};
    countdown_arm(utc_ms / 1000 + COUNTDOWN_S, utc_ms, by);
    enter_mode(Mode::Countdown, utc_ms);  // countdown overrides whatever runs
    note_locked(kind, detail, cd_.seq, by);
    return {true, nullptr};
}

ModeManager::Result ModeManager::cmd_countdown_execute(std::string_view numbers,
                                                       int64_t utc_ms, Origin by) {
    if (!numbers_valid(numbers)) return {false, "rejected"};  // wrong Numbers
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    // The Numbers as entered: "accepted - 4 8 15 16 23 42 - by ui" is the line
    // the Pearl printout exists to carry.
    return start_locked(utc_ms, by, journal::Event::Kind::CountdownExecute,
                        std::string(numbers).c_str());
}

ModeManager::Result ModeManager::cmd_countdown_start(int64_t utc_ms, Origin by) {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    return start_locked(utc_ms, by, journal::Event::Kind::CountdownStart, nullptr);
}

ModeManager::Result ModeManager::cmd_countdown_cancel(int64_t utc_ms, Origin by) {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    cd_.phase = CdPhase::Idle;
    cd_shown_ = SHOWN_NONE;
    // A cancel is a decision like any other, and a peer needs to know whose it
    // was: seq bumps so an older retained set cannot look newer.
    cd_.set_by = by;
    ++cd_.seq;
    persist();
    note_locked(journal::Event::Kind::CountdownCancel, nullptr, cd_.seq, by);
    if (mode_ == Mode::Countdown) tick_locked(utc_ms);
    return {true, nullptr};
}

ModeManager::Result ModeManager::cmd_countdown_set_target(int64_t target_utc, int64_t utc_ms,
                                                          Origin by) {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    if (target_utc <= 0) return {false, "bad epoch"};
    if (!time_.valid()) return {false, "time not synced"};
    // A millisecond epoch is the obvious integration mistake, and it used to
    // park the display on 000:00 while reporting `running`: rem_ms/1000
    // truncates to int and wraps negative.  Anything beyond a day is not a
    // countdown for this device.
    const int64_t now_s = utc_ms / 1000;
    if (target_utc <= now_s || target_utc > now_s + MAX_TARGET_AHEAD_S) {
        return {false, "epoch out of range"};
    }
    countdown_arm(target_utc, utc_ms, by);
    enter_mode(Mode::Countdown, utc_ms);
    return {true, nullptr};
}

ModeManager::Result ModeManager::cmd_preset(std::string_view name, int64_t utc_ms) {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    Frame f;
    if (name == "qmarks") f = render_qmarks(ring_, last_frame_);
    else if (name == "blank") f = render_blank(ring_, last_frame_);
    else if (name == "wifi") f = render_wifi(ring_, last_frame_);
    else if (name == "reveal") f = reveal_frame();
    else return {false, "unknown preset"};

    // A preset behaves like a held message: it stays until the mode changes.
    msg_frame_ = f;
    msg_live_ = true;
    msg_hold_ = true;
    enter_mode(Mode::Message, utc_ms);
    issue(msg_frame_, utc_ms);
    return {true, nullptr};
}

ModeManager::Result ModeManager::cmd_display_frame(const Frame& f, int64_t utc_ms) {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    // Raw frame for props and tests - deliberately does NOT change the mode;
    // the active mode may overwrite it at its next render (spec 10.2a).
    for (int i = 0; i < N_COLUMNS; ++i) {
        if (f.idx[static_cast<size_t>(i)] < 0 ||
            f.idx[static_cast<size_t>(i)] >= ring_.col(i).slot_count()) {
            return {false, "index out of range"};
        }
    }
    issue(f, utc_ms);
    // It may have displaced a scheduled land-on-tick boundary; let the
    // countdown re-derive it rather than silently skip that window.
    cd_scheduled_land_ = 0;
    if (cd_.phase == CdPhase::Running) cd_shown_ = SHOWN_NONE;
    return {true, nullptr};
}

ModeManager::Result ModeManager::cmd_cal_ramp(int col, int from, int to, int step,
                                              int dwell_s, int64_t utc_ms) {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    if (col < 0 || col >= N_COLUMNS) return {false, "bad column"};
    // An excluded column is dropped by the frame scheduler, so a walk on one
    // would report itself active and move nothing.  Refuse instead of
    // performing a walk nobody can see (spec 5.9: disabled is set by the user,
    // so this is a mistake worth naming, not a state to work around).
    if (((sched_.excluded() >> col) & 1) != 0) {
        return {false, "that column is disabled - nothing would move"};
    }
    const int n = ring_.col(col).slot_count();
    if (from < 0 || from >= n || to < 0 || to >= n) return {false, "index out of range"};
    if (step < 1 || step > n) return {false, "bad step"};
    if (dwell_s < 0 || dwell_s > 600) return {false, "bad dwell"};

    ramp_.active = true;
    ramp_.col = col;
    ramp_.to = to;
    ramp_.step = step;
    ramp_.dwell_ms = static_cast<int64_t>(dwell_s) * 1000;
    ramp_.next_index = from;
    ramp_.due_ms = utc_ms;      // first stop immediately
    ramp_.last_stop = (from == to);
    tick_locked(utc_ms);
    return {true, nullptr};
}

ModeManager::Result ModeManager::cmd_cal_ramp_stop(int64_t utc_ms) {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    if (ramp_.active) {
        ramp_.active = false;
        rendered_key_ = RENDER_NONE;
        cd_shown_ = SHOWN_NONE;
        tick_locked(utc_ms);
    }
    return {true, nullptr};
}

bool ModeManager::cal_ramp_active() const {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    return ramp_.active;
}

int ModeManager::cal_ramp_column() const {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    return ramp_.active ? ramp_.col : -1;
}

ModeManager::Result ModeManager::cmd_ring_swap(const std::function<bool()>& swap,
                                               int64_t utc_ms) {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    if (!swap || !swap()) return {true, nullptr};  // nothing was staged

    // The drums have not moved, but every index they are parked on was chosen
    // from a table that no longer exists.  Re-render from scratch.
    rendered_key_ = RENDER_NONE;
    cd_shown_ = SHOWN_NONE;
    cd_scheduled_land_ = 0;
    tick_locked(utc_ms);
    return {true, nullptr};
}

std::string ModeManager::tz_string() const {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    return tz_str_;
}

void ModeManager::set_ntp(std::string_view server) {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    ntp_ = std::string(server);
}

void ModeManager::set_journal(JournalFn fn) {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    journal_ = std::move(fn);
}

// Held-lock helper.  Everything it needs is already in hand, and the sink is
// contractually non-blocking, so this costs a queue send.
void ModeManager::note_locked(journal::Event::Kind k, const char* detail, uint32_t seq,
                              Origin by, int column) {
    if (!journal_) return;
    journal::Event e{};
    e.kind = k;
    // 0 when the clock has not synced - the journal renders those as uptime,
    // because "we do not know when" is a fact worth keeping rather than a
    // reason to invent a timestamp.
    e.utc_s = time_.valid() ? time_.now_utc() : 0;
    e.seq = seq;
    e.column = static_cast<int8_t>(column);
    // Omit rather than write "unknown": these lines are printed, and
    // "MODE CLOCK BY UNKNOWN" is noise where "MODE CLOCK" is a fact.
    if (by != Origin::Unknown) std::snprintf(e.who, sizeof e.who, "%s", origin_name(by));
    if (detail != nullptr) std::snprintf(e.detail, sizeof e.detail, "%s", detail);
    journal_(e);
}

std::string ModeManager::ntp() const {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    return ntp_;
}

ModeManager::Result ModeManager::cmd_clock_format(bool h24, int64_t utc_ms) {
    const std::lock_guard<std::mutex> lock(mu_);
    const Enter witness(*this);
    cfg_.h24 = h24;
    rendered_key_ = RENDER_NONE;  // re-render immediately in the new format
    if (mode_ == Mode::Clock) tick_locked(utc_ms);
    return {true, nullptr};
}

}  // namespace swan
