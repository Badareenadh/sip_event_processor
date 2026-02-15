
// =============================================================================
// FILE: src/presence/presence_tcp_client.cpp
// =============================================================================
#include "presence/presence_tcp_client.h"
#include "presence/presence_xml_parser.h"
#include "presence/presence_failover_manager.h"
#include "common/logger.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <cstring>

namespace sip_processor {

PresenceTcpClient::PresenceTcpClient(const Config& config,
                                       std::shared_ptr<PresenceFailoverManager> failover_mgr)
    : config_(config)
    , failover_mgr_(std::move(failover_mgr))
    , current_backoff_(config.presence_reconnect_interval)
    , parser_(std::make_unique<PresenceXmlParser>())
    , recv_buffer_(config.presence_recv_buffer_size, '\0')
{}

PresenceTcpClient::~PresenceTcpClient() { stop(); }

void PresenceTcpClient::set_event_callback(EventCallback cb) { event_callback_ = std::move(cb); }
void PresenceTcpClient::set_state_callback(StateCallback cb) { state_callback_ = std::move(cb); }

std::string PresenceTcpClient::connected_server() const {
    std::lock_guard<std::mutex> lk(server_mu_);
    if (current_server_.host.empty()) return "(none)";
    return current_server_.host + ":" + std::to_string(current_server_.port);
}

Result PresenceTcpClient::start() {
    if (running_.load(std::memory_order_acquire)) return Result::kAlreadyExists;
    if (!event_callback_) return Result::kInvalidArgument;
    stop_requested_.store(false); running_.store(true);
    reader_thread_ = std::thread(&PresenceTcpClient::reader_thread_func, this);
    LOG_INFO("PresenceTcpClient started");
    return Result::kOk;
}

void PresenceTcpClient::stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    stop_requested_.store(true);
    { std::lock_guard<std::mutex> lk(shutdown_mu_); }
    shutdown_cv_.notify_all();
    close_socket();
    if (reader_thread_.joinable()) reader_thread_.join();
    running_.store(false);
    LOG_INFO("PresenceTcpClient stopped");
}

void PresenceTcpClient::set_connection_state(ConnectionState state, const std::string& detail) {
    conn_state_.store(state);
    connected_.store(state == ConnectionState::kConnected);
    if (state_callback_) state_callback_(state, detail);
}

Result PresenceTcpClient::connect_to_server(const PresenceServerEndpoint& ep) {
    if (ep.host.empty()) return Result::kInvalidArgument;

    set_connection_state(ConnectionState::kConnecting, ep.host + ":" + std::to_string(ep.port));
    stats_.connect_attempts.fetch_add(1);

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    std::string port_str = std::to_string(ep.port);

    int gai = getaddrinfo(ep.host.c_str(), port_str.c_str(), &hints, &res);
    if (gai != 0) {
        LOG_ERROR("PresenceTcp: DNS failed for %s: %s", ep.host.c_str(), gai_strerror(gai));
        return Result::kError;
    }

    socket_fd_ = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (socket_fd_ < 0) { freeaddrinfo(res); return Result::kError; }

    int opt = 1;
    setsockopt(socket_fd_, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
    setsockopt(socket_fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    // Non-blocking connect
    int flags = fcntl(socket_fd_, F_GETFL, 0);
    if (flags >= 0) fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK);

    int cr = connect(socket_fd_, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    if (cr < 0 && errno != EINPROGRESS) { close_socket(); return Result::kError; }

    if (cr < 0) {
        struct pollfd pfd{socket_fd_, POLLOUT, 0};
        if (poll(&pfd, 1, 10000) <= 0) { close_socket(); return Result::kTimeout; }
        int sock_err = 0; socklen_t el = sizeof(sock_err);
        getsockopt(socket_fd_, SOL_SOCKET, SO_ERROR, &sock_err, &el);
        if (sock_err != 0) { close_socket(); return Result::kError; }
    }

    if (flags >= 0) fcntl(socket_fd_, F_SETFL, flags);

    struct timeval tv;
    tv.tv_sec = config_.presence_read_timeout.count(); tv.tv_usec = 0;
    setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    {
        std::lock_guard<std::mutex> lk(server_mu_);
        current_server_ = ep;
    }

    stats_.connect_successes.fetch_add(1);
    set_connection_state(ConnectionState::kConnected, ep.host + ":" + std::to_string(ep.port));
    current_backoff_ = config_.presence_reconnect_interval;

    { std::lock_guard<std::mutex> lk(heartbeat_mu_); last_heartbeat_ = Clock::now(); }
    parser_->reset();

    return Result::kOk;
}

void PresenceTcpClient::close_socket() {
    if (socket_fd_ >= 0) { shutdown(socket_fd_, SHUT_RDWR); close(socket_fd_); socket_fd_ = -1; }
    connected_.store(false);
}

void PresenceTcpClient::reader_thread_func() {
    while (!stop_requested_.load(std::memory_order_acquire)) {
        // Get next server from failover manager
        if (!failover_mgr_) break;
        auto ep = failover_mgr_->get_next_server();
        if (ep.host.empty()) {
            LOG_WARN("PresenceTcp: no servers available, waiting...");
            reconnect_with_backoff();
            continue;
        }

        Result r = connect_to_server(ep);
        if (r != Result::kOk) {
            failover_mgr_->report_failure(ep, result_to_string(r));
            stats_.failover_count.fetch_add(1);
            if (stop_requested_.load()) break;
            reconnect_with_backoff();
            continue;
        }

        failover_mgr_->report_success(ep);
        read_loop();

        // Disconnected
        close_socket();
        stats_.disconnect_count.fetch_add(1);
        set_connection_state(ConnectionState::kDisconnected);
        failover_mgr_->report_failure(ep, "disconnected");
        stats_.failover_count.fetch_add(1);

        if (!stop_requested_.load()) reconnect_with_backoff();
    }
    close_socket();
}

void PresenceTcpClient::read_loop() {
    while (!stop_requested_.load(std::memory_order_acquire)) {
        struct pollfd pfd{socket_fd_, POLLIN, 0};
        int pr = poll(&pfd, 1, 1000);

        if (pr < 0) { if (errno == EINTR) continue; return; }
        if (pr == 0) { check_heartbeat_timeout(); if (socket_fd_ < 0) return; continue; }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return;

        if (pfd.revents & POLLIN) {
            ssize_t bytes = recv(socket_fd_, recv_buffer_.data(), recv_buffer_.size(), 0);
            if (bytes <= 0) { if (bytes < 0 && (errno == EINTR || errno == EAGAIN)) continue; return; }

            stats_.bytes_received.fetch_add(static_cast<uint64_t>(bytes));

            auto pr_result = parser_->feed(recv_buffer_.data(), static_cast<size_t>(bytes));
            if (!pr_result.error.empty()) stats_.parse_errors.fetch_add(1);

            if (pr_result.received_heartbeat || !pr_result.events.empty()) {
                std::lock_guard<std::mutex> lk(heartbeat_mu_);
                last_heartbeat_ = Clock::now();
            }

            for (auto& ev : pr_result.events) {
                stats_.events_received.fetch_add(1);
                if (event_callback_) { event_callback_(std::move(ev)); stats_.events_delivered.fetch_add(1); }
            }
        }
    }
}

void PresenceTcpClient::check_heartbeat_timeout() {
    std::lock_guard<std::mutex> lk(heartbeat_mu_);
    auto elapsed = Clock::now() - last_heartbeat_;
    auto timeout = config_.presence_heartbeat_interval * config_.presence_heartbeat_miss_threshold;
    if (elapsed > timeout) {
        LOG_WARN("PresenceTcp: heartbeat timeout (%ldms)",
                 std::chrono::duration_cast<Millisecs>(elapsed).count());
        stats_.heartbeat_timeouts.fetch_add(1);
        close_socket();
    }
}

void PresenceTcpClient::reconnect_with_backoff() {
    set_connection_state(ConnectionState::kReconnecting,
                        "backoff=" + std::to_string(current_backoff_.count()) + "s");
    {
        std::unique_lock<std::mutex> lk(shutdown_mu_);
        shutdown_cv_.wait_for(lk, current_backoff_, [this] { return stop_requested_.load(); });
    }
    current_backoff_ = std::min(
        Seconds(current_backoff_.count() * 2),
        config_.presence_reconnect_max_interval);
}

} // namespace sip_processor

