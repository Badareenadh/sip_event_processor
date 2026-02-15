
// =============================================================================
// FILE: src/dispatch/dialog_dispatcher.cpp
// =============================================================================
#include "dispatch/dialog_dispatcher.h"
#include "sip/sip_dialog_id.h"
#include "common/logger.h"
#include <functional>

namespace sip_processor {

DialogDispatcher::DialogDispatcher(const Config& config,
                                     std::shared_ptr<SlowEventLogger> slow_logger,
                                     std::shared_ptr<SubscriptionStore> sub_store,
                                     SipStackManager* stack_mgr)
    : config_(config) {
    size_t n = config_.num_workers > 0 ? config_.num_workers : 8;
    workers_.reserve(n);
    for (size_t i = 0; i < n; ++i)
        workers_.push_back(std::make_unique<DialogWorker>(i, config_, slow_logger, sub_store, stack_mgr));
}

DialogDispatcher::~DialogDispatcher() { stop(); }

Result DialogDispatcher::start() {
    if (started_) return Result::kAlreadyExists;
    for (auto& w : workers_) { auto r = w->start(); if (r != Result::kOk) { stop(); return r; } }
    started_ = true; return Result::kOk;
}

void DialogDispatcher::stop() {
    if (!started_) return;
    for (auto& w : workers_) w->stop();
    started_ = false;
}

size_t DialogDispatcher::worker_index_for(const std::string& did) const {
    return std::hash<std::string>{}(did) % workers_.size();
}

Result DialogDispatcher::dispatch(std::unique_ptr<SipEvent> event) {
    if (!started_) return Result::kShuttingDown;
    if (!event || !DialogIdBuilder::is_valid(event->dialog_id)) return Result::kInvalidArgument;
    event->enqueued_at = Clock::now();
    return workers_[worker_index_for(event->dialog_id)]->enqueue(std::move(event));
}

DialogDispatcher::AggregateStats DialogDispatcher::aggregate_stats() const {
    AggregateStats a{};
    for (const auto& w : workers_) {
        const auto& s = w->stats();
        a.total_events_received += s.events_received.load();
        a.total_events_processed += s.events_processed.load();
        a.total_events_dropped += s.events_dropped.load();
        a.total_presence_triggers += s.presence_triggers_processed.load();
        a.total_dialogs_active += s.dialogs_active.load();
        a.total_dialogs_reaped += s.dialogs_reaped.load();
        a.total_slow_events += s.slow_events.load();
        uint64_t qd = s.queue_depth.load();
        if (qd > a.max_queue_depth) a.max_queue_depth = qd;
    }
    return a;
}

} // namespace sip_processor

