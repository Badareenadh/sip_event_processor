
// =============================================================================
// FILE: src/http/stats_handler.cpp
// =============================================================================
#include "http/stats_handler.h"
#include "dispatch/dialog_dispatcher.h"
#include "dispatch/stale_subscription_reaper.h"
#include "presence/presence_tcp_client.h"
#include "presence/presence_event_router.h"
#include "presence/presence_failover_manager.h"
#include "persistence/mongo_client.h"
#include "persistence/subscription_store.h"
#include "subscription/subscription_state.h"
#include "subscription/blf_subscription_index.h"
#include "common/slow_event_logger.h"
#include "common/config.h"
#include <sstream>

namespace sip_processor {

void StatsHandler::register_routes(HttpServer& server, const Dependencies& deps) {
    auto d = deps;

    server.route("GET", "/stats", [d](const HttpServer::Request& r) { return handle_stats(r, d); });
    server.route("GET", "/stats/workers", [d](const HttpServer::Request& r) { return handle_stats_workers(r, d); });
    server.route("GET", "/stats/presence", [d](const HttpServer::Request& r) { return handle_stats_presence(r, d); });
    server.route("GET", "/subscriptions", [d](const HttpServer::Request& r) { return handle_subscriptions(r, d); });
    server.route("GET", "/config", [d](const HttpServer::Request& r) { return handle_config(r, d); });
}

HttpServer::Response StatsHandler::handle_stats(const HttpServer::Request&, const Dependencies& d) {
    HttpServer::Response resp;
    std::ostringstream j;
    j << "{";

    // Dispatcher stats
    if (d.dispatcher) {
        auto agg = d.dispatcher->aggregate_stats();
        j << "\"dispatcher\":{";
        j << "\"events_received\":" << agg.total_events_received;
        j << ",\"events_processed\":" << agg.total_events_processed;
        j << ",\"events_dropped\":" << agg.total_events_dropped;
        j << ",\"presence_triggers\":" << agg.total_presence_triggers;
        j << ",\"dialogs_active\":" << agg.total_dialogs_active;
        j << ",\"dialogs_reaped\":" << agg.total_dialogs_reaped;
        j << ",\"max_queue_depth\":" << agg.max_queue_depth;
        j << ",\"slow_events\":" << agg.total_slow_events;
        j << "}";
    }

    // Registry
    auto& reg = SubscriptionRegistry::instance();
    j << ",\"subscriptions\":{";
    j << "\"total\":" << reg.total_count();
    j << ",\"blf\":" << reg.count_by_type(SubscriptionType::kBLF);
    j << ",\"mwi\":" << reg.count_by_type(SubscriptionType::kMWI);
    j << "}";

    // BLF index
    auto& idx = BlfSubscriptionIndex::instance();
    j << ",\"blf_index\":{";
    j << "\"monitored_uris\":" << idx.monitored_uri_count();
    j << ",\"total_watchers\":" << idx.total_watcher_count();
    j << "}";

    // Reaper
    if (d.reaper) {
        auto& rs = d.reaper->stats();
        j << ",\"reaper\":{";
        j << "\"scans\":" << rs.scan_count.load();
        j << ",\"expired\":" << rs.expired_reaped.load();
        j << ",\"stuck\":" << rs.stuck_reaped.load();
        j << ",\"last_scan_ms\":" << rs.last_scan_duration_ms.load();
        j << "}";
    }

    // Slow events
    if (d.slow_logger) {
        auto& ss = d.slow_logger->stats();
        auto th = d.slow_logger->thresholds();
        j << ",\"slow_events\":{";
        j << "\"warn_count\":" << ss.warn_count.load();
        j << ",\"error_count\":" << ss.error_count.load();
        j << ",\"critical_count\":" << ss.critical_count.load();
        j << ",\"max_duration_ms\":" << ss.max_duration_ms.load();
        j << ",\"warn_threshold_ms\":" << th.warn.count();
        j << ",\"error_threshold_ms\":" << th.error.count();
        j << ",\"critical_threshold_ms\":" << th.critical.count();
        j << "}";
    }

    // MongoDB
    if (d.mongo) {
        auto& ms = d.mongo->stats();
        j << ",\"mongodb\":{";
        j << "\"connected\":" << (d.mongo->is_connected() ? "true" : "false");
        j << ",\"operations\":" << ms.operations.load();
        j << ",\"errors\":" << ms.errors.load();
        j << "}";
    }

    if (d.sub_store) {
        auto& ss = d.sub_store->stats();
        j << ",\"persistence\":{";
        j << "\"upserts\":" << ss.upserts.load();
        j << ",\"deletes\":" << ss.deletes.load();
        j << ",\"loads\":" << ss.loads.load();
        j << ",\"errors\":" << ss.errors.load();
        j << ",\"batch_writes\":" << ss.batch_writes.load();
        j << ",\"queue_depth\":" << ss.queue_depth.load();
        j << "}";
    }

    j << "}";
    resp.body = j.str();
    return resp;
}

HttpServer::Response StatsHandler::handle_stats_workers(const HttpServer::Request&,
                                                          const Dependencies& d) {
    HttpServer::Response resp;
    std::ostringstream j;
    j << "{\"workers\":[";

    if (d.dispatcher) {
        for (size_t i = 0; i < d.dispatcher->num_workers(); ++i) {
            if (i > 0) j << ",";
            auto& w = d.dispatcher->worker(i);
            auto& s = w.stats();
            j << "{\"index\":" << i;
            j << ",\"events_received\":" << s.events_received.load();
            j << ",\"events_processed\":" << s.events_processed.load();
            j << ",\"events_dropped\":" << s.events_dropped.load();
            j << ",\"presence_triggers\":" << s.presence_triggers_processed.load();
            j << ",\"dialogs_active\":" << s.dialogs_active.load();
            j << ",\"queue_depth\":" << s.queue_depth.load();
            j << ",\"slow_events\":" << s.slow_events.load();
            j << "}";
        }
    }

    j << "]}";
    resp.body = j.str();
    return resp;
}

HttpServer::Response StatsHandler::handle_stats_presence(const HttpServer::Request&,
                                                           const Dependencies& d) {
    HttpServer::Response resp;
    std::ostringstream j;
    j << "{";

    if (d.presence_client) {
        auto& ps = d.presence_client->stats();
        j << "\"client\":{";
        j << "\"connected\":" << (d.presence_client->is_connected() ? "true" : "false");
        j << ",\"server\":\"" << d.presence_client->connected_server() << "\"";
        j << ",\"events_received\":" << ps.events_received.load();
        j << ",\"bytes_received\":" << ps.bytes_received.load();
        j << ",\"connect_attempts\":" << ps.connect_attempts.load();
        j << ",\"connect_successes\":" << ps.connect_successes.load();
        j << ",\"disconnects\":" << ps.disconnect_count.load();
        j << ",\"failovers\":" << ps.failover_count.load();
        j << ",\"heartbeat_timeouts\":" << ps.heartbeat_timeouts.load();
        j << "}";
    }

    if (d.presence_router) {
        auto& rs = d.presence_router->stats();
        j << ",\"router\":{";
        j << "\"events_received\":" << rs.events_received.load();
        j << ",\"events_processed\":" << rs.events_processed.load();
        j << ",\"notifications_generated\":" << rs.notifications_generated.load();
        j << ",\"watchers_not_found\":" << rs.watchers_not_found.load();
        j << ",\"queue_depth\":" << rs.queue_depth.load();
        j << "}";
    }

    if (d.failover_mgr) {
        j << ",\"servers\":[";
        auto all = d.failover_mgr->get_all_health();
        for (size_t i = 0; i < all.size(); ++i) {
            if (i > 0) j << ",";
            auto& h = all[i];
            j << "{\"host\":\"" << h.endpoint.host << "\"";
            j << ",\"port\":" << h.endpoint.port;
            j << ",\"priority\":" << h.endpoint.priority;
            j << ",\"healthy\":" << (h.is_healthy ? "true" : "false");
            j << ",\"consecutive_failures\":" << h.consecutive_failures;
            j << ",\"total_successes\":" << h.total_successes;
            j << ",\"total_failures\":" << h.total_failures;
            j << "}";
        }
        j << "]";
    }

    j << "}";
    resp.body = j.str();
    return resp;
}

HttpServer::Response StatsHandler::handle_subscriptions(const HttpServer::Request& req,
                                                          const Dependencies& d) {
    HttpServer::Response resp;
    auto& reg = SubscriptionRegistry::instance();

    auto tenant_it = req.query_params.find("tenant");
    std::vector<SubscriptionRegistry::SubscriptionInfo> subs;

    if (tenant_it != req.query_params.end()) {
        subs = reg.get_tenant_subscriptions(tenant_it->second);
    } else {
        subs = reg.get_all();
    }

    std::ostringstream j;
    j << "{\"count\":" << subs.size() << ",\"subscriptions\":[";
    for (size_t i = 0; i < subs.size() && i < 1000; ++i) {  // Limit response
        if (i > 0) j << ",";
        auto& s = subs[i];
        j << "{\"dialog_id\":\"" << s.dialog_id << "\"";
        j << ",\"tenant_id\":\"" << s.tenant_id << "\"";
        j << ",\"type\":\"" << subscription_type_to_string(s.type) << "\"";
        j << ",\"lifecycle\":\"" << lifecycle_to_string(s.lifecycle) << "\"";
        j << ",\"worker\":" << s.worker_index;
        j << "}";
    }
    if (subs.size() > 1000) j << "],\"truncated\":true";
    else j << "]";
    j << "}";

    resp.body = j.str();
    return resp;
}

HttpServer::Response StatsHandler::handle_config(const HttpServer::Request&,
                                                   const Dependencies& d) {
    HttpServer::Response resp;
    if (!d.config) { resp.status_code = 500; return resp; }
    auto& c = *d.config;

    std::ostringstream j;
    j << "{";
    j << "\"service_id\":\"" << c.service_id << "\"";
    j << ",\"num_workers\":" << c.num_workers;
    j << ",\"max_subs_per_tenant\":" << c.max_subscriptions_per_tenant;
    j << ",\"blf_ttl_sec\":" << c.blf_subscription_ttl.count();
    j << ",\"mwi_ttl_sec\":" << c.mwi_subscription_ttl.count();
    j << ",\"presence_servers\":[";
    for (size_t i = 0; i < c.presence_servers.size(); ++i) {
        if (i > 0) j << ",";
        j << "\"" << c.presence_servers[i].host << ":" << c.presence_servers[i].port << "\"";
    }
    j << "]";
    j << ",\"failover_strategy\":\"" << (c.presence_failover_strategy == FailoverStrategy::kRoundRobin ? "round_robin" :
        c.presence_failover_strategy == FailoverStrategy::kPriority ? "priority" : "random") << "\"";
    j << ",\"mongo_enabled\":" << (c.mongo_enable_persistence ? "true" : "false");
    j << ",\"mongo_uri\":\"" << "***redacted***" << "\"";
    j << ",\"mongo_database\":\"" << c.mongo_database << "\"";
    j << ",\"slow_event_warn_ms\":" << c.slow_event_warn_threshold.count();
    j << ",\"slow_event_error_ms\":" << c.slow_event_error_threshold.count();
    j << ",\"slow_event_critical_ms\":" << c.slow_event_critical_threshold.count();
    j << "}";

    resp.body = j.str();
    return resp;
}

} // namespace sip_processor

