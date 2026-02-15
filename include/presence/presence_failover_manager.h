
// =============================================================================
// FILE: include/presence/presence_failover_manager.h
// =============================================================================
#ifndef PRESENCE_FAILOVER_MANAGER_H
#define PRESENCE_FAILOVER_MANAGER_H

#include "common/types.h"
#include "common/config.h"
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>

namespace sip_processor {

// Manages a pool of presence servers with health tracking and failover.
//
// Features:
//   - Multiple failover strategies: round-robin, priority, random
//   - Per-server health tracking: consecutive failures, last success, cooldown
//   - Cooldown period after failures before retrying a server
//   - Health check results integration
//   - Thread-safe: called from TCP client reader thread
//
// Usage:
//   auto ep = failover_mgr.get_next_server();  // Returns best available server
//   ... try to connect ...
//   failover_mgr.report_success(ep);   // On successful connect
//   failover_mgr.report_failure(ep);   // On connect failure or disconnect
class PresenceFailoverManager {
public:
    explicit PresenceFailoverManager(const Config& config);
    ~PresenceFailoverManager() = default;

    struct ServerHealth {
        PresenceServerEndpoint endpoint;
        bool     is_healthy         = true;
        int      consecutive_failures = 0;
        int      total_failures     = 0;
        int      total_successes    = 0;
        TimePoint last_attempt      = {};
        TimePoint last_success      = {};
        TimePoint last_failure      = {};
        TimePoint cooldown_until    = {};  // Don't retry until this time
        Millisecs avg_latency       = Millisecs(0);
    };

    // Get the next server to try connecting to.
    // Returns empty endpoint if all servers are in cooldown.
    PresenceServerEndpoint get_next_server();

    // Report connection outcome
    void report_success(const PresenceServerEndpoint& server);
    void report_failure(const PresenceServerEndpoint& server, const std::string& reason = "");

    // Mark a specific server as unhealthy (e.g., from health check)
    void mark_unhealthy(const PresenceServerEndpoint& server);
    void mark_healthy(const PresenceServerEndpoint& server);

    // Get health status of all servers (for HTTP API)
    std::vector<ServerHealth> get_all_health() const;

    // Check if any server is available
    bool any_server_available() const;

    // Get count of healthy servers
    size_t healthy_count() const;

    // Reset all servers to healthy (e.g., after config reload)
    void reset_all();

    PresenceFailoverManager(const PresenceFailoverManager&) = delete;
    PresenceFailoverManager& operator=(const PresenceFailoverManager&) = delete;

private:
    // Find server index by host:port
    int find_server(const PresenceServerEndpoint& ep) const;
    bool is_in_cooldown(const ServerHealth& health) const;

    // Strategy implementations
    int select_round_robin();
    int select_priority();
    int select_random();

    Config config_;
    mutable std::mutex mu_;
    std::vector<ServerHealth> servers_;
    size_t round_robin_index_ = 0;
};

} // namespace sip_processor
#endif // PRESENCE_FAILOVER_MANAGER_H
