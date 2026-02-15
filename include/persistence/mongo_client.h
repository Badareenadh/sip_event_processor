
// =============================================================================
// FILE: include/persistence/mongo_client.h
// =============================================================================
#ifndef MONGO_CLIENT_H
#define MONGO_CLIENT_H

#include "common/types.h"
#include "common/config.h"
#include <atomic>
#include <string>

// Forward-declare MongoPool (from Mongodbpool library)
class MongoPool;

namespace sip_processor {

// Thread-safe MongoDB client wrapper using the custom MongoPool library.
// MongoPool manages its own static connection pool internally.
class MongoClient {
public:
    explicit MongoClient(const Config& config);
    ~MongoClient();

    Result connect();
    void disconnect();
    bool is_connected() const { return connected_.load(std::memory_order_acquire); }

    const std::string& database_name() const { return config_.mongo_database; }
    const std::string& collection_name() const { return config_.mongo_collection_subs; }

    struct MongoStats {
        std::atomic<uint64_t> operations{0};
        std::atomic<uint64_t> errors{0};
        std::atomic<uint64_t> latency_total_ms{0};
    };
    const MongoStats& stats() const { return stats_; }
    MongoStats& mutable_stats() { return stats_; }

    const Config& config() const { return config_; }

    MongoClient(const MongoClient&) = delete;
    MongoClient& operator=(const MongoClient&) = delete;

private:
    Config config_;
    std::atomic<bool> connected_{false};
    MongoStats stats_;
};

} // namespace sip_processor
#endif // MONGO_CLIENT_H
