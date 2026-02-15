
// =============================================================================
// FILE: src/http/health_handler.cpp
// =============================================================================
#include "http/health_handler.h"
#include "dispatch/dialog_dispatcher.h"
#include "presence/presence_tcp_client.h"
#include "presence/presence_failover_manager.h"
#include "persistence/mongo_client.h"
#include "sip/sip_stack_manager.h"
#include <sstream>

namespace sip_processor {

void HealthHandler::register_routes(HttpServer& server, const Dependencies& deps) {
    auto deps_copy = deps;  // Capture by value for lambda lifetime

    server.route("GET", "/health", [deps_copy](const HttpServer::Request& req) {
        return handle_health(req, deps_copy);
    });

    server.route("GET", "/ready", [deps_copy](const HttpServer::Request& req) {
        return handle_ready(req, deps_copy);
    });
}

HttpServer::Response HealthHandler::handle_health(const HttpServer::Request&,
                                                    const Dependencies& deps) {
    HttpServer::Response resp;
    bool healthy = true;
    std::ostringstream json;
    json << "{";

    // SIP stack
    bool sip_ok = deps.sip_stack && deps.sip_stack->is_running();
    json << "\"sip_stack\":" << (sip_ok ? "true" : "false");
    if (!sip_ok) healthy = false;

    // Dispatcher
    bool disp_ok = deps.dispatcher != nullptr;
    json << ",\"dispatcher\":" << (disp_ok ? "true" : "false");

    // MongoDB
    if (deps.mongo_enabled) {
        bool mongo_ok = deps.mongo && deps.mongo->is_connected();
        json << ",\"mongodb\":" << (mongo_ok ? "true" : "false");
        if (!mongo_ok) healthy = false;
    }

    // Presence (degraded, not fatal)
    bool presence_ok = deps.presence_client && deps.presence_client->is_connected();
    json << ",\"presence_feed\":" << (presence_ok ? "true" : "false");
    if (deps.presence_client) {
        json << ",\"presence_server\":\"" << deps.presence_client->connected_server() << "\"";
    }
    if (deps.failover_mgr) {
        json << ",\"presence_healthy_servers\":" << deps.failover_mgr->healthy_count();
    }

    json << ",\"healthy\":" << (healthy ? "true" : "false");
    json << ",\"degraded\":" << (!presence_ok ? "true" : "false");
    json << "}";

    resp.status_code = healthy ? 200 : 503;
    resp.body = json.str();
    return resp;
}

HttpServer::Response HealthHandler::handle_ready(const HttpServer::Request&,
                                                   const Dependencies& deps) {
    HttpServer::Response resp;
    bool ready = deps.sip_stack && deps.sip_stack->is_running() &&
                 deps.dispatcher != nullptr;
    if (deps.mongo_enabled) ready = ready && deps.mongo && deps.mongo->is_connected();

    resp.status_code = ready ? 200 : 503;
    resp.body = ready ? R"({"ready":true})" : R"({"ready":false})";
    return resp;
}

} // namespace sip_processor
