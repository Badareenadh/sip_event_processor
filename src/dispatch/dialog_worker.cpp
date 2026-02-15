
// =============================================================================
// FILE: src/dispatch/dialog_worker.cpp
// =============================================================================
#include "dispatch/dialog_worker.h"
#include "subscription/blf_processor.h"
#include "subscription/mwi_processor.h"
#include "subscription/blf_subscription_index.h"
#include "persistence/subscription_store.h"
#include "common/slow_event_logger.h"
#include "common/logger.h"

namespace sip_processor {

DialogWorker::DialogWorker(size_t idx, const Config& config,
                             std::shared_ptr<SlowEventLogger> slow_logger,
                             std::shared_ptr<SubscriptionStore> sub_store)
    : worker_index_(idx), config_(config)
    , slow_logger_(std::move(slow_logger)), sub_store_(std::move(sub_store))
    , blf_processor_(std::make_unique<BlfProcessor>())
    , mwi_processor_(std::make_unique<MwiProcessor>())
{}

DialogWorker::~DialogWorker() { stop(); }

Result DialogWorker::start() {
    if (running_.load()) return Result::kAlreadyExists;
    stop_requested_.store(false); running_.store(true);
    thread_ = std::thread(&DialogWorker::run, this);
    return Result::kOk;
}

void DialogWorker::stop() {
    if (!running_.load()) return;
    { std::lock_guard<std::mutex> lk(incoming_mu_); stop_requested_.store(true); }
    incoming_cv_.notify_one();
    if (thread_.joinable()) thread_.join();
    running_.store(false);
    for (auto& [id, ctx] : dialogs_) {
        if (ctx.record.type == SubscriptionType::kBLF)
            deindex_blf_subscription(id, ctx.record);
    }
    dialogs_.clear();
}

Result DialogWorker::enqueue(std::unique_ptr<SipEvent> event) {
    if (stop_requested_.load()) return Result::kShuttingDown;
    {
        std::lock_guard<std::mutex> lk(incoming_mu_);
        if (incoming_queue_.size() >= config_.max_incoming_queue_per_worker) {
            stats_.events_dropped.fetch_add(1); return Result::kCapacityExceeded;
        }
        incoming_queue_.push(std::move(event));
        stats_.events_received.fetch_add(1);
        stats_.queue_depth.store(incoming_queue_.size());
    }
    incoming_cv_.notify_one();
    return Result::kOk;
}

Result DialogWorker::load_recovered_subscription(SubscriptionRecord record) {
    // Called before start() — no locking needed
    DialogContext ctx;
    ctx.record = std::move(record);

    // Index BLF subscriptions
    if (ctx.record.type == SubscriptionType::kBLF && !ctx.record.blf_monitored_uri.empty()) {
        BlfSubscriptionIndex::instance().add(
            ctx.record.blf_monitored_uri, ctx.record.dialog_id, ctx.record.tenant_id);
    }

    SubscriptionRegistry::SubscriptionInfo info{
        ctx.record.dialog_id, ctx.record.tenant_id, ctx.record.type,
        ctx.record.lifecycle, ctx.record.last_activity, worker_index_};
    SubscriptionRegistry::instance().register_subscription(ctx.record.dialog_id, info);

    dialogs_.emplace(ctx.record.dialog_id, std::move(ctx));
    stats_.dialogs_active.store(dialogs_.size());

    LOG_DEBUG("Worker %zu: recovered subscription %s (%s)",
              worker_index_, record.dialog_id.c_str(),
              subscription_type_to_string(record.type));
    return Result::kOk;
}

void DialogWorker::index_blf_subscription(const std::string& did, const SubscriptionRecord& rec) {
    if (rec.type != SubscriptionType::kBLF || rec.blf_monitored_uri.empty()) return;
    if (rec.lifecycle != SubLifecycle::kActive) return;
    BlfSubscriptionIndex::instance().add(rec.blf_monitored_uri, did, rec.tenant_id);
}

void DialogWorker::deindex_blf_subscription(const std::string& did, const SubscriptionRecord& rec) {
    if (rec.type != SubscriptionType::kBLF) return;
    BlfSubscriptionIndex::instance().remove_dialog(did);
}

void DialogWorker::persist_record(const SubscriptionRecord& record, bool immediate) {
    if (!sub_store_ || !sub_store_->is_enabled()) return;
    if (immediate) sub_store_->save_immediately(record);
    else sub_store_->queue_upsert(record);
}

void DialogWorker::run() {
    std::queue<std::unique_ptr<SipEvent>> local_batch;
    std::vector<std::string> local_terminates;

    while (true) {
        {
            std::unique_lock<std::mutex> lk(incoming_mu_);
            incoming_cv_.wait_for(lk, Millisecs(100), [this] {
                return !incoming_queue_.empty() || stop_requested_.load();
            });
            if (stop_requested_.load() && incoming_queue_.empty()) {
                process_dialog_queues(); break;
            }
            std::swap(local_batch, incoming_queue_);
            stats_.queue_depth.store(0);
        }

        // Force-terminates
        { std::lock_guard<std::mutex> lk(terminate_mu_); std::swap(local_terminates, pending_terminates_); }
        for (const auto& did : local_terminates) {
            auto it = dialogs_.find(did);
            if (it != dialogs_.end()) {
                deindex_blf_subscription(did, it->second.record);
                it->second.record.lifecycle = SubLifecycle::kTerminated;
                SubscriptionRegistry::instance().unregister_subscription(did);
                if (sub_store_) sub_store_->queue_delete(did);
                while (!it->second.event_queue.empty()) it->second.event_queue.pop();
                stats_.dialogs_reaped.fetch_add(1);
            }
        }
        local_terminates.clear();

        // Distribute
        while (!local_batch.empty()) {
            auto& ev = local_batch.front();
            auto it = dialogs_.find(ev->dialog_id);
            if (it == dialogs_.end()) {
                if (ev->source == SipEventSource::kPresenceFeed) {
                    stats_.events_dropped.fetch_add(1); local_batch.pop(); continue;
                }
                handle_new_subscription(ev->dialog_id, *ev);
                it = dialogs_.find(ev->dialog_id);
                if (it == dialogs_.end()) { stats_.events_dropped.fetch_add(1); local_batch.pop(); continue; }
            }
            it->second.event_queue.push(std::move(ev));
            local_batch.pop();
        }

        process_dialog_queues();
        if (++process_cycle_ % kCleanupInterval == 0) cleanup_terminated_dialogs();
    }
}

void DialogWorker::handle_new_subscription(const std::string& did, const SipEvent& ev) {
    if (SubscriptionRegistry::instance().count_by_tenant(ev.tenant_id) >= config_.max_subscriptions_per_tenant) return;
    if (dialogs_.size() >= config_.max_dialogs_per_worker) return;

    DialogContext ctx;
    ctx.record.dialog_id = did;
    ctx.record.tenant_id = ev.tenant_id;
    ctx.record.type = ev.sub_type;
    ctx.record.lifecycle = SubLifecycle::kPending;
    if (ev.expires > 0) ctx.record.expires_at = Clock::now() + Seconds(ev.expires);
    ctx.record.from_uri = ev.from_uri;
    ctx.record.from_tag = ev.from_tag;
    ctx.record.to_uri = ev.to_uri;
    ctx.record.to_tag = ev.to_tag;
    ctx.record.call_id = ev.call_id;
    ctx.record.contact_uri = ev.contact_uri;

    if (ev.sub_type == SubscriptionType::kBLF) ctx.record.blf_monitored_uri = ev.to_uri;
    else if (ev.sub_type == SubscriptionType::kMWI) ctx.record.mwi_account_uri = ev.to_uri;

    SubscriptionRegistry::SubscriptionInfo info{did, ev.tenant_id, ev.sub_type, SubLifecycle::kPending, Clock::now(), worker_index_};
    SubscriptionRegistry::instance().register_subscription(did, info);

    // Persist immediately on creation
    persist_record(ctx.record, true);

    dialogs_.emplace(did, std::move(ctx));
    stats_.dialogs_active.store(dialogs_.size());
}

void DialogWorker::process_dialog_queues() {
    for (auto& [did, ctx] : dialogs_) {
        if (ctx.event_queue.empty()) continue;
        auto event = std::move(ctx.event_queue.front());
        ctx.event_queue.pop();
        process_event(did, ctx.record, std::move(event));
    }
}

void DialogWorker::process_event(const std::string& did, SubscriptionRecord& rec,
                                   std::unique_ptr<SipEvent> event) {
    event->dequeued_at = Clock::now();
    rec.is_processing = true;
    rec.processing_started_at = Clock::now();
    rec.touch();
    rec.events_processed++;

    // Slow event timing
    std::string ctx_str = std::string(event_category_to_string(event->category)) +
                          " " + subscription_type_to_string(rec.type);
    SlowEventLogger::Timer timer(*slow_logger_, ctx_str.c_str(), did);

    Result result = Result::kError;

    if (event->source == SipEventSource::kPresenceFeed) {
        process_presence_trigger(did, rec, *event);
        result = Result::kOk;
        stats_.presence_triggers_processed.fetch_add(1);
    } else {
        switch (rec.type) {
            case SubscriptionType::kBLF: result = blf_processor_->process(*event, rec); break;
            case SubscriptionType::kMWI: result = mwi_processor_->process(*event, rec); break;
            case SubscriptionType::kUnknown:
                if (event->sub_type != SubscriptionType::kUnknown) {
                    rec.type = event->sub_type;
                    result = (rec.type == SubscriptionType::kBLF)
                        ? blf_processor_->process(*event, rec)
                        : mwi_processor_->process(*event, rec);
                }
                break;
        }
    }

    // Lifecycle transitions
    if (event->subscription_state == "terminated" || event->expires == 0) {
        if (rec.lifecycle != SubLifecycle::kTerminated) deindex_blf_subscription(did, rec);
        rec.lifecycle = SubLifecycle::kTerminated;
        persist_record(rec, true);  // Immediate persist on terminate
        if (sub_store_) sub_store_->queue_delete(did);
    } else if (event->subscription_state == "active" && rec.lifecycle == SubLifecycle::kPending) {
        rec.lifecycle = SubLifecycle::kActive;
        index_blf_subscription(did, rec);
        persist_record(rec, true);  // Immediate persist on activation
    } else if (rec.dirty) {
        persist_record(rec, false);  // Batched persist for state updates
        rec.dirty = false;
    }

    if (event->expires > 0 && event->category == SipEventCategory::kSubscribe)
        rec.expires_at = Clock::now() + Seconds(event->expires);

    rec.is_processing = false;

    // Finish timer — logs if slow
    timer.finish();
    auto elapsed = timer.elapsed();
    if (elapsed >= config_.slow_event_warn_threshold) {
        stats_.slow_events.fetch_add(1);
    }

    stats_.events_processed.fetch_add(1);
}

void DialogWorker::process_presence_trigger(const std::string& did,
                                              SubscriptionRecord& rec,
                                              const SipEvent& event) {
    auto action = blf_processor_->process_presence_trigger(event, rec);
    if (!action.should_notify) return;

    // Store last NOTIFY body for redundancy recovery
    rec.blf_last_notify_body = action.body;
    rec.blf_notify_version++;
    rec.dirty = true;

    LOG_INFO("Worker %zu: NOTIFY dialog=%s state=%s (call=%s)",
             worker_index_, did.c_str(), event.presence_state.c_str(),
             event.presence_call_id.c_str());

    // TODO: Send via Sofia (su_msg to Sofia thread)
}

void DialogWorker::cleanup_terminated_dialogs() {
    size_t cleaned = 0;
    auto it = dialogs_.begin();
    while (it != dialogs_.end()) {
        auto& [did, ctx] = *it;
        bool remove = (ctx.record.lifecycle == SubLifecycle::kTerminated && ctx.event_queue.empty()) ||
                      (ctx.record.is_expired() && ctx.event_queue.empty());
        if (remove) {
            deindex_blf_subscription(did, ctx.record);
            SubscriptionRegistry::instance().unregister_subscription(did);
            it = dialogs_.erase(it); cleaned++;
        } else { ++it; }
    }
    if (cleaned > 0) stats_.dialogs_active.store(dialogs_.size());
}

std::vector<DialogWorker::StaleInfo> DialogWorker::get_stale_subscriptions(
    Seconds blf_ttl, Seconds mwi_ttl, Seconds stuck_timeout) const {
    std::vector<StaleInfo> stale;
    for (const auto& [did, ctx] : dialogs_) {
        const auto& rec = ctx.record;
        if (rec.lifecycle == SubLifecycle::kTerminated) continue;
        bool is_stuck = rec.is_stuck(stuck_timeout);
        Seconds ttl = (rec.type == SubscriptionType::kBLF) ? blf_ttl : mwi_ttl;
        bool is_stale = ((Clock::now() - rec.last_activity) > ttl) || rec.is_expired();
        if (is_stale || is_stuck)
            stale.push_back({did, rec.tenant_id, rec.type, rec.lifecycle, rec.last_activity, is_stuck});
    }
    return stale;
}

Result DialogWorker::force_terminate(const std::string& did) {
    std::lock_guard<std::mutex> lk(terminate_mu_);
    pending_terminates_.push_back(did);
    incoming_cv_.notify_one();
    return Result::kOk;
}

} // namespace sip_processor
