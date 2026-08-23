#include "webapi/api.h"

#include <array>
#include <vector>

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

std::string result_of(ModeManager::Result r) {
    return r.ok ? ok_result() : err_result(r.err ? r.err : "rejected");
}

const char* seconds_mode_name(SecondsMode m) {
    return m == SecondsMode::Seconds ? "seconds" : "tens";
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
    const ModesConfig cfg = ctx.modes.config();
    const MotionParams mp = ctx.motion.params();
    const SysInfo sys = ctx.sys.get();

    Writer w;
    w.obj();
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
        .end_obj();

    w.kv("time_valid", ctx.modes.time_valid());
    w.kv("wifi_glyph", ctx.modes.wifi_glyph_shown());

    w.key("cols").arr();
    for (int i = 0; i < N_COLUMNS; ++i) {
        const AxisInfo a = ctx.motion.info(i);
        w.obj()
            .kv("state", axis_state_name(a.state))
            .kv("index", a.index)
            .kv("face", face_of(ctx.ring, i, a.index))
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
            .end_obj();
    }
    w.end_arr();

    w.key("cfg").obj()
        .kv("h24", cfg.h24)
        .kv("granularity_min", cfg.granularity_min)
        .kv("seconds_mode", seconds_mode_name(cfg.seconds_mode))
        .kv("msg_dwell_s", cfg.msg_dwell_s)
        .kv("zero_hold_s", cfg.zero_hold_s)
        .kv("spin_s", cfg.spin_s)
        .kv("failure_timeout_s", cfg.failure_timeout_s)
        .kv("clock_land_on_tick", cfg.clock_land_on_tick)
        .kv("cd_land_on_tick", cfg.cd_land_on_tick)
        .kv("tz", ctx.tz)
        .kv("flaps_s_normal", mp.flaps_s_normal)
        .kv("flaps_s_alarm", mp.flaps_s_alarm)
        .kv("flaps_s_home", mp.flaps_s_home)
        .kv("accel", mp.accel)
        .kv("hall_tol", mp.hall_tol);
    write_reveal(w, ctx.ring, cfg);
    w.end_obj();

    w.key("ring").obj()
        .kv("source", ctx.ring.loaded_from_json() ? "ring.json" : "compiled")
        .kv("slots", ctx.ring.col(0).slot_count())
        .kv("descending", ctx.ring.col(0).is_descending() && ctx.ring.col(4).is_descending())
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
        .kv("uptime_s", static_cast<int64_t>(sys.uptime_s))
        .kv("reset", sys.reset_reason)
        .kv("version", sys.version)
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
    w.key("columns").arr();
    for (int c = 0; c < N_COLUMNS; ++c) {
        const RingTable& t = ring.col(c);
        w.obj();
        w.kv("slots", t.slot_count());
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

std::string do_display_frame(Context& ctx, const json::Value& p, int64_t utc_ms) {
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
            const int got = ctx.ring.col(i).index_for_token((*a)[static_cast<size_t>(i)].as_str());
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
    ctx.motion.set_params(mp);
    return ok_result();
}

std::string do_config_set(Context& ctx, const json::Value& p, int64_t utc_ms) {
    ModesConfig cfg = ctx.modes.config();
    int v = 0;

    if (const json::Value* h = member(p, "h24")) {
        ctx.modes.cmd_clock_format(h->boolean, utc_ms);
        cfg = ctx.modes.config();
    }
    if (as_int_field(p, "granularity_min", v)) {
        if (v < 1 || v > 60) return err_result("granularity_min must be 1..60");
        cfg.granularity_min = v;
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
    if (as_int_field(p, "failure_timeout_s", v)) {
        if (v < 0 || v > 86400) return err_result("failure_timeout_s out of range");
        cfg.failure_timeout_s = v;
    }
    if (const json::Value* m = member(p, "seconds_mode")) {
        const std::string_view s = m->as_str();
        if (s == "seconds") cfg.seconds_mode = SecondsMode::Seconds;
        else if (s == "tens") cfg.seconds_mode = SecondsMode::Tens;
        else return err_result("seconds_mode must be seconds|tens");
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
            const int got = ctx.ring.col(i).index_for_token(e.as_str());
            if (got < 0) {
                return err_result("reveal glyph not on that column's ring");
            }
            next[static_cast<size_t>(i)] = got;
        }
        cfg.reveal = next;
    }

    if (const json::Value* tz = member(p, "tz")) {
        if (!ctx.modes.set_tz(tz->as_str())) return err_result("bad POSIX TZ string");
        ctx.tz = std::string(tz->as_str());
    }

    ctx.modes.set_config(cfg);
    return ok_result();
}

}  // namespace

std::string handle_command(Context& ctx, std::string_view body, int64_t utc_ms) {
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
    if (c == "display.frame") return do_display_frame(ctx, p, utc_ms);

    // ---- countdown ----
    if (c == "countdown.execute") {
        const std::string_view n = p.type == json::Type::Str
                                       ? p.as_str()
                                       : (p.get("numbers") ? p.get("numbers")->as_str()
                                                           : std::string_view{});
        return result_of(ctx.modes.cmd_countdown_execute(n, utc_ms));
    }
    if (c == "countdown.start") return result_of(ctx.modes.cmd_countdown_start(utc_ms));
    if (c == "countdown.reset") return result_of(ctx.modes.cmd_countdown_reset(utc_ms));
    if (c == "countdown.cancel") return result_of(ctx.modes.cmd_countdown_cancel(utc_ms));
    if (c == "countdown.set_target") {
        const int64_t epoch = p.type == json::Type::Int
                                  ? p.number
                                  : (p.get("epoch") ? p.get("epoch")->as_int(0) : 0);
        return result_of(ctx.modes.cmd_countdown_set_target(epoch, utc_ms));
    }

    // ---- clock ----
    if (c == "clock.format") {
        const bool h24 = p.type == json::Type::Bool
                             ? p.boolean
                             : (p.get("h24") ? p.get("h24")->boolean : false);
        return result_of(ctx.modes.cmd_clock_format(h24, utc_ms));
    }

    // ---- motion / calibration ----
    if (c == "motion.rehome") {
        int col = -1;
        if (p.type == json::Type::Int) col = static_cast<int>(p.number);
        else as_int_field(p, "column", col);
        return ctx.motion.home(col) ? ok_result() : err_result("rehome rejected");
    }
    if (c == "motion.cal") {
        int col = 0, delta = 0;
        if (!as_int_field(p, "column", col)) return err_result("need column");
        if (as_int_field(p, "delta", delta)) {
            return ctx.motion.adjust_cal(col, delta) ? ok_result() : err_result("bad column");
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

    // ---- config ----
    if (c == "config.set") return do_config_set(ctx, p, utc_ms);
    if (c == "config.save") {
        return ctx.cfg.save_app(ctx.modes.config(), ctx.tz) ? ok_result()
                                                            : err_result("save failed");
    }

    // ---- ring upload (body carried separately by the HTTP route) ----
    if (c == "ring.upload") {
        const json::Value* b = member(p, "body");
        if (b == nullptr || b->type != json::Type::Str) return err_result("need body");
        std::string err;
        return ctx.ring_upload.stage(b->as_str(), &err) ? ok_result() : err_result(err);
    }

    // ---- later phases ----
    if (c == "audio.volume" || c == "audio.mute" || c == "audio.play") {
        return err_result("audio arrives in phase 5");
    }
    if (c == "system.reboot") {
        return ctx.ops.reboot() ? ok_result() : err_result("reboot unavailable");
    }

    return err_result("unknown command");
}

}  // namespace api
}  // namespace swan
