
// =============================================================================
// FILE: include/persistence/subscription_store.h
// =============================================================================
#ifndef SUBSCRIPTION_STORE_H
#define SUBSCRIPTION_STORE_H

#include "common/types.h"
#include "common/config.h"
#include "subscription/subscription_state.h"
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <queue>

namespace sip_processor {

class MongoClient;

// Persists minimal subscription state to MongoDB for cross-service redundancy.
//
// What is stored (minimal — just enough to resume on another service):
//   - dialog_id, tenant_id, subscription type, lifecycle
//   - SIP dialog identifiers (Call-ID, from-tag, to-tag, URIs)
//   - Last known BLF state + full last NOTIFY body
//   - Last known MWI counts + full last NOTIFY body
//   - Expiry time, CSeq, notify version
//
// Why store the last NOTIFY body?
//   When a subscription fails over to a redundant service, it needs to send
//   a full-state NOTIFY immediately. The stored body ensures continuity.
//
// Sync strategy:
//   - Dirty records are batched and written periodically (configurable interval)
//   - Critical events (subscription create/terminate) are written immediately
//   - Uses upsert to handle idempotent writes
//
// Recovery:
//   - On startup, load all active subscriptions from MongoDB
//   - Recreate subscription records and BLF index entries
//   - Mark all as needing a full-state NOTIFY refresh
class SubscriptionStore {
public:
    SubscriptionStore(const Config& config, std::shared_ptr<MongoClient> mongo);
    ~SubscriptionStore();

    Result start();
    void stop();

    // Queue a record for persistence (async, batched)
    void queue_upsert(const SubscriptionRecord& record);

    // Queue an immediate delete
    void queue_delete(const std::string& dialog_id);

    // Synchronous operations for critical paths
    Result save_immediately(const SubscriptionRecord& record);
    Result delete_immediately(const std::string& dialog_id);

    // Load all active subscriptions from MongoDB (for recovery on startup)
    struct StoredSubscription {
        SubscriptionRecord record;
        bool needs_full_state_notify = true;  // Recovered sub needs full NOTIFY
    };
    Result load_active_subscriptions(std::vector<StoredSubscription>& out);

    // Load a specific subscription by dialog_id
    Result load_subscription(const std::string& dialog_id, StoredSubscription& out);

    bool is_enabled() const { return enabled_; }

    struct StoreStats {
        std::atomic<uint64_t> upserts{0};
        std::atomic<uint64_t> deletes{0};
        std::atomic<uint64_t> loads{0};
        std::atomic<uint64_t> errors{0};
        std::atomic<uint64_t> batch_writes{0};
        std::atomic<uint64_t> queue_depth{0};
    };
    const StoreStats& stats() const { return stats_; }

    SubscriptionStore(const SubscriptionStore&) = delete;
    SubscriptionStore& operator=(const SubscriptionStore&) = delete;

private:
    void sync_thread_func();
    void flush_pending();

    // Serialize/deserialize subscription records
    // (Implemented using bsoncxx builders — actual implementation in .cpp)
    void serialize_record(const SubscriptionRecord& record, /* bson doc */ void* doc);
    bool deserialize_record(const void* doc, SubscriptionRecord& record);

    Config config_;
    std::shared_ptr<MongoClient> mongo_;
    bool enabled_;

    std::thread sync_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    // Pending upsert/delete queue
    struct PendingOp {
        enum Type { kUpsert, kDelete };
        Type type;
        SubscriptionRecord record;  // For upsert
        std::string dialog_id;      // For delete
    };
    std::mutex queue_mu_;
    std::condition_variable queue_cv_;
    std::queue<PendingOp> pending_ops_;

    StoreStats stats_;
};

} // namespace sip_processor
#endif // SUBSCRIPTION_STORE_H
