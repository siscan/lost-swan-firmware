#include "webapi/api.h"

#include "webapi/mqtt_bridge.h"

#include <array>
#include <vector>

#include "modes/wear.h"
#include "motion/axis_control.h"   // REHOME_RETRIES, published so the UI stops guessing
#include "ring/json_lite.h"
#include "ring/json_write.h"

namespace swan {
namespace api {
namespace {

using json::Writer;

std::string ok_result() { return R"({"ok":true})"; }

std::string err_result(std::string_view why) {
    Writer w;
    w.obj().kv("ok", false).kv("err", why).end_obj();
    return w.take();
}

// Accepted, and something is different - but not the thing the caller was
// probably after.  `ok` stays true because the command DID take effect; `note`
// is what stops that being the whole story.
std::string note_result(std::string_view note) {
    Writer w;
    w.obj().kv("ok", true).kv("note", note).end_obj();
    return w.take();
}

std::string result_of(ModeManager::Result r) {
    return r.ok ? ok_result() : err_result(r.err ? r.err : "rejected");
}

// The character a column is currently showing, by name - the UI renders names,
// never slot numbers, because a slot means different things per column.
std::string face_of(const RingSet& ring, int col, int index) {
    if (index < 0 || index >= ring.col(col).slot_count()) return "?";
    return ring.col(col).slot(index).id;
}

// countdown.reveal is stored as indices but is ALWAYS set and reported by
// name: an index means a different character on column 5 (spec 11).
void write_reveal(Writer& w, const RingSet& ring, const ModesConfig& cfg) {
    w.key("reveal").arr();
    for (int i = 0; i < N_COLUMNS; ++i) {
        const int idx = cfg.reveal[static_cast<size_t>(i)];
        if (idx < 0 || idx >= ring.col(i).slot_count()) {
            w.null();
        } else {
            w.str(ring.col(i).slot(idx).id);
        }
    }
    w.end_arr();
}

}  // namespace

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
std::string build_state(Context& ctx, int64_t utc_ms) {
    // Pinned for the whole document: a ring upload applied mid-build would
    // otherwise free the tables these lookups are reading.
    const RingSet ring = ctx.ring.snapshot();
    const ModesConfig cfg = ctx.modes.config();
    const MotionParams mp = ctx.motion.params();
    const SysInfo sys = ctx.sys.get();

    Writer w;
    w.obj();
    // Every /ws message carries the same discriminator, so the display widget
    // the simulator page already uses ("go"/"spin"/"mode"/"cue") and the full
    // state document travel the same socket and the same switch.
    w.kv("e", "state");
    w.kv("t", utc_ms);
    w.kv("mode", mode_name(ctx.modes.mode()));

    // Countdown: a deadline, so the UI derives remaining itself too, but we
    // send it so a page that just loaded is right immediately.
    const int64_t target = ctx.modes.cd_target();
    w.key("cd").obj()
        .kv("phase", cd_phase_name(ctx.modes.cd_phase()))
        .kv("target", target)
        .kv("remaining_s", target > 0 ? (target - utc_ms / 1000) : 0)
        .kv("seconds_mode", seconds_mode_name(cfg.seconds_mode))
        .kv("seconds_live_s", cfg.seconds_live_s)
        .kv("live", target > 0 && (target - utc_ms / 1000) <= cfg.seconds_live_s)
        // Who set this deadline, and the tiebreak.  Spec 7.3 has no master:
        // the display and the terminal prop are peers, so a peer has to be
        // able to tell its own decision from somebody else's.
        .kv("set_by", origin_name(ctx.modes.cd_set_by()))
        .kv("seq", static_cast<int64_t>(ctx.modes.cd_seq()))
        .end_obj();

    // Device-wide honesty flags.  A simulated display must be impossible to
    // mistake for a real one from any surface (spec 5.9).
    const ColumnConfig cols = ctx.motion.columns();
    w.key("motion").obj()
        .kv("simulated", cols.any(ColumnMode::Sim))
        .kv("sim_columns", cols.count(ColumnMode::Sim))
        .kv("disabled_columns", cols.count(ColumnMode::Disabled))
        .kv("maintenance", cols.maintenance)
        .kv("sim_available", ctx.motion.sim_available())
        .end_obj();

    // The external API's own state.  A display that thinks it is publishing
    // and is not looks identical to one nobody is listening to.
    const MqttStatus mq = ctx.mqtt.mqtt_status();
    w.key("mqtt").obj()
        .kv("enabled", mq.enabled)
        .kv("connected", mq.connected)
        .kv("uri", mq.uri)
        .kv("base", mq.base)
        // The user and the discovery prefix are settings, not secrets, and the
        // Settings form needs to show what is stored.  The PASSWORD is never
        // published - which is exactly why mqtt.config treats an absent field
        // as "keep": a form that cannot see it must still be able to save.
        .kv("user", mq.user)
        .kv("ha_prefix", mq.ha_prefix)
        .kv("dropped", static_cast<int64_t>(mq.dropped))
        .end_obj();

    w.key("prov").obj()
        .kv("portal", ctx.wifi.portal_running())
        .kv("ssid", ctx.wifi.portal_ssid())
        .kv("configured", ctx.wifi.have_credentials())
        .end_obj();

    const AudioState au = ctx.audio.audio_state();
    w.key("audio").obj()
        .kv("volume", au.volume)
        .kv("mute", au.mute)
        .kv("quiet_start_min", au.quiet_start_min)
        .kv("quiet_end_min", au.quiet_end_min)
        .kv("playing", au.playing)
        .kv("cue", au.cue)
        // How many cue files are actually present: a missing WAV is a cue that
        // will silently not fire, and "the alarm did nothing" is a bad thing to
        // discover at zero.
        .kv("cues_present", au.cues_present)
        .kv("cues_total", au.cues_total);
    w.key("cues").arr();
    for (const AudioState::Cue& c : au.cues) {
        w.obj().kv("name", c.name).kv("present", c.present)
            .kv("ms", static_cast<int64_t>(c.ms)).end_obj();
    }
    w.end_arr();
    w.end_obj();

    // The live message as a space-separated token string: the HA text entity
    // reads it back, and an entity with no readable state logs a rejection on
    // every push.
    w.kv("msg", ctx.modes.message_tokens());

    w.kv("time_valid", ctx.modes.time_valid());
    w.kv("wifi_glyph", ctx.modes.wifi_glyph_shown());

    w.key("cols").arr();
    for (int i = 0; i < N_COLUMNS; ++i) {
        const AxisInfo a = ctx.motion.info(i);
        w.obj()
            .kv("state", axis_state_name(a.state))
            .kv("index", a.index)
            .kv("face", face_of(ring, i, a.index))
            .kv("dest", a.dest_index)
            .kv("cal", a.cal_offset)
            .kv("revs", static_cast<int64_t>(a.revs))
            .kv("minor", static_cast<int64_t>(a.resync_minor))
            .kv("major", static_cast<int64_t>(a.resync_major))
            .kv("faults", static_cast<int64_t>(a.faults))
            .kv("h2h", a.hall_to_hall)
            .kv("err", a.last_hall_err)
            .kv("flips", static_cast<int64_t>(a.flips_total))
            .kv("hall", a.hall_level)
            // "settled" is the question the UI actually needs answered: an
            // index of -1 means the position is UNKNOWN, which is not the same
            // as showing the blank flap, and a column hunting for its hall
            // edge must not look like an idle one.
            .kv("settled", a.state == AxisState::Idle && a.index >= 0)
            .kv("retry", static_cast<int>(a.rehome_attempt))
            // Why it faulted, because "sensor or wiring" and "it is jammed"
            // are different call-outs, and what the column IS, because a
            // disabled hole is expected and a simulated column is not real.
            .kv("cause", fault_cause_name(a.fault_cause))
            .kv("mode", column_mode_name(a.mode))
            .end_obj();
    }
    w.end_arr();

    w.key("cfg").obj()
        .kv("h24", cfg.h24)
        .kv("granularity_min", cfg.granularity_min)
        .kv("seconds_mode", seconds_mode_name(cfg.seconds_mode))
        .kv("seconds_live_s", cfg.seconds_live_s)
        .kv("msg_dwell_s", cfg.msg_dwell_s)
        .kv("zero_hold_s", cfg.zero_hold_s)
        .kv("spin_s", cfg.spin_s)
        .kv("failure_timeout_s", cfg.failure_timeout_s)
        .kv("clock_land_on_tick", cfg.clock_land_on_tick)
        .kv("cd_land_on_tick", cfg.cd_land_on_tick)
        .kv("tz", ctx.modes.tz_string())
        .kv("flaps_s_normal", mp.flaps_s_normal)
        .kv("flaps_s_alarm", mp.flaps_s_alarm)
        .kv("flaps_s_home", mp.flaps_s_home)
        .kv("accel", mp.accel)
        .kv("hall_tol", mp.hall_tol)
        .kv("en_idle_off", mp.en_idle_off)
        .kv("failure_loop_s", cfg.failure_loop_s)
        .kv("ntp", ctx.modes.ntp());
    write_reveal(w, ring, cfg);
    w.end_obj();

    w.key("ring").obj()
        .kv("source", ring.loaded_from_json() ? "ring.json" : "compiled")
        .kv("slots", ring.col(0).slot_count())
        .kv("descending", ring.col(0).is_descending() && ring.col(4).is_descending())
        .end_obj();

    w.key("cal").obj()
        .kv("ramp_active", ctx.modes.cal_ramp_active())
        .kv("ramp_col", ctx.modes.cal_ramp_column())
        .end_obj();

    w.key("sys").obj()
        .kv("wifi", sys.wifi_state)
        .kv("ssid", sys.ssid)
        .kv("ip", sys.ip)
        .kv("host", sys.hostname)
        .kv("rssi", sys.rssi)
        .kv("heap", static_cast<int64_t>(sys.heap))
        .kv("heap_largest", static_cast<int64_t>(sys.heap_largest))
        .kv("uptime_s", static_cast<int64_t>(sys.uptime_s))
        .kv("ota_partition", sys.ota_partition)
        .kv("ota_pending", sys.ota_pending_verify)
        .kv("reset", sys.reset_reason)
        .kv("version", sys.version)
        .kv("ws_dropped", static_cast<int64_t>(sys.ws_dropped))
        // The denominator both pages print beside cols[].retry.  It was a
        // hard-coded "3" in four places against a compile-time constant with no
        // wire representation - correct today, silently wrong the day it moves.
        .kv("rehome_retries", REHOME_RETRIES)
        .kv("drivers_enabled", sys.drivers_enabled)
        .kv("step_isr_alive", sys.step_isr_alive)
        .kv("step_isr_stalls", static_cast<int64_t>(sys.step_isr_stalls))
        .end_obj();

    w.end_obj();
    return w.take();
}

// ---------------------------------------------------------------------------
// Ring document for the pickers
// ---------------------------------------------------------------------------
std::string build_ring_doc(const RingSet& ring) {
    Writer w;
    w.obj();
    w.kv("source", ring.loaded_from_json() ? "ring.json" : "compiled");
    w.kv("slot_count", ring.col(0).slot_count());
    // Presentation, straight through: the mirror should look like the wall,
    // and columns 4-5 carry the inverted drums.
    w.kv_raw("schemes", ring.schemes_json().empty() ? "{}" : ring.schemes_json());
    w.key("columns").arr();
    for (int c = 0; c < N_COLUMNS; ++c) {
        const RingTable& t = ring.col(c);
        w.obj();
        w.kv("slots", t.slot_count());
        w.kv("scheme", ring.scheme(c));
        w.kv("descending", t.is_descending());
        // Every slot, so the Calibrate walk can show the expected character
        // beside each stop (spec 4).
        w.key("ring").arr();
        for (int i = 0; i < t.slot_count(); ++i) {
            w.obj().kv("i", i).kv("id", t.slot(i).id).kv("label", t.slot(i).label)
                .kv("cat", ring_category_name(t.slot(i).cat)).end_obj();
        }
        w.end_arr();
        // Just the glyphs, for the reveal picker: a column may only be offered
        // what its own ring carries, and column 5's set differs.
        w.key("glyphs").arr();
        for (int i = 0; i < t.slot_count(); ++i) {
            if (t.slot(i).cat == RingCategory::Glyph) {
                w.obj().kv("id", t.slot(i).id).kv("label", t.slot(i).label).end_obj();
            }
        }
        w.end_arr();
        w.end_obj();
    }
    w.end_arr();
    w.end_obj();
    return w.take();
}

// ---------------------------------------------------------------------------
// Wear
// ---------------------------------------------------------------------------
namespace {

void write_wear(Writer& w, const WearEstimate& e) {
    w.obj().kv("total", static_cast<int64_t>(e.total)).kv("renders",
                                                          static_cast<int64_t>(e.renders));
    w.key("cols").arr();
    for (int i = 0; i < N_COLUMNS; ++i) w.num(static_cast<int64_t>(e.flips[static_cast<size_t>(i)]));
    w.end_arr().end_obj();
}

}  // namespace

std::string build_wear_doc(const RingSet& ring, bool h24, int seconds_live_s) {
    Writer w;
    w.obj();
    w.kv("h24", h24);
    w.kv("seconds_live_s", seconds_live_s);

    // Walking the floored values rather than all 1440 minutes keeps this cheap
    // enough to compute on the device: ~4,400 renders for the whole table.
    w.key("clock").arr();
    for (const int g : granularity_choices()) {
        w.obj();
        w.kv("granularity_min", g);
        w.key("wear");
        write_wear(w, clock_wear_per_day(ring, h24, g));
        w.end_obj();
    }
    w.end_arr();

    w.key("countdown").arr();
    const SecondsMode modes[] = {SecondsMode::Minutes, SecondsMode::Tens, SecondsMode::Seconds};
    for (const SecondsMode m : modes) {
        w.obj();
        w.kv("mode", seconds_mode_name(m));
        w.key("wear");
        write_wear(w, countdown_wear_per_run(ring, m, seconds_live_s));
        w.end_obj();
    }
    w.end_arr();

    w.end_obj();
    return w.take();
}

// ---------------------------------------------------------------------------
// Commands - the one dispatcher (spec 10.2a)
// ---------------------------------------------------------------------------
namespace {

const json::Value* member(const json::Value& v, const char* k) { return v.get(k); }

bool as_int_field(const json::Value& payload, const char* k, int& out) {
    const json::Value* v = member(payload, k);
    if (v == nullptr || v->type != json::Type::Int) return false;
    out = static_cast<int>(v->number);
    return true;
}

std::string do_message_set(Context& ctx, const json::Value& p, int64_t utc_ms) {
    const json::Value* toks = member(p, "tokens");
    if (toks == nullptr || toks->as_array() == nullptr ||
        toks->as_array()->size() != static_cast<size_t>(N_COLUMNS)) {
        return err_result("tokens must be 5 entries");
    }
    std::array<std::string, N_COLUMNS> t;
    for (int i = 0; i < N_COLUMNS; ++i) {
        t[static_cast<size_t>(i)] = std::string((*toks->as_array())[static_cast<size_t>(i)].as_str());
    }
    int dwell = 0;
    as_int_field(p, "dwell_s", dwell);
    const json::Value* hold = member(p, "hold");
    return result_of(ctx.modes.cmd_message_set(t, dwell, hold && hold->boolean, utc_ms));
}

std::string do_display_frame(Context& ctx, const RingSet& ring, const json::Value& p,
                             int64_t utc_ms) {
    Frame f;
    if (const json::Value* idx = member(p, "indices")) {
        const auto* a = idx->as_array();
        if (a == nullptr || a->size() != static_cast<size_t>(N_COLUMNS)) {
            return err_result("indices must be 5 entries");
        }
        for (int i = 0; i < N_COLUMNS; ++i) {
            f.idx[static_cast<size_t>(i)] = static_cast<int>((*a)[static_cast<size_t>(i)].as_int(-1));
        }
    } else if (const json::Value* tk = member(p, "tokens")) {
        const auto* a = tk->as_array();
        if (a == nullptr || a->size() != static_cast<size_t>(N_COLUMNS)) {
            return err_result("tokens must be 5 entries");
        }
        for (int i = 0; i < N_COLUMNS; ++i) {
            const int got = ring.col(i).index_for_token((*a)[static_cast<size_t>(i)].as_str());
            if (got < 0) return err_result("unknown token");
            f.idx[static_cast<size_t>(i)] = got;
        }
    } else {
        return err_result("need indices or tokens");
    }
    return result_of(ctx.modes.cmd_display_frame(f, utc_ms));
}

// Live motion parameters (the Calibrate sliders).  Applied immediately;
// persistence is a separate command so the page can offer both.
std::string do_motion_params(Context& ctx, const json::Value& p) {
    MotionParams mp = ctx.motion.params();
    int v = 0;
    if (as_int_field(p, "flaps_s_normal", v)) {
        if (v < 1 || v > 40) return err_result("flaps_s_normal out of range");
        mp.flaps_s_normal = v;
    }
    if (as_int_field(p, "flaps_s_alarm", v)) {
        if (v < 1 || v > 40) return err_result("flaps_s_alarm out of range");
        mp.flaps_s_alarm = v;
    }
    if (as_int_field(p, "flaps_s_home", v)) {
        if (v < 1 || v > 40) return err_result("flaps_s_home out of range");
        mp.flaps_s_home = v;
    }
    if (as_int_field(p, "accel", v)) {
        if (v < 1000 || v > 1000000) return err_result("accel out of range");
        mp.accel = v;
    }
    if (as_int_field(p, "hall_tol", v)) {
        if (v < 1 || v > 400) return err_result("hall_tol out of range");
        mp.hall_tol = v;
    }
    // Settable from nowhere at all until now - not the API, not the console -
    // while `save` persisted it faithfully.  Spec 5.7 leaves it false until the
    // bench test says a loaded drum does not creep with EN released.
    if (const json::Value* en = member(p, "en_idle_off")) {
        if (en->type != json::Type::Bool) return err_result("en_idle_off must be true or false");
        mp.en_idle_off = en->boolean;
    }
    ctx.motion.set_params(mp);
    // Keep the modes layer's copy in step.  alarm_flaps_s was seeded from
    // MotionParams once at boot and never re-synced, so raising the alarm speed
    // with the Settings slider left the zero choreography spinning at the old
    // value - and, worse, left g_fast_spin false on every column, so "a fault
    // during the alarm spin drops EN" could not fire at all.
    ModesConfig mc = ctx.modes.config();
    if (mc.alarm_flaps_s != mp.flaps_s_alarm) {
        mc.alarm_flaps_s = mp.flaps_s_alarm;
        ctx.modes.set_config(mc);
    }
    return ok_result();
}

std::string do_config_set(Context& ctx, const RingSet& ring, const json::Value& p,
                          int64_t utc_ms) {
    ModesConfig cfg = ctx.modes.config();
    int v = 0;

    if (const json::Value* h = member(p, "h24")) {
        ctx.modes.cmd_clock_format(h->boolean, utc_ms);
        cfg = ctx.modes.config();
    }
    if (as_int_field(p, "granularity_min", v)) {
        // Must divide 60.  At 7 the flooring steps 56 -> 0 across the hour, so
        // the last window of every hour is short and the display jumps.
        if (!granularity_valid(v)) {
            return err_result("granularity_min must divide 60 (1 2 3 4 5 6 10 12 15 20 30 60)");
        }
        cfg.granularity_min = v;
    }
    if (as_int_field(p, "seconds_live_s", v)) {
        if (v < 0 || v > ModeManager::COUNTDOWN_S) {
            return err_result("seconds_live_s must be 0..6480");
        }
        // Whole minutes only, for the same reason granularity_min must divide
        // 60: a boundary that does not land on a minute makes the displayed
        // value jump - upward, here, which on a one-way ring is a near-full
        // wrap in the wrong direction.
        if (v % 60 != 0) return err_result("seconds_live_s must be a whole number of minutes");
        cfg.seconds_live_s = v;
    }
    if (as_int_field(p, "msg_dwell_s", v)) {
        if (v < 1 || v > 86400) return err_result("msg_dwell_s out of range");
        cfg.msg_dwell_s = v;
    }
    if (as_int_field(p, "zero_hold_s", v)) {
        if (v < 0 || v > 60) return err_result("zero_hold_s out of range");
        cfg.zero_hold_s = v;
    }
    if (as_int_field(p, "spin_s", v)) {
        if (v < 0 || v > 120) return err_result("spin_s out of range");
        cfg.spin_s = v;
    }
    if (as_int_field(p, "failure_loop_s", v)) {
        if (v < 0 || v > 86400) return err_result("failure_loop_s out of range");
        cfg.failure_loop_s = v;
    }
    if (as_int_field(p, "failure_timeout_s", v)) {
        if (v < 0 || v > 86400) return err_result("failure_timeout_s out of range");
        cfg.failure_timeout_s = v;
    }
    if (const json::Value* m = member(p, "seconds_mode")) {
        if (!seconds_mode_from_name(m->as_str(), cfg.seconds_mode)) {
            return err_result("seconds_mode must be minutes|tens|seconds");
        }
    }
    if (const json::Value* b = member(p, "clock_land_on_tick")) cfg.clock_land_on_tick = b->boolean;
    if (const json::Value* b = member(p, "cd_land_on_tick")) cfg.cd_land_on_tick = b->boolean;

    // Reveal is set BY NAME, never by index: the same index is a different
    // character on column 5 (spec 11).  A name must exist in that column's own
    // ring, so the picker cannot offer something the drum cannot show.
    if (const json::Value* rv = member(p, "reveal")) {
        const auto* a = rv->as_array();
        if (a == nullptr || a->size() != static_cast<size_t>(N_COLUMNS)) {
            return err_result("reveal must be 5 entries");
        }
        std::array<int, N_COLUMNS> next{};
        for (int i = 0; i < N_COLUMNS; ++i) {
            const json::Value& e = (*a)[static_cast<size_t>(i)];
            if (e.is_null()) {  // explicit "no glyph on this column" -> blank
                next[static_cast<size_t>(i)] = -1;
                continue;
            }
            // A number here would be an index, and an index means a different
            // character on column 5.  Refuse it rather than resolve it.
            if (e.type != json::Type::Str) return err_result("reveal entries must be names");
            if (e.str.empty()) {
                next[static_cast<size_t>(i)] = -1;
                continue;
            }
            const int got = ring.col(i).index_for_token(e.as_str());
            if (got < 0) {
                return err_result("reveal glyph not on that column's ring");
            }
            next[static_cast<size_t>(i)] = got;
        }
        cfg.reveal = next;
    }

    if (const json::Value* ntp = member(p, "ntp")) {
        // Same story: named in spec 11, implemented in NVS, reachable from no
        // surface, so `pool.ntp.org` was effectively hard-coded.
        if (ntp->type != json::Type::Str) return err_result("ntp must be a string");
        if (ntp->as_str().size() > 127) return err_result("that value is too long (127 bytes max)");
        if (ntp->as_str().empty()) return err_result("need an NTP server");
        ctx.modes.set_ntp(ntp->as_str());
    }

    if (const json::Value* tz = member(p, "tz")) {
        // ModeManager owns the string and hands it back under its lock; a
        // mirror in Context would be written here on the HTTP task and read at
        // 20 Hz on the modes task with nothing between them.
        if (!ctx.modes.set_tz(tz->as_str())) return err_result("bad POSIX TZ string");
    }

    ctx.modes.set_config(cfg);
    return ok_result();
}

// Did a reply say the command succeeded?  Used by the maintenance gate, which
// runs a command and then decorates its OWN reply with a note - it must not
// turn a rejection into a success.
bool is_ok_doc(const std::string& res) {
    json::Value v;
    return json::parse(res, v, nullptr) && v.get("ok") != nullptr && v.get("ok")->boolean;
}

// Everything below the OTA-hold and maintenance gates.  Split out so those
// gates cannot be bypassed by a command handler that happens to be declared
// earlier, and so the gate can run a command and then qualify the answer.
std::string dispatch_after_gates(Context& ctx, const RingSet& ring, std::string_view c,
                                 const json::Value& p, int64_t utc_ms, Origin by);

}  // namespace

std::string handle_command(Context& ctx, std::string_view body, int64_t utc_ms, Origin by) {
    // Serialised across transports; see the contract on Context::dispatch_mu.
    const std::lock_guard<std::mutex> dispatch_lock(ctx.dispatch_mu);
    // Pinned for the whole dispatch, same reason as build_state.
    const RingSet ring = ctx.ring.snapshot();
    json::Value doc;
    std::string perr;
    if (!json::parse(body, doc, &perr)) return err_result("bad json: " + perr);

    const json::Value* cmd = doc.get("cmd");
    if (cmd == nullptr || cmd->type != json::Type::Str) return err_result("missing cmd");
    const std::string_view c = cmd->as_str();

    // A bare value is accepted where the payload is a single value, exactly as
    // MQTT does (spec 10.2a).
    static const json::Value kEmpty{};
    const json::Value& p = doc.get("payload") ? *doc.get("payload") : kEmpty;

    if (ctx.modes.ota_hold()) {
        // Everything that can START a move.  motion.maintenance is on the list
        // because LEAVING maintenance re-homes all five columns - the single
        // most motion any one command can cause, and it was the one omitted.
        // The rest were listed but unreachable, because this check sat below
        // their handlers; it is the first thing after the parse now.
        for (const char* blocked : {"motion.rehome", "motion.spin", "motion.cal",
                                    "motion.ramp", "motion.maintenance", "motion.column",
                                    "display.frame", "preset.set", "mode.set",
                                    "message.set", "countdown.execute", "countdown.start",
                                    "countdown.reset", "countdown.set_target"}) {
            if (c == blocked) {
                return err_result("an update is being written; motion is held");
            }
        }
    }

    // Maintenance is where "ok" lied hardest, across a dozen commands at once.
    // tick_locked returns before rendering, so mode.set / clock.format / a ring
    // swap returned ok and no column was ever commanded - and message.set,
    // preset.set and display.frame are WORSE, because they call issue()
    // directly, outside that gate: they drove the drums while somebody had
    // their hands in the mechanism, or pulsed STEP into de-energised drivers
    // after a boot in maintenance, leaving the axis believing a position it
    // never reached.
    //
    // Split by what spec 5.9 actually promises.  Manual driving works - that is
    // the point of the mode, and it is how a suspect column is exercised from
    // the Calibrate page.  Display-driving commands are refused with a reason.
    // The deadline commands are the third case: the deadline is absolute and
    // genuinely arms (spec 5.9 says a repair does not cancel a countdown), so
    // they succeed with a note rather than an error.
    if (ctx.modes.maintenance()) {
        for (const char* blocked : {"mode.set", "message.set", "preset.set", "display.frame",
                                    "clock.format"}) {
            if (c == blocked) {
                return err_result("maintenance is on - nothing is driven; leave maintenance first");
            }
        }
        for (const char* deferred : {"countdown.execute", "countdown.start", "countdown.reset",
                                     "countdown.set_target"}) {
            if (c == deferred) {
                const std::string res = dispatch_after_gates(ctx, ring, c, p, utc_ms, by);
                if (!is_ok_doc(res)) return res;
                return note_result("the deadline is running, but maintenance is on so nothing "
                                   "is being displayed or driven");
            }
        }
    }

    return dispatch_after_gates(ctx, ring, c, p, utc_ms, by);
}

namespace {

std::string dispatch_after_gates(Context& ctx, const RingSet& ring, std::string_view c,
                                 const json::Value& p, int64_t utc_ms, Origin by) {
    // ---- modes ----
    if (c == "mode.set") {
        const std::string_view m = p.as_str();
        if (m == "clock") return result_of(ctx.modes.cmd_mode_set(Mode::Clock, utc_ms));
        if (m == "message") return result_of(ctx.modes.cmd_mode_set(Mode::Message, utc_ms));
        if (m == "countdown") return result_of(ctx.modes.cmd_mode_set(Mode::Countdown, utc_ms));
        return err_result("unknown mode");
    }
    if (c == "message.set") return do_message_set(ctx, p, utc_ms);
    if (c == "preset.set") {
        const std::string_view n = p.type == json::Type::Str ? p.as_str()
                                                             : (p.get("name") ? p.get("name")->as_str()
                                                                              : std::string_view{});
        return result_of(ctx.modes.cmd_preset(n, utc_ms));
    }
    if (c == "display.frame") return do_display_frame(ctx, ring, p, utc_ms);

    // ---- countdown ----
    if (c == "countdown.execute") {
        const std::string_view n = p.type == json::Type::Str
                                       ? p.as_str()
                                       : (p.get("numbers") ? p.get("numbers")->as_str()
                                                           : std::string_view{});
        return result_of(ctx.modes.cmd_countdown_execute(n, utc_ms, by));
    }
    if (c == "countdown.start") return result_of(ctx.modes.cmd_countdown_start(utc_ms, by));
    if (c == "countdown.reset") return result_of(ctx.modes.cmd_countdown_reset(utc_ms, by));
    if (c == "countdown.cancel") return result_of(ctx.modes.cmd_countdown_cancel(utc_ms, by));
    if (c == "countdown.set_target") {
        const int64_t epoch = p.type == json::Type::Int
                                  ? p.number
                                  : (p.get("epoch") ? p.get("epoch")->as_int(0) : 0);
        return result_of(ctx.modes.cmd_countdown_set_target(epoch, utc_ms, by));
    }

    // ---- clock ----
    if (c == "clock.format") {
        const bool h24 = p.type == json::Type::Bool
                             ? p.boolean
                             : (p.get("h24") ? p.get("h24")->boolean : false);
        return result_of(ctx.modes.cmd_clock_format(h24, utc_ms));
    }

    // ---- motion / calibration ----
    // Motion is held for the duration of an OTA write (spec 10.4): the
    // flash-resident control task stalls on every sector erase, so a move
    // started now would step with nobody able to decelerate it or notice an
    // overdue Hall edge.  Refused HERE so every transport gets the same answer.
    if (c == "motion.enable") {
        // EN is GANGED: five drivers, one GPIO (spec 2.2).  There is no
        // per-column form of this and there will not be one.
        const bool on = p.type == json::Type::Bool
                            ? p.boolean
                            : (p.get("on") != nullptr && p.get("on")->boolean);
        if (!ctx.motion.set_enabled(on)) return err_result("could not change EN");
        return on ? ok_result()
                  : note_result("EN released for all five drivers - nothing can move until "
                                "it is re-enabled");
    }
    if (c == "motion.rehome") {
        int col = -1;
        if (p.type == json::Type::Int) col = static_cast<int>(p.number);
        else as_int_field(p, "column", col);
        if (ctx.motion.home(col)) return ok_result();
        // Three different refusals used to share one word.  The only ones
        // reachable are a bad index and a disabled column - and for a re-home-
        // all, EVERY column being disabled, which posted nothing and said ok.
        if (col >= 0 && col < N_COLUMNS &&
            ctx.motion.columns().mode[static_cast<size_t>(col)] == ColumnMode::Disabled) {
            return err_result("that column is disabled - it is never homed");
        }
        if (col < 0) return err_result("every column is disabled - nothing to home");
        return err_result("bad column");
    }
    if (c == "motion.cal") {
        int col = 0, delta = 0;
        if (!as_int_field(p, "column", col)) return err_result("need column");
        if (as_int_field(p, "delta", delta)) {
            switch (ctx.motion.adjust_cal(col, delta)) {
                case MotionAdmin::CalOutcome::Moved:
                    return ok_result();
                case MotionAdmin::CalOutcome::NotHomed:
                    // Applied, but nothing moved - and a nudge you cannot see
                    // is the whole failure this reply exists to prevent.
                    return note_result("offset applied, but the column is not homed "
                                       "so nothing moved - re-home it");
                case MotionAdmin::CalOutcome::BadColumn:
                    break;
            }
            return err_result("bad column");
        }
        const json::Value* save = member(p, "save");
        if (save != nullptr && save->boolean) {
            return ctx.cfg.save_motion(ctx.motion.params()) ? ok_result()
                                                            : err_result("save failed");
        }
        return err_result("need delta or save");
    }
    if (c == "motion.params") return do_motion_params(ctx, p);
    if (c == "motion.save") {
        return ctx.cfg.save_motion(ctx.motion.params()) ? ok_result() : err_result("save failed");
    }
    if (c == "motion.ramp") {
        int col = 0, from = 0, to = 0, step = 1, dwell = 1;
        if (!as_int_field(p, "column", col)) return err_result("need column");
        if (!as_int_field(p, "from", from) || !as_int_field(p, "to", to)) {
            return err_result("need from and to");
        }
        as_int_field(p, "step", step);
        as_int_field(p, "dwell_s", dwell);
        return result_of(ctx.modes.cmd_cal_ramp(col, from, to, step, dwell, utc_ms));
    }
    if (c == "motion.ramp_stop") return result_of(ctx.modes.cmd_cal_ramp_stop(utc_ms));
    if (c == "motion.column") {
        int col = 0;
        const json::Value* mv = member(p, "mode");
        if (mv == nullptr || mv->type != json::Type::Str) return err_result("need mode");
        ColumnMode m;
        if (!column_mode_from_name(mv->as_str(), m)) {
            return err_result("mode must be real|sim|disabled");
        }
        if (m == ColumnMode::Sim && !ctx.motion.sim_available()) {
            return err_result("this image was built without simulated axes");
        }
        ColumnConfig cfg = ctx.motion.columns();
        const json::Value* av = member(p, "all");
        if (av != nullptr && av->boolean) {
            for (auto& x : cfg.mode) x = m;
        } else {
            if (!as_int_field(p, "column", col)) return err_result("need column or all");
            if (col < 0 || col >= N_COLUMNS) return err_result("bad column");
            cfg.mode[static_cast<size_t>(col)] = m;
        }
        if (!ctx.motion.set_columns(cfg)) return err_result("could not apply");
        // The frame layer must skip a disabled column; renderers stay untouched.
        ctx.modes.cmd_set_excluded(cfg.excluded_mask(), utc_ms);
        return ok_result();
    }
    if (c == "motion.maintenance") {
        const bool on = p.type == json::Type::Bool
                            ? p.boolean
                            : (p.get("on") != nullptr && p.get("on")->boolean);
        ColumnConfig cfg = ctx.motion.columns();
        cfg.maintenance = on;
        if (!ctx.motion.set_columns(cfg)) return err_result("could not apply");
        const auto r = ctx.modes.cmd_maintenance(on, utc_ms);
        if (!r.ok) return err_result(r.err ? r.err : "rejected");
        // Leaving re-arms: everything re-homes, because the drums have been
        // moved by hand and nothing knows where they are.
        if (!on) ctx.motion.home(-1);
        return ok_result();
    }
    if (c == "motion.sim_fault") {
        int col = 0, value = 0;
        if (!as_int_field(p, "column", col)) return err_result("need column");
        const json::Value* kv = member(p, "kind");
        if (kv == nullptr || kv->type != json::Type::Str) return err_result("need kind");
        as_int_field(p, "value", value);
        return ctx.motion.sim_inject(col, kv->as_str(), value)
                   ? ok_result()
                   : err_result("injection rejected (is that column simulated?)");
    }
    if (c == "motion.spin") {
        int col = 0, flaps = 20, secs = 3;
        if (!as_int_field(p, "column", col)) return err_result("need column");
        as_int_field(p, "flaps_s", flaps);
        as_int_field(p, "seconds", secs);
        if (flaps < 1 || flaps > 40) return err_result("flaps_s out of range");
        if (secs < 1 || secs > 60) return err_result("seconds out of range");
        // "bad column" covered three different refusals; a disabled column is by
        // far the likeliest and the only one a user can act on.
        if (col < 0 || col >= N_COLUMNS) return err_result("bad column");
        if (ctx.motion.columns().mode[static_cast<size_t>(col)] == ColumnMode::Disabled) {
            return err_result("that column is disabled - nothing would move");
        }
        return ctx.motion.spin_open_loop(col, flaps, secs)
                   ? ok_result()
                   : err_result("that column will not accept an open-loop move right now "
                                "(homing?)");
    }

    // ---- config ----
    if (c == "config.set") return do_config_set(ctx, ring, p, utc_ms);
    if (c == "config.save") {
        return ctx.cfg.save_app(ctx.modes.config(), ctx.modes.tz_string(), ctx.modes.ntp())
                   ? ok_result()
                   : err_result("save failed");
    }

    // ---- ring upload (body carried separately by the HTTP route) ----
    if (c == "ring.upload") {
        const json::Value* b = member(p, "body");
        if (b == nullptr || b->type != json::Type::Str) return err_result("need body");
        std::string err;
        return ctx.ring_upload.stage(b->as_str(), &err) ? ok_result() : err_result(err);
    }

    if (c == "audio.volume" || c == "audio.mute" || c == "audio.quiet") {
        const AudioState a = ctx.audio.audio_state();
        int vol = a.volume, qs = a.quiet_start_min, qe = a.quiet_end_min;
        bool mute = a.mute;
        if (c == "audio.volume") {
            int v = 0;
            if (p.type == json::Type::Int) v = static_cast<int>(p.number);
            else if (!as_int_field(p, "value", v)) return err_result("need a value 0-100");
            if (v < 0 || v > 100) return err_result("volume must be 0-100");
            vol = v;
        } else if (c == "audio.mute") {
            mute = p.type == json::Type::Bool ? p.boolean
                                              : (p.get("on") != nullptr && p.get("on")->boolean);
        } else {
            // Both bounds equal means OFF, which is the [Q8] default and has to
            // stay expressible rather than becoming a 24-hour quiet period.
            if (!as_int_field(p, "start_min", qs) || !as_int_field(p, "end_min", qe)) {
                return err_result("need start_min and end_min");
            }
            if (qs < 0 || qs > 1439 || qe < 0 || qe > 1439) {
                return err_result("minutes must be 0-1439");
            }
        }
        return ctx.audio.audio_set(vol, mute, qs, qe) ? ok_result() : err_result("could not apply");
    }
    if (c == "audio.play") {
        const std::string_view name = p.type == json::Type::Str
                                          ? p.as_str()
                                          : (p.get("cue") != nullptr ? p.get("cue")->as_str()
                                                                     : std::string_view{});
        if (name.empty()) return err_result("need a cue name");
        return ctx.audio.audio_play(name) ? ok_result()
                                          : err_result("no such cue, or no file for it");
    }
    if (c == "audio.stop") {
        return ctx.audio.audio_stop() ? ok_result() : err_result("could not stop");
    }
    if (c == "wifi.credentials") {
        const json::Value* ssid = member(p, "ssid");
        if (ssid == nullptr || ssid->type != json::Type::Str || ssid->as_str().empty()) {
            return err_result("need ssid");
        }
        const json::Value* pass = member(p, "pass");
        if (ssid->as_str().size() > 32) return err_result("ssid too long (32 bytes max)");
        if (pass != nullptr && pass->as_str().size() > 63) {
            return err_result("password too long (63 bytes max)");
        }
        // And a MINIMUM, which matters more than it looks.  wifi.cpp offers any
        // non-empty password as a WPA2 PSK, and esp_wifi_set_config rejects one
        // shorter than 8 characters - but the credentials are persisted BEFORE
        // the radio is touched, so a short password used to be saved, refused,
        // and then retried identically on every boot.  Rejected here, before
        // anything is written, with a message a person can act on.
        if (pass != nullptr && !pass->as_str().empty() && pass->as_str().size() < 8) {
            return err_result("a WPA password is 8-63 characters (leave it empty for an open "
                              "network)");
        }
        const bool ok = ctx.wifi.set_credentials(
            ssid->as_str(), pass != nullptr ? pass->as_str() : std::string_view{});
        return ok ? ok_result() : err_result("could not save");
    }
    if (c == "wifi.provision") {
        // Explicit only.  Nothing infers this from a disconnect: a router
        // reboot must not drop a wall display off the LAN and into AP mode.
        const bool on = p.type == json::Type::Bool
                            ? p.boolean
                            : (p.get("on") == nullptr || p.get("on")->boolean);
        const bool ok = on ? ctx.wifi.start_portal() : ctx.wifi.stop_portal();
        return ok ? ok_result() : err_result("could not change the portal");
    }
    if (c == "mqtt.config") {
        const json::Value* en = member(p, "enabled");
        const json::Value* uri = member(p, "uri");
        if (uri != nullptr && uri->type != json::Type::Str) return err_result("uri must be a string");
        const bool want = en == nullptr || en->boolean;

        // Turning MQTT OFF must not require re-sending the broker it is
        // switching off.  It did, so an off toggle in a UI had to know the URI
        // - and `{"enabled":false}`, the obvious payload, was refused with
        // "need uri".
        const std::string stored = ctx.mqtt.mqtt_status().uri;
        const std::string_view effective_uri =
            uri != nullptr ? uri->as_str() : std::string_view(stored);
        std::string why;
        if (want) {
            if (effective_uri.empty()) return err_result("need uri");
            // Validated HERE, so a bad URI is a rejected command with a reason
            // rather than a client that fails to connect for ever with a log
            // line nobody is watching.
            if (!broker_uri_valid(effective_uri, why)) return err_result(why);
        }
        // NVS strings are bounded and this partition is 24 KB.  Refused here
        // with a reason rather than deep in the driver, and long before
        // anything can fill the partition and trigger a boot-time erase.
        for (const char* k : {"uri", "user", "pass", "base", "ha_prefix"}) {
            const json::Value* v = member(p, k);
            if (v != nullptr && v->type == json::Type::Str && v->as_str().size() > 127) {
                return err_result("that value is too long (127 bytes max)");
            }
        }
        // Absent means KEEP, all the way down: only the fields this payload
        // actually carries are changed.  A "just change the base topic" update
        // used to clear the username and password on its way past.
        MqttAdmin::MqttSettings set;
        set.enabled = want;
        const auto field = [&p](const char* k) -> std::optional<std::string_view> {
            const json::Value* v = member(p, k);
            if (v == nullptr || v->type != json::Type::Str) return std::nullopt;
            return v->as_str();
        };
        set.uri = field("uri");
        set.user = field("user");
        set.pass = field("pass");
        set.base = field("base");
        set.ha_prefix = field("ha_prefix");
        const bool ok = ctx.mqtt.mqtt_configure(set);
        return ok ? ok_result() : err_result("could not apply");
    }
    if (c == "ota.confirm") {
        return ctx.ops.ota_confirm() ? ok_result()
                                     : err_result("this image is not pending verification");
    }
    if (c == "ota.rollback") {
        return ctx.ops.ota_rollback() ? ok_result()
                                      : err_result("nothing to roll back to");
    }
    if (c == "system.reboot") {
        return ctx.ops.reboot() ? ok_result() : err_result("reboot unavailable");
    }

    return err_result("unknown command");
}

}  // namespace

}  // namespace api
}  // namespace swan
