
// =============================================================================
// FILE: include/dispatch/dialog_worker.h
// =============================================================================
#ifndef DIALOG_WORKER_H
#define DIALOG_WORKER_H

#include "common/types.h"
#include "common/config.h"
#include "sip/sip_event.h"
#include "subscription/subscription_state.h"
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <memory>

namespace sip_processor {

class BlfProcessor;
class MwiProcessor;
class SlowEventLogger;
class SubscriptionStore;

struct WorkerStats {
    std::atomic<uint64_t> events_received{0};
    std::atomic<uint64_t> events_processed{0};
    std::atomic<uint64_t> events_dropped{0};
    std::atomic<uint64_t> presence_triggers_processed{0};
    std::atomic<uint64_t> dialogs_active{0};
    std::atomic<uint64_t> dialogs_reaped{0};
    std::atomic<uint64_t> queue_depth{0};
    std::atomic<uint64_t> slow_events{0};
};

class DialogWorker {
public:
    DialogWorker(size_t worker_index, const Config& config,
                 std::shared_ptr<SlowEventLogger> slow_logger,
                 std::shared_ptr<SubscriptionStore> sub_store);
    ~DialogWorker();

    Result start();
    void stop();
    Result enqueue(std::unique_ptr<SipEvent> event);

    struct StaleInfo {
        std::string dialog_id;
        std::string tenant_id;
        SubscriptionType type;
        SubLifecycle lifecycle;
        TimePoint last_activity;
        bool is_stuck;
    };
    std::vector<StaleInfo> get_stale_subscriptions(
        Seconds blf_ttl, Seconds mwi_ttl, Seconds stuck_timeout) const;

    Result force_terminate(const std::string& dialog_id);

    // Load recovered subscriptions from MongoDB into this worker
    Result load_recovered_subscription(SubscriptionRecord record);

    const WorkerStats& stats() const { return stats_; }
    size_t worker_index() const { return worker_index_; }

    DialogWorker(const DialogWorker&) = delete;
    DialogWorker& operator=(const DialogWorker&) = delete;

private:
    void run();
    void process_dialog_queues();
    void process_event(const std::string& dialog_id, SubscriptionRecord& record,
                       std::unique_ptr<SipEvent> event);
    void process_presence_trigger(const std::string& dialog_id,
                                   SubscriptionRecord& record, const SipEvent& event);
    void handle_new_subscription(const std::string& dialog_id, const SipEvent& event);
    void cleanup_terminated_dialogs();
    void index_blf_subscription(const std::string& dialog_id, const SubscriptionRecord& rec);
    void deindex_blf_subscription(const std::string& dialog_id, const SubscriptionRecord& rec);
    void persist_record(const SubscriptionRecord& record, bool immediate = false);

    size_t worker_index_;
    Config config_;
    std::shared_ptr<SlowEventLogger> slow_logger_;
    std::shared_ptr<SubscriptionStore> sub_store_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    mutable std::mutex incoming_mu_;
    std::condition_variable incoming_cv_;
    std::queue<std::unique_ptr<SipEvent>> incoming_queue_;

    mutable std::mutex terminate_mu_;
    std::vector<std::string> pending_terminates_;

    struct DialogContext {
        SubscriptionRecord record;
        std::queue<std::unique_ptr<SipEvent>> event_queue;
    };
    std::unordered_map<std::string, DialogContext> dialogs_;

    std::unique_ptr<BlfProcessor> blf_processor_;
    std::unique_ptr<MwiProcessor> mwi_processor_;
    WorkerStats stats_;
    uint64_t process_cycle_ = 0;
    static constexpr uint64_t kCleanupInterval = 1000;
};

} // namespace sip_processor
#endif