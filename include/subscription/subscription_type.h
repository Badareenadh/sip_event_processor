
// =============================================================================
// FILE: include/subscription/subscription_type.h
// =============================================================================
#ifndef SUBSCRIPTION_TYPE_H
#define SUBSCRIPTION_TYPE_H

#include <string>

namespace sip_processor {

enum class SubscriptionType { kUnknown, kBLF, kMWI };

inline const char* subscription_type_to_string(SubscriptionType t) {
    switch (t) {
        case SubscriptionType::kBLF:     return "BLF";
        case SubscriptionType::kMWI:     return "MWI";
        case SubscriptionType::kUnknown: return "Unknown";
        default:                         return "Invalid";
    }
}

inline SubscriptionType parse_subscription_type(const char* event_header) {
    if (!event_header) return SubscriptionType::kUnknown;
    std::string ev(event_header);
    if (ev.find("dialog") != std::string::npos)          return SubscriptionType::kBLF;
    if (ev.find("message-summary") != std::string::npos) return SubscriptionType::kMWI;
    return SubscriptionType::kUnknown;
}

inline SubscriptionType subscription_type_from_string(const std::string& s) {
    if (s == "BLF") return SubscriptionType::kBLF;
    if (s == "MWI") return SubscriptionType::kMWI;
    return SubscriptionType::kUnknown;
}

} // namespace sip_processor
#endif