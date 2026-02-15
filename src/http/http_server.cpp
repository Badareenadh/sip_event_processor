
// =============================================================================
// FILE: src/http/http_server.cpp
// =============================================================================
#include "http/http_server.h"
#include "common/logger.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <algorithm>

namespace sip_processor {

HttpServer::HttpServer(const Config& config) : config_(config) {}

HttpServer::~HttpServer() { stop(); }

void HttpServer::route(const std::string& method, const std::string& path, Handler handler) {
    std::lock_guard<std::mutex> lk(routes_mu_);
    routes_[method + ":" + path] = std::move(handler);
}

Result HttpServer::start() {
    if (!config_.http_enabled) { LOG_INFO("HTTP server disabled"); return Result::kOk; }
    if (running_.load()) return Result::kAlreadyExists;

    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) { LOG_ERROR("HTTP: socket failed: %s", strerror(errno)); return Result::kError; }

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.http_port);
    inet_pton(AF_INET, config_.http_bind_address.c_str(), &addr.sin_addr);

    if (bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("HTTP: bind failed on %s:%d: %s",
                  config_.http_bind_address.c_str(), config_.http_port, strerror(errno));
        close(server_fd_); server_fd_ = -1;
        return Result::kError;
    }

    if (listen(server_fd_, static_cast<int>(config_.http_max_connections)) < 0) {
        LOG_ERROR("HTTP: listen failed"); close(server_fd_); server_fd_ = -1;
        return Result::kError;
    }

    stop_requested_.store(false); running_.store(true);
    server_thread_ = std::thread(&HttpServer::server_thread_func, this);

    LOG_INFO("HTTP server started on %s:%d", config_.http_bind_address.c_str(), config_.http_port);
    return Result::kOk;
}

void HttpServer::stop() {
    if (!running_.load()) return;
    stop_requested_.store(true);
    if (server_fd_ >= 0) { shutdown(server_fd_, SHUT_RDWR); close(server_fd_); server_fd_ = -1; }
    if (server_thread_.joinable()) server_thread_.join();
    running_.store(false);
    LOG_INFO("HTTP server stopped");
}

void HttpServer::server_thread_func() {
    while (!stop_requested_.load(std::memory_order_acquire)) {
        struct pollfd pfd{server_fd_, POLLIN, 0};
        int pr = poll(&pfd, 1, 500);
        if (pr <= 0) continue;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;

        struct sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd_, reinterpret_cast<sockaddr*>(&client_addr), &addr_len);
        if (client_fd < 0) { if (errno != EINTR) LOG_WARN("HTTP: accept failed"); continue; }

        stats_.requests_total.fetch_add(1);
        handle_client(client_fd);
        close(client_fd);
    }
}

void HttpServer::handle_client(int client_fd) {
    // Set read timeout
    struct timeval tv;
    tv.tv_sec = config_.http_read_timeout.count(); tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[8192];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return;
    buf[n] = '\0';

    Request req = parse_request(std::string(buf, n));

    // Find handler — try exact match, then prefix match
    Handler handler;
    {
        std::lock_guard<std::mutex> lk(routes_mu_);

        std::string key = req.method + ":" + req.path;
        auto it = routes_.find(key);
        if (it != routes_.end()) {
            handler = it->second;
        } else {
            // Prefix match (e.g., /subscriptions/xxx matches /subscriptions)
            for (auto& [route_key, h] : routes_) {
                auto colon = route_key.find(':');
                if (colon == std::string::npos) continue;
                std::string rm = route_key.substr(0, colon);
                std::string rp = route_key.substr(colon + 1);
                if (rm == req.method && req.path.find(rp) == 0) {
                    handler = h;
                    break;
                }
            }
        }
    }

    Response resp;
    if (handler) {
        try {
            resp = handler(req);
            stats_.requests_ok.fetch_add(1);
        } catch (const std::exception& e) {
            resp.status_code = 500;
            resp.body = R"({"error":")" + std::string(e.what()) + R"("})";
            stats_.requests_error.fetch_add(1);
        }
    } else {
        resp.status_code = 404;
        resp.body = R"({"error":"not_found","path":")" + req.path + R"("})";
    }

    std::string raw_resp = serialize_response(resp);
    send(client_fd, raw_resp.c_str(), raw_resp.size(), MSG_NOSIGNAL);
}

HttpServer::Request HttpServer::parse_request(const std::string& raw) {
    Request req;
    std::istringstream stream(raw);
    std::string line;

    // Request line: GET /path?query HTTP/1.1
    if (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto sp1 = line.find(' ');
        auto sp2 = line.find(' ', sp1 + 1);
        if (sp1 != std::string::npos && sp2 != std::string::npos) {
            req.method = line.substr(0, sp1);
            std::string full_path = line.substr(sp1 + 1, sp2 - sp1 - 1);

            auto qm = full_path.find('?');
            if (qm != std::string::npos) {
                req.path = full_path.substr(0, qm);
                req.query_string = full_path.substr(qm + 1);
                req.query_params = parse_query_string(req.query_string);
            } else {
                req.path = full_path;
            }
        }
    }

    // Headers
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        auto colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            val.erase(0, val.find_first_not_of(" \t"));
            req.headers[key] = val;
        }
    }

    return req;
}

std::unordered_map<std::string, std::string> HttpServer::parse_query_string(const std::string& qs) {
    std::unordered_map<std::string, std::string> params;
    std::istringstream stream(qs);
    std::string pair;
    while (std::getline(stream, pair, '&')) {
        auto eq = pair.find('=');
        if (eq != std::string::npos)
            params[pair.substr(0, eq)] = pair.substr(eq + 1);
        else
            params[pair] = "";
    }
    return params;
}

std::string HttpServer::serialize_response(const Response& resp) {
    std::string status_text;
    switch (resp.status_code) {
        case 200: status_text = "OK"; break;
        case 404: status_text = "Not Found"; break;
        case 500: status_text = "Internal Server Error"; break;
        case 503: status_text = "Service Unavailable"; break;
        default:  status_text = "Unknown"; break;
    }

    std::ostringstream ss;
    ss << "HTTP/1.1 " << resp.status_code << " " << status_text << "\r\n";
    ss << "Content-Type: " << resp.content_type << "\r\n";
    ss << "Content-Length: " << resp.body.size() << "\r\n";
    ss << "Connection: close\r\n";
    for (auto& [k, v] : resp.headers) ss << k << ": " << v << "\r\n";
    ss << "\r\n";
    ss << resp.body;
    return ss.str();
}

} // namespace sip_processor

