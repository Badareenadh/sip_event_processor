
// =============================================================================
// FILE: src/dispatch/stale_subscription_reaper.cpp
// =============================================================================
#include "dispatch/stale_subscription_reaper.h"
#include "dispatch/dialog_dispatcher.h"
#include "persistence/subscription_store.h"
#include "common/logger.h"

namespace sip_processor {

StaleSubscriptionReaper::StaleSubscriptionReaper(
    const Config& config, DialogDispatcher& dispatcher,
    SipStackManager* stack_mgr, std::shared_ptr<SubscriptionStore> sub_store)
    : config_(config), dispatcher_(dispatcher), stack_mgr_(stack_mgr), sub_store_(std::move(sub_store))
{}

StaleSubscriptionReaper::~StaleSubscriptionReaper() { stop(); }

Result StaleSubscriptionReaper::start() {
    if (running_.load()) return Result::kAlreadyExists;
    stop_requested_.store(false); running_.store(true);
    thread_ = std::thread(&StaleSubscriptionReaper::run, this);
    return Result::kOk;
}

void StaleSubscriptionReaper::stop() {
    if (!running_.load()) return;
    { std::lock_guard<std::mutex> lk(mu_); stop_requested_.store(true); }
    cv_.notify_one();
    if (thread_.joinable()) thread_.join();
    running_.store(false);
}

void StaleSubscriptionReaper::run() {
    while (!stop_requested_.load()) {
        { std::unique_lock<std::mutex> lk(mu_);
          cv_.wait_for(lk, config_.reaper_scan_interval, [this]{ return stop_requested_.load(); }); }
        if (stop_requested_.load()) break;
        scan_and_reap();
    }
}

void StaleSubscriptionReaper::scan_and_reap() {
    ScopedTimer timer;
    stats_.scan_count.fetch_add(1);
    size_t total = 0;

    for (size_t i = 0; i < dispatcher_.num_workers(); ++i) {
        auto& w = dispatcher_.worker(i);
        auto stale = w.get_stale_subscriptions(
            config_.blf_subscription_ttl, config_.mwi_subscription_ttl,
            config_.stuck_processing_timeout);

        for (const auto& info : stale) {
            if (info.is_stuck) stats_.stuck_reaped.fetch_add(1);
            else stats_.expired_reaped.fetch_add(1);

            w.force_terminate(info.dialog_id);
            if (sub_store_) sub_store_->queue_delete(info.dialog_id);
            total++;
        }
    }

    stats_.last_scan_duration_ms.store(timer.elapsed_ms().count());
    stats_.last_scan_stale_count.store(total);
    if (total > 0) LOG_INFO("Reaper: %zu reaped in %ldms", total, timer.elapsed_ms().count());
}

} // namespace sip_processor
