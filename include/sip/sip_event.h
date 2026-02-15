
// =============================================================================
// FILE: include/sip/sip_event.h
// =============================================================================
#ifndef SIP_EVENT_H
#define SIP_EVENT_H

#include "common/types.h"
#include "subscription/subscription_type.h"
#include <sofia-sip/nua.h>
#include <string>
#include <atomic>
#include <memory>

namespace sip_processor {

enum class SipDirection { kIncoming, kOutgoing };

enum class SipEventCategory {
    kSubscribe, kNotify, kPublish, kPresenceTrigger, kUnknown
};

inline const char* event_category_to_string(SipEventCategory c) {
    switch (c) {
        case SipEventCategory::kSubscribe:       return "SUBSCRIBE";
        case SipEventCategory::kNotify:          return "NOTIFY";
        case SipEventCategory::kPublish:         return "PUBLISH";
        case SipEventCategory::kPresenceTrigger: return "PRESENCE_TRIGGER";
        case SipEventCategory::kUnknown:         return "UNKNOWN";
        default:                                  return "INVALID";
    }
}

enum class SipEventSource { kSipStack, kPresenceFeed };

struct SipEvent {
    EventId id = 0;
    std::string dialog_id;
    std::string tenant_id;

    nua_event_t       nua_event   = nua_i_error;
    SipDirection      direction   = SipDirection::kIncoming;
    SipEventCategory  category    = SipEventCategory::kUnknown;
    SubscriptionType  sub_type    = SubscriptionType::kUnknown;
    SipEventSource    source      = SipEventSource::kSipStack;
    int               status      = 0;
    std::string       phrase;

    std::string call_id;
    std::string from_uri;
    std::string from_tag;
    std::string to_uri;
    std::string to_tag;
    std::string event_header;
    std::string content_type;
    std::string body;
    uint32_t    cseq     = 0;
    uint32_t    expires  = 0;
    std::string contact_uri;

    std::string subscription_state;
    std::string termination_reason;

    // Presence feed fields
    std::string presence_call_id;
    std::string presence_caller_uri;
    std::string presence_callee_uri;
    std::string presence_state;
    std::string presence_direction;

    TimePoint   created_at  = Clock::now();
    TimePoint   enqueued_at = {};
    TimePoint   dequeued_at = {};

    nua_handle_t* nua_handle = nullptr;

    static std::unique_ptr<SipEvent> create_from_sofia(
        nua_event_t event, int status, const char* phrase,
        nua_handle_t* nh, const sip_t* sip);

    static std::unique_ptr<SipEvent> create_presence_trigger(
        const std::string& dialog_id, const std::string& tenant_id,
        const std::string& presence_call_id,
        const std::string& caller_uri, const std::string& callee_uri,
        const std::string& blf_state, const std::string& direction,
        const std::string& dialog_info_xml_body);

    static EventId next_id();
private:
    static std::atomic<EventId> id_counter_;
};

} // namespace sip_processor
#endif