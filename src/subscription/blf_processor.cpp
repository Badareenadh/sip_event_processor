
// =============================================================================
// FILE: src/subscription/blf_processor.cpp
// =============================================================================
#include "subscription/blf_processor.h"
#include "common/logger.h"
#include <cstring>
#include <algorithm>
#include <cstdio>

namespace sip_processor {

Result BlfProcessor::process(const SipEvent& event, SubscriptionRecord& record) {
    switch (event.category) {
        case SipEventCategory::kSubscribe:
            return (event.direction == SipDirection::kIncoming)
                ? handle_subscribe(event, record)
                : handle_subscribe_response(event, record);
        case SipEventCategory::kNotify:
            return handle_notify(event, record);
        case SipEventCategory::kPublish:
            return handle_publish(event, record);
        case SipEventCategory::kPresenceTrigger:
            // Handled by process_presence_trigger() — should not reach here
            LOG_WARN("BLF: kPresenceTrigger reached process() — use process_presence_trigger()");
            return Result::kInvalidArgument;
        default:
            return Result::kInvalidArgument;
    }
}

BlfProcessor::NotifyAction BlfProcessor::process_presence_trigger(
    const SipEvent& event, SubscriptionRecord& record)
{
    NotifyAction action;

    // Only generate NOTIFY for active subscriptions
    if (record.lifecycle != SubLifecycle::kActive) {
        LOG_DEBUG("BLF: skipping presence trigger for non-active dialog=%s (lifecycle=%s)",
                  record.dialog_id.c_str(), lifecycle_to_string(record.lifecycle));
        return action;
    }

    // Check if state actually changed
    bool state_changed = (record.blf_last_state != event.presence_state) ||
                         (record.blf_presence_call_id != event.presence_call_id);

    if (!state_changed && !record.blf_last_state.empty()) {
        LOG_TRACE("BLF: no state change for dialog=%s (still %s)",
                  record.dialog_id.c_str(), record.blf_last_state.c_str());
        return action;
    }

    // Update record
    std::string prev_state = record.blf_last_state;
    record.blf_last_state       = event.presence_state;
    record.blf_last_direction   = event.presence_direction;
    record.blf_presence_call_id = event.presence_call_id;
    record.touch();

    LOG_INFO("BLF: presence trigger dialog=%s monitored=%s: %s -> %s (call=%s)",
             record.dialog_id.c_str(), record.blf_monitored_uri.c_str(),
             prev_state.empty() ? "(none)" : prev_state.c_str(),
             event.presence_state.c_str(),
             event.presence_call_id.c_str());

    // Build the dialog-info+xml body for the NOTIFY
    action.should_notify = true;
    action.content_type  = "application/dialog-info+xml";
    action.subscription_state_header = "active";

    action.body = build_dialog_info_xml(
        record.blf_monitored_uri,
        record.dialog_id,
        event.presence_call_id,
        event.presence_state,
        event.presence_direction,
        event.presence_caller_uri,
        event.presence_callee_uri);

    return action;
}

std::string BlfProcessor::build_dialog_info_xml(
    const std::string& entity_uri,
    const std::string& dialog_id,
    const std::string& call_id,
    const std::string& state,
    const std::string& direction,
    const std::string& caller_uri,
    const std::string& callee_uri) const
{
    // RFC 4235 dialog-info+xml
    // version must monotonically increase per subscription
    // (the worker handles this since it owns the record)

    std::string xml;
    xml.reserve(1024);

    xml += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    xml += "<dialog-info xmlns=\"urn:ietf:params:xml:ns:dialog-info\"\n";
    xml += "  version=\"" + std::to_string(notify_version_++) + "\"\n";
    xml += "  state=\"full\"\n";
    xml += "  entity=\"" + entity_uri + "\">\n";

    if (state != "terminated" || !call_id.empty()) {
        xml += "  <dialog id=\"" + call_id + "\"";
        if (!call_id.empty()) xml += " call-id=\"" + call_id + "\"";
        if (!direction.empty()) xml += " direction=\"" + direction + "\"";
        xml += ">\n";
        xml += "    <state>" + state + "</state>\n";

        // Include local/remote identity for richer BLF display
        if (!caller_uri.empty() && !callee_uri.empty()) {
            if (direction == "inbound" || direction == "recipient") {
                xml += "    <remote>\n";
                xml += "      <identity>" + caller_uri + "</identity>\n";
                xml += "    </remote>\n";
                xml += "    <local>\n";
                xml += "      <identity>" + callee_uri + "</identity>\n";
                xml += "    </local>\n";
            } else {
                xml += "    <local>\n";
                xml += "      <identity>" + caller_uri + "</identity>\n";
                xml += "    </local>\n";
                xml += "    <remote>\n";
                xml += "      <identity>" + callee_uri + "</identity>\n";
                xml += "    </remote>\n";
            }
        }

        xml += "  </dialog>\n";
    }

    xml += "</dialog-info>\n";
    return xml;
}

Result BlfProcessor::handle_subscribe(const SipEvent& event, SubscriptionRecord& record) {
    LOG_DEBUG("BLF: SUBSCRIBE dialog=%s from=%s to=%s expires=%u",
              record.dialog_id.c_str(), event.from_uri.c_str(),
              event.to_uri.c_str(), event.expires);

    if (!event.to_uri.empty()) record.blf_monitored_uri = event.to_uri;

    if (event.expires == 0) {
        record.lifecycle = SubLifecycle::kTerminating;
        return Result::kOk;
    }

    if (event.expires > 0) record.expires_at = Clock::now() + Seconds(event.expires);
    if (event.cseq > 0) record.cseq = event.cseq;
    if (record.lifecycle == SubLifecycle::kPending) record.lifecycle = SubLifecycle::kActive;

    return Result::kOk;
}

Result BlfProcessor::handle_notify(const SipEvent& event, SubscriptionRecord& record) {
    LOG_DEBUG("BLF: NOTIFY dialog=%s body_len=%zu", record.dialog_id.c_str(), event.body.size());

    if (!event.body.empty()) {
        DialogState state = parse_dialog_info_xml(event.body);
        if (state.valid) update_blf_state(record, state);
    }

    if (event.subscription_state == "terminated") {
        record.lifecycle = SubLifecycle::kTerminated;
    }

    return Result::kOk;
}

Result BlfProcessor::handle_subscribe_response(const SipEvent& event, SubscriptionRecord& record) {
    LOG_DEBUG("BLF: SUBSCRIBE response %d dialog=%s", event.status, record.dialog_id.c_str());

    if (event.status >= 200 && event.status < 300) {
        if (record.lifecycle == SubLifecycle::kPending) record.lifecycle = SubLifecycle::kActive;
        if (event.expires > 0) record.expires_at = Clock::now() + Seconds(event.expires);
    } else if (event.status == 481 || event.status == 489) {
        record.lifecycle = SubLifecycle::kTerminated;
    }
    return Result::kOk;
}

Result BlfProcessor::handle_publish(const SipEvent& event, SubscriptionRecord& record) {
    if (!event.body.empty()) {
        DialogState state = parse_dialog_info_xml(event.body);
        if (state.valid) update_blf_state(record, state);
    }
    return Result::kOk;
}

BlfProcessor::DialogState BlfProcessor::parse_dialog_info_xml(const std::string& body) {
    DialogState state;

    auto find_attr = [&body](const std::string& tag, const std::string& attr) -> std::string {
        auto tag_pos = body.find("<" + tag);
        if (tag_pos == std::string::npos) return "";
        auto attr_pos = body.find(attr + "=\"", tag_pos);
        if (attr_pos == std::string::npos) return "";
        auto val_start = attr_pos + attr.size() + 2;
        auto val_end = body.find('"', val_start);
        if (val_end == std::string::npos) return "";
        return body.substr(val_start, val_end - val_start);
    };

    state.entity = find_attr("dialog-info", "entity");

    auto ss = body.find("<state>");
    if (ss != std::string::npos) {
        ss += 7;
        auto se = body.find("</state>", ss);
        if (se != std::string::npos) {
            state.state = body.substr(ss, se - ss);
            auto& s = state.state;
            s.erase(0, s.find_first_not_of(" \t\n\r"));
            s.erase(s.find_last_not_of(" \t\n\r") + 1);
            state.valid = true;
        }
    }

    state.id = find_attr("dialog", "id");
    state.direction = find_attr("dialog", "direction");
    return state;
}

void BlfProcessor::update_blf_state(SubscriptionRecord& record, const DialogState& state) {
    std::string prev = record.blf_last_state;
    record.blf_last_state = state.state;
    if (!state.entity.empty()) record.blf_monitored_uri = state.entity;

    if (prev != state.state) {
        LOG_INFO("BLF: state change dialog=%s monitored=%s: %s -> %s",
                 record.dialog_id.c_str(), record.blf_monitored_uri.c_str(),
                 prev.empty() ? "(none)" : prev.c_str(), state.state.c_str());
    }
}

} // namespace sip_processor

