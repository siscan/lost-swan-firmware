// The web API, pure (spec 10.2 / 10.2a).
//
// Everything the /ws socket and the /api/ REST routes do is here: build the
// state payload, parse a command, dispatch it.  No IDF includes, so the host
// dev server and esp_http_server run the SAME code and the host tests cover
// the real thing.
//
// THERE IS ONE DISPATCHER.  Web, MQTT, CLI, button and HA all funnel into
// api::handle_command(); nothing may reach ModeManager or motion by another
// route.  Everything that mutates display state goes through ModeManager,
// whose public methods take its mutex - the HTTP task therefore never touches
// mode state unlocked (asserted at runtime by test_api's concurrency case).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "modes/mode_manager.h"
#include "motion/motion_types.h"
#include "ring/ring_runtime.h"

namespace swan {
namespace api {

// What the API needs from motion beyond what ModeManager owns.  An interface
// so the host tests and the dev server can drive simulated axes.
class MotionAdmin {
public:
    virtual ~MotionAdmin() = default;
    virtual AxisInfo info(int col) = 0;
    virtual MotionParams params() = 0;
    // Live apply, no persistence - the UI sliders use this.
    virtual void set_params(const MotionParams& p) = 0;
    virtual bool home(int col) = 0;  // col < 0 = all
    virtual bool adjust_cal(int col, int32_t delta) = 0;
    // Bench test spin (spec 10.2 Calibrate, spec 13 `spin`).  Open loop: the
    // displayed index becomes unknown until the active mode moves the column
    // back to the current frame.
    virtual bool spin_open_loop(int col, int32_t flaps_s, int seconds) = 0;
};

// Persistence, deliberately separate from live apply (the Calibrate page has
// distinct "apply" and "save" affordances).
class ConfigSink {
public:
    virtual ~ConfigSink() = default;
    virtual bool save_motion(const MotionParams& p) = 0;
    virtual bool save_app(const ModesConfig& m, std::string_view tz) = 0;
};

// Link/diagnostic facts the UI shows.  Faked on the host.
struct SysInfo {
    std::string wifi_state = "disabled";  // disabled|connecting|connected|failed
    std::string ssid;
    std::string ip;
    std::string hostname = "lost";
    int rssi = 0;
    uint32_t heap = 0;
    uint32_t uptime_s = 0;
    std::string reset_reason = "unknown";
    std::string version = "dev";
};

class SysInfoSource {
public:
    virtual ~SysInfoSource() = default;
    virtual SysInfo get() = 0;
};

// A ring upload staged by the HTTP task.  Validation happens entirely on this
// copy; the running table is untouched until the modes task applies it.
class RingStaging {
public:
    virtual ~RingStaging() = default;
    // Validate `body` and, if good, stage it and queue the swap.  Returns
    // false with *err set otherwise, leaving the running table alone.
    virtual bool stage(std::string_view body, std::string* err) = 0;
};

// Target-only operations.  An interface so the host dev server can record a
// reboot request instead of dying.
class SystemOps {
public:
    virtual ~SystemOps() = default;
    virtual bool reboot() = 0;
};

struct Context {
    ModeManager& modes;
    const RingSet& ring;
    MotionAdmin& motion;
    ConfigSink& cfg;
    SysInfoSource& sys;
    RingStaging& ring_upload;
    SystemOps& ops;
    std::string tz;  // mirror of time.tz for reporting; ModeManager owns parsing
};

// The full state document pushed on /ws (on change and at 1 Hz) and returned
// by GET /api/state.
std::string build_state(Context& ctx, int64_t utc_ms);

// The ring document for the pickers: per-column glyph lists, so the reveal
// picker can offer only what a column can actually show (column 5's set
// differs).  Larger and static, so it is a separate route rather than part of
// the 1 Hz payload.
std::string build_ring_doc(const RingSet& ring);

// Flap wear, measured by walking the REAL renderers over a whole day and a
// whole countdown run (spec 7.1 wear table, 7.3).  Every valid granularity and
// all three seconds modes in one document: the Settings page needs an exact
// figure the instant a control moves, and a lookup table in JavaScript would
// drift from the renderer the first time either changed.
std::string build_wear_doc(const RingSet& ring, bool h24, int seconds_live_s);

// Parse and dispatch one command.  `body` is {"cmd":"...","payload":...}.
// Always returns a JSON result: {"ok":true} or {"ok":false,"err":"..."}.
std::string handle_command(Context& ctx, std::string_view body, int64_t utc_ms);

// Upload limits (spec 4: the drum is physical, the table is small).
inline constexpr size_t RING_UPLOAD_MAX = 64 * 1024;

}  // namespace api
}  // namespace swan
