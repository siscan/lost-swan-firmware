// The web API (spec 10.2 / 10.2a): every command round-tripped through the
// JSON layer, the state payload, the reveal-by-name rule, the upload
// validator, and a concurrency case proving the HTTP task never reaches mode
// state without the mutex.
#include <atomic>
#include <cstring>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

#include "check.h"
#include "fake_port.h"
#include "ring/json_lite.h"
#include "audio/wav.h"
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
    // Models the target's rule rather than always succeeding: a disabled column
    // is never homed (spec 5.9), and a re-home-all with every column disabled
    // posts nothing - which used to be reported as success by both the fake and
    // the firmware, so a test written against this fake could not have caught it.
    bool home(int col) override {
        ++homes;
        last_home_col = col;
        if (col >= 0) {
            if (col >= N_COLUMNS) return false;
            return cols.mode[static_cast<size_t>(col)] != ColumnMode::Disabled;
        }
        for (int i = 0; i < N_COLUMNS; ++i) {
            if (cols.mode[static_cast<size_t>(i)] != ColumnMode::Disabled) return true;
        }
        return false;
    }
    int spins = 0;
    int32_t last_spin_flaps = 0;
    bool spin_open_loop(int col, int32_t flaps_s, int seconds) override {
        if (col < 0 || col >= N_COLUMNS || seconds <= 0) return false;
        if (cols.mode[static_cast<size_t>(col)] == ColumnMode::Disabled) return false;
        ++spins;
        last_spin_flaps = flaps_s;
        axes[static_cast<size_t>(col)].index = RING_INVALID;
        return true;
    }
    ColumnConfig cols;
    int sim_injects = 0;
    // Deliberate instrumentation, on the same principle as FakePort's
    // mailbox_lag: the real set_columns is one struct assignment, and a tear
    // between two dispatchers would be a rare interleaving that a test could
    // pass through by luck for years.  Widening the write to element-by-element
    // with a yield between makes an unserialised race tear on essentially every
    // run, so the test states the property instead of sampling it.
    std::atomic<bool> widen_window{false};
    ColumnConfig columns() override {
        if (widen_window.load(std::memory_order_relaxed)) std::this_thread::yield();
        return cols;
    }
    bool set_columns(const ColumnConfig& c) override {
        if (!widen_window.load(std::memory_order_relaxed)) {
            cols = c;
            return true;
        }
        for (int i = 0; i < N_COLUMNS; ++i) {
            cols.mode[static_cast<size_t>(i)] = c.mode[static_cast<size_t>(i)];
            std::this_thread::yield();
        }
        cols.maintenance = c.maintenance;
        return true;
    }
    bool sim_inject(int col, std::string_view kind, int32_t) override {
        if (col < 0 || col >= N_COLUMNS) return false;
        if (cols.mode[static_cast<size_t>(col)] != ColumnMode::Sim) return false;
        if (kind != "slip" && kind != "miss" && kind != "clear") return false;
        ++sim_injects;
        return true;
    }
    bool sim_available() const override { return true; }

    // Models the target: a nudge re-seeks, and a column with no home reference
    // reports that nothing moved.  `reseeks` is what the tests assert on -
    // asserting the RETURN VALUE is exactly the blind spot that let the web
    // nudge ship doing nothing.
    std::vector<std::pair<int, int>> reseeks;   // (column, index)
    api::MotionAdmin::CalOutcome adjust_cal(int col, int32_t d) override {
        if (col < 0 || col >= N_COLUMNS) return CalOutcome::BadColumn;
        ++cal_calls;
        axes[static_cast<size_t>(col)].cal_offset += d;
        const int idx = axes[static_cast<size_t>(col)].index;
        if (idx < 0) return CalOutcome::NotHomed;
        reseeks.emplace_back(col, idx);
        return CalOutcome::Moved;
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
    std::string last_ntp;
    bool save_app(const ModesConfig& m, std::string_view tz,
                  std::string_view ntp) override {
        last_ntp = std::string(ntp);
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

struct FakeMqtt : api::MqttAdmin {
    api::MqttStatus st;
    int configures = 0;
    std::string last_uri, last_user, last_pass, last_base;
    api::MqttStatus mqtt_status() override { return st; }
    bool mqtt_configure(const api::MqttAdmin::MqttSettings& in) override {
        ++configures;
        st.enabled = in.enabled;
        // Absent keeps, present replaces - the target's rule, so a test that
        // passes here means the same thing on the board.
        if (in.uri) st.uri = last_uri = std::string(*in.uri);
        if (in.user) last_user = std::string(*in.user);
        if (in.pass) last_pass = std::string(*in.pass);
        if (in.base) st.base = last_base = std::string(*in.base);
        return true;
    }
};

struct FakeWifi : api::WifiAdmin {
    std::string ssid, pass;
    bool portal = false;
    int saves = 0;
    bool set_credentials(std::string_view s, std::string_view p) override {
        ++saves;
        ssid = std::string(s);
        pass = std::string(p);
        return true;
    }
    bool start_portal() override { portal = true; return true; }
    bool stop_portal() override { portal = false; return true; }
    bool portal_running() override { return portal; }
    std::string portal_ssid() override { return "LOST-Swan-test"; }
    bool have_credentials() override { return !ssid.empty(); }
};

struct FakeAudio : api::AudioAdmin {
    api::AudioState st;
    int plays = 0;
    std::string last_cue;
    bool has_files = true;
    api::AudioState audio_state() override { return st; }
    bool audio_set(int v, bool m, int qs, int qe) override {
        st.volume = v;
        st.mute = m;
        st.quiet_start_min = qs;
        st.quiet_end_min = qe;
        return true;
    }
    bool audio_play(std::string_view cue) override {
        swan::audio::CueId id{};
        if (!swan::audio::cue_id_from_name(cue, id)) return false;
        if (!has_files) return false;
        ++plays;
        last_cue = std::string(cue);
        return true;
    }
    bool audio_stop() override { return true; }
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
    FakeMqtt mqtt;
    FakeWifi wifi;
    FakeAudio audio;
    api::RingStager stager{ring};
    api::Context ctx{mm, stager, motion, cfg, sys, stager, ops, mqtt, wifi, audio, {}};

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

// The note a reply carries when the command took effect but the visible thing
// the caller wanted did NOT happen.  Asserting on this is the point: the bug
// these tests exist for returned a bare {"ok":true}.
std::string note_of(const std::string& r) {
    json::Value v;
    if (!json::parse(r, v, nullptr)) return "<unparseable>";
    return v.get("note") ? std::string(v.get("note")->as_str()) : "";
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

    // Bench test spin, through the same dispatcher as everything else.
    CHECK(is_ok(r.cmd(
        R"({"cmd":"motion.spin","payload":{"column":4,"flaps_s":25,"seconds":3}})")));
    CHECK_EQ(r.motion.spins, 1);
    CHECK_EQ(r.motion.last_spin_flaps, 25);
    CHECK(!is_ok(r.cmd(R"({"cmd":"motion.spin","payload":{"flaps_s":25}})")));
    CHECK(!is_ok(r.cmd(
        R"({"cmd":"motion.spin","payload":{"column":0,"flaps_s":99,"seconds":3}})")));
    CHECK(!is_ok(r.cmd(
        R"({"cmd":"motion.spin","payload":{"column":0,"flaps_s":20,"seconds":0}})")));
    CHECK_EQ(r.motion.spins, 1);
    r.motion.axes[4].index = 0;  // pretend it re-homed, so later checks are sane

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

    // Audio (spec 9), now that phase 5 has landed.
    CHECK(is_ok(r.cmd(R"({"cmd":"audio.volume","payload":50})")));
    CHECK_EQ(r.audio.st.volume, 50);
    CHECK(!is_ok(r.cmd(R"({"cmd":"audio.volume","payload":140})")));
    CHECK(!is_ok(r.cmd(R"({"cmd":"audio.volume","payload":-1})")));
    CHECK(is_ok(r.cmd(R"({"cmd":"audio.mute","payload":true})")));
    CHECK(r.audio.st.mute);
    CHECK(is_ok(r.cmd(R"({"cmd":"audio.play","payload":"warn_4min"})")));
    CHECK_EQ(r.audio.plays, 1);
    CHECK(!is_ok(r.cmd(R"({"cmd":"audio.play","payload":"nope"})")));
    // A cue with no file is REFUSED, not reported as played: "the alarm did
    // nothing" is a bad thing to discover at zero.
    r.audio.has_files = false;
    CHECK(!is_ok(r.cmd(R"({"cmd":"audio.play","payload":"system_failure"})")));
    r.audio.has_files = true;
    // Quiet hours: both bounds equal means OFF, and that has to stay
    // expressible rather than becoming a 24-hour silence.
    CHECK(is_ok(r.cmd(R"({"cmd":"audio.quiet","payload":{"start_min":1320,"end_min":420}})")));
    CHECK_EQ(r.audio.st.quiet_start_min, 1320);
    CHECK(!is_ok(r.cmd(R"({"cmd":"audio.quiet","payload":{"start_min":-1,"end_min":420}})")));
    CHECK(!is_ok(r.cmd(R"({"cmd":"audio.quiet","payload":{"start_min":0}})")));

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
    // /api/ring is ~1,850 nodes - far past the device's untrusted-input budget,
    // and rightly so: it is a document WE generate for a browser, not something
    // the board ever parses.  Say so explicitly rather than inheriting a limit
    // meant for a hostile POST.
    json::Value doc;
    CHECK(json::parse(api::build_ring_doc(r.ring), doc, nullptr, 20000));
    if (doc.get("columns") == nullptr || doc.get("columns")->as_array() == nullptr) {
        CHECK(false);
        return;   // a failed parse must fail the check, not segfault the suite
    }
    const auto* cols = doc.get("columns")->as_array();
    CHECK_EQ(cols->size(), 5u);
    const auto* g0 = (*cols)[0].get("glyphs")->as_array();
    const auto* g4 = (*cols)[4].get("glyphs")->as_array();
    CHECK_EQ(g0->size(), 36u);
    CHECK_EQ(g4->size(), 29u);
    // Presentation travels with the ring so the mirror matches the wall: the
    // seconds group (cols 4-5) carries the inverted drums.
    CHECK(doc.get("schemes")->get("minutes") != nullptr);
    CHECK(doc.get("schemes")->get("seconds") != nullptr);
    CHECK((*cols)[0].get("scheme")->as_str() == "minutes");
    CHECK((*cols)[4].get("scheme")->as_str() == "seconds");

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

    // "index unknown" and "showing the blank flap" must be distinguishable in
    // the payload - on the bench they rendered identically, so five columns
    // hunting for their hall edge looked like an idle display.
    r.motion.axes[0].state = AxisState::Homing;
    r.motion.axes[0].index = RING_INVALID;
    r.motion.axes[0].rehome_attempt = 2;
    r.motion.axes[1].state = AxisState::Idle;
    r.motion.axes[1].index = RING_HOME_SLOT;   // genuinely showing blank
    json::Value vh;
    CHECK(json::parse(r.state(), vh, nullptr));
    const auto* ch = vh.get("cols")->as_array();
    CHECK((*ch)[0].get("settled")->boolean == false);
    CHECK_EQ((*ch)[0].get("index")->as_int(), RING_INVALID);
    CHECK_EQ((*ch)[0].get("retry")->as_int(), 2);
    CHECK((*ch)[0].get("state")->as_str() == "HOMING");
    CHECK((*ch)[1].get("settled")->boolean == true);
    CHECK_EQ((*ch)[1].get("retry")->as_int(), 0);
    CHECK((*ch)[1].get("face")->as_str() == "blank");
    // An Idle column with an unknown index - after an open-loop spin - is also
    // not settled, even though its state says Idle.
    r.motion.axes[2].state = AxisState::Idle;
    r.motion.axes[2].index = RING_INVALID;
    CHECK(json::parse(r.state(), vh, nullptr));
    CHECK((*vh.get("cols")->as_array())[2].get("settled")->boolean == false);
    r.motion.axes[0].state = AxisState::Idle;
    r.motion.axes[0].index = 0;
    r.motion.axes[0].rehome_attempt = 0;
    r.motion.axes[2].index = 0;

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
    // A byte limit alone does not bound the DOM: each parsed value costs an
    // order of magnitude more than the two bytes of "0," that produce it, and
    // on target the allocation failure is abort() with exceptions off - a
    // remote reboot loop from a few KB.  The parser has a node budget now.
    {
        std::string flood = "{\"slots\":[";
        while (flood.size() < 40 * 1024) flood += "0,";
        flood += "0]}";
        bad.push_back({"node-flood", flood});
    }
    {
        // Just under the byte limit, still far over the node budget.
        std::string flood = "[";
        while (flood.size() < api::RING_UPLOAD_MAX - 8) flood += "0,";
        flood += "0]";
        bad.push_back({"node-flood-under-byte-limit", flood});
    }
    {
        // 2,000 nodes is the count that rebooted the real board; the cap has
        // to stop it well before the allocator does.
        std::string flood = "{\"slots\":[";
        for (int i = 0; i < 2000; ++i) flood += "0,";
        flood += "0]}";
        bad.push_back({"node-flood-2000", flood});
    }
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

    // The cap must admit a real ring.json with room to spare - it is 535 nodes
    // against a 900 budget.  If a bigger drum ever pushes past this, the cap
    // is not the thing to raise: the DOM is, and the answer is a streaming
    // parse (the headroom on the device is thin, see json_lite.cpp).
    CHECK(r.stager.stage(good, &err));
    CHECK(r.stager.apply_pending());
    (void)r.stager.take_accepted_body();

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
// A ring swap must not free tables a reader is walking.
//
// The phase 3 review's first critical: apply_pending() reassigns the live
// RingSet, dropping the last shared_ptr to the outgoing tables, while the HTTP
// task held raw `const RingTable&` references into them across a whole
// response.  Readers now pin a snapshot, so the old tables outlive them.
// --------------------------------------------------------------------------
void test_ring_swap_vs_readers() {
    Rig r;
    const std::string good = good_ring_json();

    std::atomic<bool> stop{false};
    std::atomic<int> bad{0};
    std::atomic<int> swaps{0};
    std::atomic<int> reads{0};

    // The modes task: stage and apply, over and over.
    std::thread modes([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            std::string err;
            if (r.stager.stage(good, &err) && r.stager.apply_pending()) {
                ++swaps;
                (void)r.stager.take_accepted_body();
            }
        }
    });

    // Two HTTP tasks doing exactly what the routes do.  Every byte they read
    // has to come from a table that is still alive; under ASan or a debug
    // allocator a regression here is a use-after-free, and even without one a
    // freed-and-reused table shows up as a wrong slot count or a torn id.
    auto http = [&] {
        for (int i = 0; i < 400; ++i) {
            const std::string doc = api::build_ring_doc(r.stager.snapshot());
            if (doc.find("\"slots\":50") == std::string::npos) ++bad;
            const std::string st = api::build_state(r.ctx, r.time.utc_ms);
            if (st.find("\"cols\"") == std::string::npos) ++bad;
            // A pinned set must stay coherent for as long as it is held, even
            // though the live one is being replaced under us the whole time.
            const RingSet pinned = r.stager.snapshot();
            for (int pass = 0; pass < 20; ++pass) {
                for (int c = 0; c < N_COLUMNS; ++c) {
                    if (pinned.col(c).slot_count() != RING_SLOT_COUNT) ++bad;
                    if (pinned.col(c).slot(RING_HOME_SLOT).id != "blank") ++bad;
                }
            }
            ++reads;
        }
    };
    std::thread h1(http), h2(http);
    h1.join();
    h2.join();
    stop.store(true, std::memory_order_relaxed);
    modes.join();

    CHECK_EQ(bad.load(), 0);
    CHECK(swaps.load() > 0);
    CHECK_EQ(reads.load(), 800);
    CHECK_EQ(r.mm.max_concurrent(), 1);
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


// --------------------------------------------------------------------------
// Per-column mode, maintenance and fault injection over the ONE dispatcher
// --------------------------------------------------------------------------
void test_column_commands() {
    Rig r;

    // Default: every column real, nothing simulated, no maintenance.  A fresh
    // device must never claim otherwise.
    {
        json::Value v;
        CHECK(json::parse(r.state(), v, nullptr));
        const json::Value* m = v.get("motion");
        CHECK(m != nullptr);
        CHECK(!m->get("simulated")->boolean);
        CHECK(!m->get("maintenance")->boolean);
        CHECK_EQ(static_cast<int>(m->get("sim_columns")->number), 0);
        // Every column reports what it IS and why it faulted, so "disabled" is
        // never read as "broken" and "simulated" is never read as real.
        const json::Value* cols = v.get("cols");
        CHECK(cols != nullptr && cols->items.size() == N_COLUMNS);
        CHECK(cols->items[0].get("mode")->as_str() == "real");
        CHECK(cols->items[0].get("cause")->as_str() == "none");
    }

    CHECK(is_ok(r.cmd(R"({"cmd":"motion.column","payload":{"column":2,"mode":"sim"}})")));
    CHECK(r.motion.cols.mode[2] == ColumnMode::Sim);
    CHECK(!is_ok(r.cmd(R"({"cmd":"motion.column","payload":{"column":2,"mode":"nope"}})")));
    CHECK(!is_ok(r.cmd(R"({"cmd":"motion.column","payload":{"column":9,"mode":"sim"}})")));
    CHECK(!is_ok(r.cmd(R"({"cmd":"motion.column","payload":{"mode":"sim"}})")));
    CHECK(!is_ok(r.cmd(R"({"cmd":"motion.column","payload":{"column":0}})")));

    // "all" is the shape the console uses ("sim all"), through the same path.
    CHECK(is_ok(r.cmd(R"({"cmd":"motion.column","payload":{"all":true,"mode":"sim"}})")));
    CHECK_EQ(r.motion.cols.count(ColumnMode::Sim), N_COLUMNS);
    {
        json::Value v;
        CHECK(json::parse(r.state(), v, nullptr));
        CHECK(v.get("motion")->get("simulated")->boolean);
        CHECK_EQ(static_cast<int>(v.get("motion")->get("sim_columns")->number), N_COLUMNS);
    }

    // Disabling a column excludes it from frames through the mode manager -
    // the API does not reach the scheduler itself.
    CHECK(is_ok(r.cmd(R"({"cmd":"motion.column","payload":{"column":4,"mode":"disabled"}})")));
    CHECK_EQ(static_cast<int>(r.mm.excluded()), 0b10000);
    CHECK(is_ok(r.cmd(R"({"cmd":"motion.column","payload":{"column":4,"mode":"real"}})")));
    CHECK_EQ(static_cast<int>(r.mm.excluded()), 0);

    // Fault injection only bites on a simulated column, and only for kinds
    // the drum model actually implements.
    CHECK(is_ok(r.cmd(R"({"cmd":"motion.sim_fault","payload":{"column":0,"kind":"slip","value":200}})")));
    CHECK(is_ok(r.cmd(R"({"cmd":"motion.sim_fault","payload":{"column":0,"kind":"miss","value":2}})")));
    CHECK(!is_ok(r.cmd(R"({"cmd":"motion.sim_fault","payload":{"column":0,"kind":"explode"}})")));
    CHECK(!is_ok(r.cmd(R"({"cmd":"motion.sim_fault","payload":{"column":0}})")));
    CHECK(is_ok(r.cmd(R"({"cmd":"motion.column","payload":{"column":0,"mode":"real"}})")));
    CHECK(!is_ok(r.cmd(R"({"cmd":"motion.sim_fault","payload":{"column":0,"kind":"slip"}})")));
}

void test_maintenance_command() {
    Rig r;
    const int homes_before = r.motion.homes;

    CHECK(is_ok(r.cmd(R"({"cmd":"motion.maintenance","payload":true})")));
    CHECK(r.mm.maintenance());
    CHECK(r.motion.cols.maintenance);
    CHECK_EQ(r.motion.homes, homes_before);  // entering never moves anything

    {
        json::Value v;
        CHECK(json::parse(r.state(), v, nullptr));
        CHECK(v.get("motion")->get("maintenance")->boolean);
    }

    // Manual commands still work while suspended - that is the point: driving
    // a suspect column by hand from the Calibrate page is the repair.
    CHECK(is_ok(r.cmd(R"({"cmd":"motion.cal","payload":{"column":1,"delta":10}})")));
    CHECK(is_ok(r.cmd(R"({"cmd":"motion.spin","payload":{"column":1,"flaps_s":8,"seconds":2}})")));

    // Leaving re-arms: everything re-homes, because the drums have been moved
    // by hand and nothing knows where they are any more.
    CHECK(is_ok(r.cmd(R"({"cmd":"motion.maintenance","payload":false})")));
    CHECK(!r.mm.maintenance());
    CHECK(r.motion.homes > homes_before);
    CHECK_EQ(r.motion.last_home_col, -1);  // all five
}


// --------------------------------------------------------------------------
// One command at a time, whatever the transport
// --------------------------------------------------------------------------
// handle_command is not atomic across its sub-interfaces: motion.column reads
// columns(), edits a copy, writes set_columns(), then tells the scheduler -
// three separate critical sections.  With only the HTTP task dispatching that
// never mattered.  Phase 4 adds MQTT and a terminal prop, so two concurrent
// callers become routine, and ModeManager's own witness cannot see this - it
// only ever watches ModeManager.
void test_dispatch_is_serialised() {
    Rig r;
    r.motion.widen_window.store(true, std::memory_order_relaxed);

    // Two transports racing to set ALL FIVE columns to their own mode.  Every
    // legal outcome is uniform; a mixture proves the read-modify-write tore.
    std::atomic<int> errors{0};
    auto driver = [&](const char* mode) {
        const std::string cmd =
            std::string(R"({"cmd":"motion.column","payload":{"all":true,"mode":")") + mode +
            R"("}})";
        for (int i = 0; i < 400; ++i) {
            if (!is_ok(api::handle_command(r.ctx, cmd, r.time.utc_ms))) ++errors;
            // Observed UNDER the dispatcher's own lock.  Reading it unlocked
            // is itself a race - the other thread is legitimately mid-write
            // inside its own critical section - and a torn read there says
            // nothing about whether the dispatcher serialised.  (Linux CI
            // caught exactly that; Windows happened not to.)
            ColumnConfig seen;
            {
                const std::lock_guard<std::mutex> lock(r.ctx.dispatch_mu);
                seen = r.motion.cols;
            }
            const ColumnMode first = seen.mode[0];
            for (int c = 1; c < N_COLUMNS; ++c) {
                if (seen.mode[static_cast<size_t>(c)] != first) ++errors;  // torn
            }
        }
    };
    std::thread a(driver, "real"), b(driver, "sim");
    a.join();
    b.join();
    CHECK_EQ(errors.load(), 0);

    // ... and the final state is one of the two, never a blend.
    const ColumnMode first = r.motion.cols.mode[0];
    CHECK(first == ColumnMode::Real || first == ColumnMode::Sim);
    for (int c = 1; c < N_COLUMNS; ++c) {
        CHECK(r.motion.cols.mode[static_cast<size_t>(c)] == first);
    }
}

// --------------------------------------------------------------------------
// The deadline says who set it and when (spec 7.3)
// --------------------------------------------------------------------------
void test_countdown_identity() {
    Rig r;
    auto cd = [&](const char* field) {
        json::Value v;
        CHECK(json::parse(r.state(), v, nullptr));
        const json::Value* c = v.get("cd");
        CHECK(c != nullptr);
        return c->get(field);
    };

    // A fresh device has set nobody's deadline.
    CHECK(cd("set_by")->as_str() == "unknown");
    const int64_t seq0 = cd("seq")->as_int(-1);
    CHECK(seq0 >= 0);

    // The web UI arms it.
    CHECK(is_ok(api::handle_command(
        r.ctx, R"({"cmd":"countdown.execute","payload":"4 8 15 16 23 42"})", r.time.utc_ms,
        Origin::Ui)));
    CHECK(cd("set_by")->as_str() == "ui");
    const int64_t seq1 = cd("seq")->as_int(-1);
    CHECK(seq1 > seq0);

    // The terminal prop resets it over MQTT: whoever set it LAST wins, and the
    // sequence has to move or a peer cannot tell which of two retained
    // documents is newer.
    const int64_t tgt = r.time.utc_ms / 1000 + 3000;
    CHECK(is_ok(api::handle_command(
        r.ctx, R"({"cmd":"countdown.set_target","payload":)" + std::to_string(tgt) + "}",
        r.time.utc_ms, Origin::Mqtt)));
    CHECK(cd("set_by")->as_str() == "mqtt");
    const int64_t seq2 = cd("seq")->as_int(-1);
    CHECK(seq2 > seq1);
    CHECK_EQ(cd("target")->as_int(0), tgt);

    // A cancel is a decision too - it must not leave the last setter's name on
    // a deadline they no longer own.
    CHECK(is_ok(api::handle_command(r.ctx, R"({"cmd":"countdown.cancel"})", r.time.utc_ms,
                                    Origin::Cli)));
    CHECK(cd("set_by")->as_str() == "cli");
    CHECK(cd("seq")->as_int(-1) > seq2);

    // An unnamed transport is "unknown", never a guess.
    CHECK(is_ok(api::handle_command(r.ctx, R"({"cmd":"countdown.start"})", r.time.utc_ms)));
    CHECK(cd("set_by")->as_str() == "unknown");

    // Round-trip of the names, since they go on the wire.
    for (Origin o : {Origin::Unknown, Origin::Ui, Origin::Mqtt, Origin::Cli, Origin::Button,
                     Origin::Ha}) {
        Origin back{};
        CHECK(origin_from_name(origin_name(o), back));
        CHECK(back == o);
    }
    Origin junk{};
    CHECK(!origin_from_name("prop", junk));
}

}  // namespace

// The Calibrate page's nudge is a MOVE.  It shipped returning ok while the
// drum stood still, because the web path adjusted the offset and - unlike the
// CLI - never re-seeked.  Both halves are asserted on EFFECT (a re-seek was
// issued) rather than on the return code, which is exactly the blind spot that
// let it through: the old code returned true in both cases below.
void test_a_nudge_moves_the_column_or_says_why_not() {
    Rig r;
    for (int i = 0; i < N_COLUMNS; ++i) r.motion.axes[static_cast<size_t>(i)].index = 7;
    r.motion.reseeks.clear();

    const std::string ok = r.cmd(R"({"cmd":"motion.cal","payload":{"column":1,"delta":10}})");
    CHECK(is_ok(ok));
    CHECK_STREQ(note_of(ok).c_str(), "");
    CHECK_EQ(r.motion.reseeks.size(), 1u);          // it MOVED
    CHECK_EQ(r.motion.reseeks[0].first, 1);
    CHECK_EQ(r.motion.reseeks[0].second, 7);        // back to the face it was showing
    CHECK_EQ(r.motion.axes[1].cal_offset, 10);

    // Not homed: the offset still applies, nothing moves, and the reply has to
    // say so - a nudge you cannot see is the whole failure here.
    r.motion.axes[2].index = -1;
    r.motion.reseeks.clear();
    const std::string note = r.cmd(R"({"cmd":"motion.cal","payload":{"column":2,"delta":-10}})");
    CHECK(is_ok(note));                             // it DID take effect
    CHECK(note_of(note).find("not homed") != std::string::npos);
    CHECK_EQ(r.motion.reseeks.size(), 0u);
    CHECK_EQ(r.motion.axes[2].cal_offset, -10);

    CHECK(!is_ok(r.cmd(R"({"cmd":"motion.cal","payload":{"column":9,"delta":1}})")));
}

// A partial mqtt.config used to clear the username and password on its way
// past - verified on hardware, `swanuser` became `(none)` after a payload that
// only changed the base topic.  The state document never exposes the password,
// so a Settings form cannot round-trip it and the firmware has to hold it.
void test_mqtt_partial_update_keeps_what_it_does_not_mention() {
    Rig r;
    CHECK(is_ok(r.cmd(R"({"cmd":"mqtt.config","payload":{"enabled":true,)"
                      R"("uri":"mqtt://10.0.0.5:1883","user":"swanuser",)"
                      R"("pass":"swanpass","base":"swan/"}})")));
    CHECK_STREQ(r.mqtt.last_user.c_str(), "swanuser");
    CHECK_STREQ(r.mqtt.last_pass.c_str(), "swanpass");

    CHECK(is_ok(r.cmd(R"({"cmd":"mqtt.config","payload":{"enabled":true,"base":"prop/"}})")));
    CHECK_STREQ(r.mqtt.last_user.c_str(), "swanuser");   // kept
    CHECK_STREQ(r.mqtt.last_pass.c_str(), "swanpass");   // kept
    CHECK_STREQ(r.mqtt.st.base.c_str(), "prop/");        // changed
    CHECK_STREQ(r.mqtt.st.uri.c_str(), "mqtt://10.0.0.5:1883");

    // Present-and-empty still CLEARS, so an anonymous broker is expressible.
    CHECK(is_ok(r.cmd(R"({"cmd":"mqtt.config","payload":{"enabled":true,"user":""}})")));
    CHECK_STREQ(r.mqtt.last_user.c_str(), "");

    // Turning it OFF must not require re-sending the broker being switched off.
    CHECK(is_ok(r.cmd(R"({"cmd":"mqtt.config","payload":{"enabled":false}})")));
    CHECK(!r.mqtt.st.enabled);
    CHECK_STREQ(r.mqtt.st.uri.c_str(), "mqtt://10.0.0.5:1883");   // still remembered

    // With nothing stored at all, enabling still needs one.
    Rig fresh;
    CHECK(!is_ok(fresh.cmd(R"({"cmd":"mqtt.config","payload":{"enabled":true}})")));
    CHECK_STREQ(err_of(fresh.cmd(R"({"cmd":"mqtt.config","payload":{"enabled":true}})")).c_str(),
                "need uri");
}

// Maintenance is the state in which ok lied hardest: mode.set / clock.format
// returned ok and never rendered, and message.set / preset.set / display.frame
// were worse - they call issue() directly, outside the gate, so they drove the
// drums while somebody had their hands in the mechanism (or pulsed STEP into
// de-energised drivers after a boot in maintenance, leaving the axis believing
// a position it never reached).
//
// Spec 5.9's split, asserted: manual driving works, display-driving is refused,
// and the deadline still arms because it is absolute.
void test_maintenance_refuses_display_commands_but_not_manual_ones() {
    Rig r;
    CHECK(is_ok(r.cmd(R"({"cmd":"motion.maintenance","payload":true})")));

    for (const char* body : {R"({"cmd":"mode.set","payload":"clock"})",
                             R"({"cmd":"preset.set","payload":"qmarks"})",
                             R"({"cmd":"display.frame","payload":{"indices":[1,1,1,1,1]}})",
                             R"({"cmd":"clock.format","payload":{"h24":true}})"}) {
        const std::string res = r.cmd(body);
        CHECK(!is_ok(res));
        CHECK(err_of(res).find("maintenance") != std::string::npos);
    }

    // Manual driving is the POINT of the mode - a suspect column is exercised
    // by hand from the Calibrate page.
    for (int i = 0; i < N_COLUMNS; ++i) r.motion.axes[static_cast<size_t>(i)].index = 3;
    r.motion.reseeks.clear();
    CHECK(is_ok(r.cmd(R"({"cmd":"motion.cal","payload":{"column":0,"delta":5}})")));
    CHECK_EQ(r.motion.reseeks.size(), 1u);
    CHECK(is_ok(r.cmd(R"({"cmd":"motion.ramp","payload":)"
                      R"({"column":0,"from":0,"to":4,"step":1,"dwell_s":1}})")));
    CHECK(is_ok(r.cmd(R"({"cmd":"motion.spin","payload":{"column":0,"seconds":1}})")));

    // The deadline is absolute: it arms, and the reply says it will not be seen.
    const std::string cd = r.cmd(R"({"cmd":"countdown.start"})");
    CHECK(is_ok(cd));
    CHECK(note_of(cd).find("maintenance") != std::string::npos);
    CHECK(r.mm.cd_phase() == CdPhase::Running);

    CHECK(is_ok(r.cmd(R"({"cmd":"motion.maintenance","payload":false})")));
    CHECK(is_ok(r.cmd(R"({"cmd":"mode.set","payload":"clock"})")));
}

// A disabled column is excused from everything (spec 5.9).  spin accepted one
// anyway and ran its full duration against a drum nobody drives, and rehome-all
// with every column disabled posted nothing and said ok.
void test_disabled_columns_are_refused_not_pretended() {
    Rig r;
    CHECK(is_ok(r.cmd(R"({"cmd":"motion.column","payload":{"column":2,"mode":"disabled"}})")));
    CHECK(!is_ok(r.cmd(R"({"cmd":"motion.spin","payload":{"column":2,"seconds":1}})")));
    CHECK(err_of(r.cmd(R"({"cmd":"motion.spin","payload":{"column":2,"seconds":1}})"))
              .find("disabled") != std::string::npos);
    CHECK(!is_ok(r.cmd(R"({"cmd":"motion.rehome","payload":{"column":2}})")));
    // The other four are untouched.
    CHECK(is_ok(r.cmd(R"({"cmd":"motion.spin","payload":{"column":1,"seconds":1}})")));

    CHECK(is_ok(r.cmd(R"({"cmd":"motion.column","payload":{"all":true,"mode":"disabled"}})")));
    const std::string all = r.cmd(R"({"cmd":"motion.rehome"})");
    CHECK(!is_ok(all));
    CHECK(err_of(all).find("every column") != std::string::npos);
}

void run_tests() {
    test_command_round_trip();
    test_reveal_by_name();
    test_state_payload();
    test_cal_ramp();
    test_upload_validator();
    test_ring_swap_vs_readers();
    test_column_commands();
    test_maintenance_command();
    test_dispatch_is_serialised();
    test_countdown_identity();
    test_no_unlocked_mode_access();
    test_a_nudge_moves_the_column_or_says_why_not();
    test_mqtt_partial_update_keeps_what_it_does_not_mention();
    test_maintenance_refuses_display_commands_but_not_manual_ones();
    test_disabled_columns_are_refused_not_pretended();
}
