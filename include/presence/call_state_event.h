
// =============================================================================
// FILE: include/presence/call_state_event.h
// =============================================================================
#ifndef CALL_STATE_EVENT_H
#define CALL_STATE_EVENT_H

#include "common/types.h"
#include <string>
#include <atomic>

namespace sip_processor {

enum class CallState {
    kUnknown, kTrying, kRinging, kConfirmed, kTerminated, kHeld, kResumed
};

inline const char* call_state_to_string(CallState s) {
    switch (s) {
        case CallState::kTrying:     return "trying";
        case CallState::kRinging:    return "early";
        case CallState::kConfirmed:  return "confirmed";
        case CallState::kTerminated: return "terminated";
        case CallState::kHeld:       return "confirmed";
        case CallState::kResumed:    return "confirmed";
        default:                     return "unknown";
    }
}

inline const char* call_state_to_blf_state(CallState s) {
    return call_state_to_string(s);  // Same mapping
}

struct CallStateEvent {
    EventId     id = 0;
    std::string presence_call_id;
    std::string caller_uri;
    std::string callee_uri;
    CallState   state       = CallState::kUnknown;
    std::string direction;
    std::string tenant_id;
    std::string timestamp_str;
    TimePoint   received_at = Clock::now();
    bool        is_valid    = false;

    static EventId next_id();
private:
    static std::atomic<EventId> id_counter_;
};

} // namespace sip_processor
#endif
