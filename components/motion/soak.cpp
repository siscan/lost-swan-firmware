#include "motion/soak.h"

#include <atomic>
#include <cstring>
#include <mutex>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "journal/journal.h"
#include "motion/motion.h"
#include "ring/ring.h"

namespace swan {
namespace motion {
namespace {

constexpr const char* TAG = "soak";

std::mutex g_mu;
SoakReport g_rep;
std::atomic<bool> g_running{false};
std::atomic<bool> g_stop{false};
TaskHandle_t g_task = nullptr;

// Baselines, so the report shows what happened DURING the soak rather than
// since boot - a column with 3 resyncs from this morning is not news.
uint32_t g_base_minor[N_COLUMNS] = {};
uint32_t g_base_major[N_COLUMNS] = {};
uint32_t g_base_faults[N_COLUMNS] = {};
uint32_t g_base_revs[N_COLUMNS] = {};
uint32_t g_base_flips[N_COLUMNS] = {};

uint32_t heap_now() {
    return static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_8BIT));
}

void sample_locked(int64_t started_us) {
    const uint32_t h = heap_now();
    g_rep.heap_now = h;
    if (h < g_rep.heap_min || g_rep.heap_min == 0) g_rep.heap_min = h;
    g_rep.elapsed_s = static_cast<uint32_t>((esp_timer_get_time() - started_us) / 1000000);
    ++g_rep.samples;

    for (int i = 0; i < N_COLUMNS; ++i) {
        AxisInfo a;
        info(i, a);
        SoakColumn& c = g_rep.col[i];
        c.resync_minor = a.resync_minor - g_base_minor[i];
        c.resync_major = a.resync_major - g_base_major[i];
        c.faults = a.faults - g_base_faults[i];
        c.wraps = a.revs - g_base_revs[i];
        c.flips = a.flips_total - g_base_flips[i];
        if (a.hall_to_hall > 0) {
            if (c.h2h_min == 0 || a.hall_to_hall < c.h2h_min) c.h2h_min = a.hall_to_hall;
            if (a.hall_to_hall > c.h2h_max) c.h2h_max = a.hall_to_hall;
        }
        const int32_t e = a.last_hall_err < 0 ? -a.last_hall_err : a.last_hall_err;
        if (e > c.err_abs_max) c.err_abs_max = e;
    }
}

bool any_driveable() {
    for (int i = 0; i < N_COLUMNS; ++i) {
        AxisInfo a;
        info(i, a);
        if (a.mode != ColumnMode::Disabled) return true;
    }
    return false;
}

void soak_task(void*) {
    const int64_t started = esp_timer_get_time();
    int64_t last_log = started;
    uint32_t min_wraps = 0;

    ESP_LOGW(TAG, "soak started: %u wraps at %d flaps/s",
             static_cast<unsigned>(g_rep.target_wraps), static_cast<int>(g_rep.flaps_s));

    while (!g_stop.load(std::memory_order_relaxed)) {
        for (int i = 0; i < N_COLUMNS; ++i) {
            AxisInfo a;
            info(i, a);
            if (a.mode == ColumnMode::Disabled) continue;
            // Idle and homed: give it the next slot.  One flip at a time is
            // deliberate - it is the closed-loop path with the edge check in
            // it, and a whole-revolution open-loop spin would prove nothing
            // about registration.
            if (a.state == AxisState::Idle && ring_index_valid(a.index)) {
                go(i, (a.index + 1) % RING_SLOT_COUNT);
            } else if (a.state == AxisState::Fault) {
                // A fault during a soak is the RESULT, not an interruption:
                // record it and re-home so the run continues and the count of
                // faults means something.
                home(i);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));

        {
            const std::lock_guard<std::mutex> lock(g_mu);
            sample_locked(started);
            min_wraps = g_rep.col[0].wraps;
            for (int i = 1; i < N_COLUMNS; ++i) {
                AxisInfo a;
                info(i, a);
                if (a.mode == ColumnMode::Disabled) continue;
                if (g_rep.col[i].wraps < min_wraps) min_wraps = g_rep.col[i].wraps;
            }
        }

        // A line an hour, so an overnight console log is readable in the
        // morning rather than being the whole night at 50 Hz.
        const int64_t now = esp_timer_get_time();
        if (now - last_log >= 3600LL * 1000000LL) {
            last_log = now;
            const std::lock_guard<std::mutex> lock(g_mu);
            ESP_LOGW(TAG, "soak %u h: wraps %u, heap %u (min %u), resyncs %u/%u, faults %u",
                     static_cast<unsigned>(g_rep.elapsed_s / 3600),
                     static_cast<unsigned>(min_wraps), static_cast<unsigned>(g_rep.heap_now),
                     static_cast<unsigned>(g_rep.heap_min),
                     static_cast<unsigned>(g_rep.col[0].resync_minor),
                     static_cast<unsigned>(g_rep.col[0].resync_major),
                     static_cast<unsigned>(g_rep.col[0].faults));
        }

        if (g_rep.target_wraps != 0 && min_wraps >= g_rep.target_wraps) {
            soak_stop("reached the target");
            break;
        }
    }

    {
        const std::lock_guard<std::mutex> lock(g_mu);
        sample_locked(started);
        g_rep.running = false;
        ESP_LOGW(TAG, "soak ended (%s): %u wraps in %u s, heap %u -> %u (min %u)",
                 g_rep.stopped_because, static_cast<unsigned>(min_wraps),
                 static_cast<unsigned>(g_rep.elapsed_s), static_cast<unsigned>(g_rep.heap_start),
                 static_cast<unsigned>(g_rep.heap_now), static_cast<unsigned>(g_rep.heap_min));
    }
    g_running.store(false, std::memory_order_relaxed);
    g_task = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

bool soak_start(uint32_t wraps, int32_t flaps_s) {
    if (g_running.load(std::memory_order_relaxed)) return false;
    if (!any_driveable()) return false;

    {
        const std::lock_guard<std::mutex> lock(g_mu);
        g_rep = SoakReport{};
        g_rep.running = true;
        g_rep.target_wraps = wraps;
        g_rep.flaps_s = flaps_s;
        g_rep.heap_start = g_rep.heap_now = g_rep.heap_min = heap_now();
        g_rep.stopped_because = "";
        for (int i = 0; i < N_COLUMNS; ++i) {
            AxisInfo a;
            info(i, a);
            g_base_minor[i] = a.resync_minor;
            g_base_major[i] = a.resync_major;
            g_base_faults[i] = a.faults;
            g_base_revs[i] = a.revs;
            g_base_flips[i] = a.flips_total;
        }
    }
    if (flaps_s > 0) {
        MotionParams p = params();
        p.flaps_s_normal = flaps_s;
        set_params(p);
    }

    g_stop.store(false, std::memory_order_relaxed);
    g_running.store(true, std::memory_order_relaxed);
    // Priority 2: below the modes task, above idle.  It is a driver, not a
    // deadline.
    if (xTaskCreate(&soak_task, "swan_soak", 4096, nullptr, 2, &g_task) != pdPASS) {
        g_running.store(false, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void soak_stop(const char* why) {
    if (!g_running.load(std::memory_order_relaxed)) return;
    {
        const std::lock_guard<std::mutex> lock(g_mu);
        g_rep.stopped_because = why != nullptr ? why : "stopped";
    }
    g_stop.store(true, std::memory_order_relaxed);
}

bool soak_running() { return g_running.load(std::memory_order_relaxed); }

SoakReport soak_report() {
    const std::lock_guard<std::mutex> lock(g_mu);
    return g_rep;
}

}  // namespace motion
}  // namespace swan
