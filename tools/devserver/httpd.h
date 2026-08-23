// A tiny HTTP/1.1 + WebSocket server for the host dev server only.
//
// Not firmware: on target the same routes are served by esp_http_server.  It
// exists so the whole web UI - including the real /ws protocol - can be driven
// against the real ModeManager over simulated axes with no hardware and no new
// dependencies (CLAUDE.md: no managed components without a reason).
//
// Scope is deliberately small: one thread per connection, no keep-alive
// pipelining, no TLS, LAN/loopback only.
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace swan {
namespace devserver {

struct HttpRequest {
    std::string method;
    std::string path;   // without the query
    std::string query;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;

    // Case-insensitive lookup; empty when absent.
    std::string header(const std::string& name) const;
    bool accepts_gzip() const;
};

struct HttpResponse {
    int status = 200;
    std::string content_type = "text/plain; charset=utf-8";
    std::string body;
    std::vector<std::pair<std::string, std::string>> extra;

    static HttpResponse json(std::string body);
    static HttpResponse text(int status, std::string body);
};

// One WebSocket connection.  send() is safe from any thread.
class WsConn {
public:
    explicit WsConn(int fd) : fd_(fd) {}
    bool send(const std::string& text);
    void close();
    bool alive() const { return alive_.load(std::memory_order_relaxed); }

private:
    friend class Server;
    int fd_;
    std::mutex send_mu_;
    std::atomic<bool> alive_{true};
};

class Server {
public:
    ~Server();

    std::function<HttpResponse(const HttpRequest&)> on_http;
    std::function<void(WsConn&)> on_ws_open;
    std::function<void(WsConn&, const std::string&)> on_ws_message;
    std::function<void(WsConn&)> on_ws_close;

    // Binds and listens.  Returns false with *err set on failure.
    bool listen(int port, std::string* err);
    void run();   // blocks until stop()
    void stop();

    // Push to every live socket.  Dead ones are reaped.
    void broadcast(const std::string& text);
    size_t client_count();

private:
    void serve(int fd);
    bool ws_handshake(int fd, const HttpRequest& req);
    void ws_loop(const std::shared_ptr<WsConn>& c);

    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::mutex clients_mu_;
    std::vector<std::weak_ptr<WsConn>> clients_;
};

// Reads a file whole; returns false when it is not there.
bool read_file(const std::string& path, std::string& out);

// Content type from a file extension.  Unknown types fall back to
// application/octet-stream.
std::string content_type_for(const std::string& path);

}  // namespace devserver
}  // namespace swan
