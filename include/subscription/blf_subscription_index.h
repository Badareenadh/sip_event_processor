
// =============================================================================
// FILE: include/subscription/blf_subscription_index.h
// =============================================================================
#ifndef BLF_SUBSCRIPTION_INDEX_H
#define BLF_SUBSCRIPTION_INDEX_H

#include "common/types.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <shared_mutex>

namespace sip_processor {

class BlfSubscriptionIndex {
public:
    static BlfSubscriptionIndex& instance();
    static std::string normalize_uri(const std::string& uri);

    void add(const std::string& monitored_uri, const std::string& dialog_id,
             const std::string& tenant_id);
    void remove(const std::string& monitored_uri, const std::string& dialog_id);
    void remove_dialog(const std::string& dialog_id);

    struct BlfWatcher {
        std::string dialog_id;
        std::string tenant_id;
    };
    std::vector<BlfWatcher> lookup(const std::string& monitored_uri) const;
    std::vector<BlfWatcher> lookup(const std::string& monitored_uri,
                                   const std::string& tenant_id) const;

    size_t monitored_uri_count() const;
    size_t total_watcher_count() const;

    BlfSubscriptionIndex(const BlfSubscriptionIndex&) = delete;
    BlfSubscriptionIndex& operator=(const BlfSubscriptionIndex&) = delete;
private:
    BlfSubscriptionIndex() = default;
    mutable std::shared_mutex mu_;
    struct WatcherEntry { std::string dialog_id; std::string tenant_id; };
    std::unordered_map<std::string, std::vector<WatcherEntry>> uri_to_watchers_;
    std::unordered_map<std::string, std::string> dialog_to_uri_;
};

} // namespace sip_processor
#endif