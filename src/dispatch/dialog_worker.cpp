
// =============================================================================
// FILE: src/dispatch/dialog_worker.cpp
// =============================================================================
#include "dispatch/dialog_worker.h"
#include "subscription/blf_processor.h"
#include "subscription/mwi_processor.h"
#include "subscription/blf_subscription_index.h"
#include "subscription/subscription_type.h"
#include "persistence/subscription_store.h"
#include "sip/sip_stack_manager.h"
#include "common/slow_event_logger.h"
#include "common/logger.h"

namespace sip_processor {

DialogWorker::DialogWorker(size_t idx, const Config& config,
                             std::shared_ptr<SlowEventLogger> slow_logger,
                             std::shared_ptr<SubscriptionStore> sub_store,
                             SipStackManager* stack_mgr)
    : worker_index_(idx), config_(config)
    , slow_logger_(std::move(slow_logger)), sub_store_(std::move(sub_store))
    , stack_mgr_(stack_mgr)
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
        release_nua_handle(ctx);
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
    // Note: nua_handle is null for recovered subscriptions (no active Sofia dialog)

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

// ─────────────────────────────────────────────────────────────────────────────
// SIP response/NOTIFY sending helpers
// ─────────────────────────────────────────────────────────────────────────────

void DialogWorker::release_nua_handle(DialogContext& ctx) {
    if (ctx.nua_handle) {
        nua_handle_unref(ctx.nua_handle);
        ctx.nua_handle = nullptr;
    }
}

void DialogWorker::send_subscribe_response(DialogContext& ctx, const SipEvent& event,
                                            int status, const char* phrase) {
    if (!stack_mgr_ || !ctx.nua_handle) {
        LOG_WARN("Worker %zu: cannot respond to SUBSCRIBE dialog=%s (no stack/handle)",
                 worker_index_, ctx.record.dialog_id.c_str());
        return;
    }

    uint32_t expires = event.expires;
    if (status >= 400) expires = 0;  // No expires on error responses

    LOG_INFO("Worker %zu: SUBSCRIBE response %d %s dialog=%s expires=%u",
             worker_index_, status, phrase, ctx.record.dialog_id.c_str(), expires);

    stack_mgr_->respond_to_subscribe(ctx.nua_handle, status, phrase, expires);
    stats_.subscribe_responses_sent.fetch_add(1);
}

void DialogWorker::send_sip_notify(DialogContext& ctx, const std::string& content_type,
                                    const std::string& body, const char* sub_state) {
    if (!stack_mgr_ || !ctx.nua_handle) {
        LOG_WARN("Worker %zu: cannot send NOTIFY dialog=%s (no stack/handle)",
                 worker_index_, ctx.record.dialog_id.c_str());
        return;
    }

    const char* event_type = subscription_type_to_event_header(ctx.record.type);
    if (!event_type) {
        LOG_WARN("Worker %zu: unknown event type for NOTIFY dialog=%s",
                 worker_index_, ctx.record.dialog_id.c_str());
        return;
    }

    // Increment outgoing NOTIFY CSeq
    ctx.record.notify_cseq++;

    LOG_INFO("Worker %zu: NOTIFY dialog=%s cseq=%u event=%s state=%s body_len=%zu",
             worker_index_, ctx.record.dialog_id.c_str(), ctx.record.notify_cseq,
             event_type, sub_state, body.size());

    stack_mgr_->send_notify(ctx.nua_handle, event_type,
                             content_type.c_str(), body.c_str(), sub_state);
    stats_.notify_sent.fetch_add(1);
}

void DialogWorker::send_initial_notify(DialogContext& ctx) {
    if (!stack_mgr_ || !ctx.nua_handle) return;

    std::string body;
    std::string content_type;

    if (ctx.record.type == SubscriptionType::kBLF) {
        content_type = "application/dialog-info+xml";
        if (!ctx.record.blf_last_notify_body.empty()) {
            // Have existing state from recovery — send it
            body = ctx.record.blf_last_notify_body;
        } else {
            // No active call — send empty dialog-info
            body = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                   "<dialog-info xmlns=\"urn:ietf:params:xml:ns:dialog-info\"\n"
                   "  version=\"0\"\n"
                   "  state=\"full\"\n"
                   "  entity=\"" + ctx.record.blf_monitored_uri + "\">\n"
                   "</dialog-info>\n";
        }
    } else if (ctx.record.type == SubscriptionType::kMWI) {
        content_type = "application/simple-message-summary";
        body = "Messages-Waiting: " +
               std::string(ctx.record.mwi_new_messages > 0 ? "yes" : "no") + "\r\n"
               "Message-Account: " + ctx.record.mwi_account_uri + "\r\n"
               "Voice-Message: " + std::to_string(ctx.record.mwi_new_messages) + "/" +
               std::to_string(ctx.record.mwi_old_messages) + "\r\n";
    }

    if (!body.empty()) {
        LOG_DEBUG("Worker %zu: sending initial NOTIFY dialog=%s type=%s",
                  worker_index_, ctx.record.dialog_id.c_str(),
                  subscription_type_to_string(ctx.record.type));
        send_sip_notify(ctx, content_type, body, "active");
    }
}

void DialogWorker::handle_notify_response(const std::string& did, DialogContext& ctx,
                                           const SipEvent& event) {
    auto& rec = ctx.record;
    LOG_DEBUG("Worker %zu: NOTIFY response %d %s dialog=%s",
              worker_index_, event.status, event.phrase.c_str(), did.c_str());

    if (event.status >= 200 && event.status < 300) {
        // 2xx — NOTIFY accepted by phone
        return;
    }

    // Error responses from the phone — terminate subscription
    if (event.status == 481) {
        LOG_WARN("Worker %zu: NOTIFY got 481 for dialog=%s, terminating subscription",
                 worker_index_, did.c_str());
    } else if (event.status == 408) {
        LOG_WARN("Worker %zu: NOTIFY got 408 timeout for dialog=%s, terminating",
                 worker_index_, did.c_str());
    } else if (event.status == 489) {
        LOG_WARN("Worker %zu: NOTIFY got 489 Bad Event for dialog=%s, terminating",
                 worker_index_, did.c_str());
    } else if (event.status >= 400) {
        LOG_WARN("Worker %zu: NOTIFY got %d %s for dialog=%s, terminating",
                 worker_index_, event.status, event.phrase.c_str(), did.c_str());
    }

    if (event.status >= 400) {
        deindex_blf_subscription(did, rec);
        rec.lifecycle = SubLifecycle::kTerminated;
        persist_record(rec, true);
        if (sub_store_) sub_store_->queue_delete(did);
        stats_.notify_errors.fetch_add(1);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Main worker loop
// ─────────────────────────────────────────────────────────────────────────────

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

                // Send final NOTIFY with terminated state
                if (it->second.nua_handle && stack_mgr_) {
                    std::string term_body;
                    if (it->second.record.type == SubscriptionType::kBLF) {
                        term_body = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                    "<dialog-info xmlns=\"urn:ietf:params:xml:ns:dialog-info\"\n"
                                    "  version=\"" + std::to_string(it->second.record.blf_notify_version) + "\"\n"
                                    "  state=\"full\"\n"
                                    "  entity=\"" + it->second.record.blf_monitored_uri + "\">\n"
                                    "</dialog-info>\n";
                        send_sip_notify(it->second, "application/dialog-info+xml",
                                        term_body, "terminated");
                    } else if (it->second.record.type == SubscriptionType::kMWI) {
                        term_body = "Messages-Waiting: no\r\n";
                        send_sip_notify(it->second, "application/simple-message-summary",
                                        term_body, "terminated");
                    }
                }

                SubscriptionRegistry::instance().unregister_subscription(did);
                if (sub_store_) sub_store_->queue_delete(did);
                while (!it->second.event_queue.empty()) it->second.event_queue.pop();
                release_nua_handle(it->second);
                stats_.dialogs_reaped.fetch_add(1);
            }
        }
        local_terminates.clear();

        // Distribute events to per-dialog queues
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
    // Check tenant limit
    if (SubscriptionRegistry::instance().count_by_tenant(ev.tenant_id) >= config_.max_subscriptions_per_tenant) {
        LOG_WARN("Worker %zu: tenant %s at subscription limit, rejecting dialog=%s",
                 worker_index_, ev.tenant_id.c_str(), did.c_str());
        if (ev.nua_handle && stack_mgr_) {
            stack_mgr_->respond_to_subscribe(ev.nua_handle, 403, "Forbidden", 0);
            nua_handle_unref(ev.nua_handle);
        }
        return;
    }

    // Check worker capacity
    if (dialogs_.size() >= config_.max_dialogs_per_worker) {
        LOG_WARN("Worker %zu: at capacity, rejecting dialog=%s", worker_index_, did.c_str());
        if (ev.nua_handle && stack_mgr_) {
            stack_mgr_->respond_to_subscribe(ev.nua_handle, 503, "Service Unavailable", 0);
            nua_handle_unref(ev.nua_handle);
        }
        return;
    }

    // Check event type is supported
    if (ev.sub_type == SubscriptionType::kUnknown) {
        LOG_WARN("Worker %zu: unsupported event type for dialog=%s event=%s",
                 worker_index_, did.c_str(), ev.event_header.c_str());
        if (ev.nua_handle && stack_mgr_) {
            stack_mgr_->respond_to_subscribe(ev.nua_handle, 489, "Bad Event", 0);
            nua_handle_unref(ev.nua_handle);
        }
        return;
    }

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

    // Store Sofia handle (ref was taken by callback handler)
    ctx.nua_handle = ev.nua_handle;

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
        process_event(did, ctx, std::move(event));
    }
}

void DialogWorker::process_event(const std::string& did, DialogContext& ctx,
                                   std::unique_ptr<SipEvent> event) {
    auto& rec = ctx.record;
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
    SubLifecycle prev_lifecycle = rec.lifecycle;

    // Handle NOTIFY responses (nua_r_notify) — response to our outgoing NOTIFY
    if (event->category == SipEventCategory::kNotify &&
        event->direction == SipDirection::kOutgoing) {
        handle_notify_response(did, ctx, *event);
        result = Result::kOk;
    }
    // Handle presence trigger from presence feed
    else if (event->source == SipEventSource::kPresenceFeed) {
        process_presence_trigger(did, ctx, *event);
        result = Result::kOk;
        stats_.presence_triggers_processed.fetch_add(1);
    }
    // Handle SIP events (SUBSCRIBE, NOTIFY, PUBLISH)
    else {
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

        // Respond to SUBSCRIBE with Expires: 0 (unsubscribe)
        if (event->category == SipEventCategory::kSubscribe &&
            event->direction == SipDirection::kIncoming) {
            send_subscribe_response(ctx, *event, 200, "OK");
            // Send final NOTIFY with terminated state
            if (rec.type == SubscriptionType::kBLF) {
                std::string term_body = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                        "<dialog-info xmlns=\"urn:ietf:params:xml:ns:dialog-info\"\n"
                                        "  version=\"" + std::to_string(rec.blf_notify_version++) + "\"\n"
                                        "  state=\"full\"\n"
                                        "  entity=\"" + rec.blf_monitored_uri + "\">\n"
                                        "</dialog-info>\n";
                send_sip_notify(ctx, "application/dialog-info+xml", term_body, "terminated");
            } else if (rec.type == SubscriptionType::kMWI) {
                send_sip_notify(ctx, "application/simple-message-summary",
                                "Messages-Waiting: no\r\n", "terminated");
            }
        }

        persist_record(rec, true);
        if (sub_store_) sub_store_->queue_delete(did);
    } else if (rec.lifecycle == SubLifecycle::kActive && prev_lifecycle == SubLifecycle::kPending) {
        // Subscription just activated
        index_blf_subscription(did, rec);

        // Respond 200 OK and send initial NOTIFY
        if (event->category == SipEventCategory::kSubscribe &&
            event->direction == SipDirection::kIncoming) {
            send_subscribe_response(ctx, *event, 200, "OK");
            send_initial_notify(ctx);
        }

        persist_record(rec, true);
    } else if (event->category == SipEventCategory::kSubscribe &&
               event->direction == SipDirection::kIncoming &&
               rec.lifecycle == SubLifecycle::kActive) {
        // Re-SUBSCRIBE (refresh) — respond 200 OK
        send_subscribe_response(ctx, *event, 200, "OK");
        persist_record(rec, false);
    } else if (rec.dirty) {
        persist_record(rec, false);
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
                                              DialogContext& ctx,
                                              const SipEvent& event) {
    auto& rec = ctx.record;
    auto action = blf_processor_->process_presence_trigger(event, rec);
    if (!action.should_notify) return;

    // Store last NOTIFY body for redundancy recovery
    rec.blf_last_notify_body = action.body;
    rec.blf_notify_version++;
    rec.dirty = true;

    LOG_INFO("Worker %zu: NOTIFY dialog=%s state=%s (call=%s)",
             worker_index_, did.c_str(), event.presence_state.c_str(),
             event.presence_call_id.c_str());

    // Send the NOTIFY via Sofia SIP stack
    send_sip_notify(ctx, action.content_type, action.body,
                    action.subscription_state_header.c_str());
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
            release_nua_handle(ctx);
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
