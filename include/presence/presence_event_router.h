
// =============================================================================
// FILE: include/presence/presence_event_router.h
// =============================================================================
#ifndef PRESENCE_EVENT_ROUTER_H
#define PRESENCE_EVENT_ROUTER_H

#include "common/types.h"
#include "common/config.h"
#include "presence/call_state_event.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <memory>

namespace sip_processor {

class DialogDispatcher;
class SlowEventLogger;
struct SipEvent;

class PresenceEventRouter {
public:
    PresenceEventRouter(const Config& config, DialogDispatcher& dispatcher,
                        std::shared_ptr<SlowEventLogger> slow_logger);
    ~PresenceEventRouter();

    Result start();
    void stop();
    void on_call_state_event(CallStateEvent&& event);
    void on_connection_state_changed(bool connected, const std::string& detail);

    struct RouterStats {
        std::atomic<uint64_t> events_received{0};
        std::atomic<uint64_t> events_processed{0};
        std::atomic<uint64_t> events_dropped{0};
        std::atomic<uint64_t> notifications_generated{0};
        std::atomic<uint64_t> watchers_not_found{0};
        std::atomic<uint64_t> queue_depth{0};
    };
    const RouterStats& stats() const { return stats_; }

    PresenceEventRouter(const PresenceEventRouter&) = delete;
    PresenceEventRouter& operator=(const PresenceEventRouter&) = delete;

private:
    void router_thread_func();
    void process_call_state_event(const CallStateEvent& event);
    std::string build_dialog_info_xml(const CallStateEvent& event,
                                       const std::string& monitored_uri) const;
    std::unique_ptr<SipEvent> create_notify_trigger(
        const std::string& dialog_id, const std::string& tenant_id,
        const CallStateEvent& event, const std::string& monitored_uri);

    Config config_;
    DialogDispatcher& dispatcher_;
    std::shared_ptr<SlowEventLogger> slow_logger_;

    std::thread router_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    mutable std::mutex queue_mu_;
    std::condition_variable queue_cv_;
    std::queue<CallStateEvent> event_queue_;
    RouterStats stats_;
};

} // namespace sip_processor
#endif

