#include "cli/cli.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "config/config.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/gpio_bank.h"
#include "hal/pins.h"
#include "motion/motion.h"
#include "ring/ring.h"

namespace swan {
namespace cli {
namespace {

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

    int index = ring_index_for_token(argv[2]);
    if (index == RING_INVALID) {
        long v;
        if (!parse_long(argv[2], v) || !ring_index_valid(static_cast<int>(v))) {
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
    std::printf("col %d -> %d (%s)\n", col, index, ring_label(index));
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
    const esp_err_t err = config::save(motion::params());
    std::printf("%s\n", err == ESP_OK ? "saved" : esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

// The real frame scheduler (simultaneous starts, mode arbitration) is Phase 2;
// this is enough to walk the ring on the bench.
int cmd_frame(int argc, char** argv) {
    if (argc != N_COLUMNS + 1) {
        std::printf("usage: frame <c0> <c1> <c2> <c3> <c4>   (tokens, _ for blank, #n raw)\n");
        return 1;
    }
    int idx[N_COLUMNS];
    for (int i = 0; i < N_COLUMNS; ++i) {
        idx[i] = ring_index_for_token(argv[i + 1]);
        if (idx[i] == RING_INVALID) {
            std::printf("no ring slot named '%s'\n", argv[i + 1]);
            return 1;
        }
    }
    for (int i = 0; i < N_COLUMNS; ++i) motion::go(i, idx[i]);
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
    if (argc == 2) {
        const int i = ring_index_for_token(argv[1]);
        if (i == RING_INVALID) {
            std::printf("no ring slot named '%s'\n", argv[1]);
            return 1;
        }
        std::printf("%2d  %-12s %-28s %s\n", i, ring_char_id(i), ring_label(i),
                    ring_category_name(ring_category(i)));
        return 0;
    }
    for (int i = 0; i < RING_SLOT_COUNT; ++i) {
        std::printf("%2d  %-12s %-28s %s\n", i, ring_char_id(i), ring_label(i),
                    ring_category_name(ring_category(i)));
    }
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
    reg("frame", "frame <c0>..<c4> - set all five columns", cmd_frame);
    reg("ring", "ring [token] - list the ring table", cmd_ring);
    reg("stats", "per-column counters and state", cmd_stats);
    reg("reboot", "restart the device", cmd_reboot);

    return esp_console_start_repl(repl);
}

}  // namespace cli
}  // namespace swan
