#include "net/httpd.h"

#include "net/mqtt.h"
#include "net/ota.h"
#include "audio/player.h"
#include "net/provision.h"
#include "webapi/portal.h"

#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "ring/json_write.h"
#include "webapi/event_tap.h"
#include "webapi/ring_upload.h"

namespace swan {
namespace net {
namespace {

constexpr const char* TAG = "httpd";
constexpr const char* FS_ROOT = "/fs";        // the LittleFS mount (ring_store)
constexpr size_t CHUNK = 2048;                // static-file streaming chunk
constexpr size_t MAX_BODY = api::RING_UPLOAD_MAX;
// A command document, with room to spare: the biggest one the UI sends is a
// five-token message.set at ~200 bytes.
constexpr size_t WS_FRAME_MAX = 2048;

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

// ---------------------------------------------------------------------------
// Outbound queue
// ---------------------------------------------------------------------------
// `httpd_queue_work` posts over a UDP control socket whose mbox holds
// CONFIG_LWIP_UDP_RECVMBOX_SIZE (6) messages, and esp_http_server guards it
// with a counting semaphore of that size: past six outstanding jobs it returns
// ESP_FAIL and the work never runs.  The obvious implementation - one job per
// client per message - therefore CANNOT deliver a five-column frame.  The
// modes task runs at priority 5 and httpd at 3 on one core, so the five `go`
// events are posted back to back with no chance for the queue to drain, and
// the board logs "ctrl socket queue full, work not queued" for the overflow.
// Measured on hardware before this was fixed: a `preset.set qmarks` emitted
// five `go` events and exactly TWO reached the browser, every single time.
//
// So the queue is ours, not the control socket's: messages accumulate here and
// ONE drain job is in flight at a time, whatever the message rate and however
// many clients are connected.  That takes the control-socket usage from
// (messages x clients) to 1.
constexpr size_t WS_OUT_MAX_MSGS = 24;
constexpr size_t WS_OUT_MAX_BYTES = 8192;   // ~5 state documents; heap is 130 KB
constexpr size_t WS_DRAIN_PER_JOB = 8;      // then re-queue, so one slow client
                                            // cannot hold the httpd task for ever

std::vector<std::string> g_ws_out;   // guarded by g_ws_mu
size_t g_ws_out_bytes = 0;
bool g_ws_draining = false;
uint32_t g_ws_dropped = 0;           // published in the state payload: a silent
                                     // drop is what made this bug invisible

void ws_drain_cb(void* arg);

// Caller must hold g_ws_mu.
bool ws_kick_locked() {
    if (g_ws_draining || g_ws_out.empty()) return false;
    g_ws_draining = true;
    return true;
}

void ws_queue(std::string msg) {
    bool kick = false;
    {
        const std::lock_guard<std::mutex> lock(g_ws_mu);
        if (g_ws_fds.empty()) return;
        // Bounded both ways.  Dropping the OLDEST is the right choice: the
        // newest message is the one that describes the display as it is now.
        while (!g_ws_out.empty() &&
               (g_ws_out.size() >= WS_OUT_MAX_MSGS ||
                g_ws_out_bytes + msg.size() > WS_OUT_MAX_BYTES)) {
            g_ws_out_bytes -= g_ws_out.front().size();
            g_ws_out.erase(g_ws_out.begin());
            ++g_ws_dropped;
        }
        g_ws_out_bytes += msg.size();
        g_ws_out.push_back(std::move(msg));
        kick = ws_kick_locked();
    }
    if (kick && httpd_queue_work(g_server, ws_drain_cb, nullptr) != ESP_OK) {
        // Should not happen now that only one job is ever outstanding, but if
        // it does, clear the flag so the NEXT broadcast retries rather than
        // wedging the queue for ever.
        const std::lock_guard<std::mutex> lock(g_ws_mu);
        g_ws_draining = false;
        ESP_LOGW(TAG, "ws: could not queue the drain job");
    }
}

// Runs on the httpd task.
void ws_drain_cb(void*) {
    for (size_t n = 0; n < WS_DRAIN_PER_JOB; ++n) {
        std::string msg;
        std::vector<int> fds;
        {
            const std::lock_guard<std::mutex> lock(g_ws_mu);
            if (g_ws_out.empty()) {
                g_ws_draining = false;
                return;
            }
            msg = std::move(g_ws_out.front());
            g_ws_out.erase(g_ws_out.begin());
            g_ws_out_bytes -= msg.size();
            fds = g_ws_fds;
        }
        httpd_ws_frame_t frame = {};
        frame.type = HTTPD_WS_TYPE_TEXT;
        frame.payload = reinterpret_cast<uint8_t*>(&msg[0]);
        frame.len = msg.size();
        for (const int fd : fds) {
            // ws_remove takes the same mutex, so it must not be called with it
            // held - hence the send loop runs outside the critical section.
            if (httpd_ws_get_fd_info(g_server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
                ws_remove(fd);
                continue;
            }
            if (httpd_ws_send_frame_async(g_server, fd, &frame) == ESP_OK) continue;

            // A send failure here is NOT proof the peer is gone, and dropping
            // the fd quietly is how a healthy client goes deaf for ever.
            // `send_wait_timeout = 1` sets a non-zero SO_SNDTIMEO, and lwIP
            // reads any non-zero send timeout as "never block"
            // (api_lib.c: `if (conn->send_timeout != 0) dontblock = 1;`), so a
            // short TCP send buffer returns ERR_WOULDBLOCK **immediately**
            // (api_msg.c) rather than waiting.  TCP_SND_BUF is 5760 and a state
            // document is ~1.5 KB, so a phone whose radio naps for a few
            // hundred ms is enough.  Unregistering without closing leaves the
            // browser's socket OPEN: no `onclose`, so bus.js never reconnects,
            // and the state documents the mirror reconciles against stop
            // arriving - which would silently defeat that fix and look exactly
            // like the stale-mirror bug all over again.  Commands would still
            // work, because a reply goes back through the session's own
            // handler, which is what would make it maddening on the bench.
            //
            // So: close the socket too.  The browser sees it, reconnects after
            // 1500 ms, and ws_opened re-primes it.  A blip instead of a death.
            ws_remove(fd);
            const esp_err_t cerr = httpd_sess_trigger_close(g_server, fd);
            if (cerr != ESP_OK) {
                // It is itself an httpd_queue_work, so it can fail for the very
                // reason we are here.  Say so rather than assuming.
                ESP_LOGW(TAG, "ws: fd %d unregistered but not closed (%s)", fd,
                         esp_err_to_name(cerr));
            }
        }
    }
    // More to send: re-queue rather than looping, so a client that is slow to
    // read cannot monopolise the single httpd task.
    bool again = false;
    {
        const std::lock_guard<std::mutex> lock(g_ws_mu);
        if (g_ws_out.empty()) {
            g_ws_draining = false;
        } else {
            again = true;
        }
    }
    if (again && httpd_queue_work(g_server, ws_drain_cb, nullptr) != ESP_OK) {
        const std::lock_guard<std::mutex> lock(g_ws_mu);
        g_ws_draining = false;
    }
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
    // While the portal is up, a phone probes a well-known URL to decide whether
    // the network is captive.  Each platform wants a different answer, and the
    // wrong one means the sign-in sheet never appears and the user is simply
    // told the network has no internet.
    if (portal_active()) {
        char host[64] = {};
        httpd_req_get_hdr_value_str(req, "Host", host, sizeof host);
        switch (api::portal_probe_kind(req->uri, host, "lost.local")) {
            case api::ProbeKind::NoContent:
                httpd_resp_set_status(req, "204 No Content");
                return httpd_resp_send(req, nullptr, 0);
            case api::ProbeKind::Redirect:
                httpd_resp_set_status(req, "302 Found");
                httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
                return httpd_resp_send(req, nullptr, 0);
            case api::ProbeKind::None:
                break;
        }
    }

    if (!path_is_safe(req->uri)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_OK;
    }
    // Bounded copy rather than a truncating snprintf: a path that does not
    // fit is a 404, not a silently shortened filename.
    char rel[96];
    constexpr size_t kIndexLen = sizeof "portal.html" - 1;   // the longer of the two
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
        // While the portal is up, a PAGE request gets the setup page rather
        // than the control panel.  The control panel is useless on a display
        // that cannot reach the network - and worse, it fetches /api/ring and
        // opens a WebSocket, which a phone on an isolated access point sits
        // and waits on.  Assets are still served normally.
        if (portal_active()) {
            std::memcpy(rel + n, "portal.html", sizeof "portal.html");
        } else {
            std::memcpy(rel + n, "index.html", sizeof "index.html");
        }
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
// A recv timeout is retried, but NOT for ever: there is one httpd task, so a
// client that opens a POST, announces a content-length and then stops sending
// wedges the entire web UI for as long as it likes.  recv_wait_timeout is 5 s,
// so the bound is expressed in wall clock and the retry count is a second
// backstop against a peer that dribbles one byte per timeout.
//
// (The phase 3 review recorded this as fixed. It was not - the loop below had
// a bare `continue`. Found again while reading the path the OTA upload was
// about to copy.)
// Bounded HARD, because there is one httpd task: whatever a stalled client
// costs, it costs every other browser on the LAN at the same time.  Measured
// on the board before this was tuned: a client that announced 4096 bytes and
// sent 10 took the whole UI down for ~20 s.  With recv_wait_timeout at 2 s and
// one retry, the worst case is ~4 s.  A legitimate 9.4 KB ring.json completes
// in well under a second on a LAN, and progress resets the retry count, so a
// genuinely slow client is not punished for being slow - only for being silent.
constexpr int64_t BODY_DEADLINE_MS = 10000;
constexpr int BODY_MAX_TIMEOUTS = 1;

// `limit` is per-route.  It used to be the ring table's 24 KB for everything,
// which silently made an audio cue over 24 KB impossible - and three of the
// five placeholders that ship are bigger than that, so "replace them without a
// reflash" was false for the ones worth replacing.
esp_err_t read_body(httpd_req_t* req, std::string& out, size_t limit = MAX_BODY) {
    const size_t len = req->content_len;
    if (len > limit) return ESP_ERR_INVALID_SIZE;
    // With exceptions off (CONFIG_COMPILER_CXX_EXCEPTIONS=n) a failed allocation
    // is abort(), i.e. a remote reboot - the same mechanism the JSON node cap
    // exists to prevent, one layer earlier.  The audio route guarded itself;
    // every other route allocated the whole announced body unguarded.  Ask for
    // headroom rather than the exact size: the parse that follows needs room too.
    const size_t need = len + 4096;
    if (len > 0 && heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < need) {
        ESP_LOGW(TAG, "refusing a %u-byte body: largest free block is %u", (unsigned)len,
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
        return ESP_ERR_NO_MEM;
    }
    out.resize(len);
    size_t got = 0;
    const int64_t deadline = now_ms() + BODY_DEADLINE_MS;
    int timeouts = 0;
    while (got < len) {
        const int n = httpd_req_recv(req, &out[got], len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            // Nothing at all yet: the client announced a body and never began.
            // That is not slow, it is absent, and it gets no patience.
            if (got == 0 || ++timeouts > BODY_MAX_TIMEOUTS || now_ms() > deadline) {
                ESP_LOGW(TAG, "body stalled at %u/%u bytes; giving up",
                         static_cast<unsigned>(got), static_cast<unsigned>(len));
                return ESP_ERR_TIMEOUT;
            }
            continue;
        }
        if (n <= 0) return ESP_FAIL;
        got += static_cast<size_t>(n);
        timeouts = 0;   // progress resets the patience, not the deadline
        if (now_ms() > deadline) {
            ESP_LOGW(TAG, "body exceeded %lld ms at %u/%u bytes",
                     static_cast<long long>(BODY_DEADLINE_MS), static_cast<unsigned>(got),
                     static_cast<unsigned>(len));
            return ESP_ERR_TIMEOUT;
        }
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
    const RingSet ring = g_ctx->ring.snapshot();  // pinned for the response
    return send_json(req, api::build_ring_doc(ring));
}

// Measured, not tabulated (spec 7.1): a whole day and a whole run walked
// through the real renderers.  ~4,700 renders for the entire table, on the
// HTTP task at priority 3 - well clear of the motion path.
esp_err_t wear_handler(httpd_req_t* req) {
    const ModesConfig cfg = g_ctx->modes.config();
    // Pinned: this walks tens of thousands of renders, easily spanning several
    // modes ticks, and an upload landing in the middle would otherwise free
    // the tables underneath it.
    const RingSet ring = g_ctx->ring.snapshot();
    return send_json(req, api::build_wear_doc(ring, cfg.h24, cfg.seconds_live_s));
}

esp_err_t cmd_handler(httpd_req_t* req) {
    std::string body;
    if (read_body(req, body) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }
    // Origin::Ui, not the default Unknown.  Spec 7.3 has no master and expects
    // a peer to tell its own decision from somebody else's, which it cannot do
    // if every browser-set deadline is published as set_by "unknown" - and it
    // was, on both browser paths, while MQTT named itself correctly.
    return send_json(req, api::handle_command(*g_ctx, body, now_ms(), Origin::Ui));
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
    // Heap guard.  json_lite builds a DOM whose peak is several times the body
    // size, in vectors that must grow CONTIGUOUSLY - so total free heap is the
    // wrong number to look at.  With exceptions off an allocation failure is
    // abort(), i.e. a reboot rather than a rejected upload, which is exactly
    // what a 4 KB flood did to this board.  Refuse politely instead.
    //
    // The bound comes from the PARSER's caps, not from the body size: whatever
    // arrives, json_lite stops at MAX_NODES_UNTRUSTED values, so the worst
    // case is that many Values (~64 B each on RV32) plus the transient of one
    // container vector doubling.  Guessing from the body was wrong - a 4 KB
    // flood passed a body-derived check and then panicked the board.
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    constexpr size_t kParsePeak = json::MAX_NODES_UNTRUSTED * 64 * 17 / 10;  // ~76 KB
    const size_t need = kParsePeak + 8 * 1024;
    if (largest < need) {
        json::Writer w;
        w.obj().kv("ok", false)
            .kv("err", "not enough contiguous heap to parse this safely (" +
                        std::to_string(largest) + " B free, needs ~" +
                        std::to_string(need) + " B)")
            .end_obj();
        ESP_LOGW(TAG, "ring upload refused: %u B largest block, needs ~%u",
                 static_cast<unsigned>(largest), static_cast<unsigned>(need));
        return send_json(req, w.take());
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
// Registration happens HERE, not in ws_handler: esp_http_server answers the
// handshake itself and deliberately does not call the URI handler for it
// (httpd_uri.c: "If the request is websocket handshake, then do not call the
// uri->handler").  With the registration in the handler, g_ws_fds stayed empty
// forever and every push path was dead - no state document, no heartbeat, no
// go/spin/cue - while commands still worked, because a command reply goes back
// through the session's own ws_handler.  Needs
// CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT.
esp_err_t ws_opened(httpd_req_t* req) {
    const int fd = httpd_req_to_sockfd(req);
    ws_add(fd);
    ESP_LOGI(TAG, "ws open, fd %d (%u client%s)", fd, static_cast<unsigned>(ws_clients()),
             ws_clients() == 1 ? "" : "s");
    // Prime the page: the full state, and the mode it is in.  This goes to
    // every client rather than just the new one - one extra state document for
    // the others, which they already receive at 1 Hz, in exchange for the
    // outbound path having exactly one entry point.
    ws_queue(api::build_state(*g_ctx, now_ms()));
    ws_queue(api::mode_event(g_ctx->modes.mode()));
    return ESP_OK;
}

esp_err_t ws_handler(httpd_req_t* req) {
    // A plain GET /ws with no Upgrade header still reaches the handler with
    // HTTP_GET.  It is not a WebSocket and must never be registered as one.
    if (req->method == HTTP_GET) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "/ws requires a WebSocket upgrade");
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) return err;
    // A /ws frame carries a command document - tens of bytes, not kilobytes.
    // It used to share the ring upload's 24 KB cap, so any browser could ask the
    // single httpd task for a 24 KB allocation, and a failed one is abort().
    // The largest legitimate command is a five-token message.set at well under
    // 512 bytes.
    if (frame.len > WS_FRAME_MAX) {
        ESP_LOGW(TAG, "ws frame of %u bytes refused (max %u)", (unsigned)frame.len,
                 (unsigned)WS_FRAME_MAX);
        return ESP_ERR_INVALID_SIZE;
    }

    // Only fetch a payload when there IS one.  With len == 0 the second call
    // re-enters the header path and issues a blocking recv for the next frame's
    // first byte, which either stalls the single httpd task for the full socket
    // timeout or eats the head of the next frame and leaves the session
    // permanently mid-stream.  An empty TEXT frame, or an unsolicited empty
    // PONG, is enough to trigger it.
    std::string buf;
    if (frame.len > 0) {
        buf.assign(frame.len + 1, '\0');
        frame.payload = reinterpret_cast<uint8_t*>(&buf[0]);
        err = httpd_ws_recv_frame(req, &frame, frame.len);
        if (err != ESP_OK) return err;
        buf.resize(frame.len);
    }

    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        ws_remove(httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    if (frame.type != HTTPD_WS_TYPE_TEXT) return ESP_OK;

    const std::string res = api::handle_command(*g_ctx, buf, now_ms(), Origin::Ui);

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

// A cue WAV, uploaded from Settings -> Audio (spec 9).  Exactly the ring
// upload's shape and for exactly its reason: validated in full, written to a
// temp path, and renamed over the old one ONLY once it parses.  A truncated or
// malformed upload therefore leaves the previous cue playable rather than
// replacing the alarm with silence.
//
// The assets ship as synthesized placeholders; Nico's Swan recordings replace
// them this way, with no reflash.
constexpr size_t AUDIO_UPLOAD_MAX = 192 * 1024;

esp_err_t audio_upload_handler(httpd_req_t* req) {
    // /api/audio/<cue>
    const char* name = std::strrchr(req->uri, '/');
    audio::CueId cue{};
    if (name == nullptr || !audio::cue_id_from_name(name + 1, cue)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown cue");
        return ESP_OK;
    }
    if (req->content_len == 0 || req->content_len > AUDIO_UPLOAD_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "wav too large or empty");
        return ESP_OK;
    }
    // One contiguous block, because the parser needs the chunk table and the
    // rename has to be all-or-nothing.  Refuse rather than fragment the heap
    // to the point where the ring upload or an OTA cannot run afterwards.
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < req->content_len + 24576) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "not enough contiguous heap");
        return ESP_OK;
    }

    std::string body;
    const esp_err_t rerr = read_body(req, body, AUDIO_UPLOAD_MAX);
    if (rerr != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "upload interrupted");
        return ESP_OK;
    }
    // The whole file is in hand, so len == total_len and the clamp still
    // refuses a header that announces more than it delivers.
    const audio::WavInfo w = audio::wav_parse(
        reinterpret_cast<const uint8_t*>(body.data()), body.size(), body.size());
    if (!w.ok) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            w.err != nullptr ? w.err : "not a usable WAV");
        return ESP_OK;
    }

    // esp_littlefs refuses to rename over a file that is OPEN, and the player
    // holds the fd for the duration of a cue - which presented as a bare
    // "rename failed" if you replaced a cue while it was playing.  Stop it
    // first and give it a moment to close.
    audio::stop();
    vTaskDelay(pdMS_TO_TICKS(120));

    ::mkdir("/fs/audio", 0777);
    const std::string dest = audio::cue_path(cue);
    const std::string tmp = dest + ".tmp";
    std::FILE* f = std::fopen(tmp.c_str(), "wb");
    if (f == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "cannot write");
        return ESP_OK;
    }
    const size_t wrote = std::fwrite(body.data(), 1, body.size(), f);
    std::fclose(f);
    if (wrote != body.size()) {
        std::remove(tmp.c_str());
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "short write; nothing replaced");
        return ESP_OK;
    }
    // LittleFS rename REPLACES atomically - the ring upload learned that the
    // hard way, by removing the target first and losing the table to a
    // brownout in the window.
    if (std::rename(tmp.c_str(), dest.c_str()) != 0) {
        std::remove(tmp.c_str());
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "rename failed");
        return ESP_OK;
    }
    audio::rescan();
    ESP_LOGI(TAG, "%s replaced: %lu Hz, %lu bytes of audio", audio::cue_id_name(cue),
             static_cast<unsigned long>(w.sample_rate), static_cast<unsigned long>(w.data_bytes));

    json::Writer jw;
    jw.obj().kv("ok", true).kv("cue", audio::cue_id_name(cue))
        .kv("rate", static_cast<int64_t>(w.sample_rate))
        .kv("bytes", static_cast<int64_t>(w.data_bytes)).end_obj();
    return send_json(req, jw.take());
}

void on_socket_close(httpd_handle_t, int fd) {
    ws_remove(fd);
    {
        // Nobody left to send to: drop the backlog rather than carrying it
        // until the next client connects and receives a burst of history.
        const std::lock_guard<std::mutex> lock(g_ws_mu);
        if (g_ws_fds.empty()) {
            g_ws_out.clear();
            g_ws_out_bytes = 0;
        }
    }
    close(fd);
}

}  // namespace

void ws_broadcast(const std::string& msg) {
    if (g_server == nullptr) return;
    ws_queue(msg);
}

bool has_state_consumers() {
    // A wall clock feeding a terminal prop has no browser open at all.
    return ws_clients() > 0 || mqtt_connected();
}

uint32_t ws_dropped() {
    const std::lock_guard<std::mutex> lock(g_ws_mu);
    return g_ws_dropped;
}

size_t ws_clients() {
    const std::lock_guard<std::mutex> lock(g_ws_mu);
    return g_ws_fds.size();
}

esp_err_t httpd_start(api::Context& ctx) {
    g_ctx = &ctx;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;  // one wildcard route for the UI
    cfg.max_uri_handlers = 12;   // + /api/ota and /api/ota/status
    cfg.stack_size = 8192;   // JSON building plus a 2 KB file chunk
    cfg.lru_purge_enable = true;
    cfg.close_fn = on_socket_close;
    // A phone whose radio sleeps stops draining its window; the async send
    // then blocks the single httpd task for the whole timeout, serving nobody.
    // One second is plenty on a LAN and bounds the damage.
    cfg.send_wait_timeout = 1;
    // Two seconds, for the same reason send_wait_timeout is one: a single
    // httpd task means one unresponsive peer stalls everybody, and a gap of
    // even two seconds between TCP segments on a LAN is already pathological.
    cfg.recv_wait_timeout = 2;
    // Below the modes task (5) and far below the motion control task: nothing
    // on the network path may delay a step (CLAUDE.md hard constraints).
    cfg.task_priority = 3;
    // HTTPD_DEFAULT_CONFIG asks for 7, and esp_http_server additionally
    // requires LWIP_MAX_SOCKETS >= max_open_sockets + 3.  With
    // CONFIG_LWIP_MAX_SOCKETS at 10 that consumes the entire budget, leaving
    // none for MQTT's TCP socket or the provisioning portal's DNS socket
    // (Phase 4) - and socket exhaustion presents as intermittent refusals
    // rather than as an error anyone would notice.  Five concurrent
    // connections is ample for a wall clock on a LAN: a page is one socket,
    // its WebSocket a second.
    cfg.max_open_sockets = 5;

    const esp_err_t err = ::httpd_start(&g_server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    // Designated initialisers from here: ws_post_handshake_cb is Kconfig-gated,
    // so a positional aggregate would put the callback in the wrong slot the
    // moment that option moves.
    httpd_uri_t ws_route = {};
    ws_route.uri = "/ws";
    ws_route.method = HTTP_GET;
    ws_route.handler = ws_handler;
    ws_route.is_websocket = true;
    ws_route.ws_post_handshake_cb = ws_opened;

    const httpd_uri_t routes[] = {
        ws_route,
        {"/api/state", HTTP_GET, state_handler, nullptr, false, false, nullptr},
        {"/api/ring", HTTP_GET, ring_handler, nullptr, false, false, nullptr},
        {"/api/wear", HTTP_GET, wear_handler, nullptr, false, false, nullptr},
        {"/api/cmd", HTTP_POST, cmd_handler, nullptr, false, false, nullptr},
        {"/api/ring/upload", HTTP_POST, ring_upload_handler, nullptr, false, false, nullptr},
        {"/api/audio/*", HTTP_POST, audio_upload_handler, nullptr, false, false, nullptr},
        {"/*", HTTP_GET, static_handler, nullptr, false, false, nullptr},
    };
    // BEFORE the loop, deliberately.  esp_http_server checks handlers in
    // REGISTRATION order and the last entry above is the "/*" wildcard that
    // serves the web UI - register the OTA routes after it and the wildcard
    // swallows them, which presents as an update page that 404s its own POST.
    ESP_ERROR_CHECK(ota_register_routes(g_server));
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
