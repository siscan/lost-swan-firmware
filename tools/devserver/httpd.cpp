#include "httpd.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#define SWAN_CLOSE closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#define SWAN_CLOSE ::close
#endif

namespace swan {
namespace devserver {
namespace {

// --------------------------------------------------------------------------
// SHA-1 and base64: the WebSocket handshake needs both and nothing else does.
// --------------------------------------------------------------------------
struct Sha1 {
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    unsigned char buf[64] = {};
    size_t len = 0;
    uint64_t total = 0;

    static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

    void block(const unsigned char* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(p[i * 4]) << 24) |
                   (static_cast<uint32_t>(p[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(p[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(p[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    void update(const void* data, size_t n) {
        const unsigned char* p = static_cast<const unsigned char*>(data);
        total += n;
        while (n > 0) {
            const size_t take = (64 - len) < n ? (64 - len) : n;
            std::memcpy(buf + len, p, take);
            len += take;
            p += take;
            n -= take;
            if (len == 64) {
                block(buf);
                len = 0;
            }
        }
    }

    void finish(unsigned char out[20]) {
        const uint64_t bits = total * 8;
        const unsigned char pad = 0x80;
        update(&pad, 1);
        const unsigned char zero = 0;
        while (len != 56) update(&zero, 1);
        unsigned char tail[8];
        for (int i = 0; i < 8; ++i) tail[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
        update(tail, 8);
        for (int i = 0; i < 5; ++i) {
            out[i * 4]     = static_cast<unsigned char>(h[i] >> 24);
            out[i * 4 + 1] = static_cast<unsigned char>(h[i] >> 16);
            out[i * 4 + 2] = static_cast<unsigned char>(h[i] >> 8);
            out[i * 4 + 3] = static_cast<unsigned char>(h[i]);
        }
    }
};

std::string base64(const unsigned char* p, size_t n) {
    static const char* kTbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t a = p[i];
        const uint32_t b = (i + 1 < n) ? p[i + 1] : 0;
        const uint32_t c = (i + 2 < n) ? p[i + 2] : 0;
        const uint32_t v = (a << 16) | (b << 8) | c;
        out += kTbl[(v >> 18) & 0x3F];
        out += kTbl[(v >> 12) & 0x3F];
        out += (i + 1 < n) ? kTbl[(v >> 6) & 0x3F] : '=';
        out += (i + 2 < n) ? kTbl[v & 0x3F] : '=';
    }
    return out;
}

std::string lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool send_all(int fd, const char* p, size_t n) {
    while (n > 0) {
        const int sent = static_cast<int>(::send(fd, p, static_cast<int>(n), 0));
        if (sent <= 0) return false;
        p += sent;
        n -= static_cast<size_t>(sent);
    }
    return true;
}

bool recv_exact(int fd, char* p, size_t n) {
    while (n > 0) {
        const int got = static_cast<int>(::recv(fd, p, static_cast<int>(n), 0));
        if (got <= 0) return false;
        p += got;
        n -= static_cast<size_t>(got);
    }
    return true;
}

const char* status_text(int s) {
    switch (s) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default:  return "OK";
    }
}

}  // namespace

// --------------------------------------------------------------------------
std::string HttpRequest::header(const std::string& name) const {
    const std::string want = lower(name);
    for (const auto& h : headers) {
        if (lower(h.first) == want) return h.second;
    }
    return {};
}

bool HttpRequest::accepts_gzip() const {
    return lower(header("Accept-Encoding")).find("gzip") != std::string::npos;
}

HttpResponse HttpResponse::json(std::string body) {
    HttpResponse r;
    r.content_type = "application/json";
    r.body = std::move(body);
    return r;
}

HttpResponse HttpResponse::text(int status, std::string body) {
    HttpResponse r;
    r.status = status;
    r.body = std::move(body);
    return r;
}

// --------------------------------------------------------------------------
bool WsConn::send(const std::string& text) {
    if (!alive_.load(std::memory_order_relaxed)) return false;

    std::string hdr;
    hdr += static_cast<char>(0x81);  // FIN + text
    const size_t n = text.size();
    if (n < 126) {
        hdr += static_cast<char>(n);
    } else if (n <= 0xFFFF) {
        hdr += static_cast<char>(126);
        hdr += static_cast<char>((n >> 8) & 0xFF);
        hdr += static_cast<char>(n & 0xFF);
    } else {
        hdr += static_cast<char>(127);
        for (int i = 7; i >= 0; --i) hdr += static_cast<char>((n >> (8 * i)) & 0xFF);
    }

    const std::lock_guard<std::mutex> lock(send_mu_);
    if (!send_all(fd_, hdr.data(), hdr.size()) || !send_all(fd_, text.data(), text.size())) {
        alive_.store(false, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void WsConn::close() { alive_.store(false, std::memory_order_relaxed); }

// --------------------------------------------------------------------------
Server::~Server() {
    stop();
    if (listen_fd_ >= 0) SWAN_CLOSE(listen_fd_);
}

bool Server::listen(int port, std::string* err) {
#if defined(_WIN32)
    static bool wsa_started = false;
    if (!wsa_started) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            if (err) *err = "WSAStartup failed";
            return false;
        }
        wsa_started = true;
    }
#endif
    listen_fd_ = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (listen_fd_ < 0) {
        if (err) *err = "socket() failed";
        return false;
    }
    int on = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on),
                 sizeof on);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<unsigned short>(port));
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        if (err) *err = "port " + std::to_string(port) + " is already in use";
        SWAN_CLOSE(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (::listen(listen_fd_, 16) != 0) {
        if (err) *err = "listen() failed";
        SWAN_CLOSE(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    return true;
}

void Server::run() {
    running_.store(true, std::memory_order_relaxed);
    while (running_.load(std::memory_order_relaxed)) {
        sockaddr_in peer{};
        socklen_t plen = sizeof peer;
        const int fd = static_cast<int>(::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer),
                                                 &plen));
        if (fd < 0) {
            if (!running_.load(std::memory_order_relaxed)) break;
            continue;
        }
        int on = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
        std::thread(&Server::serve, this, fd).detach();
    }
}

void Server::stop() {
    running_.store(false, std::memory_order_relaxed);
    if (listen_fd_ >= 0) {
        SWAN_CLOSE(listen_fd_);
        listen_fd_ = -1;
    }
}

size_t Server::client_count() {
    const std::lock_guard<std::mutex> lock(clients_mu_);
    size_t n = 0;
    for (const auto& w : clients_) {
        const auto c = w.lock();
        if (c && c->alive()) ++n;
    }
    return n;
}

void Server::broadcast(const std::string& text) {
    std::vector<std::shared_ptr<WsConn>> live;
    {
        const std::lock_guard<std::mutex> lock(clients_mu_);
        std::vector<std::weak_ptr<WsConn>> keep;
        keep.reserve(clients_.size());
        for (const auto& w : clients_) {
            const auto c = w.lock();
            if (c && c->alive()) {
                live.push_back(c);
                keep.push_back(w);
            }
        }
        clients_.swap(keep);
    }
    for (const auto& c : live) c->send(text);
}

void Server::serve(int fd) {
    // Read headers.
    std::string head;
    char ch;
    while (head.size() < 16 * 1024) {
        const int got = static_cast<int>(::recv(fd, &ch, 1, 0));
        if (got <= 0) {
            SWAN_CLOSE(fd);
            return;
        }
        head += ch;
        if (head.size() >= 4 && head.compare(head.size() - 4, 4, "\r\n\r\n") == 0) break;
    }

    HttpRequest req;
    {
        std::istringstream in(head);
        std::string line;
        if (!std::getline(in, line)) {
            SWAN_CLOSE(fd);
            return;
        }
        std::istringstream first(line);
        std::string target;
        first >> req.method >> target;
        const size_t q = target.find('?');
        req.path = (q == std::string::npos) ? target : target.substr(0, q);
        req.query = (q == std::string::npos) ? std::string() : target.substr(q + 1);
        while (std::getline(in, line) && line != "\r" && !line.empty()) {
            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string v = line.substr(colon + 1);
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
            while (!v.empty() && (v.back() == '\r' || v.back() == '\n')) v.pop_back();
            req.headers.emplace_back(line.substr(0, colon), v);
        }
    }

    // WebSocket?
    if (lower(req.header("Upgrade")) == "websocket") {
        if (!ws_handshake(fd, req)) {
            SWAN_CLOSE(fd);
            return;
        }
        auto conn = std::make_shared<WsConn>(fd);
        {
            const std::lock_guard<std::mutex> lock(clients_mu_);
            clients_.push_back(conn);
        }
        if (on_ws_open) on_ws_open(*conn);
        ws_loop(conn);
        if (on_ws_close) on_ws_close(*conn);
        conn->close();
        SWAN_CLOSE(fd);
        return;
    }

    // Body, if any.  Bounded: nothing this server accepts is bigger than a
    // ring.json upload, and an unbounded read is a trivial way to hang it.
    const std::string clen = req.header("Content-Length");
    size_t n = clen.empty() ? 0 : static_cast<size_t>(std::strtoul(clen.c_str(), nullptr, 10));
    HttpResponse res;
    if (n > 1024 * 1024) {
        res = HttpResponse::text(413, "body too large");
    } else {
        if (n > 0) {
            req.body.resize(n);
            if (!recv_exact(fd, &req.body[0], n)) {
                SWAN_CLOSE(fd);
                return;
            }
        }
        res = on_http ? on_http(req) : HttpResponse::text(500, "no handler");
    }

    std::string out = "HTTP/1.1 " + std::to_string(res.status) + " " +
                      status_text(res.status) + "\r\n";
    out += "Content-Type: " + res.content_type + "\r\n";
    out += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";
    out += "Connection: close\r\n";
    for (const auto& h : res.extra) out += h.first + ": " + h.second + "\r\n";
    out += "\r\n";
    send_all(fd, out.data(), out.size());
    if (req.method != "HEAD") send_all(fd, res.body.data(), res.body.size());
    SWAN_CLOSE(fd);
}

bool Server::ws_handshake(int fd, const HttpRequest& req) {
    const std::string key = req.header("Sec-WebSocket-Key");
    if (key.empty()) return false;
    const std::string magic = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    Sha1 sha;
    sha.update(magic.data(), magic.size());
    unsigned char digest[20];
    sha.finish(digest);

    std::string out = "HTTP/1.1 101 Switching Protocols\r\n";
    out += "Upgrade: websocket\r\n";
    out += "Connection: Upgrade\r\n";
    out += "Sec-WebSocket-Accept: " + base64(digest, 20) + "\r\n\r\n";
    return send_all(fd, out.data(), out.size());
}

void Server::ws_loop(const std::shared_ptr<WsConn>& c) {
    const int fd = c->fd_;
    std::string message;
    while (c->alive()) {
        unsigned char h[2];
        if (!recv_exact(fd, reinterpret_cast<char*>(h), 2)) break;
        const bool fin = (h[0] & 0x80) != 0;
        const int op = h[0] & 0x0F;
        const bool masked = (h[1] & 0x80) != 0;
        uint64_t len = h[1] & 0x7F;
        if (len == 126) {
            unsigned char e[2];
            if (!recv_exact(fd, reinterpret_cast<char*>(e), 2)) break;
            len = (static_cast<uint64_t>(e[0]) << 8) | e[1];
        } else if (len == 127) {
            unsigned char e[8];
            if (!recv_exact(fd, reinterpret_cast<char*>(e), 8)) break;
            len = 0;
            for (int i = 0; i < 8; ++i) len = (len << 8) | e[i];
        }
        if (len > 4 * 1024 * 1024) break;  // nothing legitimate is this big

        unsigned char mask[4] = {0, 0, 0, 0};
        if (masked && !recv_exact(fd, reinterpret_cast<char*>(mask), 4)) break;

        std::string payload(static_cast<size_t>(len), '\0');
        if (len > 0 && !recv_exact(fd, &payload[0], static_cast<size_t>(len))) break;
        if (masked) {
            for (size_t i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
            }
        }

        if (op == 0x8) break;  // close
        if (op == 0x9) {       // ping -> pong, same payload
            std::string pong;
            pong += static_cast<char>(0x8A);
            pong += static_cast<char>(payload.size() & 0x7F);
            pong += payload;
            const std::lock_guard<std::mutex> lock(c->send_mu_);
            if (!send_all(fd, pong.data(), pong.size())) break;
            continue;
        }
        if (op == 0xA) continue;  // pong

        message += payload;      // op 0x1 text or 0x0 continuation
        if (!fin) continue;
        if (on_ws_message) on_ws_message(*c, message);
        message.clear();
    }
    c->close();
}

// --------------------------------------------------------------------------
bool read_file(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

std::string content_type_for(const std::string& path) {
    struct Map {
        const char* ext;
        const char* type;
    };
    static const Map kMap[] = {
        {".html", "text/html; charset=utf-8"},
        {".css",  "text/css; charset=utf-8"},
        {".js",   "application/javascript; charset=utf-8"},
        {".json", "application/json"},
        {".svg",  "image/svg+xml"},
        {".png",  "image/png"},
        {".ico",  "image/x-icon"},
        {".txt",  "text/plain; charset=utf-8"},
        {".woff2", "font/woff2"},
    };
    std::string p = lower(path);
    if (p.size() > 3 && p.compare(p.size() - 3, 3, ".gz") == 0) p.erase(p.size() - 3);
    for (const Map& m : kMap) {
        const size_t n = std::strlen(m.ext);
        if (p.size() >= n && p.compare(p.size() - n, n, m.ext) == 0) return m.type;
    }
    return "application/octet-stream";
}

}  // namespace devserver
}  // namespace swan
