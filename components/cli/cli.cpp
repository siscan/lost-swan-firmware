#include "cli/cli.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "audio/player.h"
#include "config/config.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_app_desc.h"
#include "net/wifi.h"
#include "net/mqtt.h"
#include "net/ota.h"
#include "webapi/mqtt_bridge.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "hal/gpio_bank.h"
#include "hal/pins.h"
#include "modes/mode_manager.h"
#include "motion/motion.h"
#include "motion/soak.h"
#include "ring/ring.h"
#include "ring/ring_store.h"

namespace swan {
namespace cli {
namespace {

ModeManager* g_mm = nullptr;
int64_t (*g_utc_ms)() = nullptr;
const api::RingSource* g_ring = nullptr;

bool modes_ready() {
    if (g_mm == nullptr || g_utc_ms == nullptr) {
        std::printf("modes not available\n");
        return false;
    }
    return true;
}

int print_result(ModeManager::Result r) {
    std::printf("%s\n", r.ok ? "ok" : r.err);
    return r.ok ? 0 : 1;
}

bool parse_long(const char* s, long& out) {
    char* end = nullptr;
    const long v = std::strtol(s, &end, 0);
    if (end == s || *end != '\0') return false;
    out = v;
    return true;
}

// Accepts a column number, or "all" / "-1" for every column.
bool parse_col(const char* s, int& out, bool allow_all) {
    if (allow_all && std::strcmp(s, "all") == 0) {
        out = -1;
        return true;
    }
    long v;
    if (!parse_long(s, v)) return false;
    if (allow_all && v < 0) {
        out = -1;
        return true;
    }
    if (v < 0 || v >= N_COLUMNS) return false;
    out = static_cast<int>(v);
    return true;
}

// A pinned copy, always.  Before bind_ring (boot only, single-threaded) fall
// back to the live table.
RingSet ring_now() {
    return g_ring != nullptr ? g_ring->snapshot() : ring_store::get();
}

int cmd_pins(int, char**) {
    std::printf("board: %s\n", BOARD_NAME);
    for (int i = 0; i < N_COLUMNS; ++i) {
        std::printf("  col %d  STEP=GPIO%-2d  HALL=GPIO%-2d\n", i, PIN_STEP[i], PIN_HALL[i]);
    }
    std::printf("  EN=GPIO%d (ganged, active low)   DIR: tied at the drivers, no GPIO\n", PIN_EN);
    std::printf("  I2S BCLK=GPIO%d LRCLK=GPIO%d DIN=GPIO%d\n", PIN_I2S_BCLK, PIN_I2S_LRCLK,
                PIN_I2S_DIN);
    std::printf("  BUTTON=GPIO%d  LED=GPIO%d (%s)\n", PIN_BUTTON, PIN_LED,
                LED_IS_RGB ? "WS2812" : "single");
    std::printf("  usteps/flap = %lld/%lld = %.4f   usteps/rev = %lld/%lld\n",
                static_cast<long long>(USTEPS_PER_FLAP_NUM),
                static_cast<long long>(USTEPS_PER_FLAP_DEN),
                static_cast<double>(USTEPS_PER_FLAP_NUM) / USTEPS_PER_FLAP_DEN,
                static_cast<long long>(USTEPS_PER_SPOOL_REV_NUM),
                static_cast<long long>(USTEPS_PER_SPOOL_REV_DEN));
    return 0;
}

int cmd_hall(int, char**) {
    // Raw pin level and the debounced, polarity-corrected view, so bench step 2
    // can tell a wiring problem from a polarity problem.
    const uint32_t raw = gpio_bank_read();
    const MotionParams p = motion::params();
    std::printf("hall_active_low = %s\n", p.hall_active_low ? "true" : "false");
    for (int i = 0; i < N_COLUMNS; ++i) {
        AxisInfo a;
        motion::info(i, a);
        const bool pin_high = (raw & pin_mask(PIN_HALL[i])) != 0;
        std::printf("  col %d  GPIO%-2d raw=%d  magnet=%s\n", i, PIN_HALL[i], pin_high ? 1 : 0,
                    a.hall_level ? "YES" : "no");
    }
    return 0;
}

// Bench tooling, exactly like `hall`: the button is on the BOOT pin and there is
// no other way to tell "not wired" from "wired and not pressed" - both read
// high.  Watching for a few seconds is how you check a panel button and its loom
// before deciding the firmware is at fault.
int cmd_button(int argc, char** argv) {
    const int secs = argc > 1 ? std::atoi(argv[1]) : 0;
    const auto level = [] { return (gpio_bank_read() & pin_mask(PIN_BUTTON)) == 0; };
    std::printf("BUTTON=GPIO%d (BOOT, active low, strapping - spec 2.5)\n", PIN_BUTTON);
    std::printf("  now: %s\n", level() ? "PRESSED" : "released");
    if (secs <= 0) {
        std::printf("  `button <seconds>` watches for edges\n");
        return 0;
    }
    std::printf("  watching %d s - press it\n", secs);
    bool last = level();
    int edges = 0;
    const int64_t until = esp_timer_get_time() + static_cast<int64_t>(secs) * 1000000;
    int64_t since = esp_timer_get_time();
    while (esp_timer_get_time() < until) {
        vTaskDelay(pdMS_TO_TICKS(10));
        const bool now = level();
        if (now == last) continue;
        const int64_t t = esp_timer_get_time();
        std::printf("  %8.3f s  -> %s (previous state held %.3f s)\n",
                    static_cast<double>(t) / 1e6, now ? "PRESSED" : "released",
                    static_cast<double>(t - since) / 1e6);
        since = t;
        last = now;
        ++edges;
    }
    std::printf("  %d edge%s. A clean switch gives two per press; many more is bounce the\n"
                "  40 ms debounce should still absorb, or loom pickup, which it will not.\n",
                edges, edges == 1 ? "" : "s");
    return 0;
}

int cmd_en(int argc, char** argv) {
    if (argc != 2) {
        std::printf("usage: en 0|1\n");
        return 1;
    }
    long v;
    if (!parse_long(argv[1], v)) return 1;
    motion::enable(v != 0);
    std::printf("drivers %s\n", motion::is_enabled() ? "ENABLED" : "disabled");
    return 0;
}

// Bench step 3.  The drum must turn in the DESCENDING sense - one forward flip
// DECREMENTS the displayed digit (spec 4) - and with the motor now inside the
// drum facing the other way, which level does that is not knowable on paper.
// Type `dir`, watch a flip, type `dir 1` if it went the wrong way, then `save`.
int cmd_dir(int argc, char** argv) {
    if (!swan::HAS_DIR_GPIO) {
        std::printf("this board has no DIR GPIO; DIR is tied at the drivers\n");
        return 1;
    }
    motion::MotionParams p = motion::params();
    if (argc == 1) {
        std::printf("dir_invert = %d  (GPIO%d, ganged across all five drivers)\n",
                    p.dir_invert ? 1 : 0, swan::PIN_DIR);
        std::printf("  one forward flip must DECREMENT the digit - spec 4\n");
        std::printf("  `save` persists it with the calibration\n");
        return 0;
    }
    if (argc != 2) {
        std::printf("usage: dir [0|1]\n");
        return 1;
    }
    long v;
    if (!parse_long(argv[1], v)) return 1;
    if (!motion::all_idle()) {
        std::printf("a column is moving; reversing DIR mid-move would walk it backwards\n");
        return 1;
    }
    p.dir_invert = (v != 0);
    motion::set_params(p);
    std::printf("dir_invert = %d\n", p.dir_invert ? 1 : 0);
    return 0;
}

int cmd_step(int argc, char** argv) {
    if (argc != 3) {
        std::printf("usage: step <col> <usteps>    (open loop; leaves the index unknown)\n");
        return 1;
    }
    int col;
    long n;
    if (!parse_col(argv[1], col, false) || !parse_long(argv[2], n)) return 1;
    const MotionParams p = motion::params();
    const esp_err_t err = motion::step_open_loop(col, n, p.flaps_s_home);
    std::printf("%s\n", err == ESP_OK ? "ok" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

int cmd_home(int argc, char** argv) {
    int col = -1;
    if (argc == 2 && !parse_col(argv[1], col, true)) return 1;
    const esp_err_t err = motion::home(col);
    std::printf("%s\n", err == ESP_OK ? "homing" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

int cmd_go(int argc, char** argv) {
    if (argc != 3) {
        std::printf("usage: go <col> <index|token>\n");
        return 1;
    }
    int col;
    if (!parse_col(argv[1], col, false)) return 1;

    // The RUNTIME table for this column (ring.json when loaded, else the
    // compiled fallback) - never the compiled constants directly.  Resolved
    // from where the column is now: column 5 has two slots per digit and the
    // nearest one going forward wins.
    const RingSet ring = ring_now();
    const RingTable& table = ring.col(col);
    AxisInfo cur;
    motion::info(col, cur);
    int index = table.index_for_token(argv[2], cur.index);
    if (index < 0) {
        long v;
        if (!parse_long(argv[2], v) || v < 0 || v >= table.slot_count()) {
            std::printf("no ring slot named '%s'\n", argv[2]);
            return 1;
        }
        index = static_cast<int>(v);
    }

    const esp_err_t err = motion::go(col, index);
    if (err != ESP_OK) {
        std::printf("%s\n", esp_err_to_name(err));
        return 1;
    }
    std::printf("col %d -> %d (%s)\n", col, index, table.slot(index).label.c_str());
    return 0;
}

int cmd_spin(int argc, char** argv) {
    if (argc != 4) {
        std::printf("usage: spin <col> <flaps_s> <seconds>\n");
        return 1;
    }
    int col;
    long fs, secs;
    if (!parse_col(argv[1], col, false) || !parse_long(argv[2], fs) || !parse_long(argv[3], secs)) {
        return 1;
    }
    const int64_t usteps = (static_cast<int64_t>(fs) * secs * USTEPS_PER_FLAP_NUM) /
                           USTEPS_PER_FLAP_DEN;
    const esp_err_t err = motion::step_open_loop(col, usteps, static_cast<int32_t>(fs));
    std::printf("%s: %lld usteps at %ld flaps/s\n", err == ESP_OK ? "spinning" : "failed",
                static_cast<long long>(usteps), fs);
    return err == ESP_OK ? 0 : 1;
}

// Bench step 4: measure hall_to_hall over n revolutions.  THIS IS THE COMMAND
// THAT IDENTIFIES WHICH MACHINE GOT BUILT, and the numbers are far apart
// enough that it cannot be misread (geometry.h carries the pedigree):
//
//     3200 exactly  ->  the 1:1 direct drive.  What is being built.
//     ~8242         ->  an 85T/33T rim-gear drum.  The original bridge.
//     ~7555         ->  an 85T/36T rim-gear drum.  Designed, never built.
//     ~8369         ->  68T/26T.  The stale MECHANICAL_README prose.
//
// A direct-drive drum reads 3200 with no spread at all - there is no residue
// to alternate - so any spread is itself a finding.
int cmd_revs(int argc, char** argv) {
    if (argc != 3) {
        std::printf("usage: revs <col> <n>\n");
        return 1;
    }
    int col;
    long n;
    if (!parse_col(argv[1], col, false) || !parse_long(argv[2], n) || n <= 0) return 1;

    AxisInfo a;
    motion::info(col, a);
    const uint32_t rev0 = a.revs;

    const MotionParams p = motion::params();
    const int64_t usteps = (n + 1) * USTEPS_PER_SPOOL_REV_NOMINAL;
    if (motion::step_open_loop(col, usteps, p.flaps_s_home) != ESP_OK) {
        std::printf("cannot start; column busy\n");
        return 1;
    }

    std::printf("measuring %ld revolutions at %ld flaps/s...\n", n,
                static_cast<long>(p.flaps_s_home));

    int32_t lo = 0, hi = 0;
    int64_t sum = 0;
    uint32_t seen = 0;
    uint32_t last_rev = rev0;

    while (seen < static_cast<uint32_t>(n)) {
        vTaskDelay(pdMS_TO_TICKS(20));
        motion::info(col, a);
        if (a.revs != last_rev) {
            last_rev = a.revs;
            const int32_t h = a.hall_to_hall;
            std::printf("  rev %-3lu  hall_to_hall = %ld  err = %+ld\n",
                        static_cast<unsigned long>(seen + 1), static_cast<long>(h),
                        static_cast<long>(a.last_hall_err));
            if (seen == 0 || h < lo) lo = h;
            if (seen == 0 || h > hi) hi = h;
            sum += h;
            ++seen;
        }
        if (a.state == AxisState::Idle && a.revs == last_rev) break;  // ran out of travel
        if (a.state == AxisState::Fault) {
            std::printf("column faulted during measurement\n");
            break;
        }
    }

    motion::stop(col);
    if (seen > 0) {
        std::printf("n=%lu  min=%ld  max=%ld  mean=%.2f  spread=%ld\n",
                    static_cast<unsigned long>(seen), static_cast<long>(lo),
                    static_cast<long>(hi), static_cast<double>(sum) / seen,
                    static_cast<long>(hi - lo));
        std::printf("expected %lld for the %d/%d gearing; set motion.hall_tol from the spread\n",
                    static_cast<long long>(USTEPS_PER_SPOOL_REV_NOMINAL), GEAR_DRIVEN_TEETH,
                    GEAR_DRIVE_TEETH);
    }
    std::printf("index is now unknown - re-home before `go`\n");
    return 0;
}

int cmd_cal(int argc, char** argv) {
    if (argc != 3) {
        std::printf("usage: cal <col> <+/-usteps>    (then `save`)\n");
        return 1;
    }
    int col;
    long d;
    if (!parse_col(argv[1], col, false) || !parse_long(argv[2], d)) return 1;

    // The re-seek that makes the nudge visible lives in motion::adjust_cal
    // now, so this path and the web path cannot drift apart again - they
    // did, and the web one was the one that silently did nothing.
    const esp_err_t err = motion::adjust_cal(col, static_cast<int32_t>(d));
    AxisInfo a;
    motion::info(col, a);
    std::printf("col %d cal_offset = %ld usteps%s\n", col,
                static_cast<long>(a.cal_offset),
                err == ESP_ERR_INVALID_STATE ? "  (not homed - nothing moved)" : "");
    return 0;
}

int cmd_save(int, char**) {
    esp_err_t err = config::save(motion::params());
    // Column modes and maintenance persist too: a repair left half-finished
    // must still be a repair after a power cut.
    if (err == ESP_OK) err = config::save_columns(motion::columns());
    std::printf("%s\n", err == ESP_OK ? "saved" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

// display.frame through the dispatcher (spec 10.2a) - the same path every
// other transport uses.  Falls back to raw motion::go before bind_modes.
int cmd_frame(int argc, char** argv) {
    if (argc != N_COLUMNS + 1) {
        std::printf("usage: frame <c0> <c1> <c2> <c3> <c4>   (tokens, _ for blank, #n raw)\n");
        return 1;
    }
    Frame f;
    const RingSet ring = ring_now();
    for (int i = 0; i < N_COLUMNS; ++i) {
        AxisInfo cur;
        motion::info(i, cur);
        const int idx = ring.col(i).index_for_token(argv[i + 1], cur.index);
        if (idx < 0) {
            std::printf("no ring slot named '%s'\n", argv[i + 1]);
            return 1;
        }
        f.idx[static_cast<size_t>(i)] = idx;
    }
    if (g_mm != nullptr && g_utc_ms != nullptr) {
        return print_result(g_mm->cmd_display_frame(f, g_utc_ms()));
    }
    for (int i = 0; i < N_COLUMNS; ++i) motion::go(i, f.idx[static_cast<size_t>(i)]);
    return 0;
}

int cmd_mode(int argc, char** argv) {
    if (!modes_ready()) return 1;
    if (argc != 2) {
        std::printf("usage: mode clock|message|countdown   (now: %s, countdown %s)\n",
                    mode_name(g_mm->mode()), cd_phase_name(g_mm->cd_phase()));
        return 1;
    }
    Mode m;
    if (std::strcmp(argv[1], "clock") == 0) m = Mode::Clock;
    else if (std::strcmp(argv[1], "message") == 0) m = Mode::Message;
    else if (std::strcmp(argv[1], "countdown") == 0) m = Mode::Countdown;
    else {
        std::printf("unknown mode '%s'\n", argv[1]);
        return 1;
    }
    return print_result(g_mm->cmd_mode_set(m, g_utc_ms()));
}

int cmd_msg(int argc, char** argv) {
    if (!modes_ready()) return 1;
    if (argc < N_COLUMNS + 1) {
        std::printf("usage: msg <c0>..<c4> [dwell_s] [hold]\n");
        return 1;
    }
    std::array<std::string, N_COLUMNS> toks;
    for (int i = 0; i < N_COLUMNS; ++i) toks[static_cast<size_t>(i)] = argv[i + 1];
    long dwell = 0;
    bool hold = false;
    if (argc > N_COLUMNS + 1) parse_long(argv[N_COLUMNS + 1], dwell);
    if (argc > N_COLUMNS + 2) hold = std::strcmp(argv[N_COLUMNS + 2], "hold") == 0;
    return print_result(
        g_mm->cmd_message_set(toks, static_cast<int>(dwell), hold, g_utc_ms()));
}

int cmd_countdown(int argc, char** argv) {
    if (!modes_ready()) return 1;
    if (argc < 2) {
        std::printf("usage: countdown execute <numbers...>|start|reset|cancel|target <epoch>\n");
        return 1;
    }
    const int64_t now = g_utc_ms();
    if (std::strcmp(argv[1], "execute") == 0) {
        std::string numbers;
        for (int i = 2; i < argc; ++i) {
            if (!numbers.empty()) numbers += ' ';
            numbers += argv[i];
        }
        return print_result(g_mm->cmd_countdown_execute(numbers, now));
    }
    if (std::strcmp(argv[1], "start") == 0) return print_result(g_mm->cmd_countdown_start(now));
    if (std::strcmp(argv[1], "reset") == 0) return print_result(g_mm->cmd_countdown_reset(now));
    if (std::strcmp(argv[1], "cancel") == 0) return print_result(g_mm->cmd_countdown_cancel(now));
    if (std::strcmp(argv[1], "target") == 0 && argc == 3) {
        long long epoch = 0;
        char* end = nullptr;
        epoch = std::strtoll(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0') return 1;
        return print_result(g_mm->cmd_countdown_set_target(epoch, now));
    }
    std::printf("unknown countdown command '%s'\n", argv[1]);
    return 1;
}

int cmd_preset(int argc, char** argv) {
    if (!modes_ready()) return 1;
    if (argc != 2) {
        std::printf("usage: preset qmarks|blank|reveal|wifi\n");
        return 1;
    }
    return print_result(g_mm->cmd_preset(argv[1], g_utc_ms()));
}

int cmd_h24(int argc, char** argv) {
    if (!modes_ready()) return 1;
    if (argc != 2) {
        std::printf("usage: h24 0|1\n");
        return 1;
    }
    long v;
    if (!parse_long(argv[1], v)) return 1;
    const auto r = g_mm->cmd_clock_format(v != 0, g_utc_ms());
    // Persist the format with the rest of the app config.
    config::AppConfig app;
    config::load_app(app);
    app.modes.h24 = (v != 0);
    config::save_app(app);
    return print_result(r);
}

int cmd_tz(int argc, char** argv) {
    if (!modes_ready()) return 1;
    if (argc != 2) {
        std::printf("usage: tz <posix-tz>   e.g. tz PST8PDT,M3.2.0,M11.1.0\n");
        return 1;
    }
    if (!g_mm->set_tz(argv[1])) {
        std::printf("rejected: not a valid POSIX TZ string with M-rules\n");
        return 1;
    }
    config::AppConfig app;
    config::load_app(app);
    app.tz = argv[1];
    config::save_app(app);
    std::printf("ok\n");
    return 0;
}

int cmd_soak(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "stop") == 0) {
        motion::soak_stop("stopped from the console");
        std::printf("stopping\n");
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "start") == 0) {
        long wraps = 0, flaps = 0;
        if (argc >= 3) parse_long(argv[2], wraps);
        if (argc >= 4) parse_long(argv[3], flaps);
        if (!motion::soak_start(static_cast<uint32_t>(wraps), static_cast<int32_t>(flaps))) {
            std::printf("could not start (already running, or every column is disabled)\n");
            return 1;
        }
        std::printf("soak started: %ld wraps%s\n", wraps,
                    wraps == 0 ? " (until stopped)" : "");
        return 0;
    }

    const motion::SoakReport r = motion::soak_report();
    std::printf("soak     : %s%s%s\n", r.running ? "RUNNING" : "idle",
                r.stopped_because[0] ? " - " : "", r.stopped_because);
    std::printf("elapsed  : %lu s   target %lu wraps   %ld flaps/s\n",
                static_cast<unsigned long>(r.elapsed_s),
                static_cast<unsigned long>(r.target_wraps), static_cast<long>(r.flaps_s));
    std::printf("heap     : start %lu  now %lu  min %lu   (%ld over the run)\n",
                static_cast<unsigned long>(r.heap_start), static_cast<unsigned long>(r.heap_now),
                static_cast<unsigned long>(r.heap_min),
                static_cast<long>(r.heap_now) - static_cast<long>(r.heap_start));
    std::printf("col  wraps   flips  minor  major faults   h2h min/max  |err|max\n");
    for (int i = 0; i < N_COLUMNS; ++i) {
        const motion::SoakColumn& c = r.col[i];
        std::printf("%3d %6lu %7lu %6lu %6lu %6lu   %5ld/%-5ld %8ld\n", i,
                    static_cast<unsigned long>(c.wraps), static_cast<unsigned long>(c.flips),
                    static_cast<unsigned long>(c.resync_minor),
                    static_cast<unsigned long>(c.resync_major),
                    static_cast<unsigned long>(c.faults), static_cast<long>(c.h2h_min),
                    static_cast<long>(c.h2h_max), static_cast<long>(c.err_abs_max));
    }
    return 0;
}

int cmd_stats(int, char**) {
    for (int i = 0; i < N_COLUMNS; ++i) {
        AxisInfo a;
        motion::info(i, a);
        std::printf("col %d %-7s idx=%-3d pos=%-10lld hall=%-10lld cal=%-6ld v=%-5ld\n", i,
                    axis_state_name(a.state), a.index, static_cast<long long>(a.pos_abs),
                    static_cast<long long>(a.hall_abs), static_cast<long>(a.cal_offset),
                    static_cast<long>(a.velocity));
        std::printf(
            "      flips=%-8lu revs=%-5lu minor=%-5lu major=%-5lu faults=%lu h2h=%ld err=%+ld\n",
            static_cast<unsigned long>(a.flips_total), static_cast<unsigned long>(a.revs),
            static_cast<unsigned long>(a.resync_minor),
            static_cast<unsigned long>(a.resync_major), static_cast<unsigned long>(a.faults),
            static_cast<long>(a.hall_to_hall), static_cast<long>(a.last_hall_err));
    }
    std::printf("drivers %s, free heap %lu\n", motion::is_enabled() ? "ENABLED" : "disabled",
                static_cast<unsigned long>(esp_get_free_heap_size()));
    return 0;
}

int cmd_ring(int argc, char** argv) {
    // The shared runtime table (a column with its own ring differs; the
    // Calibrate page's per-column walk arrives in Phase 3).
    const RingSet ring = ring_now();
    const RingTable& table = ring.col(0);
    if (argc == 2) {
        const int i = table.index_for_token(argv[1]);
        if (i < 0) {
            std::printf("no ring slot named '%s'\n", argv[1]);
            return 1;
        }
        std::printf("%2d  %-12s %-28s %s\n", i, table.slot(i).id.c_str(),
                    table.slot(i).label.c_str(), ring_category_name(table.slot(i).cat));
        return 0;
    }
    std::printf("source: %s   descending: %s\n",
                ring.loaded_from_json() ? "ring.json" : "compiled",
                table.is_descending() ? "yes" : "NO");
    for (int i = 0; i < table.slot_count(); ++i) {
        std::printf("%2d  %-12s %-28s %s\n", i, table.slot(i).id.c_str(),
                    table.slot(i).label.c_str(), ring_category_name(table.slot(i).cat));
    }
    return 0;
}

// spec 13 `wifi ...`.  Credentials live in NVS; provisioning over a captive
// portal is Phase 4, so this is how the display joins a network for now.
// What NVS ACTUALLY holds, read back rather than reported from RAM.
//
// This exists for the OTA survival test (BRINGUP 22-25).  /api/state cannot
// answer the question: ModeManager DEFERS the countdown resume until SNTP has
// synced, so cd.target reads 0 for the first seconds after any reboot even
// when the deadline is perfectly intact - and reading the display too early
// reports a correct rollback as a lost deadline.  `persist` needs no clock.
int cmd_audio(int argc, char** argv) {
    audio::AudioSettings s = audio::settings();
    if (argc == 1 || std::strcmp(argv[1], "status") == 0) {
        const audio::Status st = audio::status();
        std::printf("volume : %d%s\n", s.volume, s.mute ? "  (MUTED)" : "");
        std::printf("quiet  : %s\n",
                    s.quiet_start_min == s.quiet_end_min ? "off"
                                                         : "on");
        std::printf("playing: %s\n", st.playing ? st.cue.c_str() : "-");
        for (size_t i = 0; i < audio::CUE_COUNT; ++i) {
            // The duration, not just "present": a cue can parse, report ok and
            // be two milliseconds long, which is indistinguishable from a
            // working one until you are standing next to the speaker.
            if (st.have[i]) {
                std::printf("  %-16s %lu.%02lu s\n",
                            audio::cue_id_name(static_cast<audio::CueId>(i)),
                            static_cast<unsigned long>(st.ms[i] / 1000),
                            static_cast<unsigned long>((st.ms[i] % 1000) / 10));
            } else {
                std::printf("  %-16s MISSING\n",
                            audio::cue_id_name(static_cast<audio::CueId>(i)));
            }
        }
        std::printf("underruns: %lu\n", static_cast<unsigned long>(st.underruns));
        return 0;
    }
    if (std::strcmp(argv[1], "play") == 0 && argc >= 3) {
        audio::CueId id{};
        if (!audio::cue_id_from_name(argv[2], id)) {
            std::printf("unknown cue\n");
            return 1;
        }
        // -1: bypass quiet hours.  Somebody at the console is testing the
        // speaker, and silence would look like a broken amp.
        audio::play(id, -1);
        return 0;
    }
    if (std::strcmp(argv[1], "stop") == 0) {
        audio::stop();
        return 0;
    }
    if (std::strcmp(argv[1], "vol") == 0 && argc >= 3) {
        s.volume = std::atoi(argv[2]);
        if (s.volume < 0 || s.volume > 100) {
            std::printf("volume must be 0-100\n");
            return 1;
        }
    } else if (std::strcmp(argv[1], "mute") == 0) {
        s.mute = argc < 3 || std::atoi(argv[2]) != 0;
    } else {
        std::printf("usage: audio status | audio play <cue> | audio stop | "
                    "audio vol <0-100> | audio mute [0|1]\n");
        return 1;
    }
    audio::set_settings(s);
    config::AudioConfig c;
    c.volume = s.volume;
    c.mute = s.mute;
    c.quiet_start_min = s.quiet_start_min;
    c.quiet_end_min = s.quiet_end_min;
    const esp_err_t err = config::save_audio(c);
    std::printf("%s\n", err == ESP_OK ? "saved" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

int cmd_persist(int, char**) {
    swan::ColumnConfig cols;
    config::load_columns(cols);
    std::printf("col_mode :");
    for (int i = 0; i < N_COLUMNS; ++i) std::printf(" %s", column_mode_name(cols.mode[i]));
    std::printf("\nmaint    : %s\n", cols.maintenance ? "ON" : "off");

    swan::CdPersist cd;
    const bool have = config::countdown_store().load(cd);
    if (have) {
        std::printf("cd_phase : %s\ncd_target: %lld\ncd_seq   : %lu\ncd_setby : %s\n",
                    cd_phase_name(cd.phase), static_cast<long long>(cd.target_utc),
                    static_cast<unsigned long>(cd.seq), origin_name(cd.set_by));
    } else {
        std::printf("cd_*     : (none stored)\n");
    }

    MotionParams mp;
    config::load(mp);
    std::printf("cal      :");
    for (int i = 0; i < N_COLUMNS; ++i) std::printf(" %+ld", static_cast<long>(mp.cal[i]));
    std::printf("\n");

    config::WifiConfig w;
    config::load_wifi(w);
    std::printf("wifi     : %s\n", w.ssid.empty() ? "(none)" : w.ssid.c_str());
    config::MqttConfig m;
    config::load_mqtt(m);
    std::printf("mqtt     : %s %s\n", m.enabled ? "on" : "off",
                m.uri.empty() ? "(none)" : m.uri.c_str());

    const esp_app_desc_t* d = esp_app_get_description();
    const net::OtaState o = net::ota_status();
    std::printf("image    : %s (%s)%s\n", d != nullptr ? d->version : "?",
                o.running_partition.c_str(),
                o.pending_verify ? "  PENDING_VERIFY" : "");
    return 0;
}

int cmd_ota(int argc, char** argv) {
    const net::OtaState o = net::ota_status();
    if (argc == 1 || std::strcmp(argv[1], "status") == 0) {
        const esp_app_desc_t* d = esp_app_get_description();
        std::printf("image    : %s\n", d != nullptr ? d->version : "?");
        std::printf("partition: %s\n", o.running_partition.c_str());
        std::printf("pending  : %s\n", o.pending_verify ? "YES - must confirm or roll back"
                                                         : "no (confirmed)");
        std::printf("verdict  : %s\n", o.boot_verdict.c_str());
        if (!o.last_error.empty()) std::printf("last err : %s\n", o.last_error.c_str());
        return 0;
    }
    if (std::strcmp(argv[1], "confirm") == 0) {
        const esp_err_t err = net::ota_confirm();
        std::printf("%s\n", err == ESP_OK ? "confirmed" : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (std::strcmp(argv[1], "rollback") == 0) {
        std::printf("rolling back and rebooting\n");
        net::ota_rollback_and_reboot();
        return 0;
    }
    std::printf("usage: ota status | ota confirm | ota rollback\n");
    return 1;
}

int cmd_mqtt(int argc, char** argv) {
    config::MqttConfig c;
    config::load_mqtt(c);
    if (argc == 1 || std::strcmp(argv[1], "status") == 0) {
        std::printf("mqtt     : %s\n", c.enabled ? (net::mqtt_connected() ? "CONNECTED"
                                                                          : "enabled, offline")
                                                  : "off");
        std::printf("broker   : %s\n", c.uri.empty() ? "(none)" : c.uri.c_str());
        std::printf("user     : %s\n", c.user.empty() ? "(none)" : c.user.c_str());
        // Whether one is STORED, never what it is.  A partial mqtt.config used
        // to clear it silently, and there was no way to tell from any surface.
        std::printf("password : %s\n", c.pass.empty() ? "(none)" : "(set)");
        std::printf("base     : %s\n", c.base.c_str());
        std::printf("dropped  : %lu\n", static_cast<unsigned long>(net::mqtt_dropped()));
        return 0;
    }
    if (std::strcmp(argv[1], "off") == 0) {
        c.enabled = false;
        const esp_err_t err = config::save_mqtt(c);
        if (err == ESP_OK) net::mqtt_reconfigure();
        std::printf("%s\n", err == ESP_OK ? "mqtt off" : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc < 2) {
        std::printf("usage: mqtt <uri> [user] [pass] | mqtt status | mqtt off | "
                    "mqtt base <topic>\n");
        return 1;
    }
    if (std::strcmp(argv[1], "base") == 0) {
        if (argc < 3) {
            std::printf("usage: mqtt base <topic>\n");
            return 1;
        }
        c.base = argv[2];
        const esp_err_t err = config::save_mqtt(c);
        if (err == ESP_OK) net::mqtt_reconfigure();
        std::printf("%s\n", err == ESP_OK ? "saved" : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    // Validated with the same pure check the API uses, so the console and the
    // Settings page reject exactly the same strings for the same reasons.
    std::string why;
    if (!api::broker_uri_valid(argv[1], why)) {
        std::printf("rejected: %s\n", why.c_str());
        return 1;
    }
    c.enabled = true;
    c.uri = argv[1];
    if (argc >= 3) c.user = argv[2];
    if (argc >= 4) c.pass = argv[3];
    const esp_err_t err = config::save_mqtt(c);
    if (err == ESP_OK) net::mqtt_reconfigure();
    std::printf("%s\n", err == ESP_OK ? "saved; connecting" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

int cmd_wifi(int argc, char** argv) {
    if (argc == 1 || std::strcmp(argv[1], "status") == 0) {
        const net::WifiStatus w = net::status();
        std::printf("wifi   : %s\n", net::wifi_state_name(w.state));
        std::printf("ssid   : %s\n", w.ssid.empty() ? "(none)" : w.ssid.c_str());
        std::printf("ip     : %s\n", w.ip.empty() ? "(none)" : w.ip.c_str());
        std::printf("rssi   : %d dBm\n", w.rssi);
        std::printf("drops  : %lu\n", static_cast<unsigned long>(w.disconnects));
        return 0;
    }
    if (std::strcmp(argv[1], "clear") == 0) {
        const esp_err_t err = net::set_credentials("", "");
        std::printf("%s\n", err == ESP_OK ? "cleared" : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (argc < 3) {
        std::printf("usage: wifi <ssid> <password> | wifi status | wifi clear\n");
        return 1;
    }
    const esp_err_t err = net::set_credentials(argv[1], argv[2]);
    std::printf("%s\n", err == ESP_OK ? "saved; connecting" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

// Push the exclusion mask down to the frame layer whenever a column's mode
// changes.  Through ModeManager, so it happens under the same lock every
// command takes, and so the display re-renders around the new hole.
void apply_columns() {
    if (g_mm == nullptr || g_utc_ms == nullptr) return;
    g_mm->cmd_set_excluded(motion::columns().excluded_mask(), g_utc_ms());
}

// spec 5.9.  `col` sets what a column IS; `sim` is a shorthand for the common
// build-out case plus the fault-injection bench tools.
int cmd_col(int argc, char** argv) {
    ColumnConfig c = motion::columns();
    if (argc == 1) {
        for (int i = 0; i < N_COLUMNS; ++i) {
            AxisInfo a;
            motion::info(i, a);
            std::printf("  col %d  %-8s  %-7s%s\n", i, column_mode_name(c.mode[i]),
                        axis_state_name(a.state),
                        a.state == AxisState::Fault
                            ? (std::string("  (") + fault_cause_name(a.fault_cause) + ")").c_str()
                            : "");
        }
        std::printf("  maintenance: %s\n", c.maintenance ? "ON" : "off");
        return 0;
    }
    if (argc < 3) {
        std::printf("usage: col <0-4|all> real|sim|disabled\n");
        return 1;
    }
    ColumnMode m;
    if (!column_mode_from_name(argv[2], m)) {
        std::printf("mode must be real, sim or disabled\n");
        return 1;
    }
    if (std::strcmp(argv[1], "all") == 0) {
        for (auto& x : c.mode) x = m;
    } else {
        const int col = std::atoi(argv[1]);
        if (col < 0 || col >= N_COLUMNS) {
            std::printf("column must be 0..%d\n", N_COLUMNS - 1);
            return 1;
        }
        c.mode[col] = m;
    }
    motion::set_columns(c);
    apply_columns();
    std::printf("ok; `save` to persist\n");
    return 0;
}

int cmd_sim(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "fault") == 0) {
        if (argc < 4) {
            std::printf("usage: sim fault <col> slip <usteps> | miss <edges> | clear\n");
            return 1;
        }
        const int col = std::atoi(argv[2]);
        esp_err_t err = ESP_ERR_INVALID_ARG;
        if (std::strcmp(argv[3], "slip") == 0 && argc >= 5) {
            err = motion::sim_inject_slip(col, std::atoi(argv[4]));
        } else if (std::strcmp(argv[3], "miss") == 0 && argc >= 5) {
            err = motion::sim_inject_miss(col, static_cast<uint32_t>(std::atol(argv[4])));
        } else if (std::strcmp(argv[3], "clear") == 0) {
            err = motion::sim_clear_faults(col);
        }
        std::printf("%s\n", err == ESP_OK ? "ok" : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }

    ColumnConfig c = motion::columns();
    if (argc == 1) {
        std::printf("usage: sim all|<col> [off] | sim fault <col> ...\n");
        return 1;
    }
    const bool off = (argc >= 3 && std::strcmp(argv[2], "off") == 0);
    const ColumnMode m = off ? ColumnMode::Real : ColumnMode::Sim;
    if (std::strcmp(argv[1], "all") == 0) {
        for (auto& x : c.mode) x = m;
    } else {
        const int col = std::atoi(argv[1]);
        if (col < 0 || col >= N_COLUMNS) {
            std::printf("column must be 0..%d\n", N_COLUMNS - 1);
            return 1;
        }
        c.mode[col] = m;
    }
    motion::set_columns(c);
    apply_columns();
    std::printf("ok; `save` to persist\n");
    return 0;
}

int cmd_maint(int argc, char** argv) {
    if (g_mm == nullptr || g_utc_ms == nullptr) {
        std::printf("modes not available\n");
        return 1;
    }
    ColumnConfig c = motion::columns();
    if (argc == 1) {
        std::printf("maintenance: %s\n", c.maintenance ? "ON" : "off");
        return 0;
    }
    const bool on = std::atoi(argv[1]) != 0 || std::strcmp(argv[1], "on") == 0;
    c.maintenance = on;
    motion::set_columns(c);
    MotionParams p = motion::params();
    p.maintenance = on;
    motion::set_params(p);
    g_mm->cmd_maintenance(on, g_utc_ms());
    // PERSIST IT.  Spec 5.9 makes maintenance survive a reboot deliberately -
    // pulling power mid-repair must not restart a countdown on top of your
    // hands - and that is a safety claim, not a convenience.  The dispatcher
    // path saved it (bindings.cpp) and this one did not, so `maint on` from
    // the console - which is what BRINGUP tells a bench operator to type
    // before touching a drum - was forgotten by the next boot.
    const esp_err_t merr = config::save_columns(motion::columns());
    // Leaving maintenance re-homes from motion::enable(true) now - one rule
    // in one place - so there is no explicit home(-1) here any more.
    std::printf("maintenance %s%s%s\n", on ? "ON - nothing moves on its own" : "off",
                on ? "" : "; re-homing",
                merr == ESP_OK ? "" : " (NOT SAVED - it will not survive a reboot)");
    return merr == ESP_OK ? 0 : 1;
}

int cmd_reboot(int, char**) {
    std::printf("rebooting\n");
    esp_restart();
    return 0;
}

void reg(const char* cmd, const char* help, esp_console_cmd_func_t fn) {
    // Field-by-field rather than designated initialisers: C++ requires those to
    // match declaration order, and esp_console_cmd_t has gained fields across
    // IDF releases.
    esp_console_cmd_t c = {};
    c.command = cmd;
    c.help = help;
    c.hint = nullptr;
    c.func = fn;
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}

}  // namespace

void bind_ring(const api::RingSource* src) { g_ring = src; }

void bind_modes(ModeManager* mm, int64_t (*utc_ms_fn)()) {
    g_mm = mm;
    g_utc_ms = utc_ms_fn;
}

esp_err_t start() {
    esp_console_repl_t* repl = nullptr;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "swan>";
    repl_cfg.max_cmdline_length = 128;

    esp_console_dev_usb_serial_jtag_config_t dev_cfg =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_console_new_repl_usb_serial_jtag(&dev_cfg, &repl_cfg, &repl), "cli",
                        "repl");

    esp_console_register_help_command();
    reg("pins", "show the resolved pin map and drive constants", cmd_pins);
    reg("hall", "live Hall levels, raw and debounced", cmd_hall);
    reg("button", "button [seconds] - live BOOT/button level, and edges", cmd_button);
    reg("en", "en 0|1 - driver enable (ganged)", cmd_en);
    reg("dir", "dir [0|1] - ganged direction; bench step 3 sets it", cmd_dir);
    reg("step", "step <col> <usteps> - open loop", cmd_step);
    reg("home", "home <col>|all", cmd_home);
    reg("go", "go <col> <index|token>", cmd_go);
    reg("spin", "spin <col> <flaps_s> <seconds>", cmd_spin);
    reg("revs", "revs <col> <n> - measure hall_to_hall", cmd_revs);
    reg("cal", "cal <col> <+/-usteps> - nudge the calibration offset", cmd_cal);
    reg("save", "persist the current config to NVS", cmd_save);
    reg("wifi", "wifi <ssid> <pass> | wifi status | wifi clear", cmd_wifi);
    reg("mqtt", "mqtt <uri> [user] [pass] | mqtt status | mqtt off | mqtt base <topic>",
        cmd_mqtt);
    reg("audio", "audio status | play <cue> | stop | vol <0-100> | mute [0|1]",
        cmd_audio);
    reg("persist", "persist - what NVS actually holds (needs no clock)", cmd_persist);
    reg("ota", "ota status | ota confirm | ota rollback", cmd_ota);
    reg("col", "col [<0-4>|all real|sim|disabled] - per-column mode", cmd_col);
    reg("sim", "sim all|<col> [off] | sim fault <col> slip|miss|clear", cmd_sim);
    reg("maint", "maint [0|1] - maintenance mode", cmd_maint);
    reg("frame", "frame <c0>..<c4> - set all five columns", cmd_frame);
    reg("ring", "ring [token] - list the ring table", cmd_ring);
    reg("mode", "mode clock|message|countdown", cmd_mode);
    reg("msg", "msg <c0>..<c4> [dwell_s] [hold]", cmd_msg);
    reg("countdown", "countdown execute <numbers>|start|reset|cancel|target <epoch>", cmd_countdown);
    reg("preset", "preset qmarks|blank|reveal|wifi", cmd_preset);
    reg("h24", "h24 0|1 - clock format (persisted)", cmd_h24);
    reg("tz", "tz <posix-tz> - timezone (persisted)", cmd_tz);
    reg("soak", "soak start <wraps> [flaps_s] | soak stop | soak - overnight wrap test",
        cmd_soak);
    reg("stats", "per-column counters and state", cmd_stats);
    reg("reboot", "restart the device", cmd_reboot);

    return esp_console_start_repl(repl);
}

}  // namespace cli
}  // namespace swan
