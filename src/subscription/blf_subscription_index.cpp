
// =============================================================================
// FILE: src/subscription/blf_subscription_index.cpp
// =============================================================================
#include "subscription/blf_subscription_index.h"
#include "common/logger.h"
#include <algorithm>
#include <cctype>

namespace sip_processor {

BlfSubscriptionIndex& BlfSubscriptionIndex::instance() {
    static BlfSubscriptionIndex index;
    return index;
}

std::string BlfSubscriptionIndex::normalize_uri(const std::string& uri) {
    if (uri.empty()) return "";

    std::string normalized = uri;

    // Strip angle brackets: <sip:user@host> → sip:user@host
    if (normalized.front() == '<') normalized.erase(0, 1);
    if (!normalized.empty() && normalized.back() == '>') normalized.pop_back();

    // Strip URI parameters: sip:user@host;transport=tcp → sip:user@host
    auto semi = normalized.find(';');
    if (semi != std::string::npos) normalized.erase(semi);

    // Strip port if default: sip:user@host:5060 → sip:user@host
    // Find the host:port part (after @)
    auto at_pos = normalized.find('@');
    if (at_pos != std::string::npos) {
        auto colon = normalized.find(':', at_pos);
        if (colon != std::string::npos) {
            std::string port_str = normalized.substr(colon + 1);
            if (port_str == "5060") normalized.erase(colon);
        }
    }

    // Lowercase the scheme and host (but NOT the user part)
    // sip:User@Host.COM → sip:User@host.com
    auto scheme_end = normalized.find(':');
    if (scheme_end != std::string::npos) {
        for (size_t i = 0; i <= scheme_end; ++i)
            normalized[i] = static_cast<char>(std::tolower(normalized[i]));
    }

    if (at_pos != std::string::npos && at_pos < normalized.size()) {
        for (size_t i = at_pos + 1; i < normalized.size(); ++i)
            normalized[i] = static_cast<char>(std::tolower(normalized[i]));
    }

    // Ensure sip: prefix
    if (normalized.find("sip:") != 0 && normalized.find("sips:") != 0) {
        normalized = "sip:" + normalized;
    }

    return normalized;
}

void BlfSubscriptionIndex::add(const std::string& monitored_uri,
                                const std::string& dialog_id,
                                const std::string& tenant_id) {
    if (monitored_uri.empty() || dialog_id.empty()) {
        LOG_WARN("BlfIndex::add: empty uri or dialog_id");
        return;
    }

    std::string norm_uri = normalize_uri(monitored_uri);

    std::unique_lock<std::shared_mutex> lk(mu_);

    // Check for duplicate
    auto it = dialog_to_uri_.find(dialog_id);
    if (it != dialog_to_uri_.end()) {
        // Already indexed — remove old mapping if URI changed
        if (it->second != norm_uri) {
            auto& old_watchers = uri_to_watchers_[it->second];
            old_watchers.erase(
                std::remove_if(old_watchers.begin(), old_watchers.end(),
                    [&](const WatcherEntry& w) { return w.dialog_id == dialog_id; }),
                old_watchers.end());
            if (old_watchers.empty()) uri_to_watchers_.erase(it->second);
        } else {
            return; // Already indexed with same URI
        }
    }

    uri_to_watchers_[norm_uri].push_back({dialog_id, tenant_id});
    dialog_to_uri_[dialog_id] = norm_uri;

    LOG_DEBUG("BlfIndex: added watcher dialog=%s for uri=%s (total watchers for uri: %zu)",
              dialog_id.c_str(), norm_uri.c_str(), uri_to_watchers_[norm_uri].size());
}

void BlfSubscriptionIndex::remove(const std::string& monitored_uri,
                                   const std::string& dialog_id) {
    std::string norm_uri = normalize_uri(monitored_uri);

    std::unique_lock<std::shared_mutex> lk(mu_);

    auto it = uri_to_watchers_.find(norm_uri);
    if (it != uri_to_watchers_.end()) {
        auto& watchers = it->second;
        watchers.erase(
            std::remove_if(watchers.begin(), watchers.end(),
                [&](const WatcherEntry& w) { return w.dialog_id == dialog_id; }),
            watchers.end());
        if (watchers.empty()) uri_to_watchers_.erase(it);
    }

    dialog_to_uri_.erase(dialog_id);

    LOG_DEBUG("BlfIndex: removed watcher dialog=%s for uri=%s", dialog_id.c_str(), norm_uri.c_str());
}

void BlfSubscriptionIndex::remove_dialog(const std::string& dialog_id) {
    std::unique_lock<std::shared_mutex> lk(mu_);

    auto it = dialog_to_uri_.find(dialog_id);
    if (it == dialog_to_uri_.end()) return;

    std::string norm_uri = it->second;
    dialog_to_uri_.erase(it);

    auto wit = uri_to_watchers_.find(norm_uri);
    if (wit != uri_to_watchers_.end()) {
        auto& watchers = wit->second;
        watchers.erase(
            std::remove_if(watchers.begin(), watchers.end(),
                [&](const WatcherEntry& w) { return w.dialog_id == dialog_id; }),
            watchers.end());
        if (watchers.empty()) uri_to_watchers_.erase(wit);
    }
}

std::vector<BlfSubscriptionIndex::BlfWatcher>
BlfSubscriptionIndex::lookup(const std::string& monitored_uri) const {
    std::string norm_uri = normalize_uri(monitored_uri);

    std::shared_lock<std::shared_mutex> lk(mu_);

    auto it = uri_to_watchers_.find(norm_uri);
    if (it == uri_to_watchers_.end()) return {};

    std::vector<BlfWatcher> result;
    result.reserve(it->second.size());
    for (const auto& w : it->second) {
        result.push_back({w.dialog_id, w.tenant_id});
    }
    return result;
}

std::vector<BlfSubscriptionIndex::BlfWatcher>
BlfSubscriptionIndex::lookup(const std::string& monitored_uri,
                              const std::string& tenant_id) const {
    std::string norm_uri = normalize_uri(monitored_uri);

    std::shared_lock<std::shared_mutex> lk(mu_);

    auto it = uri_to_watchers_.find(norm_uri);
    if (it == uri_to_watchers_.end()) return {};

    std::vector<BlfWatcher> result;
    for (const auto& w : it->second) {
        if (w.tenant_id == tenant_id) {
            result.push_back({w.dialog_id, w.tenant_id});
        }
    }
    return result;
}

size_t BlfSubscriptionIndex::monitored_uri_count() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    return uri_to_watchers_.size();
}

size_t BlfSubscriptionIndex::total_watcher_count() const {
    std::shared_lock<std::shared_mutex> lk(mu_);
    size_t total = 0;
    for (const auto& [uri, watchers] : uri_to_watchers_) total += watchers.size();
    return total;
}

} // namespace sip_processor
