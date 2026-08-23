#include "net/httpd.h"

#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "esp_http_server.h"
#include "esp_log.h"
#include "ring/json_write.h"
#include "webapi/ring_upload.h"

namespace swan {
namespace net {
namespace {

constexpr const char* TAG = "httpd";
constexpr const char* FS_ROOT = "/fs";        // the LittleFS mount (ring_store)
constexpr size_t CHUNK = 2048;                // static-file streaming chunk
constexpr size_t MAX_BODY = api::RING_UPLOAD_MAX;

httpd_handle_t g_server = nullptr;
api::Context* g_ctx = nullptr;

// Open WebSocket sockets.  esp_http_server has no enumeration callback we can
// use for broadcast, so the handler keeps the list itself.
std::mutex g_ws_mu;
std::vector<int> g_ws_fds;

// UTC milliseconds, the timebase every webapi entry point takes.  NOT
// esp_timer: that is uptime, and the countdown is an absolute deadline.
int64_t now_ms() {
    timeval tv;
    gettimeofday(&tv, nullptr);
    return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

void ws_add(int fd) {
    const std::lock_guard<std::mutex> lock(g_ws_mu);
    for (const int f : g_ws_fds) {
        if (f == fd) return;
    }
    g_ws_fds.push_back(fd);
}

void ws_remove(int fd) {
    const std::lock_guard<std::mutex> lock(g_ws_mu);
    for (size_t i = 0; i < g_ws_fds.size(); ++i) {
        if (g_ws_fds[i] == fd) {
            g_ws_fds.erase(g_ws_fds.begin() + static_cast<long>(i));
            return;
        }
    }
}

// One queued async send.  Owns its payload; freed in the work callback.
struct WsSend {
    int fd;
    std::string text;
};

void ws_send_cb(void* arg) {
    auto* job = static_cast<WsSend*>(arg);
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t*>(&job->text[0]);
    frame.len = job->text.size();
    if (httpd_ws_send_frame_async(g_server, job->fd, &frame) != ESP_OK) ws_remove(job->fd);
    delete job;
}

// ---------------------------------------------------------------------------
// Static files
// ---------------------------------------------------------------------------
const char* content_type_for(const char* path) {
    struct Map {
        const char* ext;
        const char* type;
    };
    static const Map kMap[] = {
        {".html", "text/html"},        {".css", "text/css"},
        {".js", "application/javascript"}, {".json", "application/json"},
        {".svg", "image/svg+xml"},     {".png", "image/png"},
        {".ico", "image/x-icon"},      {".txt", "text/plain"},
    };
    const size_t n = std::strlen(path);
    for (const Map& m : kMap) {
        const size_t e = std::strlen(m.ext);
        if (n >= e && std::strcmp(path + n - e, m.ext) == 0) return m.type;
    }
    return "application/octet-stream";
}

bool path_is_safe(const char* uri) {
    if (std::strstr(uri, "..") != nullptr) return false;
    return uri[0] == '/';
}

esp_err_t send_file(httpd_req_t* req, const char* rel, bool gzipped) {
    char path[176];
    std::snprintf(path, sizeof path, "%s%s%s", FS_ROOT, rel, gzipped ? ".gz" : "");
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return ESP_ERR_NOT_FOUND;

    // Typed from `rel`, not from `path`: the browser wants the type of the
    // content, not of the .gz encoding.
    httpd_resp_set_type(req, content_type_for(rel));
    if (gzipped) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");

    static char buf[CHUNK];
    size_t got = 0;
    esp_err_t err = ESP_OK;
    while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, static_cast<ssize_t>(got)) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
    }
    std::fclose(f);
    if (err == ESP_OK) httpd_resp_send_chunk(req, nullptr, 0);
    return err;
}

esp_err_t static_handler(httpd_req_t* req) {
    if (!path_is_safe(req->uri)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_OK;
    }
    // Bounded copy rather than a truncating snprintf: a path that does not
    // fit is a 404, not a silently shortened filename.
    char rel[96];
    constexpr size_t kIndexLen = sizeof "index.html" - 1;
    const size_t ulen = std::strlen(req->uri);
    if (ulen + kIndexLen >= sizeof rel) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "path too long");
        return ESP_OK;
    }
    std::memcpy(rel, req->uri, ulen + 1);
    char* q = std::strchr(rel, '?');
    if (q != nullptr) *q = '\0';
    const size_t n = std::strlen(rel);
    if (n == 0 || rel[n - 1] == '/') {
        std::memcpy(rel + n, "index.html", kIndexLen + 1);
    }

    // Gzipped first: that is how the assets ship (spec 15 phase 3, step e).
    if (send_file(req, rel, true) == ESP_OK) return ESP_OK;
    if (send_file(req, rel, false) == ESP_OK) return ESP_OK;
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// REST
// ---------------------------------------------------------------------------
esp_err_t read_body(httpd_req_t* req, std::string& out) {
    const size_t len = req->content_len;
    if (len > MAX_BODY) return ESP_ERR_INVALID_SIZE;
    out.resize(len);
    size_t got = 0;
    while (got < len) {
        const int n = httpd_req_recv(req, &out[got], len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0) return ESP_FAIL;
        got += static_cast<size_t>(n);
    }
    return ESP_OK;
}

esp_err_t send_json(httpd_req_t* req, const std::string& body) {
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body.c_str(), static_cast<ssize_t>(body.size()));
}

esp_err_t state_handler(httpd_req_t* req) {
    return send_json(req, api::build_state(*g_ctx, now_ms()));
}

esp_err_t ring_handler(httpd_req_t* req) {
    return send_json(req, api::build_ring_doc(g_ctx->ring));
}

esp_err_t cmd_handler(httpd_req_t* req) {
    std::string body;
    if (read_body(req, body) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    return send_json(req, api::handle_command(*g_ctx, body, now_ms()));
}

// The upload is validated on THIS task into a staging table; the running table
// is swapped in by the modes task (ring_store.h contract).  A rejected upload
// never reaches the filesystem, so a truncated POST cannot brick the boot.
esp_err_t ring_upload_handler(httpd_req_t* req) {
    std::string body;
    const esp_err_t rerr = read_body(req, body);
    if (rerr == ESP_ERR_INVALID_SIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ring.json too large");
        return ESP_OK;
    }
    if (rerr != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "upload interrupted");
        return ESP_OK;
    }
    std::string err;
    if (!g_ctx->ring_upload.stage(body, &err)) {
        json::Writer w;
        w.obj().kv("ok", false).kv("err", err).end_obj();
        ESP_LOGW(TAG, "ring upload rejected: %s", err.c_str());
        return send_json(req, w.take());
    }
    ESP_LOGI(TAG, "ring upload staged (%u bytes); modes task will apply",
             static_cast<unsigned>(body.size()));
    return send_json(req, R"({"ok":true})");
}

// ---------------------------------------------------------------------------
// WebSocket
// ---------------------------------------------------------------------------
esp_err_t ws_handler(httpd_req_t* req) {
    if (req->method == HTTP_GET) {  // the handshake
        const int fd = httpd_req_to_sockfd(req);
        ws_add(fd);
        ESP_LOGI(TAG, "ws open, fd %d", fd);
        // Prime the page: the full state, then the current mode.
        auto* s1 = new WsSend{fd, api::build_state(*g_ctx, now_ms())};
        httpd_queue_work(g_server, ws_send_cb, s1);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) return err;
    if (frame.len > MAX_BODY) return ESP_ERR_INVALID_SIZE;

    std::string buf(frame.len + 1, '\0');
    frame.payload = reinterpret_cast<uint8_t*>(&buf[0]);
    err = httpd_ws_recv_frame(req, &frame, frame.len);
    if (err != ESP_OK) return err;
    buf.resize(frame.len);

    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        ws_remove(httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    if (frame.type != HTTPD_WS_TYPE_TEXT) return ESP_OK;

    const std::string res = api::handle_command(*g_ctx, buf, now_ms());

    // Echo the caller's id so a page can match request to result.
    int64_t id = 0;
    json::Value doc;
    if (json::parse(buf, doc, nullptr) && doc.get("id") != nullptr) {
        id = doc.get("id")->as_int(0);
    }
    json::Writer w;
    w.obj().kv("e", "result").kv("id", id).kv_raw("res", res).end_obj();

    const std::string out = w.take();
    httpd_ws_frame_t reply = {};
    reply.type = HTTPD_WS_TYPE_TEXT;
    reply.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(out.data()));
    reply.len = out.size();
    return httpd_ws_send_frame(req, &reply);
}

void on_socket_close(httpd_handle_t, int fd) {
    ws_remove(fd);
    close(fd);
}

}  // namespace

void ws_broadcast(const std::string& msg) {
    if (g_server == nullptr) return;
    std::vector<int> fds;
    {
        const std::lock_guard<std::mutex> lock(g_ws_mu);
        fds = g_ws_fds;
    }
    for (const int fd : fds) {
        auto* job = new WsSend{fd, msg};
        if (httpd_queue_work(g_server, ws_send_cb, job) != ESP_OK) delete job;
    }
}

size_t ws_clients() {
    const std::lock_guard<std::mutex> lock(g_ws_mu);
    return g_ws_fds.size();
}

esp_err_t httpd_start(api::Context& ctx) {
    g_ctx = &ctx;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;  // one wildcard route for the UI
    cfg.max_uri_handlers = 8;
    cfg.stack_size = 8192;   // JSON building plus a 2 KB file chunk
    cfg.lru_purge_enable = true;
    cfg.close_fn = on_socket_close;
    // Below the modes task (5) and far below the motion control task: nothing
    // on the network path may delay a step (CLAUDE.md hard constraints).
    cfg.task_priority = 3;

    const esp_err_t err = ::httpd_start(&g_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t routes[] = {
        {"/ws", HTTP_GET, ws_handler, nullptr, true, false, nullptr},
        {"/api/state", HTTP_GET, state_handler, nullptr, false, false, nullptr},
        {"/api/ring", HTTP_GET, ring_handler, nullptr, false, false, nullptr},
        {"/api/cmd", HTTP_POST, cmd_handler, nullptr, false, false, nullptr},
        {"/api/ring/upload", HTTP_POST, ring_upload_handler, nullptr, false, false, nullptr},
        {"/*", HTTP_GET, static_handler, nullptr, false, false, nullptr},
    };
    for (const httpd_uri_t& r : routes) ESP_ERROR_CHECK(httpd_register_uri_handler(g_server, &r));

    ESP_LOGI(TAG, "serving on port %d", cfg.server_port);
    return ESP_OK;
}

esp_err_t httpd_stop() {
    if (g_server == nullptr) return ESP_OK;
    const esp_err_t err = ::httpd_stop(g_server);
    g_server = nullptr;
    {
        const std::lock_guard<std::mutex> lock(g_ws_mu);
        g_ws_fds.clear();
    }
    return err;
}

}  // namespace net
}  // namespace swan
