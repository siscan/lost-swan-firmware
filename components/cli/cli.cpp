#include "cli/cli.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "config/config.h"
#include "esp_check.h"
#include "esp_console.h"
#include "net/wifi.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/gpio_bank.h"
#include "hal/pins.h"
#include "modes/mode_manager.h"
#include "motion/motion.h"
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

// Bench step 4: measure hall_to_hall over n revolutions.  Expect ~8242 for the
// 85/33 gearing; a consistent ~8369 means the drum is on the old 68/26 parts
// and geometry.h needs correcting (see spec 3).
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

    motion::adjust_cal(col, static_cast<int32_t>(d));
    AxisInfo a;
    motion::info(col, a);
    std::printf("col %d cal_offset = %ld usteps\n", col, static_cast<long>(a.cal_offset));

    // Re-seek the same index so the nudge is visible immediately.
    if (ring_index_valid(a.index)) motion::go(col, a.index);
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
    if (!on) motion::home(-1);  // leaving re-arms and re-homes
    std::printf("maintenance %s%s\n", on ? "ON - nothing moves on its own" : "off",
                on ? "" : "; re-homing");
    return 0;
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
    reg("en", "en 0|1 - driver enable (ganged)", cmd_en);
    reg("step", "step <col> <usteps> - open loop", cmd_step);
    reg("home", "home <col>|all", cmd_home);
    reg("go", "go <col> <index|token>", cmd_go);
    reg("spin", "spin <col> <flaps_s> <seconds>", cmd_spin);
    reg("revs", "revs <col> <n> - measure hall_to_hall", cmd_revs);
    reg("cal", "cal <col> <+/-usteps> - nudge the calibration offset", cmd_cal);
    reg("save", "persist the current config to NVS", cmd_save);
    reg("wifi", "wifi <ssid> <pass> | wifi status | wifi clear", cmd_wifi);
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
    reg("stats", "per-column counters and state", cmd_stats);
    reg("reboot", "restart the device", cmd_reboot);

    return esp_console_start_repl(repl);
}

}  // namespace cli
}  // namespace swan
