
// =============================================================================
// FILE: include/subscription/mwi_processor.h
// =============================================================================
#ifndef MWI_PROCESSOR_H
#define MWI_PROCESSOR_H
#include "sip/sip_event.h"
#include "subscription/subscription_state.h"
#include "common/types.h"
namespace sip_processor {
class MwiProcessor {
public:
    MwiProcessor() = default;
    ~MwiProcessor() = default;
    Result process(const SipEvent& event, SubscriptionRecord& record);
    MwiProcessor(const MwiProcessor&) = delete;
    MwiProcessor& operator=(const MwiProcessor&) = delete;
private:
    Result handle_subscribe(const SipEvent& event, SubscriptionRecord& record);
    Result handle_notify(const SipEvent& event, SubscriptionRecord& record);
    Result handle_subscribe_response(const SipEvent& event, SubscriptionRecord& record);
    Result handle_publish(const SipEvent& event, SubscriptionRecord& record);
    struct MessageSummary { bool messages_waiting=false; int new_messages=0, old_messages=0, new_urgent=0, old_urgent=0; std::string account; bool valid=false; };
    MessageSummary parse_message_summary(const std::string& body);
    void update_mwi_state(SubscriptionRecord& record, const MessageSummary& summary);
};
} // namespace sip_processor
#endif