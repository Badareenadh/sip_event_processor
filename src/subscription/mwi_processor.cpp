
// =============================================================================
// FILE: src/subscription/mwi_processor.cpp
// =============================================================================
#include "subscription/mwi_processor.h"
#include "common/logger.h"
#include <sstream>
#include <algorithm>
#include <cctype>

namespace sip_processor {

Result MwiProcessor::process(const SipEvent& event, SubscriptionRecord& record) {
    switch (event.category) {
        case SipEventCategory::kSubscribe:
            return (event.direction == SipDirection::kIncoming)
                ? handle_subscribe(event, record)
                : handle_subscribe_response(event, record);
        case SipEventCategory::kNotify:
            return handle_notify(event, record);
        case SipEventCategory::kPublish:
            return handle_publish(event, record);
        default:
            return Result::kInvalidArgument;
    }
}

Result MwiProcessor::handle_subscribe(const SipEvent& event, SubscriptionRecord& record) {
    LOG_DEBUG("MWI: SUBSCRIBE dialog=%s from=%s expires=%u",
              record.dialog_id.c_str(), event.from_uri.c_str(), event.expires);

    if (!event.to_uri.empty()) record.mwi_account_uri = event.to_uri;

    if (event.expires == 0) {
        record.lifecycle = SubLifecycle::kTerminating;
        return Result::kOk;
    }

    if (event.expires > 0) record.expires_at = Clock::now() + Seconds(event.expires);
    if (event.cseq > 0) record.cseq = event.cseq;
    if (record.lifecycle == SubLifecycle::kPending) record.lifecycle = SubLifecycle::kActive;

    return Result::kOk;
}

Result MwiProcessor::handle_notify(const SipEvent& event, SubscriptionRecord& record) {
    if (event.body.empty()) return Result::kOk;

    MessageSummary summary = parse_message_summary(event.body);
    if (summary.valid) update_mwi_state(record, summary);

    if (event.subscription_state == "terminated")
        record.lifecycle = SubLifecycle::kTerminated;

    return Result::kOk;
}

Result MwiProcessor::handle_subscribe_response(const SipEvent& event, SubscriptionRecord& record) {
    if (event.status >= 200 && event.status < 300) {
        if (record.lifecycle == SubLifecycle::kPending) record.lifecycle = SubLifecycle::kActive;
        if (event.expires > 0) record.expires_at = Clock::now() + Seconds(event.expires);
    } else if (event.status == 481 || event.status == 489 || event.status == 403) {
        record.lifecycle = SubLifecycle::kTerminated;
    }
    return Result::kOk;
}

Result MwiProcessor::handle_publish(const SipEvent& event, SubscriptionRecord& record) {
    if (!event.body.empty()) {
        MessageSummary summary = parse_message_summary(event.body);
        if (summary.valid) update_mwi_state(record, summary);
    }
    return Result::kOk;
}

MwiProcessor::MessageSummary MwiProcessor::parse_message_summary(const std::string& body) {
    MessageSummary summary;
    std::istringstream stream(body);
    std::string line;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto start = line.find_first_not_of(" \t");
        if (start == std::string::npos) continue;
        line = line.substr(start);

        std::string lower = line;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        if (lower.find("messages-waiting:") == 0) {
            auto vs = line.find(':');
            if (vs != std::string::npos) {
                std::string val = line.substr(vs + 1);
                val.erase(0, val.find_first_not_of(" \t"));
                val.erase(val.find_last_not_of(" \t") + 1);
                std::transform(val.begin(), val.end(), val.begin(), ::tolower);
                summary.messages_waiting = (val == "yes");
                summary.valid = true;
            }
        } else if (lower.find("message-account:") == 0) {
            auto vs = line.find(':');
            if (vs != std::string::npos) {
                summary.account = line.substr(vs + 1);
                auto& a = summary.account;
                a.erase(0, a.find_first_not_of(" \t"));
                a.erase(a.find_last_not_of(" \t") + 1);
            }
        } else if (lower.find("voice-message:") == 0) {
            auto vs = line.find(':');
            if (vs != std::string::npos) {
                std::string val = line.substr(vs + 1);
                int n = 0, o = 0, nu = 0, ou = 0;
                if (sscanf(val.c_str(), " %d/%d (%d/%d)", &n, &o, &nu, &ou) >= 2 ||
                    sscanf(val.c_str(), " %d/%d", &n, &o) >= 2) {
                    summary.new_messages = n;
                    summary.old_messages = o;
                    summary.new_urgent = nu;
                    summary.old_urgent = ou;
                    summary.valid = true;
                }
            }
        }
    }
    return summary;
}

void MwiProcessor::update_mwi_state(SubscriptionRecord& record,
                                     const MessageSummary& summary) {
    int pn = record.mwi_new_messages, po = record.mwi_old_messages;
    record.mwi_new_messages = summary.new_messages;
    record.mwi_old_messages = summary.old_messages;
    if (!summary.account.empty()) record.mwi_account_uri = summary.account;

    if (pn != summary.new_messages || po != summary.old_messages) {
        LOG_INFO("MWI: change dialog=%s account=%s: new=%d->%d old=%d->%d",
                 record.dialog_id.c_str(), record.mwi_account_uri.c_str(),
                 pn, summary.new_messages, po, summary.old_messages);
    }
}

} // namespace sip_processor

