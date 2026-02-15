// =============================================================================
// FILE: src/presence/presence_event_router.cpp
// =============================================================================
#include "presence/presence_event_router.h"
#include "dispatch/dialog_dispatcher.h"
#include "subscription/blf_subscription_index.h"
#include "sip/sip_event.h"
#include "common/slow_event_logger.h"
#include "common/logger.h"

namespace sip_processor {

PresenceEventRouter::PresenceEventRouter(const Config& config,
                                         DialogDispatcher& dispatcher,
                                         std::shared_ptr<SlowEventLogger> slow_logger)
    : config_(config), dispatcher_(dispatcher), slow_logger_(std::move(slow_logger))
{}

PresenceEventRouter::~PresenceEventRouter() { stop(); }

Result PresenceEventRouter::start() {
    if (running_.load(std::memory_order_acquire)) return Result::kAlreadyExists;
    stop_requested_.store(false);
    running_.store(true);
    router_thread_ = std::thread(&PresenceEventRouter::router_thread_func, this);
    LOG_INFO("PresenceEventRouter started");
    return Result::kOk;
}

void PresenceEventRouter::stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    {
        std::lock_guard<std::mutex> lk(queue_mu_);
        stop_requested_.store(true);
    }
    queue_cv_.notify_one();
    if (router_thread_.joinable()) router_thread_.join();
    running_.store(false);
    LOG_INFO("PresenceEventRouter stopped");
}

void PresenceEventRouter::on_call_state_event(CallStateEvent&& event) {
    stats_.events_received.fetch_add(1, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> lk(queue_mu_);
        if (event_queue_.size() >= config_.presence_max_pending_events) {
            stats_.events_dropped.fetch_add(1, std::memory_order_relaxed);
            LOG_WARN("PresenceRouter: queue full, dropping event (call=%s)",
                     event.presence_call_id.c_str());
            return;
        }
        event_queue_.push(std::move(event));
        stats_.queue_depth.store(event_queue_.size(), std::memory_order_relaxed);
    }
    queue_cv_.notify_one();
}

void PresenceEventRouter::on_connection_state_changed(bool connected,
                                                       const std::string& detail) {
    LOG_INFO("PresenceRouter: connection state changed: %s (%s)",
             connected ? "connected" : "disconnected", detail.c_str());
}

void PresenceEventRouter::router_thread_func() {
    LOG_INFO("PresenceRouter: thread started");

    while (!stop_requested_.load(std::memory_order_acquire)) {
        CallStateEvent event;
        {
            std::unique_lock<std::mutex> lk(queue_mu_);
            queue_cv_.wait(lk, [this] {
                return !event_queue_.empty() || stop_requested_.load(std::memory_order_acquire);
            });
            if (stop_requested_.load() && event_queue_.empty()) break;
            if (event_queue_.empty()) continue;

            event = std::move(event_queue_.front());
            event_queue_.pop();
            stats_.queue_depth.store(event_queue_.size(), std::memory_order_relaxed);
        }

        process_call_state_event(event);
    }

    LOG_INFO("PresenceRouter: thread exiting");
}

void PresenceEventRouter::process_call_state_event(const CallStateEvent& event) {
    if (!event.is_valid) return;

    SlowEventLogger::Timer timer(*slow_logger_, "PRESENCE_ROUTE", event.presence_call_id);

    // Look up all BLF watchers monitoring the callee URI
    auto watchers = BlfSubscriptionIndex::instance().lookup(event.callee_uri);

    // Also look up watchers monitoring the caller URI (for outbound BLF)
    auto caller_watchers = BlfSubscriptionIndex::instance().lookup(event.caller_uri);
    watchers.insert(watchers.end(), caller_watchers.begin(), caller_watchers.end());

    if (watchers.empty()) {
        stats_.watchers_not_found.fetch_add(1, std::memory_order_relaxed);
        LOG_TRACE("PresenceRouter: no watchers for callee=%s caller=%s",
                  event.callee_uri.c_str(), event.caller_uri.c_str());
        stats_.events_processed.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    LOG_DEBUG("PresenceRouter: routing call=%s state=%s to %zu watchers",
              event.presence_call_id.c_str(),
              call_state_to_string(event.state),
              watchers.size());

    // Determine the monitored URI for XML body
    std::string blf_state = call_state_to_blf_state(event.state);

    for (const auto& watcher : watchers) {
        // Determine which URI this watcher is monitoring
        std::string monitored_uri = event.callee_uri;
        // If the watcher matches the caller lookup, use caller URI
        for (const auto& cw : caller_watchers) {
            if (cw.dialog_id == watcher.dialog_id) {
                monitored_uri = event.caller_uri;
                break;
            }
        }

        auto trigger = create_notify_trigger(
            watcher.dialog_id, watcher.tenant_id, event, monitored_uri);

        if (trigger) {
            Result r = dispatcher_.dispatch(std::move(trigger));
            if (r == Result::kOk) {
                stats_.notifications_generated.fetch_add(1, std::memory_order_relaxed);
            } else {
                LOG_WARN("PresenceRouter: dispatch failed for dialog=%s: %s",
                         watcher.dialog_id.c_str(), result_to_string(r));
            }
        }
    }

    stats_.events_processed.fetch_add(1, std::memory_order_relaxed);
}

std::string PresenceEventRouter::build_dialog_info_xml(
    const CallStateEvent& event, const std::string& monitored_uri) const
{
    std::string blf_state = call_state_to_blf_state(event.state);

    std::string xml;
    xml.reserve(1024);

    xml += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml += "<dialog-info xmlns=\"urn:ietf:params:xml:ns:dialog-info\"\n";
    xml += "  state=\"full\"\n";
    xml += "  entity=\"" + monitored_uri + "\">\n";

    if (blf_state != "terminated" || !event.presence_call_id.empty()) {
        xml += "  <dialog id=\"" + event.presence_call_id + "\"";
        if (!event.presence_call_id.empty())
            xml += " call-id=\"" + event.presence_call_id + "\"";
        if (!event.direction.empty())
            xml += " direction=\"" + event.direction + "\"";
        xml += ">\n";
        xml += "    <state>" + blf_state + "</state>\n";

        if (!event.caller_uri.empty() && !event.callee_uri.empty()) {
            xml += "    <remote>\n";
            xml += "      <identity>" + event.caller_uri + "</identity>\n";
            xml += "    </remote>\n";
            xml += "    <local>\n";
            xml += "      <identity>" + event.callee_uri + "</identity>\n";
            xml += "    </local>\n";
        }

        xml += "  </dialog>\n";
    }

    xml += "</dialog-info>\n";
    return xml;
}

std::unique_ptr<SipEvent> PresenceEventRouter::create_notify_trigger(
    const std::string& dialog_id,
    const std::string& tenant_id,
    const CallStateEvent& event,
    const std::string& monitored_uri)
{
    std::string blf_state = call_state_to_blf_state(event.state);
    std::string xml_body = build_dialog_info_xml(event, monitored_uri);

    return SipEvent::create_presence_trigger(
        dialog_id, tenant_id,
        event.presence_call_id,
        event.caller_uri,
        event.callee_uri,
        blf_state,
        event.direction,
        xml_body);
}

} // namespace sip_processor
