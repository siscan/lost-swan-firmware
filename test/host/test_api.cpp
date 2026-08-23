// The web API (spec 10.2 / 10.2a): every command round-tripped through the
// JSON layer, the state payload, the reveal-by-name rule, the upload
// validator, and a concurrency case proving the HTTP task never reaches mode
// state without the mutex.
#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "check.h"
#include "fake_port.h"
#include "ring/json_lite.h"
#include "webapi/api.h"
#include "webapi/ring_upload.h"

using namespace swan;
using namespace swan::testfakes;

namespace {

// --------------------------------------------------------------------------
// Fakes for the API's interfaces
// --------------------------------------------------------------------------
struct FakeMotion : api::MotionAdmin {
    std::array<AxisInfo, N_COLUMNS> axes{};
    MotionParams p;
    int homes = 0;
    int last_home_col = -99;
    int cal_calls = 0;

    FakeMotion() {
        for (int i = 0; i < N_COLUMNS; ++i) {
            axes[static_cast<size_t>(i)].state = AxisState::Idle;
            axes[static_cast<size_t>(i)].index = 0;
            axes[static_cast<size_t>(i)].dest_index = 0;
        }
    }
    AxisInfo info(int col) override { return axes[static_cast<size_t>(col)]; }
    MotionParams params() override { return p; }
    void set_params(const MotionParams& np) override { p = np; }
    bool home(int col) override {
        ++homes;
        last_home_col = col;
        return true;
    }
    bool adjust_cal(int col, int32_t d) override {
        if (col < 0 || col >= N_COLUMNS) return false;
        ++cal_calls;
        axes[static_cast<size_t>(col)].cal_offset += d;
        return true;
    }
};

struct FakeCfgSink : api::ConfigSink {
    int motion_saves = 0, app_saves = 0;
    MotionParams last_motion;
    ModesConfig last_modes;
    std::string last_tz;
    bool save_motion(const MotionParams& mp) override {
        ++motion_saves;
        last_motion = mp;
        return true;
    }
    bool save_app(const ModesConfig& m, std::string_view tz) override {
        ++app_saves;
        last_modes = m;
        last_tz = std::string(tz);
        return true;
    }
};

struct FakeSys : api::SysInfoSource {
    api::SysInfo s;
    api::SysInfo get() override { return s; }
};

struct FakeOps : api::SystemOps {
    int reboots = 0;
    bool reboot() override {
        ++reboots;
        return true;
    }
};

struct Rig {
    RingSet ring = RingSet::compiled_fallback();
    FakePort port;
    FrameScheduler sched{port, {15, 82000}};
    FakeTime time;
    FakeStore store;
    FakeCues cues;
    ModeManager mm{ring, sched, time, store, cues};
    FakeMotion motion;
    FakeCfgSink cfg;
    FakeSys sys;
    FakeOps ops;
    api::RingStager stager{ring};
    api::Context ctx{mm, ring, motion, cfg, sys, stager, ops, "PST8PDT,M3.2.0,M11.1.0"};

    Rig() {
        mm.set_config(ModesConfig{});
        CHECK(mm.set_tz("PST8PDT,M3.2.0,M11.1.0"));
        time.utc_ms = 1787000000000LL;
        port.now_ms = time.utc_ms;
        mm.begin(time.utc_ms);
    }

    std::string cmd(const std::string& body) {
        return api::handle_command(ctx, body, time.utc_ms);
    }
    std::string state() { return api::build_state(ctx, time.utc_ms); }
};

bool is_ok(const std::string& r) {
    json::Value v;
    return json::parse(r, v, nullptr) && v.get("ok") && v.get("ok")->boolean;
}

std::string err_of(const std::string& r) {
    json::Value v;
    if (!json::parse(r, v, nullptr)) return "<unparseable>";
    return v.get("err") ? std::string(v.get("err")->as_str()) : "";
}

// --------------------------------------------------------------------------
// Every command, through the JSON layer
// --------------------------------------------------------------------------
void test_command_round_trip() {
    Rig r;

    // Mode switching.
    CHECK(is_ok(r.cmd(R"({"cmd":"mode.set","payload":"countdown"})")));
    CHECK(r.mm.mode() == Mode::Countdown);
    CHECK(is_ok(r.cmd(R"({"cmd":"mode.set","payload":"clock"})")));
    CHECK(r.mm.mode() == Mode::Clock);
    CHECK(!is_ok(r.cmd(R"({"cmd":"mode.set","payload":"nope"})")));

    // Message with tokens, dwell and hold.
    CHECK(is_ok(r.cmd(
        R"({"cmd":"message.set","payload":{"tokens":["eye","_","qmark","ankh","4"],"dwell_s":30,"hold":true}})")));
    CHECK(r.mm.mode() == Mode::Message);
    CHECK(!is_ok(r.cmd(R"({"cmd":"message.set","payload":{"tokens":["eye","_"]}})")));
    // "cycle" exists on ring A but was dropped from column 5's ring.
    CHECK(!is_ok(r.cmd(
        R"({"cmd":"message.set","payload":{"tokens":["_","_","_","_","cycle"]}})")));

    // Presets, both payload shapes.
    CHECK(is_ok(r.cmd(R"({"cmd":"preset.set","payload":"qmarks"})")));
    CHECK(is_ok(r.cmd(R"({"cmd":"preset.set","payload":{"name":"blank"}})")));
    CHECK(!is_ok(r.cmd(R"({"cmd":"preset.set","payload":"nope"})")));

    // display.frame by index and by token; never changes the mode.
    const Mode before = r.mm.mode();
    CHECK(is_ok(r.cmd(R"({"cmd":"display.frame","payload":{"indices":[1,2,3,4,5]}})")));
    CHECK(r.mm.mode() == before);
    CHECK(is_ok(r.cmd(
        R"({"cmd":"display.frame","payload":{"tokens":["eye","ankh","qmark","0","9"]}})")));
    CHECK(!is_ok(r.cmd(R"({"cmd":"display.frame","payload":{"indices":[1,2,3,4,99]}})")));
    CHECK(!is_ok(r.cmd(R"({"cmd":"display.frame","payload":{}})")));

    // Countdown: the ritual, the bypasses, the explicit deadline.
    CHECK(!is_ok(r.cmd(R"({"cmd":"countdown.execute","payload":"4 8 15 16 23 43"})")));
    CHECK(is_ok(r.cmd(R"({"cmd":"countdown.execute","payload":"4 8 15 16 23 42"})")));
    CHECK(r.mm.cd_phase() == CdPhase::Running);
    CHECK(is_ok(r.cmd(R"({"cmd":"countdown.cancel"})")));
    CHECK(r.mm.cd_phase() == CdPhase::Idle);
    CHECK(is_ok(r.cmd(R"({"cmd":"countdown.start"})")));
    CHECK(is_ok(r.cmd(R"({"cmd":"countdown.reset"})")));
    const int64_t tgt = r.time.utc_ms / 1000 + 4242;
    CHECK(is_ok(
        r.cmd(R"({"cmd":"countdown.set_target","payload":)" + std::to_string(tgt) + "}")));
    CHECK_EQ(r.mm.cd_target(), tgt);
    CHECK(!is_ok(r.cmd(R"({"cmd":"countdown.set_target","payload":0})")));

    // Clock format.
    CHECK(is_ok(r.cmd(R"({"cmd":"clock.format","payload":true})")));
    CHECK(r.mm.config().h24);
    CHECK(is_ok(r.cmd(R"({"cmd":"clock.format","payload":{"h24":false}})")));
    CHECK(!r.mm.config().h24);

    // Motion.
    CHECK(is_ok(r.cmd(R"({"cmd":"motion.rehome","payload":2})")));
    CHECK_EQ(r.motion.last_home_col, 2);
    CHECK(is_ok(r.cmd(R"({"cmd":"motion.rehome"})")));
    CHECK_EQ(r.motion.last_home_col, -1);  // all
    CHECK(is_ok(r.cmd(R"({"cmd":"motion.cal","payload":{"column":1,"delta":-10}})")));
    CHECK_EQ(r.motion.axes[1].cal_offset, -10);
    CHECK(!is_ok(r.cmd(R"({"cmd":"motion.cal","payload":{"delta":5}})")));
    CHECK(is_ok(r.cmd(R"({"cmd":"motion.cal","payload":{"column":1,"save":true}})")));
    CHECK_EQ(r.cfg.motion_saves, 1);

    // Live params apply immediately and are NOT persisted by themselves.
    CHECK(is_ok(r.cmd(
        R"({"cmd":"motion.params","payload":{"flaps_s_normal":22,"accel":90000}})")));
    CHECK_EQ(r.motion.p.flaps_s_normal, 22);
    CHECK_EQ(r.motion.p.accel, 90000);
    CHECK_EQ(r.cfg.motion_saves, 1);  // unchanged - save is a separate command
    CHECK(is_ok(r.cmd(R"({"cmd":"motion.save"})")));
    CHECK_EQ(r.cfg.motion_saves, 2);
    CHECK_EQ(r.cfg.last_motion.flaps_s_normal, 22);
    // Range checks.
    CHECK(!is_ok(r.cmd(R"({"cmd":"motion.params","payload":{"flaps_s_normal":0}})")));
    CHECK(!is_ok(r.cmd(R"({"cmd":"motion.params","payload":{"accel":10}})")));
    CHECK_EQ(r.motion.p.flaps_s_normal, 22);  // rejected leaves it alone

    // Config.
    CHECK(is_ok(r.cmd(R"({"cmd":"config.set","payload":{"granularity_min":5}})")));
    CHECK_EQ(r.mm.config().granularity_min, 5);
    CHECK(!is_ok(r.cmd(R"({"cmd":"config.set","payload":{"granularity_min":0}})")));
    CHECK(!is_ok(r.cmd(R"({"cmd":"config.set","payload":{"granularity_min":61}})")));
    CHECK(is_ok(r.cmd(R"({"cmd":"config.set","payload":{"seconds_mode":"tens"}})")));
    CHECK(r.mm.config().seconds_mode == SecondsMode::Tens);
    CHECK(!is_ok(r.cmd(R"({"cmd":"config.set","payload":{"seconds_mode":"halves"}})")));
    CHECK(is_ok(r.cmd(R"({"cmd":"config.set","payload":{"tz":"CET-1CEST,M3.5.0,M10.5.0/3"}})")));
    CHECK(!is_ok(r.cmd(R"({"cmd":"config.set","payload":{"tz":"nonsense"}})")));
    CHECK(is_ok(r.cmd(R"({"cmd":"config.save"})")));
    CHECK_EQ(r.cfg.app_saves, 1);

    // Later phases answer honestly rather than pretending.
    CHECK(!is_ok(r.cmd(R"({"cmd":"audio.volume","payload":50})")));
    CHECK(err_of(r.cmd(R"({"cmd":"audio.play","payload":"warn_4min"})")).find("phase 5") !=
          std::string::npos);

    // Reboot goes through the ops interface.
    CHECK(is_ok(r.cmd(R"({"cmd":"system.reboot"})")));
    CHECK_EQ(r.ops.reboots, 1);

    // Malformed input never throws and never half-applies.
    CHECK(!is_ok(r.cmd("")));
    CHECK(!is_ok(r.cmd("not json")));
    CHECK(!is_ok(r.cmd("{}")));
    CHECK(!is_ok(r.cmd(R"({"cmd":123})")));
    CHECK(!is_ok(r.cmd(R"({"cmd":"nope.nope"})")));
    CHECK(!is_ok(r.cmd(R"({"cmd":"message.set","payload":"oops"})")));
}

// --------------------------------------------------------------------------
// countdown.reveal is set BY NAME, and only with glyphs that column can show
// --------------------------------------------------------------------------
void test_reveal_by_name() {
    Rig r;

    CHECK(is_ok(r.cmd(
        R"({"cmd":"config.set","payload":{"reveal":["eye","ankh","qmark","scarab","duat"]}})")));
    const ModesConfig cfg = r.mm.config();
    for (int i = 0; i < N_COLUMNS; ++i) {
        CHECK(cfg.reveal[static_cast<size_t>(i)] >= 0);
    }
    // Each name resolved against ITS OWN column's ring.
    CHECK_EQ(cfg.reveal[0], r.ring.col(0).index_for_token("eye"));
    CHECK_EQ(cfg.reveal[4], r.ring.col(4).index_for_token("duat"));

    // The state payload reports names, never indices.
    json::Value st;
    CHECK(json::parse(r.state(), st, nullptr));
    const auto* rev = st.get("cfg")->get("reveal")->as_array();
    CHECK_EQ(rev->size(), 5u);
    CHECK((*rev)[0].as_str() == "eye");
    CHECK((*rev)[4].as_str() == "duat");

    // A glyph absent from column 5's ring is refused - "cycle" is on ring A
    // only.  The picker must not offer it, and the API must not accept it.
    CHECK(!is_ok(r.cmd(
        R"({"cmd":"config.set","payload":{"reveal":["eye","ankh","qmark","scarab","cycle"]}})")));
    CHECK_EQ(r.mm.config().reveal[4], cfg.reveal[4]);  // unchanged

    // Indices are not names: a numeric entry is refused outright.
    CHECK(!is_ok(r.cmd(R"({"cmd":"config.set","payload":{"reveal":[13,14,15,16,17]}})")));

    // null clears a column back to blank.
    CHECK(is_ok(r.cmd(
        R"({"cmd":"config.set","payload":{"reveal":[null,null,null,null,null]}})")));
    for (int i = 0; i < N_COLUMNS; ++i) CHECK_EQ(r.mm.config().reveal[static_cast<size_t>(i)], -1);
    CHECK(json::parse(r.state(), st, nullptr));
    CHECK((*st.get("cfg")->get("reveal")->as_array())[0].is_null());

    // The picker's source of truth: per-column glyph lists.  Column 5's set is
    // smaller and must not contain the glyphs it dropped.
    json::Value doc;
    CHECK(json::parse(api::build_ring_doc(r.ring), doc, nullptr));
    const auto* cols = doc.get("columns")->as_array();
    CHECK_EQ(cols->size(), 5u);
    const auto* g0 = (*cols)[0].get("glyphs")->as_array();
    const auto* g4 = (*cols)[4].get("glyphs")->as_array();
    CHECK_EQ(g0->size(), 36u);
    CHECK_EQ(g4->size(), 29u);
    bool col5_has_cycle = false;
    for (const auto& g : *g4) {
        if (g.get("id")->as_str() == "cycle") col5_has_cycle = true;
    }
    CHECK(!col5_has_cycle);
}

// --------------------------------------------------------------------------
// The state payload
// --------------------------------------------------------------------------
void test_state_payload() {
    Rig r;
    r.sys.s.wifi_state = "connected";
    r.sys.s.ssid = "dharma";
    r.sys.s.ip = "192.168.1.42";
    r.sys.s.rssi = -55;
    r.sys.s.heap = 123456;
    r.motion.axes[2].index = 17;
    r.motion.axes[2].state = AxisState::Moving;
    r.motion.axes[2].revs = 7;

    json::Value v;
    const std::string s = r.state();
    if (!json::parse(s, v, nullptr)) {
        CHECK(false);
        std::printf("  unparseable state: %.200s\n", s.c_str());
        return;
    }
    CHECK(v.get("mode") != nullptr);
    CHECK(v.get("t")->as_int() == r.time.utc_ms);
    CHECK(v.get("cols")->as_array()->size() == 5u);

    const auto& c2 = (*v.get("cols")->as_array())[2];
    CHECK_EQ(c2.get("index")->as_int(), 17);
    CHECK(c2.get("state")->as_str() == "MOVING");
    CHECK_EQ(c2.get("revs")->as_int(), 7);
    // Faces are reported by name - a slot number means nothing to a reader.
    CHECK(c2.get("face")->as_str() == r.ring.col(2).slot(17).id);

    CHECK(v.get("sys")->get("wifi")->as_str() == "connected");
    CHECK(v.get("sys")->get("ip")->as_str() == "192.168.1.42");
    CHECK_EQ(v.get("sys")->get("rssi")->as_int(), -55);
    CHECK(v.get("ring")->get("descending")->boolean);
    CHECK(v.get("ring")->get("source")->as_str() == "compiled");
    CHECK_EQ(v.get("cfg")->get("granularity_min")->as_int(), 15);
    CHECK(v.get("cal")->get("ramp_active")->boolean == false);

    // Strings that need escaping survive the round trip.
    r.sys.s.ssid = "quote\" back\\slash \x01ctl";
    json::Value v2;
    CHECK(json::parse(r.state(), v2, nullptr));
    CHECK(v2.get("sys")->get("ssid")->as_str() == "quote\" back\\slash \x01ctl");
}

// --------------------------------------------------------------------------
// The calibration ramp
// --------------------------------------------------------------------------
void test_cal_ramp() {
    Rig r;
    r.port.gos.clear();  // drop the boot clock render
    CHECK(is_ok(r.cmd(
        R"({"cmd":"motion.ramp","payload":{"column":0,"from":40,"to":49,"step":3,"dwell_s":1}})")));
    CHECK(r.mm.cal_ramp_active());
    CHECK_EQ(r.mm.cal_ramp_column(), 0);

    // Walks 40, 43, 46, 49 and stops of its own accord.
    for (int i = 0; i < 100 && r.mm.cal_ramp_active(); ++i) {
        r.time.utc_ms += 100;
        r.port.now_ms = r.time.utc_ms;
        r.mm.tick(r.time.utc_ms);
    }
    CHECK(!r.mm.cal_ramp_active());  // finished on its own

    std::vector<int> stops;
    for (const auto& g : r.port.gos) {
        if (g.col == 0) stops.push_back(g.index);
    }
    CHECK_EQ(stops.size(), 4u);
    if (stops.size() == 4) {
        CHECK_EQ(stops[0], 40);
        CHECK_EQ(stops[1], 43);
        CHECK_EQ(stops[2], 46);
        CHECK_EQ(stops[3], 49);
    }
    // The walk owns the display: no other column is disturbed while it runs.
    CHECK_EQ(r.port.gos_for(1), 0);
    CHECK_EQ(r.port.gos_for(4), 0);

    // Handing the display back lets the mode render again.
    r.port.gos.clear();
    r.time.utc_ms += 60000;
    r.port.now_ms = r.time.utc_ms;
    r.mm.tick(r.time.utc_ms);
    CHECK(!r.port.gos.empty());

    // Bad arguments are refused.
    CHECK(!is_ok(r.cmd(R"({"cmd":"motion.ramp","payload":{"column":9,"from":0,"to":1}})")));
    CHECK(!is_ok(r.cmd(R"({"cmd":"motion.ramp","payload":{"column":0,"from":0,"to":99}})")));
    CHECK(!is_ok(r.cmd(
        R"({"cmd":"motion.ramp","payload":{"column":0,"from":0,"to":9,"step":0}})")));
    CHECK(!is_ok(r.cmd(R"({"cmd":"motion.ramp","payload":{"column":0}})")));

    // Stopping mid-walk hands the display back to the mode.
    CHECK(is_ok(r.cmd(
        R"({"cmd":"motion.ramp","payload":{"column":3,"from":0,"to":40,"step":1,"dwell_s":5}})")));
    CHECK(r.mm.cal_ramp_active());
    CHECK(is_ok(r.cmd(R"({"cmd":"motion.ramp_stop"})")));
    CHECK(!r.mm.cal_ramp_active());
}

// --------------------------------------------------------------------------
// Upload validator: malformed, truncated, oversized, wrong slot count
// --------------------------------------------------------------------------
std::string good_ring_json() {
    // Rebuild the shipped tables into an upload document.
    const RingSet base = RingSet::compiled_fallback();
    auto tbl = [&](int c) {
        std::string out = "[";
        for (int i = 0; i < RING_SLOT_COUNT; ++i) {
            out += "{\"i\":" + std::to_string(i) + ",\"id\":\"" + base.col(c).slot(i).id +
                   "\",\"cat\":\"" + ring_category_name(base.col(c).slot(i).cat) + "\"},";
        }
        out.back() = ']';
        return out;
    };
    return "{\"slots\":" + tbl(0) + ",\"columns\":[{},{},{},{},{\"ring\":" + tbl(4) + "}]}";
}

void test_upload_validator() {
    Rig r;
    const std::string good = good_ring_json();
    std::string err;

    // The happy path stages but does NOT touch the live table until the modes
    // task applies it.
    CHECK(!r.ring.loaded_from_json());
    CHECK(r.stager.stage(good, &err));
    CHECK(r.stager.pending());
    CHECK(!r.ring.loaded_from_json());  // still the compiled table
    CHECK(r.stager.apply_pending());
    CHECK(r.ring.loaded_from_json());
    CHECK(!r.stager.pending());
    CHECK(!r.stager.apply_pending());  // idempotent
    CHECK(!r.stager.take_accepted_body().empty());

    // Everything below must be rejected AND leave the running table intact.
    const bool was_json = r.ring.loaded_from_json();
    struct Case {
        const char* name;
        std::string body;
    };
    std::vector<Case> bad;
    bad.push_back({"empty", ""});
    bad.push_back({"garbage", "this is not json"});
    bad.push_back({"truncated-mid-object", good.substr(0, good.size() / 2)});
    bad.push_back({"truncated-one-byte-short", good.substr(0, good.size() - 1)});
    bad.push_back({"oversized", std::string(api::RING_UPLOAD_MAX + 1, 'x')});
    bad.push_back({"no-slots", R"({"columns":[]})"});
    bad.push_back({"slots-not-array", R"({"slots":42})"});
    bad.push_back({"three-slots",
                   R"({"slots":[{"i":0,"id":"blank","cat":"blank"},)"
                   R"({"i":1,"id":"0","cat":"digit"},{"i":2,"id":"1","cat":"digit"}]})"});
    bad.push_back({"sparse-index", R"({"slots":[{"i":1,"id":"blank","cat":"blank"}]})"});
    bad.push_back({"unknown-category", R"({"slots":[{"i":0,"id":"x","cat":"weird"}]})"});
    // 51 slots: one too many for a physical drum.
    {
        std::string s = good;
        const std::string tail = R"(,{"i":50,"id":"extra","cat":"glyph"}])";
        const size_t pos = s.find("],\"columns\"");
        s = s.substr(0, pos) + tail + s.substr(pos + 1);
        bad.push_back({"fifty-one-slots", s});
    }
    // A valid-shaped table that cannot render a role its column needs: ring B
    // on every column strips AM/PM from column 1.
    {
        const RingSet base = RingSet::compiled_fallback();
        std::string out = "[";
        for (int i = 0; i < RING_SLOT_COUNT; ++i) {
            out += "{\"i\":" + std::to_string(i) + ",\"id\":\"" + base.col(4).slot(i).id +
                   "\",\"cat\":\"" + ring_category_name(base.col(4).slot(i).cat) + "\"},";
        }
        out.back() = ']';
        bad.push_back({"ringB-everywhere", "{\"slots\":" + out + "}"});
    }

    for (const auto& c : bad) {
        err.clear();
        if (r.stager.stage(c.body, &err)) {
            CHECK(false);
            std::printf("  upload case '%s' was ACCEPTED\n", c.name);
            continue;
        }
        if (err.empty()) {
            CHECK(false);
            std::printf("  upload case '%s' rejected with no reason\n", c.name);
        }
        CHECK(!r.stager.pending());
        CHECK_EQ(r.ring.loaded_from_json(), was_json);  // running table untouched
        // ...and the table still works.
        CHECK_EQ(r.ring.col(0).index_for_role(Role::Blank), RING_HOME_SLOT);
        CHECK(r.ring.validate_roles(nullptr));
    }

    // Rejected uploads never reach the "accepted body" the target persists.
    CHECK(r.stager.take_accepted_body().empty());

    // Byte-by-byte truncation of a good document: every prefix is rejected and
    // nothing is left staged.
    for (size_t n = 1; n < good.size(); n += 97) {
        err.clear();
        CHECK(!r.stager.stage(good.substr(0, n), &err));
        CHECK(!r.stager.pending());
    }
}

// --------------------------------------------------------------------------
// No HTTP-task path touches ModeManager without the mutex.
//
// ModeManager bumps a witness counter inside its lock on every public entry
// point; if an HTTP-side call ever reached mode state without taking it, two
// threads would be inside at once and max_concurrent() would exceed 1.
// --------------------------------------------------------------------------
void test_no_unlocked_mode_access() {
    Rig r;
    std::atomic<bool> stop{false};
    std::atomic<int> errors{0};

    // The "modes task": ticks flat out.  Time is held constant so the test
    // measures contention, not mode behaviour - a tick with an unchanged clock
    // is a legitimate call and takes exactly the same lock.
    std::thread modes([&] {
        const int64_t t = r.time.utc_ms;
        while (!stop.load(std::memory_order_relaxed)) r.mm.tick(t);
    });

    // Two "HTTP tasks": commands and state reads, exactly as the server does.
    auto http = [&](int seed) {
        static const char* cmds[] = {
            R"({"cmd":"mode.set","payload":"clock"})",
            R"({"cmd":"mode.set","payload":"countdown"})",
            R"({"cmd":"countdown.start"})",
            R"({"cmd":"countdown.cancel"})",
            R"({"cmd":"preset.set","payload":"qmarks"})",
            R"({"cmd":"config.set","payload":{"granularity_min":5}})",
            R"({"cmd":"clock.format","payload":true})",
            R"({"cmd":"motion.ramp","payload":{"column":0,"from":40,"to":45,"step":1,"dwell_s":0}})",
            R"({"cmd":"motion.ramp_stop"})",
            R"({"cmd":"display.frame","payload":{"indices":[0,0,0,0,0]}})",
        };
        for (int i = seed; i < seed + 4000; ++i) {
            const std::string res = api::handle_command(
                r.ctx, cmds[static_cast<size_t>(i) % (sizeof cmds / sizeof cmds[0])],
                r.time.utc_ms);
            if (res.empty()) ++errors;
            const std::string st = api::build_state(r.ctx, r.time.utc_ms);
            if (st.size() < 10) ++errors;
        }
    };
    std::thread h1(http, 0), h2(http, 5);

    h1.join();
    h2.join();
    stop.store(true, std::memory_order_relaxed);
    modes.join();

    CHECK_EQ(errors.load(), 0);
    // The witness: never two threads inside ModeManager at once.
    CHECK_EQ(r.mm.max_concurrent(), 1);
}

}  // namespace

void run_tests() {
    test_command_round_trip();
    test_reveal_by_name();
    test_state_payload();
    test_cal_ramp();
    test_upload_validator();
    test_no_unlocked_mode_access();
}
