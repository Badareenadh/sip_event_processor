
// =============================================================================
// FILE: src/common/config.cpp
// =============================================================================
#include "common/config.h"
#include "common/logger.h"
#include <thread>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstdlib>

namespace sip_processor {

std::unordered_map<std::string, std::string> Config::parse_ini(const std::string& path) {
    std::unordered_map<std::string, std::string> map;
    std::ifstream file(path);
    if (!file.is_open()) {
        LOG_WARN("Cannot open config file: %s", path.c_str());
        return map;
    }

    std::string section, line;
    while (std::getline(file, line)) {
        // Trim
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;

        if (line[0] == '[') {
            auto end = line.find(']');
            if (end != std::string::npos) section = line.substr(1, end - 1);
            continue;
        }

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        key.erase(key.find_last_not_of(" \t") + 1);
        key.erase(0, key.find_first_not_of(" \t"));
        val.erase(0, val.find_first_not_of(" \t"));
        val.erase(val.find_last_not_of(" \t") + 1);

        // Support env var substitution: ${ENV_VAR}
        size_t pos = 0;
        while ((pos = val.find("${", pos)) != std::string::npos) {
            auto end = val.find('}', pos);
            if (end == std::string::npos) break;
            std::string env_name = val.substr(pos + 2, end - pos - 2);
            const char* env_val = std::getenv(env_name.c_str());
            val.replace(pos, end - pos + 1, env_val ? env_val : "");
        }

        std::string full_key = section.empty() ? key : section + "." + key;
        map[full_key] = val;
    }
    return map;
}

std::string Config::get_or(const std::unordered_map<std::string, std::string>& m,
                            const std::string& key, const std::string& def) {
    auto it = m.find(key); return (it != m.end()) ? it->second : def;
}

int Config::get_int(const std::unordered_map<std::string, std::string>& m,
                     const std::string& key, int def) {
    auto it = m.find(key);
    if (it == m.end()) return def;
    try { return std::stoi(it->second); } catch (...) { return def; }
}

size_t Config::get_size(const std::unordered_map<std::string, std::string>& m,
                         const std::string& key, size_t def) {
    auto it = m.find(key);
    if (it == m.end()) return def;
    try { return std::stoull(it->second); } catch (...) { return def; }
}

bool Config::get_bool(const std::unordered_map<std::string, std::string>& m,
                       const std::string& key, bool def) {
    auto it = m.find(key);
    if (it == m.end()) return def;
    return (it->second == "true" || it->second == "1" || it->second == "yes");
}

std::vector<PresenceServerEndpoint> Config::parse_servers(const std::string& csv) {
    std::vector<PresenceServerEndpoint> servers;
    std::istringstream stream(csv);
    std::string token;
    int priority = 0;

    while (std::getline(stream, token, ',')) {
        token.erase(0, token.find_first_not_of(" \t"));
        token.erase(token.find_last_not_of(" \t") + 1);
        if (token.empty()) continue;

        PresenceServerEndpoint ep;
        auto colon = token.rfind(':');
        if (colon != std::string::npos) {
            ep.host = token.substr(0, colon);
            try { ep.port = static_cast<uint16_t>(std::stoi(token.substr(colon + 1))); }
            catch (...) { ep.port = 9000; }
        } else {
            ep.host = token;
            ep.port = 9000;
        }
        ep.priority = priority++;
        ep.weight = 1;
        servers.push_back(std::move(ep));
    }
    return servers;
}

Config Config::load_defaults() {
    Config cfg;
    unsigned int hw = std::thread::hardware_concurrency();
    cfg.num_workers = (hw > 0) ? hw : 8;

    // Default single presence server
    cfg.presence_servers.push_back({"127.0.0.1", 9000, 0, 1});

    LOG_INFO("Config: defaults loaded, %zu workers", cfg.num_workers);
    return cfg;
}

Config Config::load_from_file(const std::string& path) {
    auto m = parse_ini(path);
    if (m.empty()) {
        LOG_WARN("Config: empty or missing file '%s', using defaults", path.c_str());
        return load_defaults();
    }

    Config c;

    // General
    c.service_id     = get_or(m, "general.service_id", c.service_id);
    c.instance_name  = get_or(m, "general.instance_name", c.instance_name);
    c.log_level_str  = get_or(m, "general.log_level", c.log_level_str);

    // SIP
    c.sip_bind_url   = get_or(m, "sip.bind_url", c.sip_bind_url);
    c.sip_user_agent = get_or(m, "sip.user_agent", c.sip_user_agent);
    c.sip_transport  = get_or(m, "sip.transport", c.sip_transport);

    // Dispatcher
    c.num_workers = get_size(m, "dispatcher.num_workers", 0);
    if (c.num_workers == 0) {
        unsigned int hw = std::thread::hardware_concurrency();
        c.num_workers = (hw > 0) ? hw : 8;
    }
    c.max_incoming_queue_per_worker = get_size(m, "dispatcher.max_incoming_queue_per_worker", c.max_incoming_queue_per_worker);
    c.max_dialogs_per_worker        = get_size(m, "dispatcher.max_dialogs_per_worker", c.max_dialogs_per_worker);

    // Tenant
    c.max_subscriptions_per_tenant = get_size(m, "tenant.max_subscriptions_per_tenant", c.max_subscriptions_per_tenant);

    // Reaper
    c.blf_subscription_ttl     = Seconds(get_int(m, "reaper.blf_subscription_ttl_sec", 3600));
    c.mwi_subscription_ttl     = Seconds(get_int(m, "reaper.mwi_subscription_ttl_sec", 7200));
    c.reaper_scan_interval     = Seconds(get_int(m, "reaper.scan_interval_sec", 60));
    c.stuck_processing_timeout = Seconds(get_int(m, "reaper.stuck_processing_timeout_sec", 30));

    // Presence
    std::string servers_csv = get_or(m, "presence.servers", "127.0.0.1:9000");
    c.presence_servers = parse_servers(servers_csv);
    c.presence_reconnect_interval     = Seconds(get_int(m, "presence.reconnect_interval_sec", 5));
    c.presence_reconnect_max_interval = Seconds(get_int(m, "presence.reconnect_max_interval_sec", 60));
    c.presence_read_timeout           = Seconds(get_int(m, "presence.read_timeout_sec", 30));
    c.presence_recv_buffer_size       = get_size(m, "presence.recv_buffer_size", 65536);
    c.presence_heartbeat_interval     = Seconds(get_int(m, "presence.heartbeat_interval_sec", 15));
    c.presence_heartbeat_miss_threshold = get_int(m, "presence.heartbeat_miss_threshold", 3);
    c.presence_max_pending_events     = get_size(m, "presence.max_pending_events", 100000);
    c.presence_failover_strategy = parse_failover_strategy(get_or(m, "presence.failover_strategy", "round_robin"));
    c.presence_health_check_interval = Seconds(get_int(m, "presence.health_check_interval_sec", 30));
    c.presence_server_cooldown       = Seconds(get_int(m, "presence.server_cooldown_sec", 120));

    // MongoDB
    c.mongo_uri                  = get_or(m, "mongodb.uri", c.mongo_uri);
    c.mongo_database             = get_or(m, "mongodb.database", c.mongo_database);
    c.mongo_collection_subs      = get_or(m, "mongodb.collection_subscriptions", c.mongo_collection_subs);
    c.mongo_collection_blf_state = get_or(m, "mongodb.collection_blf_state", c.mongo_collection_blf_state);
    c.mongo_pool_min_size        = get_int(m, "mongodb.pool_min_size", c.mongo_pool_min_size);
    c.mongo_pool_max_size        = get_int(m, "mongodb.pool_max_size", c.mongo_pool_max_size);
    c.mongo_write_concern        = get_or(m, "mongodb.write_concern", c.mongo_write_concern);
    c.mongo_read_preference      = get_or(m, "mongodb.read_preference", c.mongo_read_preference);
    c.mongo_connect_timeout      = Millisecs(get_int(m, "mongodb.connect_timeout_ms", 5000));
    c.mongo_socket_timeout       = Millisecs(get_int(m, "mongodb.socket_timeout_ms", 10000));
    c.mongo_sync_interval        = Seconds(get_int(m, "mongodb.sync_interval_sec", 5));
    c.mongo_batch_size           = get_size(m, "mongodb.batch_size", 500);
    c.mongo_enable_persistence   = get_bool(m, "mongodb.enable_persistence", true);

    // Slow event
    c.slow_event_warn_threshold     = Millisecs(get_int(m, "slow_event.warn_threshold_ms", 50));
    c.slow_event_error_threshold    = Millisecs(get_int(m, "slow_event.error_threshold_ms", 200));
    c.slow_event_critical_threshold = Millisecs(get_int(m, "slow_event.critical_threshold_ms", 1000));
    c.slow_event_log_stack_trace    = get_bool(m, "slow_event.log_stack_trace", false);

    // HTTP
    c.http_enabled         = get_bool(m, "http.enabled", true);
    c.http_bind_address    = get_or(m, "http.bind_address", c.http_bind_address);
    c.http_port            = static_cast<uint16_t>(get_int(m, "http.port", 8080));
    c.http_read_timeout    = Seconds(get_int(m, "http.read_timeout_sec", 30));
    c.http_write_timeout   = Seconds(get_int(m, "http.write_timeout_sec", 30));
    c.http_max_connections = get_size(m, "http.max_connections", 100);

    // Logging
    c.log_directory         = get_or(m, "logging.directory", c.log_directory);
    c.log_base_name         = get_or(m, "logging.base_name", c.log_base_name);
    c.log_console_level_str = get_or(m, "logging.console_level", c.log_console_level_str);
    c.log_max_file_size_mb  = get_size(m, "logging.max_file_size_mb", 50);
    c.log_max_rotated_files = get_int(m, "logging.max_rotated_files", 10);

    LOG_INFO("Config: loaded from '%s' — %zu workers, %zu presence servers, mongo=%s http=%s:%d",
             path.c_str(), c.num_workers, c.presence_servers.size(),
             c.mongo_enable_persistence ? "enabled" : "disabled",
             c.http_bind_address.c_str(), c.http_port);

    return c;
}

} // namespace sip_processor

