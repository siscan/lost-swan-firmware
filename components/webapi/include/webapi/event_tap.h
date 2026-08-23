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

}  // namespace api
}  // namespace swan
