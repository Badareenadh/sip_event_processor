
// =============================================================================
// FILE: include/http/health_handler.h
// =============================================================================
#ifndef HEALTH_HANDLER_H
#define HEALTH_HANDLER_H

#include "http/http_server.h"
#include <memory>
#include <functional>

namespace sip_processor {

class DialogDispatcher;
class PresenceTcpClient;
class PresenceFailoverManager;
class MongoClient;
class SipStackManager;

// Registers health and readiness endpoints on the HTTP server.
// Health is determined by:
//   - SIP stack running
//   - At least one worker thread alive
//   - MongoDB connected (if persistence enabled)
//   - Presence feed connected (degraded if not)
class HealthHandler {
public:
    struct Dependencies {
        DialogDispatcher*       dispatcher      = nullptr;
        SipStackManager*        sip_stack       = nullptr;
        PresenceTcpClient*      presence_client = nullptr;
        PresenceFailoverManager* failover_mgr   = nullptr;
        MongoClient*            mongo           = nullptr;
        bool                    mongo_enabled   = false;
    };

    static void register_routes(HttpServer& server, const Dependencies& deps);

private:
    static HttpServer::Response handle_health(const HttpServer::Request& req,
                                               const Dependencies& deps);
    static HttpServer::Response handle_ready(const HttpServer::Request& req,
                                              const Dependencies& deps);
};

} // namespace sip_processor
#endif
