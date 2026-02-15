
// =============================================================================
// FILE: include/dispatch/stale_subscription_reaper.h
// =============================================================================
#ifndef STALE_SUBSCRIPTION_REAPER_H
#define STALE_SUBSCRIPTION_REAPER_H
#include "common/types.h"
#include "common/config.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
namespace sip_processor {
class DialogDispatcher;
class SipStackManager;
class SubscriptionStore;

class StaleSubscriptionReaper {
public:
    StaleSubscriptionReaper(const Config& config, DialogDispatcher& dispatcher,
                            SipStackManager* stack_mgr,
                            std::shared_ptr<SubscriptionStore> sub_store);
    ~StaleSubscriptionReaper();
    Result start();
    void stop();
    struct ReaperStats {
        std::atomic<uint64_t> scan_count{0};
        std::atomic<uint64_t> expired_reaped{0};
        std::atomic<uint64_t> stuck_reaped{0};
        std::atomic<uint64_t> last_scan_duration_ms{0};
        std::atomic<uint64_t> last_scan_stale_count{0};
    };
    const ReaperStats& stats() const { return stats_; }
    StaleSubscriptionReaper(const StaleSubscriptionReaper&) = delete;
    StaleSubscriptionReaper& operator=(const StaleSubscriptionReaper&) = delete;
private:
    void run();
    void scan_and_reap();
    Config config_;
    DialogDispatcher& dispatcher_;
    SipStackManager* stack_mgr_;
    std::shared_ptr<SubscriptionStore> sub_store_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::mutex mu_;
    std::condition_variable cv_;
    ReaperStats stats_;
};
} // namespace sip_processor
#endif