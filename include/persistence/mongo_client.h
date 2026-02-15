
// =============================================================================
// FILE: include/persistence/mongo_client.h
// =============================================================================
#ifndef MONGO_CLIENT_H
#define MONGO_CLIENT_H

#include "common/types.h"
#include "common/config.h"
#include <memory>
#include <mutex>
#include <atomic>
#include <string>

// Forward declarations — actual mongo headers in .cpp
namespace mongocxx { inline namespace v_noabi {
    class instance;
    class pool;
    class client;
    class database;
    class collection;
}}

namespace sip_processor {

// Thread-safe MongoDB client pool wrapper.
// Manages connection pool, provides scoped clients for operations.
class MongoClient {
public:
    explicit MongoClient(const Config& config);
    ~MongoClient();

    Result connect();
    void disconnect();
    bool is_connected() const { return connected_.load(std::memory_order_acquire); }

    // RAII handle — acquires a client from the pool.
    // Implementation uses pool::entry internally (defined in .cpp).
    class ScopedClient {
    public:
        ScopedClient(MongoClient& parent);
        ~ScopedClient();

        ScopedClient(ScopedClient&&) noexcept;
        ScopedClient& operator=(ScopedClient&&) = delete;
        ScopedClient(const ScopedClient&) = delete;
        ScopedClient& operator=(const ScopedClient&) = delete;

        bool valid() const { return valid_; }
        mongocxx::database database();
        mongocxx::collection collection(const std::string& name);

    private:
        struct Impl;
        MongoClient& parent_;
        std::unique_ptr<Impl> impl_;
        bool valid_ = false;
    };

    ScopedClient acquire();

    struct MongoStats {
        std::atomic<uint64_t> operations{0};
        std::atomic<uint64_t> errors{0};
        std::atomic<uint64_t> latency_total_ms{0};
    };
    const MongoStats& stats() const { return stats_; }

    const Config& config() const { return config_; }

    MongoClient(const MongoClient&) = delete;
    MongoClient& operator=(const MongoClient&) = delete;

private:
    Config config_;
    std::unique_ptr<mongocxx::instance> instance_;
    std::unique_ptr<mongocxx::pool> pool_;
    std::atomic<bool> connected_{false};
    MongoStats stats_;
};

} // namespace sip_processor
#endif // MONGO_CLIENT_H

