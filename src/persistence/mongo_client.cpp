
// =============================================================================
// FILE: src/persistence/mongo_client.cpp
// =============================================================================
#include "persistence/mongo_client.h"
#include "common/logger.h"
#include "MongoPool.h"

namespace sip_processor {

MongoClient::MongoClient(const Config& config) : config_(config) {}

MongoClient::~MongoClient() { disconnect(); }

Result MongoClient::connect() {
    int pool_size = config_.mongo_pool_max_size > 0 ? config_.mongo_pool_max_size : 10;

    int rc = MongoPool::init(config_.mongo_uri.c_str(), pool_size);
    if (rc != 0) {
        LOG_ERROR("MongoPool::init failed for URI: %s", config_.mongo_uri.c_str());
        return Result::kPersistenceError;
    }

    // Verify connectivity with a count query on the subscriptions collection
    MongoPool pool;
    bson_t *query = bson_new();  // empty query
    int ok = pool.Execute(query, nullptr, MONGO_FIND_COUNT,
                          __FILE__, __LINE__, __func__,
                          config_.mongo_database.c_str(),
                          config_.mongo_collection_subs.c_str());
    if (!ok) {
        LOG_ERROR("MongoDB ping failed: %s/%s",
                  config_.mongo_uri.c_str(), config_.mongo_database.c_str());
        return Result::kPersistenceError;
    }

    connected_.store(true);
    LOG_INFO("MongoDB connected via MongoPool: %s/%s (pool_size=%d)",
             config_.mongo_uri.c_str(), config_.mongo_database.c_str(), pool_size);
    return Result::kOk;
}

void MongoClient::disconnect() {
    connected_.store(false);
}

} // namespace sip_processor
