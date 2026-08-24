// The IDF shell around the journal and the log ring: a queue, one low-priority
// writer task, and one file.
//
// Nothing in here may block a producer.  The modes task records events from
// inside its own lock (a fault, a countdown milestone, a mode change), so the
// producer side is a fixed-size copy into a queue with a zero timeout - no
// allocation, no file, no mutex that anything else holds.
#include "journal/journal.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cerrno>
#include <cstring>
#include <vector>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "journal/log_ring.h"

namespace swan {
namespace journal {
namespace {

constexpr const char* TAG = "journal";
constexpr const char* PATH = "/fs/journal.jsonl";
constexpr const char* TMP = "/fs/journal.tmp";

// 8 KB of internal RAM.  See the note in log_ring.h for why this is a byte
// budget rather than spec 12's "~200 lines".
constexpr std::size_t LOG_BYTES = 8192;
DRAM_ATTR char g_log_store[LOG_BYTES];
LogRing g_log(g_log_store, LOG_BYTES);
portMUX_TYPE g_log_lock = portMUX_INITIALIZER_UNLOCKED;
vprintf_like_t g_prev_vprintf = nullptr;

// 24 events is about a minute of the worst realistic burst (five columns
// faulting and recovering during a countdown).  Full means DROP, counted.
constexpr UBaseType_t QUEUE_LEN = 24;
QueueHandle_t g_queue = nullptr;
TaskHandle_t g_task = nullptr;

RotationPolicy g_policy;
std::atomic<uint32_t> g_dropped{0};
std::atomic<uint32_t> g_rotations{0};
std::atomic<bool> g_writable{false};

int64_t now_uptime_s() { return esp_timer_get_time() / 1000000; }

// ---------------------------------------------------------------------------
// The log ring, behind esp_log_set_vprintf
// ---------------------------------------------------------------------------
int log_vprintf(const char* fmt, va_list args) {
    // Format once into a stack buffer.  The console still gets the original
    // through the previous handler, so nothing is lost by capturing.
    char line[LOG_LINE_MAX + 1];
    va_list copy;
    va_copy(copy, args);
    const int n = vsnprintf(line, sizeof line, fmt, copy);
    va_end(copy);
    if (n > 0) {
        portENTER_CRITICAL(&g_log_lock);
        g_log.push(line, static_cast<std::size_t>(n) < sizeof line
                             ? static_cast<std::size_t>(n)
                             : sizeof line - 1);
        portEXIT_CRITICAL(&g_log_lock);
    }
    return g_prev_vprintf != nullptr ? g_prev_vprintf(fmt, args) : n;
}

// ---------------------------------------------------------------------------
// The file
// ---------------------------------------------------------------------------
std::size_t count_lines(std::size_t* bytes_out) {
    std::FILE* f = std::fopen(PATH, "rb");
    if (f == nullptr) {
        if (bytes_out != nullptr) *bytes_out = 0;
        return 0;
    }
    std::size_t lines = 0, bytes = 0;
    char buf[256];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        bytes += n;
        for (std::size_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') ++lines;
        }
    }
    std::fclose(f);
    if (bytes_out != nullptr) *bytes_out = bytes;
    return lines;
}

void rotate() {
    std::FILE* f = std::fopen(PATH, "rb");
    if (f == nullptr) return;
    std::vector<std::string> lines;
    std::string cur;
    char buf[256];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        for (std::size_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                if (!cur.empty()) lines.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(buf[i]);
            }
        }
    }
    std::fclose(f);
    // A trailing partial line is a power cut mid-append.  Dropping it is the
    // whole reason the format is one self-contained object per line.
    const std::string keep = compact(g_policy, lines);

    std::FILE* t = std::fopen(TMP, "wb");
    if (t == nullptr) {
        ESP_LOGW(TAG, "rotate: cannot open %s", TMP);
        return;
    }
    const bool ok = std::fwrite(keep.data(), 1, keep.size(), t) == keep.size();
    std::fclose(t);
    if (!ok) {
        std::remove(TMP);
        ESP_LOGW(TAG, "rotate: short write, keeping the old file");
        return;
    }
    // LittleFS rename replaces atomically (spec 17, 2026-08-23): do NOT remove
    // the destination first, or a brownout in that window loses the journal.
    if (std::rename(TMP, PATH) != 0) {
        ESP_LOGW(TAG, "rotate: rename failed (%d)", errno);
        std::remove(TMP);
        return;
    }
    g_rotations.fetch_add(1, std::memory_order_relaxed);
    ESP_LOGI(TAG, "journal rotated: kept the newest %u of %u entries",
             static_cast<unsigned>(g_policy.keep_entries), static_cast<unsigned>(lines.size()));
}

void writer_task(void*) {
    std::size_t entries = 0, bytes = 0;
    entries = count_lines(&bytes);
    g_writable.store(true, std::memory_order_relaxed);

    for (;;) {
        Event e{};
        // Block here, not in the producer.  A 2 s wake-up also flushes a batch
        // that arrived just before the queue went quiet.
        if (xQueueReceive(g_queue, &e, pdMS_TO_TICKS(2000)) != pdTRUE) continue;

        std::string batch = encode(e);
        // Drain whatever else is waiting: one open-append-close for a burst
        // rather than one per event, which is what makes this wear-aware.
        while (xQueueReceive(g_queue, &e, 0) == pdTRUE) batch += encode(e);

        std::FILE* f = std::fopen(PATH, "ab");
        if (f == nullptr) {
            g_writable.store(false, std::memory_order_relaxed);
            ESP_LOGW(TAG, "cannot append to %s", PATH);
            continue;
        }
        const bool ok = std::fwrite(batch.data(), 1, batch.size(), f) == batch.size();
        std::fclose(f);
        if (!ok) {
            ESP_LOGW(TAG, "short write; the entry is lost but the file is intact");
            continue;
        }
        g_writable.store(true, std::memory_order_relaxed);

        for (const char c : batch) {
            if (c == '\n') ++entries;
        }
        bytes += batch.size();
        if (needs_rotation(g_policy, entries, bytes)) {
            rotate();
            entries = count_lines(&bytes);
        }
    }
}

}  // namespace

void init() {
    if (g_queue != nullptr) return;
    g_queue = xQueueCreate(QUEUE_LEN, sizeof(Event));
    if (g_queue == nullptr) {
        ESP_LOGE(TAG, "no queue; the journal is disabled this boot");
        return;
    }
    // Priority 1: below the modes task (5), below httpd (3), above idle.  A
    // journal write is never the most important thing happening.
    xTaskCreate(&writer_task, "swan_journal", 4096, nullptr, 1, &g_task);
}

bool record(const Event& e) {
    if (g_queue == nullptr) return false;
    Event copy = e;
    if (copy.uptime_s == 0) copy.uptime_s = static_cast<uint32_t>(now_uptime_s());
    // Zero timeout, always.  This is called from the modes task inside its own
    // lock; blocking here would stop the display to write a log line about it.
    if (xQueueSend(g_queue, &copy, 0) != pdTRUE) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

bool note_countdown(Event::Kind k, int64_t utc_s, uint32_t seq, const char* who,
                    const char* numbers) {
    Event e{};
    e.kind = k;
    e.utc_s = utc_s;
    e.seq = seq;
    if (who != nullptr) std::snprintf(e.who, sizeof e.who, "%s", who);
    if (numbers != nullptr) std::snprintf(e.detail, sizeof e.detail, "%s", numbers);
    return record(e);
}

bool note_fault(int64_t utc_s, int column, const char* cause) {
    Event e{};
    e.kind = Event::Kind::Fault;
    e.utc_s = utc_s;
    e.column = static_cast<int8_t>(column);
    if (cause != nullptr) std::snprintf(e.detail, sizeof e.detail, "%s", cause);
    return record(e);
}

bool note_recover(int64_t utc_s, int column, int attempts) {
    Event e{};
    e.kind = Event::Kind::Recover;
    e.utc_s = utc_s;
    e.column = static_cast<int8_t>(column);
    std::snprintf(e.detail, sizeof e.detail, "after %d", attempts);
    return record(e);
}

bool note_mode(int64_t utc_s, const char* mode) {
    Event e{};
    e.kind = Event::Kind::ModeChange;
    e.utc_s = utc_s;
    if (mode != nullptr) std::snprintf(e.detail, sizeof e.detail, "%s", mode);
    return record(e);
}

std::string read(std::size_t max_lines) {
    std::FILE* f = std::fopen(PATH, "rb");
    if (f == nullptr) return {};
    std::vector<std::string> lines;
    std::string cur;
    char buf[256];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        for (std::size_t i = 0; i < n; ++i) {
            if (buf[i] == '\n') {
                if (!cur.empty()) lines.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(buf[i]);
            }
        }
    }
    std::fclose(f);
    const std::size_t from =
        (max_lines != 0 && lines.size() > max_lines) ? lines.size() - max_lines : 0;
    std::string out;
    for (std::size_t i = from; i < lines.size(); ++i) {
        out += lines[i];
        out.push_back('\n');
    }
    return out;
}

Stats stats() {
    Stats s;
    std::size_t bytes = 0;
    s.entries = count_lines(&bytes);
    s.bytes = bytes;
    s.dropped = g_dropped.load(std::memory_order_relaxed);
    s.rotations = g_rotations.load(std::memory_order_relaxed);
    s.writable = g_writable.load(std::memory_order_relaxed);
    return s;
}

void log_capture_start() {
    if (g_prev_vprintf != nullptr) return;
    g_prev_vprintf = esp_log_set_vprintf(&log_vprintf);
}

std::string log_read(std::size_t max_bytes) {
    // The ring is written from a critical section; copy under it and format
    // outside, so a log read never delays a log write.
    portENTER_CRITICAL(&g_log_lock);
    std::string out = g_log.read(max_bytes);
    portEXIT_CRITICAL(&g_log_lock);
    return out;
}

void log_clear() {
    portENTER_CRITICAL(&g_log_lock);
    g_log.clear();
    portEXIT_CRITICAL(&g_log_lock);
}

LogStats log_stats() {
    LogStats s;
    portENTER_CRITICAL(&g_log_lock);
    s.lines = g_log.lines();
    s.used = g_log.used();
    s.capacity = g_log.capacity();
    s.dropped = g_log.dropped();
    portEXIT_CRITICAL(&g_log_lock);
    return s;
}

}  // namespace journal
}  // namespace swan
