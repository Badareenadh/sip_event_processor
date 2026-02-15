
// =============================================================================
// FILE: include/common/config.h
// =============================================================================
#ifndef COMMON_CONFIG_H
#define COMMON_CONFIG_H

#include "common/types.h"
#include <cstddef>
#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace sip_processor {

// Presence server endpoint for failover
struct PresenceServerEndpoint {
    std::string host;
    uint16_t    port     = 0;
    int         priority = 0;  // Lower = higher priority (for priority strategy)
    int         weight   = 1;  // For weighted strategies
};

// Failover strategy
enum class FailoverStrategy {
    kRoundRobin,
    kPriority,
    kRandom
};

inline FailoverStrategy parse_failover_strategy(const std::string& s) {
    if (s == "round_robin")  return FailoverStrategy::kRoundRobin;
    if (s == "priority")     return FailoverStrategy::kPriority;
    if (s == "random")       return FailoverStrategy::kRandom;
    return FailoverStrategy::kRoundRobin;
}

struct Config {
    // General
    std::string service_id     = "sip-proc-01";
    std::string instance_name  = "sip_event_processor";
    std::string log_level_str  = "info";

    // SIP stack
    std::string sip_bind_url   = "sip:*:5060";
    std::string sip_user_agent = "SIPEventProcessor/3.0";
    std::string sip_transport  = "udp";

    // Dispatcher
    size_t num_workers                   = 0;
    size_t max_incoming_queue_per_worker = 50000;
    size_t max_dialogs_per_worker        = 2000000;

    // Tenant
    size_t max_subscriptions_per_tenant  = 5000;

    // Reaper
    Seconds blf_subscription_ttl         = Seconds(3600);
    Seconds mwi_subscription_ttl         = Seconds(7200);
    Seconds reaper_scan_interval         = Seconds(60);
    Seconds stuck_processing_timeout     = Seconds(30);

    // Presence — multi-server failover
    std::vector<PresenceServerEndpoint> presence_servers;
    Seconds  presence_reconnect_interval     = Seconds(5);
    Seconds  presence_reconnect_max_interval = Seconds(60);
    Seconds  presence_read_timeout           = Seconds(30);
    size_t   presence_recv_buffer_size       = 65536;
    Seconds  presence_heartbeat_interval     = Seconds(15);
    int      presence_heartbeat_miss_threshold = 3;
    size_t   presence_max_pending_events     = 100000;
    FailoverStrategy presence_failover_strategy = FailoverStrategy::kRoundRobin;
    Seconds  presence_health_check_interval  = Seconds(30);
    Seconds  presence_server_cooldown        = Seconds(120);

    // MongoDB
    std::string mongo_uri                    = "mongodb://localhost:27017";
    std::string mongo_database               = "sip_event_processor";
    std::string mongo_collection_subs        = "subscriptions";
    std::string mongo_collection_blf_state   = "blf_state";
    int         mongo_pool_min_size          = 2;
    int         mongo_pool_max_size          = 10;
    std::string mongo_write_concern          = "majority";
    std::string mongo_read_preference        = "primaryPreferred";
    Millisecs   mongo_connect_timeout        = Millisecs(5000);
    Millisecs   mongo_socket_timeout         = Millisecs(10000);
    Seconds     mongo_sync_interval          = Seconds(5);
    size_t      mongo_batch_size             = 500;
    bool        mongo_enable_persistence     = true;

    // Slow event logging thresholds
    Millisecs slow_event_warn_threshold      = Millisecs(50);
    Millisecs slow_event_error_threshold     = Millisecs(200);
    Millisecs slow_event_critical_threshold  = Millisecs(1000);
    bool      slow_event_log_stack_trace     = false;

    // HTTP server
    bool        http_enabled            = true;
    std::string http_bind_address       = "0.0.0.0";
    uint16_t    http_port               = 8080;
    Seconds     http_read_timeout       = Seconds(30);
    Seconds     http_write_timeout      = Seconds(30);
    size_t      http_max_connections    = 100;

    // Logging
    std::string log_directory           = "/var/log/sip_processor";
    std::string log_base_name           = "sip_processor";
    std::string log_console_level_str   = "warn";
    size_t      log_max_file_size_mb    = 50;
    int         log_max_rotated_files   = 10;

    // Parse from INI-style config file
    static Config load_from_file(const std::string& path);
    static Config load_defaults();

private:
    // INI parser helper
    static std::unordered_map<std::string, std::string> parse_ini(const std::string& path);
    static std::string get_or(const std::unordered_map<std::string, std::string>& m,
                               const std::string& key, const std::string& def);
    static int get_int(const std::unordered_map<std::string, std::string>& m,
                        const std::string& key, int def);
    static size_t get_size(const std::unordered_map<std::string, std::string>& m,
                            const std::string& key, size_t def);
    static bool get_bool(const std::unordered_map<std::string, std::string>& m,
                          const std::string& key, bool def);
    static std::vector<PresenceServerEndpoint> parse_servers(const std::string& csv);
};

} // namespace sip_processor
#endif // COMMON_CONFIG_H