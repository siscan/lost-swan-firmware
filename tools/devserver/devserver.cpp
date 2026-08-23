// Host dev server (spec 15 phase 3, step a).
//
// Serves web/ and speaks the REAL /ws protocol against the REAL ModeManager,
// FrameScheduler and webapi dispatcher, over five simulated axes running the
// real control core.  The page it serves is the same page LittleFS will hold,
// so the UI and the firmware logic are one codebase and the UI is clickable
// months before a board exists.
//
//   build_host/devserver [--port 8080] [--root web] [--ring data/ring.json]
//                        [--tz PST8PDT,M3.2.0,M11.1.0]
//
// Not firmware: esp_http_server serves the same routes on target.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "httpd.h"
#include "ring/json_lite.h"
#include "ring/json_write.h"
#include "sim_columns.h"
#include "webapi/api.h"
#include "webapi/ring_upload.h"

using namespace swan;
using namespace swan::devserver;

namespace {

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// --------------------------------------------------------------------------
// The bits of the device the API needs that the simulation does not provide.
// --------------------------------------------------------------------------
struct RealTime final : TimeSource {
    int64_t now_utc() override { return now_ms() / 1000; }
    bool valid() override { return true; }  // the host clock is always set
};

// A countdown store in memory: the dev server is not meant to survive a
// restart, and writing NVS-shaped state into the repo would be noise.
struct MemStore final : CountdownStore {
    CdPersist s{};
    bool have = false;
    bool load(CdPersist& out) override {
        if (!have) return false;
        out = s;
        return true;
    }
    void save(const CdPersist& v) override {
        s = v;
        have = true;
    }
};

// The dev server has no broker.  Reporting "configured but never connected"
// is the honest answer and keeps the Settings page exercisable.
struct DevMqtt final : api::MqttAdmin {
    api::MqttStatus st;
    api::MqttStatus mqtt_status() override { return st; }
    bool mqtt_configure(bool enabled, std::string_view uri, std::string_view,
                        std::string_view, std::string_view base, std::string_view) override {
        st.enabled = enabled;
        st.uri = std::string(uri);
        if (!base.empty()) st.base = std::string(base);
        std::printf("mqtt.config enabled=%d uri=%s\n", enabled ? 1 : 0, st.uri.c_str());
        return true;
    }
};

struct DevWifi final : api::WifiAdmin {
    bool portal = false;
    std::string ssid;
    bool set_credentials(std::string_view s, std::string_view) override {
        ssid = std::string(s);
        std::printf("[dev] wifi.credentials ssid=%s\n", ssid.c_str());
        return true;
    }
    bool start_portal() override { portal = true; return true; }
    bool stop_portal() override { portal = false; return true; }
    bool portal_running() override { return portal; }
    std::string portal_ssid() override { return "LOST-Swan-dev"; }
    bool have_credentials() override { return !ssid.empty(); }
};

struct DevOps final : api::SystemOps {
    std::atomic<int> reboots{0};
    bool reboot() override {
        ++reboots;
        std::printf("[dev] system.reboot requested (ignored on the host)\n");
        return true;
    }
};

struct DevSys final : api::SysInfoSource {
    api::SysInfo s;
    int64_t t0 = now_ms();
    api::SysInfo get() override {
        s.uptime_s = static_cast<uint32_t>((now_ms() - t0) / 1000);
        return s;
    }
};

// The dev server does not write NVS; it reports the save so the UI's
// "saved" affordance is exercised end to end.
struct DevCfgSink final : api::ConfigSink {
    std::atomic<int> motion_saves{0};
    std::atomic<int> app_saves{0};
    bool save_motion(const MotionParams&) override {
        ++motion_saves;
        std::printf("[dev] motion params saved (host: not persisted)\n");
        return true;
    }
    bool save_app(const ModesConfig&, std::string_view tz) override {
        ++app_saves;
        std::printf("[dev] app config saved, tz=%.*s (host: not persisted)\n",
                    static_cast<int>(tz.size()), tz.data());
        return true;
    }
};

// --------------------------------------------------------------------------
// Motion event tap.  Wraps the port so every go/spin the scheduler issues is
// pushed on /ws in the shape the simulator page already renders.
// --------------------------------------------------------------------------
class Broadcaster {
public:
    void attach(Server* s) { srv_ = s; }
    void push(const std::string& msg) {
        if (srv_ != nullptr) srv_->broadcast(msg);
    }

private:
    Server* srv_ = nullptr;
};

class TapPort final : public MotionPort {
public:
    TapPort(SimColumns& inner, Broadcaster& out) : inner_(inner), out_(out) {}

    void set_flaps(int32_t f) { flaps_ = f; }

    Col col(int i) override { return inner_.col(i); }

    bool go(int i, int index) override {
        const bool ok = inner_.go(i, index);
        if (ok) {
            json::Writer w;
            w.obj().kv("e", "go").kv("col", i).kv("idx", index).kv("flaps", flaps_).end_obj();
            out_.push(w.take());
        }
        return ok;
    }

    bool spin(int i, int32_t flaps_s, int seconds) override {
        const bool ok = inner_.spin(i, flaps_s, seconds);
        if (ok) {
            json::Writer w;
            w.obj().kv("e", "spin").kv("col", i).kv("flaps", flaps_s).kv("secs", seconds)
                .end_obj();
            out_.push(w.take());
        }
        return ok;
    }

private:
    SimColumns& inner_;
    Broadcaster& out_;
    int32_t flaps_ = 15;
};

class TapCues final : public CueSink {
public:
    explicit TapCues(Broadcaster& out) : out_(out) {}
    void on_cue(Cue c) override {
        const char* n = c == Cue::Warn4Min   ? "warn_4min"
                        : c == Cue::Warn1Min ? "warn_1min"
                                             : "system_failure";
        json::Writer w;
        w.obj().kv("e", "cue").kv("name", n).end_obj();
        out_.push(w.take());
        std::printf("[dev] cue %s\n", n);
    }

private:
    Broadcaster& out_;
};

// --------------------------------------------------------------------------
// Static files
// --------------------------------------------------------------------------
bool path_is_safe(const std::string& p) {
    if (p.find("..") != std::string::npos) return false;
    for (const char c : p) {
        if (c == '\\' || c == ':' || c == '\0') return false;
    }
    return true;
}

HttpResponse serve_static(const std::string& root, const HttpRequest& req) {
    std::string rel = req.path;
    if (rel.empty() || rel.back() == '/') rel += "index.html";
    if (!path_is_safe(rel)) return HttpResponse::text(400, "bad path");
    const std::string full = root + rel;

    std::string body;
    // Prefer the gzipped asset when the client takes it - the same file
    // LittleFS will hold (spec 15 phase 3, step e).
    if (req.accepts_gzip() && read_file(full + ".gz", body)) {
        HttpResponse r;
        r.content_type = content_type_for(full);
        r.body = std::move(body);
        r.extra.emplace_back("Content-Encoding", "gzip");
        r.extra.emplace_back("Cache-Control", "no-cache");
        return r;
    }
    if (read_file(full, body)) {
        HttpResponse r;
        r.content_type = content_type_for(full);
        r.body = std::move(body);
        r.extra.emplace_back("Cache-Control", "no-cache");
        return r;
    }
    return HttpResponse::text(404, "not found: " + rel);
}

std::string arg_after(int argc, char** argv, const char* flag, const std::string& dflt) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) return argv[i + 1];
    }
    return dflt;
}

}  // namespace

int main(int argc, char** argv) {
    // Unbuffered: the banner and the [dev] lines must show up even when
    // stdout is a pipe or a redirect, and UCRT treats _IOLBF as full
    // buffering, so line buffering would not do it.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const int http_port = std::atoi(arg_after(argc, argv, "--port", "8080").c_str());
    const std::string root = arg_after(argc, argv, "--root", "web");
    const std::string ring_path = arg_after(argc, argv, "--ring", "data/ring.json");
    const std::string tz = arg_after(argc, argv, "--tz", "PST8PDT,M3.2.0,M11.1.0");

    // --- the device ---
    RingSet ring = RingSet::compiled_fallback();
    std::string ring_text;
    if (read_file(ring_path, ring_text)) {
        std::string err;
        if (ring.load_json(ring_text, &err)) {
            std::printf("ring   : %s\n", ring_path.c_str());
        } else {
            std::printf("ring   : %s REJECTED (%s) - using the compiled fallback\n",
                        ring_path.c_str(), err.c_str());
        }
    } else {
        std::printf("ring   : compiled fallback (%s not found)\n", ring_path.c_str());
    }

    Broadcaster bcast;
    SimColumns sim;
    TapPort port(sim, bcast);
    FrameScheduler sched(port, {15, 82000});
    RealTime clock_src;
    MemStore store;
    TapCues cues(bcast);
    ModeManager modes(ring, sched, clock_src, store, cues);
    api::RingStager stager(ring);
    DevCfgSink cfg_sink;
    DevSys sysinfo;
    DevOps ops;
    DevMqtt mqtt;
    DevWifi wifi;
    // The stager is BOTH the pinned ring source and the upload sink.
    api::Context ctx{modes, stager, sim, cfg_sink, sysinfo, stager, ops, mqtt, wifi, {}};

    sysinfo.s.wifi_state = "connected";
    sysinfo.s.ssid = "host-dev-server";
    sysinfo.s.ip = "127.0.0.1";
    sysinfo.s.version = "dev";
    sysinfo.s.reset_reason = "poweron";

    ModesConfig mcfg;
    modes.set_config(mcfg);
    if (!modes.set_tz(tz)) {
        std::printf("tz     : %s REJECTED - staying on UTC\n", tz.c_str());
    }

    // One device mutex around the simulation AND every HTTP-side call, so the
    // lock order is always device -> ModeManager and SimColumns needs no lock
    // of its own.  This is the host stand-in for the target's task split.
    std::mutex dev_mu;

    // Boot exactly as the firmware does: home first, THEN start the modes.
    // Not both at once - the per-axis mailbox is single-slot and replace-on-
    // write, so a frame issued before the home request drains would silently
    // supersede it (spec 6 semantics, and the reason main() waits on target).
    {
        const std::lock_guard<std::mutex> lock(dev_mu);
        sim.home_all();
    }

    // --- the server ---
    Server server;
    bcast.attach(&server);
    std::string err;
    if (!server.listen(http_port, &err)) {
        std::printf("listen failed: %s\n", err.c_str());
        return 1;
    }

    auto state_now = [&] {
        const std::lock_guard<std::mutex> lock(dev_mu);
        return api::build_state(ctx, now_ms());
    };

    auto run_command = [&](const std::string& body) {
        const std::lock_guard<std::mutex> lock(dev_mu);
        return api::handle_command(ctx, body, now_ms());
    };

    server.on_http = [&](const HttpRequest& req) -> HttpResponse {
        if (req.path == "/api/state") return HttpResponse::json(state_now());
        if (req.path == "/api/ring") {
            return HttpResponse::json(api::build_ring_doc(stager.snapshot()));
        }
        if (req.path == "/api/wear") {
            const ModesConfig mc = modes.config();
            return HttpResponse::json(
                api::build_wear_doc(stager.snapshot(), mc.h24, mc.seconds_live_s));
        }
        if (req.path == "/api/cmd") {
            if (req.method != "POST") return HttpResponse::text(405, "POST only");
            return HttpResponse::json(run_command(req.body));
        }
        if (req.path == "/api/ring/upload") {
            if (req.method != "POST") return HttpResponse::text(405, "POST only");
            // The upload arrives as the raw body rather than wrapped in JSON:
            // the validator is the same one either way, and the target's
            // handler streams the body straight into the staging buffer.
            // No device mutex: staging is precisely the work the HTTP task
            // is allowed to do alone - it validates into a throwaway table
            // and never touches the live one.
            std::string uerr;
            if (!stager.stage(req.body, &uerr)) {
                json::Writer w;
                w.obj().kv("ok", false).kv("err", uerr).end_obj();
                return HttpResponse::json(w.take());
            }
            return HttpResponse::json(R"({"ok":true})");
        }
        if (req.path.rfind("/api/", 0) == 0) return HttpResponse::text(404, "no such route");
        return serve_static(root, req);
    };

    server.on_ws_open = [&](WsConn& c) {
        c.send(state_now());
        json::Writer w;
        w.obj().kv("e", "mode").kv("name", mode_name(modes.mode())).end_obj();
        c.send(w.take());
    };

    server.on_ws_message = [&](WsConn& c, const std::string& msg) {
        const std::string res = run_command(msg);
        // Echo the caller's id back so a page can match request to result.
        json::Value doc;
        int64_t id = 0;
        if (json::parse(msg, doc, nullptr) && doc.get("id")) id = doc.get("id")->as_int(0);
        json::Writer w;
        w.obj().kv("e", "result").kv("id", id).kv_raw("res", res).end_obj();
        c.send(w.take());
        bcast.push(state_now());
    };

    std::printf("\n  LOST Swan dev server\n");
    std::printf("  http://localhost:%d/        (UI, from %s/)\n", http_port, root.c_str());
    std::printf("  ws://localhost:%d/ws        (state push + go/spin/cue events)\n", http_port);
    std::printf("  GET  /api/state  /api/ring  /api/wear\n");
    std::printf("  POST /api/cmd  /api/ring/upload\n\n");

    // --- the device thread: 25 ms simulation + mode tick, 1 Hz heartbeat ---
    std::atomic<bool> stop{false};
    std::thread device([&] {
        constexpr int64_t kStepMs = 25;
        int64_t last_state = 0;
        Mode last_mode = modes.mode();
        std::string last_payload;
        bool begun = false;
        while (!stop.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kStepMs));
            const int64_t t = now_ms();
            std::string payload;
            bool mode_changed = false;
            Mode m;
            {
                const std::lock_guard<std::mutex> lock(dev_mu);
                sim.advance(kStepMs);
                if (!begun) {
                    if (!sim.all_homed()) {
                        continue;  // watch the drums find home first
                    }
                    begun = true;
                    modes.begin(t);
                    std::printf("[dev] all five columns homed; modes running\n");
                }
                // The staged ring swap belongs to the modes context, never to
                // the HTTP task (ring_store.h contract).
                if (stager.pending()) {
                    bool applied = false;
                    modes.cmd_ring_swap([&] { return applied = stager.apply_pending(); }, t);
                    if (applied) {
                        std::printf("[dev] ring.json applied by the modes task\n");
                        (void)stager.take_accepted_body();
                    }
                }
                const MotionParams mp = sim.params();
                port.set_flaps(mp.flaps_s_normal);
                sched.set_timing({mp.flaps_s_normal, mp.accel});
                modes.tick(t);
                m = modes.mode();
                mode_changed = (m != last_mode);
                payload = api::build_state(ctx, t);
            }
            if (mode_changed) {
                last_mode = m;
                json::Writer w;
                w.obj().kv("e", "mode").kv("name", mode_name(m)).end_obj();
                bcast.push(w.take());
            }
            // On change, plus a 1 Hz heartbeat (spec 10.2).  The timestamp
            // changes every tick, so the comparison starts after it.  Changes
            // are rate-limited: go/spin/cue/mode already carry the animation,
            // and a 1.5 KB document at the tick rate is bandwidth the target
            // has better uses for.
            // Display state only, from "mode" up to "sys": heap and uptime
            // jitter every tick and would keep "changed" permanently true.
            const size_t k = payload.find("\"mode\"");
            const size_t e = payload.find(",\"sys\":");
            std::string tail = (k == std::string::npos)
                                   ? payload
                                   : payload.substr(k, e == std::string::npos
                                                           ? std::string::npos : e - k);
            const bool changed = (tail != last_payload) && (t - last_state >= 200);
            if (changed || t - last_state >= 1000) {
                last_state = t;
                last_payload = std::move(tail);
                bcast.push(payload);
            }
        }
    });

    server.run();
    stop.store(true, std::memory_order_relaxed);
    device.join();
    return 0;
}
