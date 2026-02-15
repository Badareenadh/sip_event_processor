
// =============================================================================
// FILE: include/http/stats_handler.h
// =============================================================================
#ifndef STATS_HANDLER_H
#define STATS_HANDLER_H

#include "http/http_server.h"
#include <memory>

namespace sip_processor {

class DialogDispatcher;
class PresenceTcpClient;
class PresenceEventRouter;
class PresenceFailoverManager;
class StaleSubscriptionReaper;
class MongoClient;
class SubscriptionStore;
class SlowEventLogger;
class SipStackManager;
struct Config;

// Registers stats, subscription, and config endpoints on the HTTP server.
class StatsHandler {
public:
    struct Dependencies {
        const Config*            config           = nullptr;
        DialogDispatcher*        dispatcher       = nullptr;
        SipStackManager*         sip_stack        = nullptr;
        PresenceTcpClient*       presence_client  = nullptr;
        PresenceEventRouter*     presence_router  = nullptr;
        PresenceFailoverManager* failover_mgr     = nullptr;
        StaleSubscriptionReaper* reaper           = nullptr;
        MongoClient*             mongo            = nullptr;
        SubscriptionStore*       sub_store        = nullptr;
        SlowEventLogger*         slow_logger      = nullptr;
    };

    static void register_routes(HttpServer& server, const Dependencies& deps);

private:
    static HttpServer::Response handle_stats(const HttpServer::Request& req,
                                              const Dependencies& deps);
    static HttpServer::Response handle_stats_workers(const HttpServer::Request& req,
                                                      const Dependencies& deps);
    static HttpServer::Response handle_stats_presence(const HttpServer::Request& req,
                                                       const Dependencies& deps);
    static HttpServer::Response handle_subscriptions(const HttpServer::Request& req,
                                                      const Dependencies& deps);
    static HttpServer::Response handle_config(const HttpServer::Request& req,
                                               const Dependencies& deps);
};

} // namespace sip_processor
#endif
