
// =============================================================================
// FILE: src/subscription/subscription_state.cpp
// =============================================================================
#include "subscription/subscription_state.h"
#include "common/logger.h"

namespace sip_processor {

SubscriptionRegistry& SubscriptionRegistry::instance() {
    static SubscriptionRegistry registry;
    return registry;
}

void SubscriptionRegistry::register_subscription(const std::string& dialog_id,
                                                   const SubscriptionInfo& info) {
    std::lock_guard<std::mutex> lk(mu_);
    auto [it, inserted] = subscriptions_.emplace(dialog_id, info);
    if (inserted) tenant_counts_[info.tenant_id]++;
    else it->second = info;
}

void SubscriptionRegistry::unregister_subscription(const std::string& dialog_id) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = subscriptions_.find(dialog_id);
    if (it != subscriptions_.end()) {
        auto tc = tenant_counts_.find(it->second.tenant_id);
        if (tc != tenant_counts_.end()) {
            if (tc->second > 0) tc->second--;
            if (tc->second == 0) tenant_counts_.erase(tc);
        }
        subscriptions_.erase(it);
    }
}

bool SubscriptionRegistry::lookup(const std::string& dialog_id,
                                   SubscriptionInfo& out) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = subscriptions_.find(dialog_id);
    if (it != subscriptions_.end()) { out = it->second; return true; }
    return false;
}

std::vector<SubscriptionRegistry::SubscriptionInfo>
SubscriptionRegistry::get_tenant_subscriptions(const TenantId& tenant) const {
    std::vector<SubscriptionInfo> result;
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& [id, info] : subscriptions_)
        if (info.tenant_id == tenant) result.push_back(info);
    return result;
}

std::vector<SubscriptionRegistry::SubscriptionInfo>
SubscriptionRegistry::get_all() const {
    std::vector<SubscriptionInfo> result;
    std::lock_guard<std::mutex> lk(mu_);
    result.reserve(subscriptions_.size());
    for (const auto& [id, info] : subscriptions_)
        result.push_back(info);
    return result;
}

size_t SubscriptionRegistry::total_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return subscriptions_.size();
}

size_t SubscriptionRegistry::count_by_type(SubscriptionType type) const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t c = 0;
    for (const auto& [id, info] : subscriptions_) if (info.type == type) c++;
    return c;
}

size_t SubscriptionRegistry::count_by_tenant(const TenantId& tenant) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = tenant_counts_.find(tenant);
    return (it != tenant_counts_.end()) ? it->second : 0;
}

} // namespace sip_processor

