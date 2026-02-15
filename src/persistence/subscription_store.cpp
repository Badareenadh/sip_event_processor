// =============================================================================
// FILE: src/persistence/subscription_store.cpp
// =============================================================================
#include "persistence/subscription_store.h"
#include "persistence/mongo_client.h"
#include "common/logger.h"
#include "MongoPool.h"

#include <mongoc/mongoc.h>

namespace sip_processor {

SubscriptionStore::SubscriptionStore(const Config& config, std::shared_ptr<MongoClient> mongo)
    : config_(config), mongo_(std::move(mongo)), enabled_(config.mongo_enable_persistence)
{}

SubscriptionStore::~SubscriptionStore() { stop(); }

Result SubscriptionStore::start() {
    if (!enabled_) { LOG_INFO("SubStore: persistence disabled"); return Result::kOk; }
    if (!mongo_ || !mongo_->is_connected()) return Result::kError;

    stop_requested_.store(false); running_.store(true);
    sync_thread_ = std::thread(&SubscriptionStore::sync_thread_func, this);

    LOG_INFO("SubStore started (sync=%lds, batch=%zu)",
             config_.mongo_sync_interval.count(), config_.mongo_batch_size);
    return Result::kOk;
}

void SubscriptionStore::stop() {
    if (!running_.load()) return;
    { std::lock_guard<std::mutex> lk(queue_mu_); stop_requested_.store(true); }
    queue_cv_.notify_one();
    if (sync_thread_.joinable()) sync_thread_.join();
    flush_pending();
    running_.store(false);
    LOG_INFO("SubStore stopped");
}

void SubscriptionStore::queue_upsert(const SubscriptionRecord& record) {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lk(queue_mu_);
    pending_ops_.push({PendingOp::kUpsert, record, record.dialog_id});
    stats_.queue_depth.store(pending_ops_.size(), std::memory_order_relaxed);
    queue_cv_.notify_one();
}

void SubscriptionStore::queue_delete(const std::string& dialog_id) {
    if (!enabled_) return;
    std::lock_guard<std::mutex> lk(queue_mu_);
    SubscriptionRecord empty;
    pending_ops_.push({PendingOp::kDelete, empty, dialog_id});
    queue_cv_.notify_one();
}

Result SubscriptionStore::save_immediately(const SubscriptionRecord& record) {
    if (!enabled_ || !mongo_ || !mongo_->is_connected()) return Result::kOk;

    ScopedTimer timer;

    auto now_ms = static_cast<int32_t>(
        std::chrono::duration_cast<Millisecs>(
            std::chrono::system_clock::now().time_since_epoch()).count() / 1000);

    auto expires_ms = record.expires_at != TimePoint{}
        ? static_cast<int32_t>(
            std::chrono::duration_cast<Millisecs>(
                record.expires_at.time_since_epoch()).count() / 1000)
        : 0;

    // Build the filter: { "dialog_id": "<value>" }
    bson_t *filter = bson_new();
    BSON_APPEND_UTF8(filter, "dialog_id", record.dialog_id.c_str());

    // Build the $set update document
    bson_t *update = bson_new();
    bson_t set_child;
    BSON_APPEND_DOCUMENT_BEGIN(update, "$set", &set_child);

    BSON_APPEND_UTF8(&set_child, "dialog_id",            record.dialog_id.c_str());
    BSON_APPEND_UTF8(&set_child, "tenant_id",            record.tenant_id.c_str());
    BSON_APPEND_UTF8(&set_child, "type",                 subscription_type_to_string(record.type));
    BSON_APPEND_UTF8(&set_child, "lifecycle",            lifecycle_to_string(record.lifecycle));
    BSON_APPEND_INT32(&set_child, "cseq",                static_cast<int32_t>(record.cseq));
    BSON_APPEND_UTF8(&set_child, "blf_monitored_uri",    record.blf_monitored_uri.c_str());
    BSON_APPEND_UTF8(&set_child, "blf_last_state",       record.blf_last_state.c_str());
    BSON_APPEND_UTF8(&set_child, "blf_last_direction",   record.blf_last_direction.c_str());
    BSON_APPEND_UTF8(&set_child, "blf_presence_call_id", record.blf_presence_call_id.c_str());
    BSON_APPEND_UTF8(&set_child, "blf_last_notify_body", record.blf_last_notify_body.c_str());
    BSON_APPEND_INT32(&set_child, "blf_notify_version",  static_cast<int32_t>(record.blf_notify_version));
    BSON_APPEND_INT32(&set_child, "mwi_new_messages",    record.mwi_new_messages);
    BSON_APPEND_INT32(&set_child, "mwi_old_messages",    record.mwi_old_messages);
    BSON_APPEND_UTF8(&set_child, "mwi_account_uri",      record.mwi_account_uri.c_str());
    BSON_APPEND_UTF8(&set_child, "mwi_last_notify_body", record.mwi_last_notify_body.c_str());
    BSON_APPEND_UTF8(&set_child, "from_uri",             record.from_uri.c_str());
    BSON_APPEND_UTF8(&set_child, "from_tag",             record.from_tag.c_str());
    BSON_APPEND_UTF8(&set_child, "to_uri",               record.to_uri.c_str());
    BSON_APPEND_UTF8(&set_child, "to_tag",               record.to_tag.c_str());
    BSON_APPEND_UTF8(&set_child, "call_id",              record.call_id.c_str());
    BSON_APPEND_UTF8(&set_child, "contact_uri",          record.contact_uri.c_str());
    BSON_APPEND_INT32(&set_child, "updated_at",          now_ms);
    BSON_APPEND_INT32(&set_child, "expires_at",          expires_ms);
    BSON_APPEND_UTF8(&set_child, "service_id",           config_.service_id.c_str());

    bson_append_document_end(update, &set_child);

    // MongoPool::Execute with MONGO_UPDATE does update_many(filter, update).
    // For upsert behavior, we first try an update; if no match, do an insert.
    // Since MongoPool wraps mongoc_collection_update_many without upsert option,
    // we implement upsert as: try update, if no docs matched, insert the full doc.

    // First, check if the document exists
    MongoPool count_pool;
    int count_ok = count_pool.Execute(filter, nullptr, MONGO_FIND_COUNT,
                                      __FILE__, __LINE__, __func__,
                                      config_.mongo_database.c_str(),
                                      config_.mongo_collection_subs.c_str());

    bool exists = (count_ok && count_pool.getDcount() > 0);

    if (exists) {
        // Update existing document
        // Need a fresh filter since MongoPool::Clear destroys the bson passed to it
        bson_t *filter2 = bson_new();
        BSON_APPEND_UTF8(filter2, "dialog_id", record.dialog_id.c_str());

        MongoPool update_pool;
        int ok = update_pool.Execute(filter2, update, MONGO_UPDATE,
                                     __FILE__, __LINE__, __func__,
                                     config_.mongo_database.c_str(),
                                     config_.mongo_collection_subs.c_str());
        if (!ok) {
            stats_.errors.fetch_add(1, std::memory_order_relaxed);
            LOG_ERROR("SubStore: update failed for %s", record.dialog_id.c_str());
            bson_destroy(filter);
            return Result::kPersistenceError;
        }
    } else {
        // Insert new document (flatten the $set fields into a top-level doc)
        bson_t *insert_doc = bson_new();
        BSON_APPEND_UTF8(insert_doc, "dialog_id",            record.dialog_id.c_str());
        BSON_APPEND_UTF8(insert_doc, "tenant_id",            record.tenant_id.c_str());
        BSON_APPEND_UTF8(insert_doc, "type",                 subscription_type_to_string(record.type));
        BSON_APPEND_UTF8(insert_doc, "lifecycle",            lifecycle_to_string(record.lifecycle));
        BSON_APPEND_INT32(insert_doc, "cseq",                static_cast<int32_t>(record.cseq));
        BSON_APPEND_UTF8(insert_doc, "blf_monitored_uri",    record.blf_monitored_uri.c_str());
        BSON_APPEND_UTF8(insert_doc, "blf_last_state",       record.blf_last_state.c_str());
        BSON_APPEND_UTF8(insert_doc, "blf_last_direction",   record.blf_last_direction.c_str());
        BSON_APPEND_UTF8(insert_doc, "blf_presence_call_id", record.blf_presence_call_id.c_str());
        BSON_APPEND_UTF8(insert_doc, "blf_last_notify_body", record.blf_last_notify_body.c_str());
        BSON_APPEND_INT32(insert_doc, "blf_notify_version",  static_cast<int32_t>(record.blf_notify_version));
        BSON_APPEND_INT32(insert_doc, "mwi_new_messages",    record.mwi_new_messages);
        BSON_APPEND_INT32(insert_doc, "mwi_old_messages",    record.mwi_old_messages);
        BSON_APPEND_UTF8(insert_doc, "mwi_account_uri",      record.mwi_account_uri.c_str());
        BSON_APPEND_UTF8(insert_doc, "mwi_last_notify_body", record.mwi_last_notify_body.c_str());
        BSON_APPEND_UTF8(insert_doc, "from_uri",             record.from_uri.c_str());
        BSON_APPEND_UTF8(insert_doc, "from_tag",             record.from_tag.c_str());
        BSON_APPEND_UTF8(insert_doc, "to_uri",               record.to_uri.c_str());
        BSON_APPEND_UTF8(insert_doc, "to_tag",               record.to_tag.c_str());
        BSON_APPEND_UTF8(insert_doc, "call_id",              record.call_id.c_str());
        BSON_APPEND_UTF8(insert_doc, "contact_uri",          record.contact_uri.c_str());
        BSON_APPEND_INT32(insert_doc, "updated_at",          now_ms);
        BSON_APPEND_INT32(insert_doc, "expires_at",          expires_ms);
        BSON_APPEND_UTF8(insert_doc, "service_id",           config_.service_id.c_str());

        MongoPool insert_pool;
        int ok = insert_pool.Execute(insert_doc, nullptr, MONGO_INSERT,
                                     __FILE__, __LINE__, __func__,
                                     config_.mongo_database.c_str(),
                                     config_.mongo_collection_subs.c_str());
        // filter and update are owned by count_pool / not needed for insert path
        bson_destroy(filter);
        bson_destroy(update);

        if (!ok) {
            stats_.errors.fetch_add(1, std::memory_order_relaxed);
            LOG_ERROR("SubStore: insert failed for %s", record.dialog_id.c_str());
            return Result::kPersistenceError;
        }

        stats_.upserts.fetch_add(1, std::memory_order_relaxed);
        mongo_->mutable_stats().operations.fetch_add(1, std::memory_order_relaxed);
        return Result::kOk;
    }

    // Update path cleanup — filter was consumed by count_pool, update by update_pool
    stats_.upserts.fetch_add(1, std::memory_order_relaxed);
    mongo_->mutable_stats().operations.fetch_add(1, std::memory_order_relaxed);
    return Result::kOk;
}

Result SubscriptionStore::delete_immediately(const std::string& dialog_id) {
    if (!enabled_ || !mongo_ || !mongo_->is_connected()) return Result::kOk;

    bson_t *filter = bson_new();
    BSON_APPEND_UTF8(filter, "dialog_id", dialog_id.c_str());

    MongoPool pool;
    int ok = pool.Execute(filter, nullptr, MONGO_DELETE,
                          __FILE__, __LINE__, __func__,
                          config_.mongo_database.c_str(),
                          config_.mongo_collection_subs.c_str());
    if (!ok) {
        stats_.errors.fetch_add(1, std::memory_order_relaxed);
        LOG_ERROR("SubStore: delete failed for %s", dialog_id.c_str());
        return Result::kPersistenceError;
    }

    stats_.deletes.fetch_add(1, std::memory_order_relaxed);
    mongo_->mutable_stats().operations.fetch_add(1, std::memory_order_relaxed);
    return Result::kOk;
}

Result SubscriptionStore::load_active_subscriptions(std::vector<StoredSubscription>& out) {
    if (!enabled_ || !mongo_ || !mongo_->is_connected()) return Result::kOk;

    // Build filter: { "lifecycle": { "$in": ["Active", "Pending"] } }
    // Since MongoPool only extracts UTF8/INT32/BOOL, and $in requires an array,
    // we do two separate queries: one for "Active", one for "Pending".

    auto load_by_lifecycle = [&](const char* lifecycle_str) -> Result {
        bson_t *query = bson_new();
        BSON_APPEND_UTF8(query, "lifecycle", lifecycle_str);

        MongoPool pool;
        int ok = pool.Execute(query, nullptr, MONGO_FIND,
                              __FILE__, __LINE__, __func__,
                              config_.mongo_database.c_str(),
                              config_.mongo_collection_subs.c_str());
        if (!ok) {
            LOG_ERROR("SubStore: find failed for lifecycle=%s", lifecycle_str);
            return Result::kPersistenceError;
        }

        while (pool.NextRow()) {
            StoredSubscription stored;
            auto& rec = stored.record;

            rec.dialog_id            = pool.getString("dialog_id");
            rec.tenant_id            = pool.getString("tenant_id");
            rec.type                 = subscription_type_from_string(pool.getString("type"));
            rec.lifecycle            = lifecycle_from_string(pool.getString("lifecycle"));
            rec.cseq                 = static_cast<uint32_t>(pool.getInt("cseq"));
            rec.blf_monitored_uri    = pool.getString("blf_monitored_uri");
            rec.blf_last_state       = pool.getString("blf_last_state");
            rec.blf_last_direction   = pool.getString("blf_last_direction");
            rec.blf_presence_call_id = pool.getString("blf_presence_call_id");
            rec.blf_last_notify_body = pool.getString("blf_last_notify_body");
            rec.blf_notify_version   = static_cast<uint32_t>(pool.getInt("blf_notify_version"));
            rec.mwi_new_messages     = pool.getInt("mwi_new_messages");
            rec.mwi_old_messages     = pool.getInt("mwi_old_messages");
            rec.mwi_account_uri      = pool.getString("mwi_account_uri");
            rec.mwi_last_notify_body = pool.getString("mwi_last_notify_body");
            rec.from_uri             = pool.getString("from_uri");
            rec.from_tag             = pool.getString("from_tag");
            rec.to_uri               = pool.getString("to_uri");
            rec.to_tag               = pool.getString("to_tag");
            rec.call_id              = pool.getString("call_id");
            rec.contact_uri          = pool.getString("contact_uri");

            int exp_sec = pool.getInt("expires_at");
            if (exp_sec > 0) {
                rec.expires_at = TimePoint(Seconds(exp_sec));
            }

            rec.last_activity = Clock::now();
            stored.needs_full_state_notify = true;

            if (rec.dialog_id.empty()) continue;

            out.push_back(std::move(stored));
            stats_.loads.fetch_add(1, std::memory_order_relaxed);
        }

        return Result::kOk;
    };

    Result r1 = load_by_lifecycle("Active");
    Result r2 = load_by_lifecycle("Pending");

    if (r1 != Result::kOk || r2 != Result::kOk) {
        stats_.errors.fetch_add(1, std::memory_order_relaxed);
        LOG_ERROR("SubStore: load failed");
        return Result::kPersistenceError;
    }

    mongo_->mutable_stats().operations.fetch_add(2, std::memory_order_relaxed);
    LOG_INFO("SubStore: loaded %zu active subscriptions", out.size());
    return Result::kOk;
}

Result SubscriptionStore::load_subscription(const std::string& dialog_id,
                                              StoredSubscription& out) {
    if (!enabled_ || !mongo_ || !mongo_->is_connected()) return Result::kNotFound;

    bson_t *query = bson_new();
    BSON_APPEND_UTF8(query, "dialog_id", dialog_id.c_str());

    MongoPool pool;
    int ok = pool.Execute(query, nullptr, MONGO_FIND,
                          __FILE__, __LINE__, __func__,
                          config_.mongo_database.c_str(),
                          config_.mongo_collection_subs.c_str());
    if (!ok) {
        stats_.errors.fetch_add(1, std::memory_order_relaxed);
        return Result::kPersistenceError;
    }

    if (!pool.NextRow()) {
        return Result::kNotFound;
    }

    auto& rec = out.record;
    rec.dialog_id            = pool.getString("dialog_id");
    rec.tenant_id            = pool.getString("tenant_id");
    rec.type                 = subscription_type_from_string(pool.getString("type"));
    rec.lifecycle            = lifecycle_from_string(pool.getString("lifecycle"));
    rec.cseq                 = static_cast<uint32_t>(pool.getInt("cseq"));
    rec.blf_monitored_uri    = pool.getString("blf_monitored_uri");
    rec.blf_last_state       = pool.getString("blf_last_state");
    rec.blf_last_direction   = pool.getString("blf_last_direction");
    rec.blf_presence_call_id = pool.getString("blf_presence_call_id");
    rec.blf_last_notify_body = pool.getString("blf_last_notify_body");
    rec.blf_notify_version   = static_cast<uint32_t>(pool.getInt("blf_notify_version"));
    rec.mwi_new_messages     = pool.getInt("mwi_new_messages");
    rec.mwi_old_messages     = pool.getInt("mwi_old_messages");
    rec.mwi_account_uri      = pool.getString("mwi_account_uri");
    rec.mwi_last_notify_body = pool.getString("mwi_last_notify_body");
    rec.from_uri             = pool.getString("from_uri");
    rec.from_tag             = pool.getString("from_tag");
    rec.to_uri               = pool.getString("to_uri");
    rec.to_tag               = pool.getString("to_tag");
    rec.call_id              = pool.getString("call_id");
    rec.contact_uri          = pool.getString("contact_uri");

    int exp_sec = pool.getInt("expires_at");
    if (exp_sec > 0) {
        rec.expires_at = TimePoint(Seconds(exp_sec));
    }

    rec.last_activity = Clock::now();
    out.needs_full_state_notify = true;

    mongo_->mutable_stats().operations.fetch_add(1, std::memory_order_relaxed);
    return Result::kOk;
}

void SubscriptionStore::sync_thread_func() {
    while (!stop_requested_.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lk(queue_mu_);
            queue_cv_.wait_for(lk, config_.mongo_sync_interval, [this] {
                return stop_requested_.load() ||
                       pending_ops_.size() >= config_.mongo_batch_size;
            });
        }
        if (stop_requested_.load() && pending_ops_.empty()) break;
        flush_pending();
    }
}

void SubscriptionStore::flush_pending() {
    std::queue<PendingOp> batch;
    {
        std::lock_guard<std::mutex> lk(queue_mu_);
        std::swap(batch, pending_ops_);
        stats_.queue_depth.store(0, std::memory_order_relaxed);
    }

    if (batch.empty()) return;

    ScopedTimer timer;
    size_t count = batch.size();

    while (!batch.empty()) {
        auto& op = batch.front();
        if (op.type == PendingOp::kUpsert) {
            save_immediately(op.record);
        } else {
            delete_immediately(op.dialog_id);
        }
        batch.pop();
    }

    stats_.batch_writes.fetch_add(1, std::memory_order_relaxed);
    auto ms = timer.elapsed_ms().count();
    if (ms > 100) {
        LOG_WARN("SubStore: batch flush of %zu ops took %ldms", count, ms);
    }
}

} // namespace sip_processor
