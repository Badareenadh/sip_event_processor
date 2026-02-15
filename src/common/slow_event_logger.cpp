
// =============================================================================
// FILE: src/common/slow_event_logger.cpp
// =============================================================================
#include "common/slow_event_logger.h"

namespace sip_processor {

SlowEventLogger::SlowEventLogger(const Config& config)
    : warn_ms_(config.slow_event_warn_threshold.count())
    , error_ms_(config.slow_event_error_threshold.count())
    , critical_ms_(config.slow_event_critical_threshold.count())
    , log_stack_trace_(config.slow_event_log_stack_trace)
{}

void SlowEventLogger::set_thresholds(Millisecs warn, Millisecs error, Millisecs critical) {
    warn_ms_.store(warn.count(), std::memory_order_relaxed);
    error_ms_.store(error.count(), std::memory_order_relaxed);
    critical_ms_.store(critical.count(), std::memory_order_relaxed);
}

SlowEventLogger::Thresholds SlowEventLogger::thresholds() const {
    return {
        Millisecs(warn_ms_.load(std::memory_order_relaxed)),
        Millisecs(error_ms_.load(std::memory_order_relaxed)),
        Millisecs(critical_ms_.load(std::memory_order_relaxed))
    };
}

void SlowEventLogger::check_and_log(const char* operation,
                                      const std::string& dialog_id,
                                      const std::string& extra_context,
                                      Millisecs elapsed) {
    int64_t ms = elapsed.count();

    // Update max duration
    uint64_t prev_max = stats_.max_duration_ms.load(std::memory_order_relaxed);
    while (static_cast<uint64_t>(ms) > prev_max) {
        if (stats_.max_duration_ms.compare_exchange_weak(prev_max, ms,
                std::memory_order_relaxed)) break;
    }

    int64_t crit = critical_ms_.load(std::memory_order_relaxed);
    int64_t err  = error_ms_.load(std::memory_order_relaxed);
    int64_t warn = warn_ms_.load(std::memory_order_relaxed);

    if (ms >= crit) {
        stats_.critical_count.fetch_add(1, std::memory_order_relaxed);
        LOG_ERROR("SLOW_EVENT CRITICAL: %s took %ldms dialog=%s %s",
                  operation, ms, dialog_id.c_str(), extra_context.c_str());
    } else if (ms >= err) {
        stats_.error_count.fetch_add(1, std::memory_order_relaxed);
        LOG_ERROR("SLOW_EVENT: %s took %ldms dialog=%s %s",
                  operation, ms, dialog_id.c_str(), extra_context.c_str());
    } else if (ms >= warn) {
        stats_.warn_count.fetch_add(1, std::memory_order_relaxed);
        LOG_WARN("SLOW_EVENT: %s took %ldms dialog=%s %s",
                 operation, ms, dialog_id.c_str(), extra_context.c_str());
    }
}

SlowEventLogger::Timer::Timer(SlowEventLogger& logger, const char* operation,
                                const std::string& dialog_id, const std::string& extra)
    : logger_(logger), operation_(operation), dialog_id_(dialog_id)
    , extra_context_(extra), start_(Clock::now())
{}

SlowEventLogger::Timer::~Timer() {
    if (!finished_) finish();
}

void SlowEventLogger::Timer::finish() {
    if (finished_) return;
    finished_ = true;
    auto elapsed = std::chrono::duration_cast<Millisecs>(Clock::now() - start_);
    logger_.check_and_log(operation_, dialog_id_, extra_context_, elapsed);
}

} // namespace sip_processor
