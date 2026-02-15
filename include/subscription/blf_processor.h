
// =============================================================================
// FILE: include/subscription/blf_processor.h
// =============================================================================
#ifndef BLF_PROCESSOR_H
#define BLF_PROCESSOR_H
#include "sip/sip_event.h"
#include "subscription/subscription_state.h"
#include "common/types.h"
namespace sip_processor {
class BlfProcessor {
public:
    BlfProcessor() = default;
    ~BlfProcessor() = default;
    Result process(const SipEvent& event, SubscriptionRecord& record);
    struct NotifyAction {
        bool should_notify = false;
        std::string body;
        std::string content_type;
        std::string subscription_state_header;
    };
    NotifyAction process_presence_trigger(const SipEvent& event, SubscriptionRecord& record);
    BlfProcessor(const BlfProcessor&) = delete;
    BlfProcessor& operator=(const BlfProcessor&) = delete;
private:
    Result handle_subscribe(const SipEvent& event, SubscriptionRecord& record);
    Result handle_notify(const SipEvent& event, SubscriptionRecord& record);
    Result handle_subscribe_response(const SipEvent& event, SubscriptionRecord& record);
    Result handle_publish(const SipEvent& event, SubscriptionRecord& record);
    struct DialogState { std::string entity, state, direction, id; bool valid = false; };
    DialogState parse_dialog_info_xml(const std::string& body);
    void update_blf_state(SubscriptionRecord& record, const DialogState& state);
    std::string build_dialog_info_xml(const std::string& entity_uri, const std::string& dialog_id,
        const std::string& call_id, const std::string& state, const std::string& direction,
        const std::string& caller_uri, const std::string& callee_uri) const;
    mutable uint32_t notify_version_ = 0;
};
} // namespace sip_processor
#endif