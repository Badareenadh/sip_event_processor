

// =============================================================================
// FILE: include/http/http_server.h
// =============================================================================
#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "common/types.h"
#include "common/config.h"
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <vector>

namespace sip_processor {

// Minimal embedded HTTP server for health checks, stats, and admin operations.
//
// Endpoints:
//   GET  /health          → Health check (200 OK / 503 Unhealthy)
//   GET  /ready           → Readiness check
//   GET  /stats           → Full system statistics JSON
//   GET  /stats/workers   → Per-worker stats
//   GET  /stats/presence  → Presence connection stats
//   GET  /stats/mongo     → MongoDB stats
//   GET  /subscriptions                      → All subscriptions summary
//   GET  /subscriptions?tenant=<id>          → Subscriptions for tenant
//   GET  /subscriptions/<dialog_id>          → Single subscription detail
//   GET  /config          → Current configuration (redacted)
//
// Implementation: Single-threaded select-based HTTP/1.1 server.
// For production, consider replacing with a library (cpp-httplib, crow, etc.)
class HttpServer {
public:
    explicit HttpServer(const Config& config);
    ~HttpServer();

    // HTTP request/response types
    struct Request {
        std::string method;
        std::string path;
        std::string query_string;
        std::unordered_map<std::string, std::string> query_params;
        std::unordered_map<std::string, std::string> headers;
        std::string body;
    };

    struct Response {
        int status_code = 200;
        std::string content_type = "application/json";
        std::string body;
        std::unordered_map<std::string, std::string> headers;
    };

    using Handler = std::function<Response(const Request&)>;

    // Register a route handler
    void route(const std::string& method, const std::string& path, Handler handler);

    Result start();
    void stop();
    bool is_running() const { return running_.load(std::memory_order_acquire); }

    struct ServerStats {
        std::atomic<uint64_t> requests_total{0};
        std::atomic<uint64_t> requests_ok{0};
        std::atomic<uint64_t> requests_error{0};
        std::atomic<uint64_t> active_connections{0};
    };
    const ServerStats& stats() const { return stats_; }

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

private:
    void server_thread_func();
    void handle_client(int client_fd);
    Request parse_request(const std::string& raw);
    std::string serialize_response(const Response& resp);
    std::unordered_map<std::string, std::string> parse_query_string(const std::string& qs);

    Config config_;
    int server_fd_ = -1;
    std::thread server_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    // Route table: "METHOD:path" → handler
    std::mutex routes_mu_;
    std::unordered_map<std::string, Handler> routes_;

    ServerStats stats_;
};

} // namespace sip_processor
#endif // HTTP_SERVER_H
