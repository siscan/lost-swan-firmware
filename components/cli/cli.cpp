#include "cli/cli.h"

#include <cctype>
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
#include "motion/axis_control.h"
#include "motion/motion.h"
#include "motion/bench.h"
#include "motion/bench_policy.h"
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

// The speed ladder's rung limit.  Eight is far more than a ladder anybody
// would stand at a vise for, and it keeps the array on the REPL stack.
constexpr int MAX_RUNGS = 8;

// Wait for the operator, on the console they are already looking at.  Returns
// false if they typed q/Q, which is how a ladder is abandoned without having
// to reach for Ctrl-C.  getchar() on this REPL blocks on the USB-Serial-JTAG
// driver, so this costs nothing while it waits.
bool wait_for_enter() {
    // BOUNDED, because this runs on the console REPL task and an unbounded
    // wait on stdin is a wedged console.  getchar() returns EOF rather than
    // blocking when the VFS is in a non-blocking mode, so without the bound
    // the loop below would spin at 50 Hz for ever waiting for a keypress
    // nobody is there to make.  Ten minutes is far longer than anyone stands
    // watching one rung and short enough that a walked-away-from bench does
    // not stay busy.
    const int64_t until = esp_timer_get_time() + 600LL * 1000000;
    while (esp_timer_get_time() < until) {
        const int c = std::getchar();
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == 'q' || c == 'Q') return false;
        if (c == '\n' || c == '\r') return true;
    }
    std::printf("    no keypress in 10 minutes - stopping rather than running the\n");
    std::printf("    next rung at a bench nobody is standing at.\n");
    return false;
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
    std::printf("  EN=GPIO%d (ganged, active low)\n", PIN_EN);
    if (HAS_DIR_GPIO) {
        std::printf("  DIR=GPIO%d (ganged) dir_invert=%d\n", PIN_DIR,
                    motion::params().dir_invert ? 1 : 0);
    } else {
        std::printf("  DIR: tied at the drivers - no spare non-strapping pin here\n");
    }
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
    MotionParams p = motion::params();
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
    if (!motion::set_params(p)) {
        std::printf("refused - see the log\n");
        return 1;
    }
    std::printf("dir_invert = %d\n", p.dir_invert ? 1 : 0);
    return 0;
}

// The stand-in bench session (BRINGUP 28b gate 3).  Present in every build so
// the refusal is legible: on a normal image `bench` explains that it is not a
// bench image rather than reporting an unknown command.
int cmd_bench(int argc, char** argv) {
    if (!motion::BENCH_BUILD) {
        const esp_app_desc_t* d = esp_app_get_description();
        std::printf("not a bench image (this is %s)\n", d != nullptr ? d->version : "?");
        std::printf("the stand-in session needs a capped build:\n");
        std::printf("  .\\build.ps1 -B build-bench -DSWAN_BENCH=ON app\n");
        return 1;
    }
    if (argc >= 2 && std::strcmp(argv[1], "stop") == 0) {
        motion::bench_stop("stopped from the console");
        std::printf("stopping\n");
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "samples") == 0) {
        // The run's own per-minute record, out of the buffer that no other
        // component can write to.  The log ring is shared and the first real
        // gate-3 soak had its entire record evicted from it (bench.h).
        const motion::BenchSampleMeta meta = motion::bench_sample_meta();
        if (meta.count == 0) {
            std::printf("no samples; `bench soak <col>` records one a minute\n");
            return 0;
        }
        if (meta.dropped != 0) {
            std::printf("  %u earlier sample(s) dropped - this is the tail of a long run\n",
                        static_cast<unsigned>(meta.dropped));
        }
        if (meta.open_loop) {
            std::printf("  %-8s %-10s %-10s\n", "t(s)", "flaps", "heap");
        } else {
            std::printf("  %-8s %-10s %-10s %-7s %-13s %-7s %-7s\n",
                        "t(s)", "flaps", "heap", "revs", "h2h", "minor", "major");
        }
        // One at a time: the whole set by value is ~2 KB and this runs on the
        // console REPL task.  See the note on bench_sample_at.
        for (int i = 0; i < meta.count; ++i) {
            motion::BenchSample b{};
            if (!motion::bench_sample_at(i, b)) break;
            if (meta.open_loop) {
                std::printf("  %-8u %-10u %-10u\n", static_cast<unsigned>(b.elapsed_s),
                            static_cast<unsigned>(b.flaps),
                            static_cast<unsigned>(b.heap_now));
            } else {
                std::printf("  %-8u %-10u %-10u %-7u %-6d..%-6d %-7u %-7u\n",
                            static_cast<unsigned>(b.elapsed_s),
                            static_cast<unsigned>(b.flaps),
                            static_cast<unsigned>(b.heap_now),
                            static_cast<unsigned>(b.edges), static_cast<int>(b.h2h_min),
                            static_cast<int>(b.h2h_max),
                            static_cast<unsigned>(b.resync_minor),
                            static_cast<unsigned>(b.resync_major));
            }
        }
        if (meta.open_loop) {
            std::printf("  OPEN LOOP - edge figures n/a (no hall)\n");
        }
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "spin") == 0) {
        if (argc != 5) {
            std::printf("usage: bench spin <col> <flaps_s> <seconds>   (cap %d flaps/s)\n",
                        static_cast<int>(motion::BENCH_MAX_FLAPS_S));
            return 1;
        }
        long col, fs, secs;
        if (!parse_long(argv[2], col) || !parse_long(argv[3], fs) ||
            !parse_long(argv[4], secs)) {
            return 1;
        }
        return motion::bench_spin_start(static_cast<int>(col), static_cast<int32_t>(fs),
                                        static_cast<int>(secs))
                   ? 0
                   : 1;
    }
    if (argc >= 2 && std::strcmp(argv[1], "soak") == 0) {
        if (argc < 3 || argc > 5) {
            std::printf("usage: bench soak <col> [minutes] [tick_s]\n");
            std::printf("  default 60 minutes, one flap a second\n");
            std::printf("  closed loop when the column is homed, open loop when not\n");
            std::printf("  `bench samples` prints the run's own per-minute record\n");
            return 1;
        }
        long col, mins = 60, tick = 1;
        if (!parse_long(argv[2], col)) return 1;
        if (argc >= 4 && !parse_long(argv[3], mins)) return 1;
        if (argc >= 5 && !parse_long(argv[4], tick)) return 1;
        if (mins < 1 || mins > 1440) {
            std::printf("minutes must be 1..1440\n");
            return 1;
        }
        // SAY WHICH HALF OF THE ANSWER YOU ARE ABOUT TO GET, BEFORE THE HOUR.
        //
        // The soak falls back to open-loop flapping without a home reference,
        // which is right and was added deliberately for gate 3 branch B - a
        // module with no Hall fitted at all.  Branch A HAS one, and an
        // unhomed column looks identical to a hall-less one from in here.
        // So the difference is stated at the prompt rather than discovered in
        // the report an hour later, when the only fix is another hour.
        if (col >= 0 && col < N_COLUMNS) {
            AxisInfo ha{};
            motion::info(static_cast<int>(col), ha);
            if (!ha.hall_valid) {
                std::printf("\n  NO HOME REFERENCE ON COLUMN %ld.\n", col);
                std::printf("  This run will be OPEN LOOP and will answer the THERMAL\n");
                std::printf("  question only - no edges, no resyncs, no hall_to_hall.\n");
                std::printf("  If a magnet IS fitted, `home %ld` first and the same hour\n", col);
                std::printf("  also proves the drum kept its registration.\n\n");
            } else {
                std::printf("  closed loop: edge verification is live for the whole run\n");
            }
        }
        motion::BenchSchedule sch;
        sch.total_s = static_cast<uint32_t>(mins * 60);
        sch.tick_s = static_cast<uint32_t>(tick);
        if (!motion::bench_soak_start(static_cast<int>(col), sch)) return 1;
        std::printf("soak started: column %ld, %ld min, one flap every %ld s\n", col,
                    mins, tick);
        std::printf("leave it alone.  `bench` for progress, `bench stop` to abort.\n");
        return 0;
    }
    // Status.
    const motion::BenchStats st = motion::bench_report();
    // The cap is a build parameter, so the drum-revolution figure is derived
    // rather than the words "1 drum rev/s" being printed beside whatever
    // number the image happens to carry.
    std::printf("bench image, cap %d flaps/s (%.2f drum rev/s); show spin (%d) is absent\n",
                static_cast<int>(motion::BENCH_MAX_FLAPS_S),
                static_cast<double>(motion::BENCH_MAX_FLAPS_S) / N_RING,
                static_cast<int>(motion::SHOW_SPIN_FLAPS_S));
    if (!motion::bench_running() && st.total_s == 0) {
        std::printf("no run yet.  `bench soak <col>` starts the hour.\n");
        return 0;
    }
    // Open loop: print the flap count and the faults, and say the edge figures
    // do not exist rather than showing four zeroes that read as clean results.
    // `flaps` is usteps ISSUED / 64 either way - it is a duty figure, never a
    // measurement of drum motion.
    if (st.open_loop) {
        std::printf("  column %d  %u/%u s  flaps=%u (usteps issued)\n", st.column,
                    static_cast<unsigned>(st.elapsed_s), static_cast<unsigned>(st.total_s),
                    static_cast<unsigned>(st.flaps));
        std::printf("  OPEN LOOP - no hall fitted.  revs / h2h / err / resyncs: "
                    "n/a (no hall)\n");
        std::printf("  faults=%u\n", static_cast<unsigned>(st.faults));
    } else {
        std::printf("  column %d  %u/%u s  flaps=%u revs=%u\n", st.column,
                    static_cast<unsigned>(st.elapsed_s), static_cast<unsigned>(st.total_s),
                    static_cast<unsigned>(st.flaps), static_cast<unsigned>(st.edges));
        std::printf("  h2h %d..%d  worst err %d  minor=%u major=%u faults=%u\n",
                    static_cast<int>(st.h2h_min), static_cast<int>(st.h2h_max),
                    static_cast<int>(st.err_abs_max),
                    static_cast<unsigned>(st.resync_minor),
                    static_cast<unsigned>(st.resync_major),
                    static_cast<unsigned>(st.faults));
    }
    std::printf("  %s\n", motion::bench_running() ? "RUNNING" : st.stopped_because);
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

// WHY A HOMING FAILURE PRINTS A DECISION TREE (BRINGUP 28c step 3b).
//
// A homing pass that finds no edge is ONE observation with THREE causes, and
// the firmware cannot tell them apart: a dead sensor, a magnet the sensor
// never passes close enough to, and a magnet in backwards all produce exactly
// "1.2 revolutions, no edge".  Spec 5.8 says so about the classifier and it is
// just as true here - `no_hall` names the SIGNATURE, not the fault.
//
// What separates them is three things a person does with their hands, in a
// fixed order, and the order matters: checking polarity before checking that
// the sensor is powered at all wastes the evening that docs/ref/BOM.md gotcha
// 2 has warned about since the beginning.  So the tree is printed at the
// moment it is needed, on the console the person is already looking at.
//
// The firmware contributes the one fact it CAN observe and a person cannot:
// whether the hall asserted at any point during the pass it just ran.  That
// turns branch 2 from a question into an answer.
void print_home_tree(int col, bool saw_magnet_during_pass) {
    std::printf("\n");
    std::printf("  COLUMN %d DID NOT FIND A HALL EDGE.\n", col);
    std::printf("  One observation, three causes. Work down, in this order.\n\n");

    std::printf("  During the pass just now, the hall read magnet=%s.\n\n",
                saw_magnet_during_pass ? "YES at least once" : "no, the whole way round");

    std::printf("  1. Turn the drum by hand with `hall` on screen.\n");
    std::printf("     NEVER reads magnet=YES, at any angle\n");
    std::printf("        -> THE SENSOR IS UNPOWERED OR MISWIRED.\n");
    std::printf("           VCC to 3V3, GND to the CENTRE lead, OUT to GPIO%d,\n",
                PIN_HALL[col]);
    std::printf("           and the 10 k pull-up from OUT to 3V3 - without it the\n");
    std::printf("           open-drain output cannot pull the pin up at all.\n");
    std::printf("           Check the two OUTER leads are not swapped.\n\n");

    std::printf("  2. Reads magnet=YES by hand, but never during `home`\n");
    std::printf("        -> AIR GAP, or the magnet is not where the sensor sweeps.\n");
    std::printf("           By hand you can hold the magnet anywhere; the drum\n");
    std::printf("           can only take it past one fixed point. Close the gap\n");
    std::printf("           to 1-2 mm and check the sensor faces the disc track\n");
    std::printf("           the magnet actually runs on.\n\n");

    std::printf("  3. Nothing from the drum magnet, but a SPARE held to the\n");
    std::printf("     sensor DOES trip it\n");
    std::printf("        -> POLARITY. The magnet is glued in backwards.\n");
    std::printf("           The A1121 is unipolar: a SOUTH pole turns it on and a\n");
    std::printf("           north pole does nothing at all. Mark the spare face\n");
    std::printf("           that trips it S, then hold that face to the glued\n");
    std::printf("           magnet: REPEL = south = correct, ATTRACT = north =\n");
    std::printf("           re-glue it. `motion.hall_active_low` does NOT fix\n");
    std::printf("           this - there is no transition for it to invert.\n\n");

    std::printf("  Full procedure: docs/BENCH_WIRING.md section 2a, and the\n");
    std::printf("  illustrated pages. BRINGUP 28c step 3b has the blanks.\n\n");
}

int cmd_home(int argc, char** argv) {
    int col = -1;
    if (argc == 2 && !parse_col(argv[1], col, true)) return 1;
    const esp_err_t err = motion::home(col);
    if (err != ESP_OK) {
        std::printf("%s\n", esp_err_to_name(err));
        return 1;
    }
    if (col < 0) {
        // `home all` is five staggered passes; watching them would tie the
        // REPL up for half a minute and the tree is per-column anyway.
        std::printf("homing all five (staggered); `stats` for the outcome\n");
        return 0;
    }

    // WATCHED, like `revs`, because the answer arrives seconds later and on
    // another task.  A bare "homing" leaves the operator reading a scrollback
    // for a log line, and the log line cannot say what the hall did.
    std::printf("homing column %d...\n", col);
    bool saw_magnet = false;
    AxisInfo a{};
    // 1.2 revolutions at the homing speed, three automatic retries, plus the
    // settle move and a margin.  Generous on purpose: timing out EARLY here
    // would print a failure tree for a column that was about to succeed.
    const int64_t deadline_ms = esp_timer_get_time() / 1000 + 45000;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));
        motion::info(col, a);
        if (a.hall_level) saw_magnet = true;
        if (a.state == AxisState::Idle && a.hall_valid) {
            std::printf("homed: index %d, cal_offset %ld usteps\n", a.index,
                        static_cast<long>(a.cal_offset));
            std::printf("  the edge is the reference; `revs %d 10` measures the\n", col);
            std::printf("  distance between edges and `cal %d +/-n` places blank.\n", col);
            return 0;
        }
        if (a.state == AxisState::Fault) {
            std::printf("FAULT: %s\n", fault_cause_name(a.fault_cause));
            print_home_tree(col, saw_magnet);
            return 1;
        }
        if (esp_timer_get_time() / 1000 > deadline_ms) {
            std::printf("still %s after 45 s - not waiting further.\n",
                        a.state == AxisState::Homing ? "homing" : "not homed");
            print_home_tree(col, saw_magnet);
            return 1;
        }
    }
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
    if (err == ESP_ERR_NOT_SUPPORTED) {
        std::printf("REFUSED: %ld flaps/s is over this image's cap of %d flaps/s.\n",
                    fs, static_cast<int>(motion::BENCH_MAX_FLAPS_S));
        std::printf("  This is a bench image. The cap is compiled in: rebuild with\n");
        std::printf("  -DSWAN_BENCH_CAP=<n> if the mechanism on the vise has changed.\n");
        return 1;
    }
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

    // `revs` measures the distance BETWEEN hall edges.  With no sensor or no
    // magnet fitted there are none, so the old code stepped the whole distance
    // - 35,200 usteps, 69 s at the default 8 flaps/s - counted nothing, and
    // printed only "index is now unknown", which reads like success.  A silent
    // no-op at a bench is how a session gets misled, so refuse up front and say
    // which of the two exits the operator probably wanted.
    //
    // hall_valid is the right predicate and not "is it homed": it means one
    // edge has been latched since the most recent homing pass began, which is
    // a strictly lower bar, and bench.cpp already uses !hall_valid for exactly
    // this question.  It is RAM-only, so it is false after every power cycle
    // and false on the latched no_hall FAULT a hall-less module boots into.
    if (!a.hall_valid) {
        const long secs = static_cast<long>(usteps / (p.flaps_s_home * USTEPS_PER_FLAP_NUM /
                                                      USTEPS_PER_FLAP_DEN));
        std::printf("REFUSED: column %d has no hall reference - no edge has been seen.\n", col);
        std::printf("  `revs` measures the distance BETWEEN hall edges.  Without one it would\n");
        std::printf("  step %lld usteps (~%ld s at %ld flaps/s) and print nothing.\n",
                    static_cast<long long>(usteps), secs, static_cast<long>(p.flaps_s_home));
        std::printf("  If a magnet IS fitted: `home %d` first.\n", col);
        std::printf("  If you just want the drum to turn: `spin %d <flaps_s> <seconds>`,\n", col);
        std::printf("  or `step %d %lld` for exactly one revolution.\n",
                    col, static_cast<long long>(USTEPS_PER_SPOOL_REV_NOMINAL));
        return 1;
    }

    const esp_err_t started = motion::step_open_loop(col, usteps, p.flaps_s_home);
    if (started != ESP_OK) {
        // Same INVALID_STATE covers both, and "busy" sends you looking for a
        // move that is not running.
        std::printf("cannot start: column %d is %s\n", col,
                    motion::columns().mode[col] == ColumnMode::Disabled ? "disabled (`col n real`)"
                                                                     : "busy - homing or moving");
        return 1;
    }

    std::printf("measuring %ld revolutions at %ld flaps/s...\n", n,
                static_cast<long>(p.flaps_s_home));

    int32_t lo = 0, hi = 0;
    int64_t sum = 0;
    uint32_t seen = 0;
    uint32_t last_rev = rev0;
    // THE STATISTIC SPEC 5.4 ACTUALLY GRADES ON.  It bands |err| - the
    // difference between where an edge arrived and where the last one plus a
    // revolution said it would - not the spread of hall_to_hall.  They are the
    // same number on a healthy drum and they part company the moment one
    // revolution is long and the next one short, which is exactly when the
    // tolerance matters.
    int32_t worst_err = 0;

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
            const int32_t e = a.last_hall_err < 0 ? -a.last_hall_err : a.last_hall_err;
            if (e > worst_err) worst_err = e;
            sum += h;
            ++seen;
        }
        if (a.state == AxisState::Idle && a.revs == last_rev) break;  // ran out of travel
        if (a.state == AxisState::Fault) {
            std::printf("column faulted during measurement\n");
            break;
        }
        if (a.state == AxisState::Homing) {
            // A slip took the column into an automatic re-home.  Say so: the
            // sample set is truncated and the stop below aborts that pass.
            std::printf("column started re-homing mid-measurement; sample set is short\n");
            break;
        }
        // The up-front check cannot see a hall that WORKED and has since died -
        // hall_valid is stale true.  Bail at HOME_LIMIT past the last latched
        // edge: 1.2 revolutions, the same distance spec 5.5 gives a homing pass
        // before it calls the edge missing, and 960 usteps before the control
        // core's own missed-edge threshold raises an unretried `jam`.  A dead
        // sensor is not a stopped drum (spec 5.8) and should not enter the
        // journal as one.  Measured from hall_abs, not from the run start, and
        // only on a poll where revs did not change - info() promises each group
        // is internally consistent, not that the groups agree with each other.
        if (a.pos_abs - a.hall_abs > HOME_LIMIT) {
            std::printf("no edge in %lld usteps (1.2 revolutions) - stopping.\n",
                        static_cast<long long>(HOME_LIMIT));
            std::printf("  Either the sensor has stopped answering, or an edge was late by\n");
            std::printf("  more than 640 usteps and this cut it off.  It cannot tell them\n");
            std::printf("  apart; `hall` reads the input directly.\n");
            break;
        }
    }

    motion::stop(col);
    if (seen > 0) {
        const MotionParams cur = motion::params();
        std::printf("n=%lu  min=%ld  max=%ld  mean=%.2f  spread=%ld  worst |err|=%ld\n",
                    static_cast<unsigned long>(seen), static_cast<long>(lo),
                    static_cast<long>(hi), static_cast<double>(sum) / seen,
                    static_cast<long>(hi - lo), static_cast<long>(worst_err));
        std::printf("expected %lld EXACTLY at the 1:1 direct drive: there is no residue\n",
                    static_cast<long long>(USTEPS_PER_SPOOL_REV_NOMINAL));
        std::printf("to alternate, so ANY spread is a finding and not rounding.\n\n");

        // THE MEASURED hall_tol CANDIDATE (BRINGUP 28c step 3c).
        //
        // 16 is a DERIVED default - a quarter of a flap, geometry and nothing
        // more.  Spec 5.4 has said since the drive change that the real value
        // comes from measured edge repeatability, and this is the measurement.
        //
        // The rule, stated so the number is not a black box: the silent band
        // must be wider than the repeatability of a healthy drum, or ordinary
        // jitter is reported as a major resync.  Twice the worst error seen
        // over n revolutions is the margin; the floor of 2 exists because a
        // band of 0 or 1 would make the next quantisation step a "fault".
        const int32_t derived = static_cast<int32_t>(ring_target_usteps(1) / 4);
        int32_t cand = worst_err * 2;
        if (cand < 2) cand = 2;
        std::printf("  hall_tol: derived %ld (a quarter flap), measured candidate %ld\n",
                    static_cast<long>(derived), static_cast<long>(cand));
        std::printf("  currently %ld in this image.\n", static_cast<long>(cur.hall_tol));
        if (!hall_tol_plausible(cand)) {
            std::printf("  THE SPREAD IS TOO WIDE TO ABSORB. %ld usteps is over half a\n",
                        static_cast<long>(cand));
            std::printf("  flap (%ld), and a tolerance there swallows more than half of\n",
                        static_cast<long>(ring_target_usteps(1)));
            std::printf("  every real slip. This is a MECHANICAL finding - a slipping\n");
            std::printf("  coupling, a marginal magnet, or a microstep setting that is\n");
            std::printf("  not 1/16. Do not widen the tolerance to make it go away.\n");
        } else if (cand > derived) {
            std::printf("  The drum is less repeatable than a quarter flap, so %ld would\n",
                        static_cast<long>(derived));
            std::printf("  report ordinary jitter as a major resync. Use %ld:\n",
                        static_cast<long>(cand));
            std::printf("    motion.params hall_tol=%ld   then `save`\n",
                        static_cast<long>(cand));
        } else {
            std::printf("  Repeatability is better than a quarter flap, so the measurement\n");
            std::printf("  gives no reason to widen. KEEP %ld: narrowing to the noise\n",
                        static_cast<long>(derived));
            std::printf("  floor buys earlier detection of tiny slips and pays for it\n");
            std::printf("  with false major resyncs the first warm afternoon.\n");
        }
        std::printf("  Record BOTH numbers in BRINGUP 28c step 3c - the derived one is\n");
        std::printf("  what shipped and the measured one is what the drum did.\n");
    }
    std::printf("index is now unknown - re-home before `go`\n");
    return 0;
}

// ===========================================================================
// THE SPEED LADDER  (BRINGUP 28c step 6)
// ===========================================================================
//
// `ramp <col> <r1,r2,...> <dwell_s>` - an explicit list of rates, each held
// for dwell_s seconds, CLOSED LOOP, reporting the hall figures per rung and
// stopping for a human observation before the next one.
//
// WHY A LIST AND NOT A SWEEP.  Spec 14.1 step 5 says "sweep 10 -> 25 flaps/s
// and note where flaps stop clearing cleanly", which is a shape that only
// works if the interesting thing is a threshold.  It is not: what fails first
// on a loaded drum is a CARD doing something - a double, a flutter, a late
// seat - and those are eyes-only.  A sweep gives you one number and no
// evidence; a ladder of 2, 5, 10, 20 gives four rungs you can describe and
// compare, and the gaps between them are where the behaviour changes.
//
// WHY CLOSED LOOP.  Gate 3 ran open loop because there was no Hall.  There is
// one now, so every rung goes through motion::go and therefore through the
// edge verification in spec 5.4 - which means a rung can report that the drum
// LOST REGISTRATION at that speed, which is the one failure an open-loop spin
// cannot see at all.  A rung that looks fine to the eye and reports a major
// resync is the most valuable line this command can print.
//
// NOT motion.ramp.  The dispatcher command of that name is the Calibrate
// page's INDEX WALK - it steps through ring positions at one speed.  This is
// the same word for a different thing, and it lives here rather than in the
// dispatcher for the reason the whole serial CLI does (CLAUDE.md): it is
// bench tooling that has to work before ModeManager means anything.
int cmd_ramp(int argc, char** argv) {
    if (argc != 4) {
        std::printf("usage: ramp <col> <r1,r2,...> <dwell_s>\n");
        std::printf("  e.g. ramp 0 2,5,10,20 30   - closed loop, one rung at a time\n");
        if (motion::BENCH_BUILD) {
            std::printf("  this image refuses any rung over %d flaps/s\n",
                        static_cast<int>(motion::BENCH_MAX_FLAPS_S));
        }
        return 1;
    }
    int col;
    long dwell;
    if (!parse_col(argv[1], col, false) || !parse_long(argv[3], dwell)) return 1;
    if (dwell < 1 || dwell > 600) {
        std::printf("dwell_s must be 1..600\n");
        return 1;
    }

    // Parse the list first and REFUSE THE WHOLE LADDER if any rung is over
    // the cap.  Refusing rung four after running three is worse than useless:
    // it leaves a partial result that reads like a complete one.
    int32_t rungs[MAX_RUNGS];
    int n = 0;
    const char* s = argv[2];
    while (*s != 0) {
        char* end = nullptr;
        const long v = std::strtol(s, &end, 10);
        if (end == s) {
            std::printf("cannot read a rate at \"%s\"\n", s);
            return 1;
        }
        if (n >= MAX_RUNGS) {
            std::printf("at most %d rungs\n", MAX_RUNGS);
            return 1;
        }
        if (v < 1 || v > 400) {
            std::printf("%ld flaps/s is not a sensible rate\n", v);
            return 1;
        }
        if (motion::bench_speed_refused(static_cast<int32_t>(v))) {
            std::printf("REFUSED: rung %ld is over this image's cap of %d flaps/s.\n",
                        v, static_cast<int>(motion::BENCH_MAX_FLAPS_S));
            std::printf("  Nothing was run. The cap is compiled in; rebuild with\n");
            std::printf("  -DSWAN_BENCH_CAP=<n> if the mechanism has changed.\n");
            return 1;
        }
        rungs[n++] = static_cast<int32_t>(v);
        s = end;
        while (*s == ',' || *s == ' ') ++s;
    }
    if (n == 0) {
        std::printf("no rates given\n");
        return 1;
    }

    AxisInfo a{};
    motion::info(col, a);
    if (!a.hall_valid) {
        // Refused rather than quietly falling back to open loop.  An
        // open-loop ladder reports no edge errors and no resyncs, and four
        // rungs of zeroes read exactly like four clean rungs.  That is the
        // same lie bench.cpp had to be taught not to tell on 2026-09-11.
        std::printf("REFUSED: column %d has no hall reference.\n", col);
        std::printf("  This ladder is CLOSED LOOP - that is the point of running it\n");
        std::printf("  now rather than at gate 3. `home %d` first.\n", col);
        return 1;
    }

    const MotionParams saved = motion::params();
    std::printf("\n  SPEED LADDER, column %d, %d rung%s, %ld s each\n", col, n,
                n == 1 ? "" : "s", dwell);
    std::printf("  Closed loop: every rung is graded by the edge verification.\n");
    std::printf("  WATCH THE CARDS. Doubles and flutter are eyes-only - no\n");
    std::printf("  counter in this firmware can see a card that did not seat.\n\n");

    int rc = 0;
    for (int i = 0; i < n; ++i) {
        MotionParams pr = saved;
        pr.flaps_s_normal = rungs[i];
        if (!motion::set_params(pr)) {
            std::printf("  rung %d (%ld flaps/s) REFUSED; ladder stopped.\n", i + 1,
                        static_cast<long>(rungs[i]));
            rc = 1;
            break;
        }

        AxisInfo b0{};
        motion::info(col, b0);
        const uint32_t rev0 = b0.revs, min0 = b0.resync_minor, maj0 = b0.resync_major;
        const uint32_t flt0 = b0.faults, flip0 = b0.flips_total;
        int32_t worst = 0;
        int index = b0.index >= 0 ? b0.index : 0;

        std::printf("  --- rung %d of %d: %ld flaps/s ---\n", i + 1, n,
                    static_cast<long>(rungs[i]));
        const int64_t until = esp_timer_get_time() + static_cast<int64_t>(dwell) * 1000000;
        bool faulted = false;
        while (esp_timer_get_time() < until) {
            AxisInfo c{};
            motion::info(col, c);
            if (c.state == AxisState::Fault) {
                std::printf("  FAULTED at %ld flaps/s: %s\n",
                            static_cast<long>(rungs[i]), fault_cause_name(c.fault_cause));
                faulted = true;
                break;
            }
            const int32_t e = c.last_hall_err < 0 ? -c.last_hall_err : c.last_hall_err;
            if (e > worst) worst = e;
            if (c.state == AxisState::Idle) {
                index = (index + 1) % RING_SLOT_COUNT;
                motion::go(col, index);
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        motion::stop(col);

        AxisInfo b1{};
        motion::info(col, b1);
        // MISSED STEPS, stated for what it is: the difference between the
        // usteps the DDA issued and the distance the hall says the drum
        // actually covered.  It is only meaningful over whole revolutions,
        // so a rung that saw no edge reports n/a rather than a number.
        const uint32_t revs = b1.revs - rev0;
        const uint32_t flips = b1.flips_total - flip0;
        std::printf("    flips %lu   drum revolutions %lu\n",
                    static_cast<unsigned long>(flips), static_cast<unsigned long>(revs));
        if (revs > 0) {
            std::printf("    hall_to_hall %ld   worst |err| %ld usteps\n",
                        static_cast<long>(b1.hall_to_hall), static_cast<long>(worst));
            const long missed = static_cast<long>(flips) * USTEPS_PER_FLAP_NUM -
                                static_cast<long>(revs) * USTEPS_PER_SPOOL_REV_NOMINAL;
            std::printf("    issued - covered  %+ld usteps  (%+.2f flaps)\n", missed,
                        static_cast<double>(missed) / USTEPS_PER_FLAP_NUM);
        } else {
            std::printf("    no edge this rung - hall figures n/a\n");
        }
        std::printf("    resyncs %lu minor, %lu major   faults %lu\n",
                    static_cast<unsigned long>(b1.resync_minor - min0),
                    static_cast<unsigned long>(b1.resync_major - maj0),
                    static_cast<unsigned long>(b1.faults - flt0));
        if (faulted) {
            rc = 1;
            break;
        }

        // THE PROMPT.  The firmware has said everything it can; the rest of
        // this rung is in somebody's eyes, and asking for it BEFORE the next
        // rung is what makes the answer specific to this speed.  Spec 17,
        // 2026-08-23: the firmware detects a stall and cannot detect a card
        // fluttering or failing to seat.
        std::printf("\n    WATCH THE CARDS AT THIS SPEED, then record:\n");
        std::printf("      finger behaviour  clean / hesitant / dragging\n");
        std::printf("      doubles           none / occasional / frequent\n");
        std::printf("      seating           flat / late / bouncing\n");
        if (i + 1 < n) {
            std::printf("\n    ENTER to run rung %d (%ld flaps/s), or `q` to stop.\n",
                        i + 2, static_cast<long>(rungs[i + 1]));
            if (!wait_for_enter()) {
                std::printf("    stopped after rung %d.\n", i + 1);
                break;
            }
        }
    }

    if (!motion::set_params(saved)) {
        std::printf("  could not restore the original speeds - check `stats`.\n");
        rc = 1;
    }
    std::printf("\n  Ladder done. Speeds restored (%ld flaps/s normal).\n",
                static_cast<long>(saved.flaps_s_normal));
    std::printf("  The index is where the last rung left it; `home %d` to be sure.\n", col);
    return rc;
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

// on|off|1|0|true|false|yes|no, case-insensitive.  FALSE means "not one of
// those", so a caller can refuse instead of guessing - which matters wherever
// the two directions are not equally safe.
bool parse_onoff(const char* s, bool& out) {
    if (s == nullptr || *s == '\0') return false;
    char b[8] = {};
    size_t n = 0;
    while (s[n] != '\0') {
        if (n >= sizeof(b) - 1) return false;  // longer than any token we take
        b[n] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[n])));
        ++n;
    }
    if (std::strcmp(b, "on") == 0 || std::strcmp(b, "1") == 0 ||
        std::strcmp(b, "true") == 0 || std::strcmp(b, "yes") == 0) {
        out = true;
        return true;
    }
    if (std::strcmp(b, "off") == 0 || std::strcmp(b, "0") == 0 ||
        std::strcmp(b, "false") == 0 || std::strcmp(b, "no") == 0) {
        out = false;
        return true;
    }
    return false;
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
    const bool was = c.maintenance;
    // PARSE STRICTLY, AND REFUSE WHAT IS NOT RECOGNISED.  This used to accept
    // only a non-zero number or the exact lowercase "on", so every other token
    // - "ON", "True", "yes", a typo - silently became `maint off`.  The
    // capitalisation is the firmware's OWN: `maint` prints "maintenance: ON"
    // and the confirmation reads "maintenance ON - nothing moves on its own",
    // so `maint ON` is exactly what somebody mirroring the console types.
    // That was survivable while leaving maintenance posted no Home.  Now that
    // it correctly re-homes, the same typo turns a drum at the vise while the
    // person believes they have just ENTERED maintenance - which is the state
    // 5.9 and the park-pin procedure require before hands go near the
    // mechanism.  Defaulting an unrecognised word to the moving direction is
    // the wrong way round.
    bool on = false;
    if (!parse_onoff(argv[1], on)) {
        std::printf("usage: maint on|off  (got '%s' - nothing changed)\n", argv[1]);
        return 1;
    }
    c.maintenance = on;
    motion::set_columns(c);
    MotionParams p = motion::params();
    p.maintenance = on;
    // A read-modify-write of the live params, so the only way this refuses
    // is a live value already over the cap - which config::load makes
    // impossible at boot.  Checked anyway: leaving the control core out of
    // maintenance while the console prints "maintenance on" is the exact
    // shape of the defect fixed on 2026-09-12.
    if (!motion::set_params(p)) {
        std::printf("refused - maintenance NOT applied to the control core\n");
        return 1;
    }
    g_mm->cmd_maintenance(on, g_utc_ms());
    // PERSIST IT.  Spec 5.9 makes maintenance survive a reboot deliberately -
    // pulling power mid-repair must not restart a countdown on top of your
    // hands - and that is a safety claim, not a convenience.  The dispatcher
    // path saved it (bindings.cpp) and this one did not, so `maint on` from
    // the console - which is what BRINGUP tells a bench operator to type
    // before touching a drum - was forgotten by the next boot.
    const esp_err_t merr = config::save_columns(motion::columns());

    // Leaving maintenance re-homes (spec 5.9), and THIS path posts it - the
    // same explicit home(-1) the dispatcher does, so the console and HTTP are
    // one behaviour rather than two that happen to agree in one state.
    // maintenance_exit_homes() carries the reasoning; the short version is
    // that motion::enable(true) returns early when EN is already asserted,
    // which is precisely what `maint on` then `en 1` leaves behind.
    const bool leaving = maintenance_exit_homes(was, on);
    const bool homed = leaving && motion::home(-1) == ESP_OK;

    // THREE outcomes, not two.  "re-homing" used to be printed unconditionally,
    // including when nothing had been posted at all.  But "nothing to re-home"
    // is a statement about the COLUMNS - every one disabled - and must not also
    // be printed for `maint off` typed when maintenance was already off, which
    // posts nothing because there was no transition and says nothing about any
    // column.  Two different facts that a single flag collapses into one.
    const char* tail = "";
    if (!on) {
        tail = !leaving ? " (it was already off)"
                        : (homed ? "; re-homing"
                                 : "; nothing to re-home - every column is disabled");
    }
    std::printf("maintenance %s%s%s\n", on ? "ON - nothing moves on its own" : "off",
                tail,
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
    reg("bench",
        "bench [soak <col> [min] [tick] | spin <col> <fs> <s> | stop | samples] - stand-in session",
        cmd_bench);
    reg("step", "step <col> <usteps> - open loop", cmd_step);
    reg("home", "home <col>|all", cmd_home);
    reg("go", "go <col> <index|token>", cmd_go);
    reg("spin", "spin <col> <flaps_s> <seconds>", cmd_spin);
    reg("revs", "revs <col> <n> - measure hall_to_hall", cmd_revs);
    reg("ramp", "ramp <col> <r1,r2,...> <dwell_s> - closed-loop speed ladder",
        cmd_ramp);
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
