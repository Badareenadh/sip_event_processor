
// =============================================================================
// FILE: include/dispatch/dialog_dispatcher.h
// =============================================================================
#ifndef DIALOG_DISPATCHER_H
#define DIALOG_DISPATCHER_H
#include "common/types.h"
#include "common/config.h"
#include "dispatch/dialog_worker.h"
#include "sip/sip_event.h"
#include <vector>
#include <memory>

namespace sip_processor {
class SlowEventLogger;
class SubscriptionStore;
class SipStackManager;

class DialogDispatcher {
public:
    DialogDispatcher(const Config& config,
                     std::shared_ptr<SlowEventLogger> slow_logger,
                     std::shared_ptr<SubscriptionStore> sub_store,
                     SipStackManager* stack_mgr = nullptr);
    ~DialogDispatcher();
    Result start();
    void stop();
    Result dispatch(std::unique_ptr<SipEvent> event);
    size_t worker_index_for(const std::string& dialog_id) const;
    size_t num_workers() const { return workers_.size(); }
    DialogWorker& worker(size_t idx) { return *workers_[idx]; }
    const DialogWorker& worker(size_t idx) const { return *workers_[idx]; }

    struct AggregateStats {
        uint64_t total_events_received = 0, total_events_processed = 0;
        uint64_t total_events_dropped = 0, total_presence_triggers = 0;
        uint64_t total_dialogs_active = 0, total_dialogs_reaped = 0;
        uint64_t max_queue_depth = 0, total_slow_events = 0;
    };
    AggregateStats aggregate_stats() const;

    DialogDispatcher(const DialogDispatcher&) = delete;
    DialogDispatcher& operator=(const DialogDispatcher&) = delete;
private:
    Config config_;
    std::vector<std::unique_ptr<DialogWorker>> workers_;
    bool started_ = false;
};
} // namespace sip_processor
#endif
