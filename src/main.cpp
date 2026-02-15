
// =============================================================================
// FILE: src/main.cpp
// =============================================================================
#include "common/config.h"
#include "common/logger.h"
#include "common/slow_event_logger.h"
#include "sip/sip_callback_handler.h"
#include "sip/sip_stack_manager.h"
#include "dispatch/dialog_dispatcher.h"
#include "dispatch/stale_subscription_reaper.h"
#include "presence/presence_tcp_client.h"
#include "presence/presence_event_router.h"
#include "presence/presence_failover_manager.h"
#include "persistence/mongo_client.h"
#include "persistence/subscription_store.h"
#include "subscription/blf_subscription_index.h"
#include "http/http_server.h"
#include "http/health_handler.h"
#include "http/stats_handler.h"
#include <csignal>
#include <atomic>

using namespace sip_processor;

static std::atomic<bool> g_shutdown{false};

static void signal_handler(int sig) {
    LOG_INFO("Signal %d received", sig);
    g_shutdown.store(true, std::memory_order_release);
}

int main(int argc, char* argv[]) {
    Logger::instance().set_level(LogLevel::kInfo);
    LOG_INFO("SIP Event Processor v3.0 starting...");

    // 1. Load config
    Config config = (argc > 1) ? Config::load_from_file(argv[1]) : Config::load_defaults();

    // Configure file-based logging with rotation
    Logger::instance().configure(
        config.log_directory,
        config.log_base_name,
        parse_log_level(config.log_console_level_str),
        config.log_max_file_size_mb * 1024 * 1024,
        config.log_max_rotated_files);
    Logger::instance().set_level(parse_log_level(config.log_level_str));

    // Signals
    struct sigaction sa{}; sa.sa_handler = signal_handler; sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr); sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    // 2. Shared components
    auto slow_logger = std::make_shared<SlowEventLogger>(config);

    // 3. MongoDB
    std::shared_ptr<MongoClient> mongo;
    std::shared_ptr<SubscriptionStore> sub_store;
    if (config.mongo_enable_persistence) {
        mongo = std::make_shared<MongoClient>(config);
        if (mongo->connect() != Result::kOk) {
            LOG_FATAL("MongoDB connection failed"); return 1;
        }
        sub_store = std::make_shared<SubscriptionStore>(config, mongo);
        sub_store->start();
    } else {
        sub_store = std::make_shared<SubscriptionStore>(config, nullptr);
    }

    // 4. Dispatcher
    DialogDispatcher dispatcher(config, slow_logger, sub_store);
    SipCallbackHandler::set_dispatcher(&dispatcher);

    // 5. Recovery: load subscriptions from MongoDB BEFORE starting dispatcher
    if (sub_store && sub_store->is_enabled()) {
        std::vector<SubscriptionStore::StoredSubscription> recovered;
        if (sub_store->load_active_subscriptions(recovered) == Result::kOk) {
            LOG_INFO("Recovering %zu subscriptions from MongoDB...", recovered.size());
            for (auto& stored : recovered) {
                size_t widx = dispatcher.worker_index_for(stored.record.dialog_id);
                dispatcher.worker(widx).load_recovered_subscription(std::move(stored.record));
            }
            LOG_INFO("Recovery complete: %zu subscriptions loaded", recovered.size());
        }
    }

    if (dispatcher.start() != Result::kOk) { LOG_FATAL("Dispatcher start failed"); return 1; }

    // 6. SIP stack
    SipStackManager stack(config);
    if (stack.start() != Result::kOk) { LOG_FATAL("SIP stack failed"); return 1; }

    // 7. Presence failover + router + TCP client
    auto failover_mgr = std::make_shared<PresenceFailoverManager>(config);

    PresenceEventRouter presence_router(config, dispatcher, slow_logger);
    presence_router.start();

    PresenceTcpClient presence_client(config, failover_mgr);
    presence_client.set_event_callback([&](CallStateEvent&& ev) {
        presence_router.on_call_state_event(std::move(ev));
    });
    presence_client.set_state_callback([&](PresenceTcpClient::ConnectionState state,
                                           const std::string& detail) {
        presence_router.on_connection_state_changed(
            state == PresenceTcpClient::ConnectionState::kConnected, detail);
    });
    presence_client.start();  // Non-fatal if it fails

    // 8. Reaper
    StaleSubscriptionReaper reaper(config, dispatcher, &stack, sub_store);
    reaper.start();

    // 9. HTTP server
    HttpServer http(config);
    if (config.http_enabled) {
        HealthHandler::Dependencies hdeps{&dispatcher, &stack, &presence_client,
                                           failover_mgr.get(), mongo.get(), config.mongo_enable_persistence};
        HealthHandler::register_routes(http, hdeps);

        StatsHandler::Dependencies sdeps{&config, &dispatcher, &stack, &presence_client,
                                          &presence_router, failover_mgr.get(), &reaper,
                                          mongo.get(), sub_store.get(), slow_logger.get()};
        StatsHandler::register_routes(http, sdeps);

        http.start();
    }

    LOG_INFO("All components started. service_id=%s", config.service_id.c_str());

    // Main loop
    uint64_t tick = 0;
    while (!g_shutdown.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(Seconds(1));
        if (++tick % 30 == 0) {
            auto agg = dispatcher.aggregate_stats();
            LOG_INFO("Stats: events=%lu/%lu dialogs=%lu slow=%lu presence=%s",
                     agg.total_events_processed, agg.total_events_received,
                     agg.total_dialogs_active, agg.total_slow_events,
                     presence_client.is_connected() ? "connected" : "disconnected");
        }
    }

    // Shutdown (reverse order)
    LOG_INFO("Shutting down...");
    http.stop();
    reaper.stop();
    presence_client.stop();
    presence_router.stop();
    stack.stop();
    SipCallbackHandler::set_dispatcher(nullptr);
    dispatcher.stop();
    if (sub_store) sub_store->stop();
    if (mongo) mongo->disconnect();

    LOG_INFO("SIP Event Processor stopped cleanly.");
    return 0;
}