
// =============================================================================
// FILE: include/presence/presence_tcp_client.h
// =============================================================================
#ifndef PRESENCE_TCP_CLIENT_H
#define PRESENCE_TCP_CLIENT_H

#include "common/types.h"
#include "common/config.h"
#include "presence/call_state_event.h"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <memory>
#include <vector>

namespace sip_processor {

class PresenceXmlParser;
class PresenceFailoverManager;

class PresenceTcpClient {
public:
    using EventCallback = std::function<void(CallStateEvent&&)>;

    enum class ConnectionState { kDisconnected, kConnecting, kConnected, kReconnecting };
    using StateCallback = std::function<void(ConnectionState, const std::string&)>;

    PresenceTcpClient(const Config& config,
                      std::shared_ptr<PresenceFailoverManager> failover_mgr);
    ~PresenceTcpClient();

    void set_event_callback(EventCallback cb);
    void set_state_callback(StateCallback cb);

    Result start();
    void stop();

    bool is_connected() const { return connected_.load(std::memory_order_acquire); }
    ConnectionState connection_state() const { return conn_state_.load(std::memory_order_acquire); }

    // Currently connected server info
    std::string connected_server() const;

    struct ClientStats {
        std::atomic<uint64_t> events_received{0};
        std::atomic<uint64_t> events_delivered{0};
        std::atomic<uint64_t> bytes_received{0};
        std::atomic<uint64_t> connect_attempts{0};
        std::atomic<uint64_t> connect_successes{0};
        std::atomic<uint64_t> disconnect_count{0};
        std::atomic<uint64_t> failover_count{0};
        std::atomic<uint64_t> heartbeat_timeouts{0};
        std::atomic<uint64_t> parse_errors{0};
    };
    const ClientStats& stats() const { return stats_; }

    PresenceTcpClient(const PresenceTcpClient&) = delete;
    PresenceTcpClient& operator=(const PresenceTcpClient&) = delete;

private:
    void reader_thread_func();
    Result connect_to_server(const PresenceServerEndpoint& ep);
    void close_socket();
    void read_loop();
    void reconnect_with_backoff();
    void check_heartbeat_timeout();
    void set_connection_state(ConnectionState state, const std::string& detail = "");

    Config config_;
    std::shared_ptr<PresenceFailoverManager> failover_mgr_;

    int socket_fd_ = -1;
    PresenceServerEndpoint current_server_;
    mutable std::mutex server_mu_;

    std::thread reader_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> connected_{false};
    std::atomic<ConnectionState> conn_state_{ConnectionState::kDisconnected};

    std::mutex shutdown_mu_;
    std::condition_variable shutdown_cv_;

    Seconds current_backoff_;

    TimePoint last_heartbeat_;
    std::mutex heartbeat_mu_;

    std::unique_ptr<PresenceXmlParser> parser_;
    EventCallback event_callback_;
    StateCallback state_callback_;
    ClientStats stats_;
    std::vector<char> recv_buffer_;
};

} // namespace sip_processor
#endif

