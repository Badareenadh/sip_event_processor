
// =============================================================================
// FILE: include/common/slow_event_logger.h
// =============================================================================
#ifndef SLOW_EVENT_LOGGER_H
#define SLOW_EVENT_LOGGER_H

#include "common/types.h"
#include "common/config.h"
#include "common/logger.h"
#include <atomic>
#include <string>

namespace sip_processor {

// Logs warnings when event processing exceeds configured thresholds.
// Usage:
//   SlowEventLogger::Timer timer(slow_logger, "SUBSCRIBE", dialog_id);
//   ... process event ...
//   timer.finish(); // or let destructor call it
//
// Auto-logs at appropriate level based on elapsed time:
//   > warn_threshold:     LOG_WARN
//   > error_threshold:    LOG_ERROR
//   > critical_threshold: LOG_ERROR + metrics bump
class SlowEventLogger {
public:
    explicit SlowEventLogger(const Config& config);

    // Update thresholds at runtime (e.g., via HTTP API)
    void set_thresholds(Millisecs warn, Millisecs error, Millisecs critical);

    struct Thresholds {
        Millisecs warn;
        Millisecs error;
        Millisecs critical;
    };
    Thresholds thresholds() const;

    // RAII timer for automatic logging
    class Timer {
    public:
        Timer(SlowEventLogger& logger,
              const char* operation,
              const std::string& dialog_id,
              const std::string& extra_context = "");
        ~Timer();

        // Explicit finish (prevents double-log in destructor)
        void finish();

        Millisecs elapsed() const {
            return std::chrono::duration_cast<Millisecs>(Clock::now() - start_);
        }

    private:
        SlowEventLogger& logger_;
        const char* operation_;
        std::string dialog_id_;
        std::string extra_context_;
        TimePoint start_;
        bool finished_ = false;
    };

    // Stats
    struct Stats {
        std::atomic<uint64_t> warn_count{0};
        std::atomic<uint64_t> error_count{0};
        std::atomic<uint64_t> critical_count{0};
        std::atomic<uint64_t> max_duration_ms{0};
    };
    const Stats& stats() const { return stats_; }

private:
    friend class Timer;
    void check_and_log(const char* operation,
                       const std::string& dialog_id,
                       const std::string& extra_context,
                       Millisecs elapsed);

    std::atomic<int64_t> warn_ms_;
    std::atomic<int64_t> error_ms_;
    std::atomic<int64_t> critical_ms_;
    bool log_stack_trace_;
    Stats stats_;
};

} // namespace sip_processor
#endif // SLOW_EVENT_LOGGER_H