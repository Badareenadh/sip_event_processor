
// =============================================================================
// FILE: include/subscription/subscription_state.h
// =============================================================================
#ifndef SUBSCRIPTION_STATE_H
#define SUBSCRIPTION_STATE_H

#include "common/types.h"
#include "subscription/subscription_type.h"
#include <string>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace sip_processor {

enum class SubLifecycle { kPending, kActive, kTerminating, kTerminated };

inline const char* lifecycle_to_string(SubLifecycle s) {
    switch (s) {
        case SubLifecycle::kPending:     return "Pending";
        case SubLifecycle::kActive:      return "Active";
        case SubLifecycle::kTerminating: return "Terminating";
        case SubLifecycle::kTerminated:  return "Terminated";
        default:                         return "Unknown";
    }
}

inline SubLifecycle lifecycle_from_string(const std::string& s) {
    if (s == "Pending")     return SubLifecycle::kPending;
    if (s == "Active")      return SubLifecycle::kActive;
    if (s == "Terminating") return SubLifecycle::kTerminating;
    if (s == "Terminated")  return SubLifecycle::kTerminated;
    return SubLifecycle::kPending;
}

struct SubscriptionRecord {
    std::string  dialog_id;
    std::string  tenant_id;
    SubscriptionType type       = SubscriptionType::kUnknown;
    SubLifecycle lifecycle      = SubLifecycle::kPending;
    TimePoint    created_at     = Clock::now();
    TimePoint    last_activity  = Clock::now();
    TimePoint    expires_at     = {};
    uint32_t     cseq           = 0;
    uint64_t     events_processed = 0;
    bool         is_processing  = false;
    TimePoint    processing_started_at = {};
    bool         dirty          = false;  // Needs MongoDB sync

    // BLF-specific
    std::string  blf_monitored_uri;
    std::string  blf_last_state;
    std::string  blf_last_direction;
    std::string  blf_presence_call_id;
    std::string  blf_last_notify_body;   // Full last NOTIFY body for redundancy recovery
    uint32_t     blf_notify_version = 0;

    // MWI-specific
    int          mwi_new_messages     = 0;
    int          mwi_old_messages     = 0;
    std::string  mwi_account_uri;
    std::string  mwi_last_notify_body;

    // SIP headers for re-creating dialog on redundant service
    std::string  from_uri;
    std::string  from_tag;
    std::string  to_uri;
    std::string  to_tag;
    std::string  call_id;
    std::string  contact_uri;

    void touch() { last_activity = Clock::now(); dirty = true; }
    bool is_expired() const {
        if (expires_at == TimePoint{}) return false;
        return Clock::now() > expires_at;
    }
    bool is_stuck(Seconds timeout) const {
        if (!is_processing) return false;
        return (Clock::now() - processing_started_at) > timeout;
    }
};

class SubscriptionRegistry {
public:
    static SubscriptionRegistry& instance();

    struct SubscriptionInfo {
        std::string      dialog_id;
        std::string      tenant_id;
        SubscriptionType type;
        SubLifecycle     lifecycle;
        TimePoint        last_activity;
        size_t           worker_index;
    };

    void register_subscription(const std::string& dialog_id, const SubscriptionInfo& info);
    void unregister_subscription(const std::string& dialog_id);
    bool lookup(const std::string& dialog_id, SubscriptionInfo& out) const;
    std::vector<SubscriptionInfo> get_tenant_subscriptions(const TenantId& tenant) const;
    std::vector<SubscriptionInfo> get_all() const;

    size_t total_count() const;
    size_t count_by_type(SubscriptionType type) const;
    size_t count_by_tenant(const TenantId& tenant) const;

    SubscriptionRegistry(const SubscriptionRegistry&) = delete;
    SubscriptionRegistry& operator=(const SubscriptionRegistry&) = delete;
private:
    SubscriptionRegistry() = default;
    mutable std::mutex mu_;
    std::unordered_map<std::string, SubscriptionInfo> subscriptions_;
    std::unordered_map<TenantId, size_t> tenant_counts_;
};

} // namespace sip_processor
#endif