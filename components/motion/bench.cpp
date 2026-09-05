// The stand-in bench session — IDF shell.  See bench.h for what it is for.
//
// Deliberately its own file and its own task rather than a mode: modes render
// time, and this is a duty cycle being held for an hour so somebody can put a
// hand on a motor case.  It drives motion:: directly, one flap at a time,
// closed loop, which is the path with the edge verification in it.
#include "motion/bench.h"

#include <atomic>
#include <cstdio>
#include <mutex>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "motion/bench_policy.h"
#include "motion/motion.h"
#include "ring/geometry.h"
#include "ring/ring.h"

namespace swan {
namespace motion {
namespace {

constexpr const char* TAG = "bench";

std::mutex g_mu;
BenchStats g_stats;
BenchSchedule g_sched;
std::atomic<bool> g_running{false};
std::atomic<bool> g_stop{false};
TaskHandle_t g_task = nullptr;

// Baselines, so the report is what happened DURING the run rather than since
// boot - a column carrying three resyncs from this morning is not news.
uint32_t g_base_minor = 0, g_base_major = 0, g_base_faults = 0;
uint32_t g_base_revs = 0, g_base_flips = 0;

uint32_t heap_now() {
    return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_8BIT));
}

void sample_locked(int col, int64_t started_us) {
    AxisInfo a{};
    motion::info(col, a);
    g_stats.elapsed_s =
        static_cast<uint32_t>((esp_timer_get_time() - started_us) / 1000000);
    g_stats.flaps = a.flips_total - g_base_flips;
    g_stats.edges = a.revs - g_base_revs;
    g_stats.resync_minor = a.resync_minor - g_base_minor;
    g_stats.resync_major = a.resync_major - g_base_major;
    g_stats.faults = a.faults - g_base_faults;
    if (a.hall_to_hall != 0) {
        if (g_stats.h2h_min == 0 || a.hall_to_hall < g_stats.h2h_min) {
            g_stats.h2h_min = a.hall_to_hall;
        }
        if (a.hall_to_hall > g_stats.h2h_max) g_stats.h2h_max = a.hall_to_hall;
    }
    const int32_t err = a.last_hall_err < 0 ? -a.last_hall_err : a.last_hall_err;
    if (err > g_stats.err_abs_max) g_stats.err_abs_max = err;
    g_stats.heap_now = heap_now();
    if (g_stats.heap_now < g_stats.heap_min) g_stats.heap_min = g_stats.heap_now;
}

// The closing report.  Printed rather than logged, because a person is reading
// it at a vise with a hand on a warm motor, and it ends in a question that
// BRINGUP §28b has a blank for.
void print_verdict_prompt(const BenchStats& s, const BenchSchedule& sched) {
    const unsigned el = static_cast<unsigned>(s.elapsed_s);
    const unsigned tot = static_cast<unsigned>(s.total_s);
    const unsigned flaps = static_cast<unsigned>(s.flaps);
    const unsigned want = static_cast<unsigned>(bench_expected_flaps(sched));
    const unsigned revs = static_cast<unsigned>(s.edges);
    const unsigned minor = static_cast<unsigned>(s.resync_minor);
    const unsigned major = static_cast<unsigned>(s.resync_major);
    const unsigned faults = static_cast<unsigned>(s.faults);
    const unsigned h0 = static_cast<unsigned>(s.heap_start);
    const unsigned h1 = static_cast<unsigned>(s.heap_now);
    const unsigned h2 = static_cast<unsigned>(s.heap_min);
    const int lo = static_cast<int>(s.h2h_min);
    const int hi = static_cast<int>(s.h2h_max);
    const int worst = static_cast<int>(s.err_abs_max);

    std::printf("\n");
    std::printf("=====================================================\n");
    std::printf("  STAND-IN BENCH SOAK - %s\n", s.completed ? "COMPLETE" : "STOPPED EARLY");
    std::printf("=====================================================\n");
    std::printf("  column %d, %u of %u s, %u flaps (expected %u)\n", s.column, el, tot,
                flaps, want);
    std::printf("  drum revolutions   %u\n", revs);
    std::printf("  hall_to_hall       %d..%d   (3200 exactly is the direct drive)\n", lo, hi);
    std::printf("  worst edge error   %d usteps\n", worst);
    std::printf("  resyncs            %u minor, %u major\n", minor, major);
    std::printf("  faults             %u\n", faults);
    std::printf("  heap               %u start, %u now, %u min\n", h0, h1, h2);
    if (!s.completed) {
        std::printf("  stopped because    %s\n", s.stopped_because);
        std::printf("\n  A SHORT RUN IS NOT A SHORTER ANSWER - it is no answer.\n");
        std::printf("  The heat question needs the full duration.  Run it again.\n");
        std::printf("=====================================================\n\n");
        return;
    }
    std::printf("\n");
    std::printf("  NOW PUT A HAND ON THE MOTOR CASE.\n");
    std::printf("\n");
    std::printf("  The motor is sealed inside a PLA drum that softens at\n");
    std::printf("  55-60 C.  There is no temperature sensor in this build,\n");
    std::printf("  on purpose - your hand is the instrument.\n");
    std::printf("\n");
    std::printf("    comfortable to hold           -> well inside margin\n");
    std::printf("    hot but you can keep it there -> around 45-50 C, marginal\n");
    std::printf("    you snatch your hand away     -> FAIL, go to UART/IHOLD\n");
    std::printf("\n");
    std::printf("  Record the verdict AND the Vref you measured in\n");
    std::printf("  docs/BRINGUP.md 28b gate 3.  An unrecorded verdict is a\n");
    std::printf("  run that has to happen twice.\n");
    std::printf("=====================================================\n\n");
}

void bench_task(void* arg) {
    const int col = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    const int64_t started_us = esp_timer_get_time();
    BenchSchedule sched;
    {
        const std::lock_guard<std::mutex> lk(g_mu);
        sched = g_sched;
    }

    ESP_LOGW(TAG, "stand-in soak: column %d, %u s, one flap every %u s", col,
             static_cast<unsigned>(sched.total_s),
             static_cast<unsigned>(sched.tick_s));
    ESP_LOGW(TAG, "the coils hold for >93%% of this run - that is the test");

    uint32_t last_flap_s = 0;
    bool any_flap = false;
    uint32_t last_report_s = 0;
    int index = 0;

    while (!g_stop.load(std::memory_order_relaxed)) {
        const uint32_t elapsed =
            static_cast<uint32_t>((esp_timer_get_time() - started_us) / 1000000);

        if (bench_run_over(sched, elapsed)) {
            const std::lock_guard<std::mutex> lk(g_mu);
            g_stats.completed = true;
            g_stats.stopped_because = "finished";
            break;
        }

        if (bench_flap_due(sched, elapsed, last_flap_s, any_flap)) {
            // ONE FLAP FORWARD.  The ring is descending, so this is also the
            // direction a countdown ticks - the bench is exercising the real
            // sense, not an arbitrary one.
            index = (index + 1) % RING_SLOT_COUNT;
            motion::go(col, index);
            last_flap_s = elapsed;
            any_flap = true;
        }

        {
            const std::lock_guard<std::mutex> lk(g_mu);
            sample_locked(col, started_us);
        }

        // A line a minute: enough to see the shape of an hour in a scrollback,
        // few enough that the console is usable while it runs.
        if (elapsed >= last_report_s + 60) {
            last_report_s = elapsed;
            const std::lock_guard<std::mutex> lk(g_mu);
            ESP_LOGI(TAG, "%u/%u s  flaps=%u revs=%u h2h=%d..%d minor=%u major=%u heap=%u",
                     static_cast<unsigned>(g_stats.elapsed_s),
                     static_cast<unsigned>(g_stats.total_s),
                     static_cast<unsigned>(g_stats.flaps),
                     static_cast<unsigned>(g_stats.edges),
                     static_cast<int>(g_stats.h2h_min),
                     static_cast<int>(g_stats.h2h_max),
                     static_cast<unsigned>(g_stats.resync_minor),
                     static_cast<unsigned>(g_stats.resync_major),
                     static_cast<unsigned>(g_stats.heap_now));
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    BenchStats final_stats;
    {
        const std::lock_guard<std::mutex> lk(g_mu);
        sample_locked(col, started_us);
        g_stats.running = false;
        final_stats = g_stats;
    }
    g_running.store(false, std::memory_order_relaxed);
    print_verdict_prompt(final_stats, sched);
    g_task = nullptr;
    vTaskDelete(nullptr);
}

bool column_drivable(int col) {
    if (col < 0 || col >= N_COLUMNS) return false;
    // A SIMULATED COLUMN IS REFUSED, and this is the whole point of the build.
    // Everything in this repository since 2026-08-23 has run against modelled
    // drums; this session exists to put current through a real motor and feel
    // the case afterwards.  A modelled drum would produce a beautiful hour of
    // logs and answer nothing, and it would answer nothing SILENTLY.
    const ColumnConfig cc = motion::columns();
    if (cc.mode[col] != ColumnMode::Real) {
        ESP_LOGE(TAG, "column %d is not REAL - the stand-in test needs a real "
                      "motor, a real driver and a real hall.  `col %d real` first.",
                 col, col);
        return false;
    }
    AxisInfo a{};
    motion::info(col, a);
    return a.state != AxisState::Fault;
}

}  // namespace

bool bench_soak_start(int column, const BenchSchedule& s) {
    if (!BENCH_BUILD) {
        ESP_LOGE(TAG, "not a bench build; rebuild with -DSWAN_BENCH=ON");
        return false;
    }
    if (g_running.load(std::memory_order_relaxed)) return false;
    if (!column_drivable(column)) return false;
    if (!motion::is_enabled()) {
        ESP_LOGE(TAG, "drivers are released; the coils must be energised for this");
        return false;
    }

    AxisInfo a{};
    motion::info(column, a);
    {
        const std::lock_guard<std::mutex> lk(g_mu);
        g_sched = s;
        g_stats = BenchStats{};
        g_stats.running = true;
        g_stats.column = column;
        g_stats.total_s = s.total_s;
        g_stats.heap_start = heap_now();
        g_stats.heap_now = g_stats.heap_start;
        g_stats.heap_min = g_stats.heap_start;
        g_base_minor = a.resync_minor;
        g_base_major = a.resync_major;
        g_base_faults = a.faults;
        g_base_revs = a.revs;
        g_base_flips = a.flips_total;
    }
    g_stop.store(false, std::memory_order_relaxed);
    g_running.store(true, std::memory_order_relaxed);
    // Priority 1: below the modes task and far below the motion tick.  This
    // thing runs for an hour and must never be the reason a step is late.
    if (xTaskCreate(bench_task, "bench", 4096,
                    reinterpret_cast<void*>(static_cast<intptr_t>(column)), 1,
                    &g_task) != pdPASS) {
        g_running.store(false, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool bench_spin_start(int column, int32_t flaps_s, int seconds) {
    if (!BENCH_BUILD) {
        ESP_LOGE(TAG, "not a bench build; rebuild with -DSWAN_BENCH=ON");
        return false;
    }
    if (!column_drivable(column)) return false;
    // REFUSED, not clamped.  A spin asks for a specific speed for a specific
    // reason; running it eight times slower than asked without saying so would
    // be its own kind of lie.
    if (bench_speed_refused(flaps_s)) {
        ESP_LOGE(TAG,
                 "%d flaps/s refused: the stand-in axle is PRINTED PLA and this "
                 "build caps at %d flaps/s (1 drum rev/s).  The %d flaps/s show "
                 "spin is not available in a bench image at any setting.",
                 static_cast<int>(flaps_s), static_cast<int>(BENCH_MAX_FLAPS_S),
                 static_cast<int>(SHOW_SPIN_FLAPS_S));
        return false;
    }
    // The slow inspection spin is open-loop stepping, like the CLI's `spin`:
    // it leaves the displayed index unknown, which is correct and expected -
    // nothing on this bench is displaying anything.
    const int64_t usteps = static_cast<int64_t>(flaps_s) * seconds * USTEPS_PER_FLAP_NUM;
    return motion::step_open_loop(column, usteps, flaps_s) == ESP_OK;
}

void bench_stop(const char* why) {
    if (!g_running.load(std::memory_order_relaxed)) return;
    {
        const std::lock_guard<std::mutex> lk(g_mu);
        g_stats.stopped_because = why ? why : "stopped";
    }
    g_stop.store(true, std::memory_order_relaxed);
}

bool bench_running() { return g_running.load(std::memory_order_relaxed); }

BenchStats bench_report() {
    const std::lock_guard<std::mutex> lk(g_mu);
    return g_stats;
}

}  // namespace motion
}  // namespace swan
