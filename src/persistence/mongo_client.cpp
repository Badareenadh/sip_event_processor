
// =============================================================================
// FILE: src/persistence/mongo_client.cpp
// =============================================================================
#include "persistence/mongo_client.h"
#include "common/logger.h"

#include <mongocxx/instance.hpp>
#include <mongocxx/pool.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/uri.hpp>
#include <mongocxx/exception/exception.hpp>
#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>

namespace sip_processor {

MongoClient::MongoClient(const Config& config) : config_(config) {}

MongoClient::~MongoClient() { disconnect(); }

Result MongoClient::connect() {
    try {
        instance_ = std::make_unique<mongocxx::instance>();

        mongocxx::uri uri{config_.mongo_uri};
        mongocxx::options::pool pool_opts;

        pool_ = std::make_unique<mongocxx::pool>(uri);
        connected_.store(true);

        // Verify connection with a ping
        auto client = pool_->acquire();
        auto db = (*client)[config_.mongo_database];
        auto cmd = bsoncxx::builder::stream::document{} << "ping" << 1
                   << bsoncxx::builder::stream::finalize;
        db.run_command(cmd.view());

        LOG_INFO("MongoDB connected: %s/%s", config_.mongo_uri.c_str(), config_.mongo_database.c_str());
        return Result::kOk;

    } catch (const mongocxx::exception& e) {
        LOG_ERROR("MongoDB connect failed: %s", e.what());
        return Result::kPersistenceError;
    }
}

void MongoClient::disconnect() {
    connected_.store(false);
    pool_.reset();
    instance_.reset();
}

// Pimpl for ScopedClient — stores pool::entry which is the correct RAII guard
struct MongoClient::ScopedClient::Impl {
    mongocxx::pool::entry entry;
    explicit Impl(mongocxx::pool::entry e) : entry(std::move(e)) {}
};

MongoClient::ScopedClient::ScopedClient(MongoClient& parent) : parent_(parent) {
    if (!parent_.pool_ || !parent_.is_connected()) return;
    try {
        auto entry = parent_.pool_->acquire();
        impl_ = std::make_unique<Impl>(std::move(entry));
        valid_ = true;
    } catch (const mongocxx::exception& e) {
        LOG_ERROR("MongoDB acquire client failed: %s", e.what());
        impl_.reset();
        valid_ = false;
    }
}

MongoClient::ScopedClient::~ScopedClient() = default;
MongoClient::ScopedClient::ScopedClient(ScopedClient&&) noexcept = default;

mongocxx::database MongoClient::ScopedClient::database() {
    return (*impl_->entry)[parent_.config_.mongo_database];
}

mongocxx::collection MongoClient::ScopedClient::collection(const std::string& name) {
    return database()[name];
}

MongoClient::ScopedClient MongoClient::acquire() {
    return ScopedClient(*this);
}

} // namespace sip_processor
