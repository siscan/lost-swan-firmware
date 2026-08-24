// Motion and cue taps that turn what the scheduler actually did into the
// /ws event stream (spec 10.2).
//
// The events are the same shape the simulator's recorded traces use -
// {"e":"go"}, {"e":"spin"}, {"e":"cue"} - so web/flap.js renders a live
// display and a replayed trace with one code path.
//
// Pure: the sink is a callback, so the host dev server pushes into its own
// broadcaster and the firmware pushes into esp_http_server's WebSocket fds.
// A tap only observes; it never changes what the inner port returns.
#pragma once

#include <functional>
#include <string>
#include <utility>

#include "frame/frame.h"
#include "modes/mode_manager.h"
#include "ring/json_write.h"

namespace swan {
namespace api {

using EventSink = std::function<void(const std::string&)>;

class EventTapPort final : public MotionPort {
public:
    EventTapPort(MotionPort& inner, EventSink sink)
        : inner_(inner), sink_(std::move(sink)) {}

    // The flap rate the animation should use.  The scheduler owns the real
    // one; this is told about it so the browser animates at the right speed.
    void set_flaps(int32_t f) { flaps_ = f; }

    Col col(int i) override { return inner_.col(i); }

    bool go(int i, int index) override {
        const bool ok = inner_.go(i, index);
        if (ok && sink_) {
            json::Writer w;
            w.obj().kv("e", "go").kv("col", i).kv("idx", index).kv("flaps", flaps_).end_obj();
            sink_(w.take());
        }
        return ok;
    }

    bool spin(int i, int32_t flaps_s, int seconds) override {
        const bool ok = inner_.spin(i, flaps_s, seconds);
        if (ok && sink_) {
            json::Writer w;
            w.obj().kv("e", "spin").kv("col", i).kv("flaps", flaps_s).kv("secs", seconds)
                .end_obj();
            sink_(w.take());
        }
        return ok;
    }

private:
    MotionPort& inner_;
    EventSink sink_;
    int32_t flaps_ = 15;
};

inline const char* cue_name(Cue c) {
    switch (c) {
        case Cue::Warn4Min:      return "warn_4min";
        case Cue::Warn1Min:      return "warn_1min";
        case Cue::SystemFailure: return "system_failure";
    }
    return "?";
}

// Forwards a cue to the real sink (audio in phase 5, a log line before that)
// AND onto /ws, so the UI can show it.
class EventTapCues final : public CueSink {
public:
    EventTapCues(CueSink& inner, EventSink sink) : inner_(inner), sink_(std::move(sink)) {}

    void on_cue(Cue c) override {
        inner_.on_cue(c);
        if (sink_) {
            json::Writer w;
            w.obj().kv("e", "cue").kv("name", cue_name(c)).end_obj();
            sink_(w.take());
        }
    }

private:
    CueSink& inner_;
    EventSink sink_;
};

// The mode-change event, emitted by whoever ticks the modes.
inline std::string mode_event(Mode m) {
    json::Writer w;
    w.obj().kv("e", "mode").kv("name", mode_name(m)).end_obj();
    return w.take();
}

// THE REVEAL LANDED - every column confirmed on the reveal frame after the
// alarm spin.  A CROSS-REPO CONTRACT as of 2026-08-24: the terminal prop keys
// its SYSTEM_FAILURE beat off this, and it goes to BOTH /ws and swan/event.
//
// Two shapes now share swan/event and a consumer tells them apart by which key
// is present: a command RESULT carries "cmd" (and "res"), an announcement
// carries "e".  This is the first announcement; it is deliberately spelled the
// same way as the /ws vocabulary ("go", "spin", "cue", "mode") so one switch
// serves both transports.
//
// `seq` is the countdown's, so a peer can tell which run landed - the same
// number swan/countdown carries.
inline std::string reveal_event(uint32_t seq, int64_t utc_s) {
    json::Writer w;
    w.obj()
        .kv("e", "reveal")
        .kv("seq", static_cast<int64_t>(seq))
        .kv("t", utc_s)
        .end_obj();
    return w.take();
}

}  // namespace api
}  // namespace swan
