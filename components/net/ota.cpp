#include "net/ota.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include <sys/time.h>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/boot_health.h"
#include "motion/motion.h"
#include "net/mqtt.h"
#include "ring/json_write.h"

namespace swan {
namespace net {
namespace {

constexpr const char* TAG = "ota";

// 4 KB: one flash sector.  With OTA_WITH_SEQUENTIAL_WRITES esp_ota_write erases
// one sector per chunk, interleaved with recv, instead of erasing the whole
// 2.5 MB partition up front - which would be ~640 back-to-back erases with the
// cache disabled, around twenty seconds in which NO task runs.  That is longer
// than the task watchdog now tolerates, and the watchdog is deliberately set to
// panic (spec 10.4), so the sequential form is not a preference.
constexpr size_t CHUNK = 4096;
// TWO bounds, because they answer different questions.
//
// The total budget has to be generous: a 1.5 MB image over weak WiFi is a
// legitimately long transfer (5.8 s on a good LAN, measured).  But there is one
// httpd task, and while this handler runs nothing else is served and the
// display is held - so a client that has simply STOPPED must not get the full
// budget.  Progress resets the stall bound; the total is never reset.
constexpr int64_t RECV_TOTAL_MS = 120000;
constexpr int64_t RECV_STALL_MS = 10000;
// ... and a floor, because a client trickling one byte at a time is technically
// making progress and would otherwise get the whole 120 s.  A real 1.5 MB
// upload needs ~12 KB/s to finish inside the budget at all; 1 KB/s is
// twelve times slower than that and still cuts a deliberate slowloris to 30 s.
constexpr int64_t RECV_FLOOR_AFTER_MS = 30000;
constexpr uint32_t RECV_FLOOR_BYTES = 30000;

api::Context* g_ctx = nullptr;
std::mutex g_mu;
std::atomic<bool> g_in_progress{false};
std::atomic<uint32_t> g_received{0};
std::string g_last_error;
std::atomic<bool> g_pending_verify{false};
std::string g_boot_verdict = "n/a";

int64_t now_ms() { return esp_timer_get_time() / 1000; }   // uptime, for deadlines

// UTC milliseconds, which is what ModeManager's timebase is.  Releasing the
// hold with 0 made it re-render for 1970.
int64_t wall_ms() {
    timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

void set_error(const std::string& e) {
    const std::lock_guard<std::mutex> lock(g_mu);
    g_last_error = e;
}

// The version tag carries the board map and the sim/release flavour; nothing in
// esp_app_desc_t does (see webapi/ota_policy.h).
std::string running_tag_part(int which) {
    const esp_app_desc_t* d = esp_app_get_description();
    if (d == nullptr) return {};
    const std::string v(d->version);
    const size_t plus = v.find('+');
    if (plus == std::string::npos) return {};
    const std::string tag = v.substr(plus + 1);
    const size_t dot = tag.find('.');
    if (which == 0) return dot == std::string::npos ? tag : tag.substr(0, dot);
    return dot == std::string::npos ? std::string{} : tag.substr(dot + 1);
}

// ---------------------------------------------------------------------------
// The confirm watcher (spec 10.4)
// ---------------------------------------------------------------------------
void watcher_task(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!g_pending_verify.load(std::memory_order_relaxed)) continue;

        api::BootHealth h;
        h.app_main_completed = swan::app_main_completed();
        h.config_ok = swan::config_init_ok();
        h.nvs_was_erased = swan::nvs_was_erased();
        h.httpd_ok = swan::httpd_started();
        h.modes_ticks = swan::modes_tick_count();
        h.motion_ticks = motion::control_ticks();
        h.uptime_s = static_cast<uint32_t>(esp_timer_get_time() / 1000000);

        const api::BootVerdict v = api::ota_evaluate(h);
        {
            const std::lock_guard<std::mutex> lock(g_mu);
            g_boot_verdict = api::boot_verdict_name(v);
        }
        if (v == api::BootVerdict::Wait) continue;

        if (v == api::BootVerdict::Confirm) {
            if (h.nvs_was_erased) {
                // Confirming anyway (see ota_evaluate), but this must not pass
                // unremarked: every setting is back to its default, which
                // includes the column modes and the calibration.
                ESP_LOGE(TAG, "*** NVS WAS ERASED THIS BOOT - every setting is at its "
                              "default. Check `persist`. ***");
            }
            if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
                g_pending_verify.store(false, std::memory_order_relaxed);
                ESP_LOGI(TAG, "image confirmed: app_main done, config ok (no NVS erase), "
                              "%lu modes ticks, %lu motion ticks, httpd up",
                         static_cast<unsigned long>(h.modes_ticks),
                         static_cast<unsigned long>(h.motion_ticks));
            }
            continue;
        }

        ESP_LOGE(TAG, "image did NOT confirm within %lus; rolling back",
                 static_cast<unsigned long>(api::OTA_CONFIRM_DEADLINE_S));
        mqtt_go_offline();
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

// ---------------------------------------------------------------------------
// The upload route
// ---------------------------------------------------------------------------
esp_err_t send_json(httpd_req_t* req, const std::string& body, const char* status = "200 OK") {
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body.data(), static_cast<ssize_t>(body.size()));
}

std::string refuse(const char* verdict, const char* reason) {
    json::Writer w;
    w.obj().kv("ok", false).kv("verdict", verdict).kv("err", reason).end_obj();
    return w.take();
}

esp_err_t ota_post(httpd_req_t* req) {
    if (g_ctx == nullptr) {
        return send_json(req, refuse("not_ready", "the display is still starting up"),
                         "503 Service Unavailable");
    }
    if (g_in_progress.exchange(true)) {
        return send_json(req, refuse("busy", "an update is already running"), "409 Conflict");
    }
    struct Guard {
        ~Guard() { g_in_progress.store(false); }
    } guard;

    g_received.store(0, std::memory_order_relaxed);
    set_error("");

    // `?force=1` is how the two soft refusals - a wrong-board image, and a
    // release image onto a board with simulated columns - are overridden.
    bool force = false;
    {
        char q[64] = {};
        if (httpd_req_get_url_query_str(req, q, sizeof q) == ESP_OK) {
            char v[8] = {};
            if (httpd_query_key_value(q, "force", v, sizeof v) == ESP_OK) {
                force = v[0] == '1' || v[0] == 't' || v[0] == 'y';
            }
        }
    }

    // Buffer exactly the header, decide, and only then touch the flash.
    std::vector<uint8_t> head;
    head.reserve(api::OTA_HEADER_BYTES);
    uint8_t buf[CHUNK];
    const int64_t start = now_ms();
    const int64_t deadline = start + RECV_TOTAL_MS;
    int64_t last_progress = now_ms();
    int timeouts = 0;

    while (head.size() < api::OTA_HEADER_BYTES) {
        // UNCONDITIONALLY, not only on a timeout.  recv_wait_timeout is 2 s and
        // httpd_req_recv returns as soon as one byte is available, so a client
        // dribbling a byte every 1.5 s never produces a timeout - and the
        // single httpd task is parked here for as long as it likes.  read_body
        // already had this bound; the OTA copy did not.
        if (now_ms() > deadline || now_ms() - last_progress > RECV_STALL_MS) {
            return send_json(req, refuse("timeout", "the upload stalled"),
                             "408 Request Timeout");
        }
        const int n = httpd_req_recv(req, reinterpret_cast<char*>(buf),
                                     std::min(CHUNK, api::OTA_HEADER_BYTES - head.size()));
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 2 || now_ms() > deadline) {
                return send_json(req, refuse("timeout", "the upload stalled"), "408 Request Timeout");
            }
            continue;
        }
        if (n <= 0) {
            return send_json(req, refuse("aborted", "the upload was interrupted"),
                             "400 Bad Request");
        }
        head.insert(head.end(), buf, buf + n);
        timeouts = 0;
        last_progress = now_ms();
    }

    api::OtaPrecheck pre;
    pre.image = api::sniff_image(head.data(), head.size());
    pre.running_project = ota_running_project();
    pre.running_board = ota_running_board();
    pre.running_pending_verify = g_pending_verify.load(std::memory_order_relaxed);
    pre.maintenance = motion::columns().maintenance;
    pre.any_simulated_column = motion::columns().any(ColumnMode::Sim);
    pre.free_heap = esp_get_free_heap_size();
    pre.force = force;
    pre.all_axes_idle = true;
    for (int i = 0; i < N_COLUMNS; ++i) {
        AxisInfo a;
        motion::info(i, a);
        if (a.state == AxisState::Moving || a.state == AxisState::Homing || a.velocity != 0) {
            pre.all_axes_idle = false;
        }
    }

    const api::OtaVerdict verdict = api::ota_gate(pre);
    if (verdict != api::OtaVerdict::Allow) {
        ESP_LOGW(TAG, "refused: %s", api::ota_verdict_reason(verdict));
        set_error(api::ota_verdict_reason(verdict));
        return send_json(req, refuse(api::ota_verdict_name(verdict),
                                     api::ota_verdict_reason(verdict)),
                         "400 Bad Request");
    }
    ESP_LOGI(TAG, "accepting %s %s (%u bytes announced)", pre.image.project_name.c_str(),
             pre.image.version.c_str(), static_cast<unsigned>(req->content_len));

    // Motion is held for the duration (spec 10.4).  NOT by dropping EN: that
    // de-energizes all five (ganged) and a loaded drum may creep, which is also
    // what lets an aborted upload resume without a re-home.
    g_ctx->modes.cmd_ota_hold(true, 0);
    // MQTT, the mqtt task and lwIP are all flash-resident and stall on every
    // sector erase; say goodbye properly rather than letting the broker time us
    // out mid-write.
    mqtt_go_offline();

    const esp_partition_t* target = esp_ota_get_next_update_partition(nullptr);
    esp_ota_handle_t handle = 0;
    // OTA_WITH_SEQUENTIAL_WRITES, not the announced size: see CHUNK above.
    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        g_ctx->modes.cmd_ota_hold(false, wall_ms());
        mqtt_reconfigure();
        set_error(esp_err_to_name(err));
        return send_json(req, refuse("begin_failed", esp_err_to_name(err)),
                         "500 Internal Server Error");
    }

    // One place that undoes everything the upload took, because there are six
    // exits and three of them used to forget something: the hold was released
    // with epoch 0 (so the display re-rendered a frame for 1970), and MQTT was
    // never restarted at all - leaving the device retained-offline in Home
    // Assistant until the next reboot.
    auto release = [&]() {
        g_ctx->modes.cmd_ota_hold(false, wall_ms());
        mqtt_reconfigure();   // reconnects if it is configured; a no-op if not
    };
    auto fail = [&](const char* what, const char* status) {
        esp_ota_abort(handle);
        release();
        set_error(what);
        ESP_LOGE(TAG, "%s", what);
        return send_json(req, refuse("write_failed", what), status);
    };

    err = esp_ota_write(handle, head.data(), head.size());
    if (err != ESP_OK) return fail(esp_err_to_name(err), "500 Internal Server Error");
    g_received.store(static_cast<uint32_t>(head.size()), std::memory_order_relaxed);

    for (;;) {
        // Same bound, and it matters more here: by this point the motion hold
        // is taken and MQTT is stopped, so a stalled client freezes the clock
        // and the cues as well as the web server.
        if (now_ms() > deadline || now_ms() - last_progress > RECV_STALL_MS) {
            return fail("the upload stalled", "408 Request Timeout");
        }
        if (now_ms() - start > RECV_FLOOR_AFTER_MS &&
            g_received.load(std::memory_order_relaxed) < RECV_FLOOR_BYTES) {
            return fail("the upload is too slow to finish", "408 Request Timeout");
        }
        const int n = httpd_req_recv(req, reinterpret_cast<char*>(buf), CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 2 || now_ms() > deadline) return fail("the upload stalled",
                                                                  "408 Request Timeout");
            continue;
        }
        if (n == 0) break;                       // the client closed: done
        if (n < 0) return fail("the upload was interrupted", "400 Bad Request");
        timeouts = 0;
        last_progress = now_ms();
        err = esp_ota_write(handle, buf, static_cast<size_t>(n));
        if (err != ESP_OK) return fail(esp_err_to_name(err), "500 Internal Server Error");
        const uint32_t got = g_received.fetch_add(static_cast<uint32_t>(n),
                                                  std::memory_order_relaxed) +
                             static_cast<uint32_t>(n);
        if (req->content_len > 0 && got >= req->content_len) break;
        // No esp_task_wdt_reset here: the httpd task is deliberately NOT
        // subscribed to the watchdog, so the call was a no-op whose comment
        // claimed a protection it did not provide.  What actually keeps the
        // watchdog quiet is that each 4 KB chunk is ONE sector erase - tens of
        // milliseconds - and the task blocks on recv in between, which is when
        // everything else runs.
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        release();
        set_error(esp_err_to_name(err));
        // ESP_ERR_OTA_VALIDATE_FAILED here means a truncated or corrupt image;
        // the running one is untouched.
        return send_json(req, refuse("validate_failed", esp_err_to_name(err)),
                         "400 Bad Request");
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        release();
        set_error(esp_err_to_name(err));
        return send_json(req, refuse("set_boot_failed", esp_err_to_name(err)),
                         "500 Internal Server Error");
    }

    ESP_LOGW(TAG, "update written to %s (%lu bytes); rebooting in 1 s", target->label,
             static_cast<unsigned long>(g_received.load(std::memory_order_relaxed)));
    json::Writer w;
    w.obj().kv("ok", true).kv("bytes", static_cast<int64_t>(g_received.load()))
        .kv("partition", target->label).kv("note", "rebooting; the new image must confirm "
                                                   "itself or it will roll back")
        .end_obj();
    send_json(req, w.take());

    // Deferred so the response actually reaches the browser.
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

esp_err_t ota_status_get(httpd_req_t* req) {
    const OtaState s = ota_status();
    json::Writer w;
    w.obj()
        .kv("pending_verify", s.pending_verify)
        .kv("in_progress", s.in_progress)
        .kv("received", static_cast<int64_t>(s.received))
        .kv("partition", s.running_partition)
        .kv("boot_verdict", s.boot_verdict)
        .kv("err", s.last_error)
        .end_obj();
    return send_json(req, w.take());
}

}  // namespace

void ota_bind(api::Context& ctx) { g_ctx = &ctx; }

esp_err_t ota_init() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (running != nullptr && esp_ota_get_state_partition(running, &st) == ESP_OK) {
        g_pending_verify.store(st == ESP_OTA_IMG_PENDING_VERIFY, std::memory_order_relaxed);
    }
    if (g_pending_verify.load(std::memory_order_relaxed)) {
        ESP_LOGW(TAG, "*** this image is PENDING_VERIFY - it must confirm itself within %lus "
                      "or the bootloader rolls back ***",
                 static_cast<unsigned long>(api::OTA_CONFIRM_DEADLINE_S));
    }
    // Priority 1: the lowest thing that still runs.  It must not compete with
    // anything, and it has 120 s to reach a verdict.
    if (xTaskCreate(&watcher_task, "swan_otachk", 3072, nullptr, 1, nullptr) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

OtaState ota_status() {
    OtaState s;
    s.pending_verify = g_pending_verify.load(std::memory_order_relaxed);
    s.in_progress = g_in_progress.load(std::memory_order_relaxed);
    s.received = g_received.load(std::memory_order_relaxed);
    const esp_partition_t* running = esp_ota_get_running_partition();
    s.running_partition = running != nullptr ? running->label : "?";
    const std::lock_guard<std::mutex> lock(g_mu);
    s.last_error = g_last_error;
    s.boot_verdict = g_boot_verdict;
    return s;
}

esp_err_t ota_confirm() {
    if (!g_pending_verify.load(std::memory_order_relaxed)) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) g_pending_verify.store(false, std::memory_order_relaxed);
    return err;
}

esp_err_t ota_rollback_and_reboot() {
    mqtt_go_offline();
    return esp_ota_mark_app_invalid_rollback_and_reboot();
}

std::string ota_running_project() {
    const esp_app_desc_t* d = esp_app_get_description();
    return d != nullptr ? std::string(d->project_name) : std::string{};
}
std::string ota_running_board() { return running_tag_part(0); }
bool ota_pending_verify() { return g_pending_verify.load(std::memory_order_relaxed); }

esp_err_t ota_register_routes(void* server) {
    auto* s = static_cast<httpd_handle_t>(server);
    httpd_uri_t post = {};
    post.uri = "/api/ota";
    post.method = HTTP_POST;
    post.handler = ota_post;
    esp_err_t err = httpd_register_uri_handler(s, &post);
    if (err != ESP_OK) return err;
    httpd_uri_t get = {};
    get.uri = "/api/ota/status";
    get.method = HTTP_GET;
    get.handler = ota_status_get;
    return httpd_register_uri_handler(s, &get);
}

}  // namespace net
}  // namespace swan
