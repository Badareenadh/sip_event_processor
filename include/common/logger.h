// =============================================================================
// FILE: include/common/logger.h
// =============================================================================
#ifndef COMMON_LOGGER_H
#define COMMON_LOGGER_H

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <chrono>
#include <pthread.h>

namespace sip_processor {

enum class LogLevel {
    kTrace = 0,
    kDebug = 1,
    kInfo  = 2,
    kWarn  = 3,
    kError = 4,
    kFatal = 5
};

inline const char* log_level_name(LogLevel level) {
    switch (level) {
        case LogLevel::kTrace: return "TRACE";
        case LogLevel::kDebug: return "DEBUG";
        case LogLevel::kInfo:  return "INFO";
        case LogLevel::kWarn:  return "WARN";
        case LogLevel::kError: return "ERROR";
        case LogLevel::kFatal: return "FATAL";
        default:               return "UNKNOWN";
    }
}

inline LogLevel parse_log_level(const std::string& s) {
    if (s == "trace") return LogLevel::kTrace;
    if (s == "debug") return LogLevel::kDebug;
    if (s == "info")  return LogLevel::kInfo;
    if (s == "warn")  return LogLevel::kWarn;
    if (s == "error") return LogLevel::kError;
    if (s == "fatal") return LogLevel::kFatal;
    return LogLevel::kInfo;
}

struct LogSinkConfig {
    std::string file_path;
    size_t max_file_size_bytes = 50 * 1024 * 1024;  // 50 MB
    int    max_rotated_files   = 10;
    LogLevel min_level         = LogLevel::kTrace;
    bool   also_stderr         = false;
};

class LogSink {
public:
    explicit LogSink(const LogSinkConfig& config);
    ~LogSink();

    void write(LogLevel level, const char* formatted_msg, size_t len);
    void flush();

    LogSink(const LogSink&) = delete;
    LogSink& operator=(const LogSink&) = delete;

private:
    void open_file();
    std::string rotated_path(int index) const;
    bool needs_rotation() const;
    void rotate();

    LogSinkConfig config_;
    std::mutex mu_;
    FILE* fp_ = nullptr;
    size_t current_size_ = 0;
};

class Logger {
public:
    static Logger& instance();

    void set_level(LogLevel level) { level_.store(level, std::memory_order_relaxed); }
    LogLevel level() const { return level_.load(std::memory_order_relaxed); }

    void configure(const std::string& log_dir,
                   const std::string& base_name,
                   LogLevel console_level,
                   size_t max_file_size_bytes,
                   int max_rotated_files);

    void add_sink(std::unique_ptr<LogSink> sink);

    void log(LogLevel level, const char* file, int line, const char* fmt, ...)
        __attribute__((format(printf, 5, 6)));

    void log_slow(const char* file, int line, const char* fmt, ...)
        __attribute__((format(printf, 4, 5)));

    void flush_all();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

private:
    Logger();
    ~Logger();

    size_t format_message(char* buf, size_t buf_size,
                          LogLevel level, const char* file, int line,
                          const char* fmt, va_list args);

    std::atomic<LogLevel> level_;
    std::mutex configure_mu_;
    std::vector<std::unique_ptr<LogSink>> sinks_;
    std::unique_ptr<LogSink> slow_event_sink_;
    bool stderr_fallback_ = true;
    bool configured_ = false;
};

// Logging macros
#define LOG_TRACE(fmt, ...) \
    sip_processor::Logger::instance().log(sip_processor::LogLevel::kTrace, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) \
    sip_processor::Logger::instance().log(sip_processor::LogLevel::kDebug, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) \
    sip_processor::Logger::instance().log(sip_processor::LogLevel::kInfo, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...) \
    sip_processor::Logger::instance().log(sip_processor::LogLevel::kWarn, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) \
    sip_processor::Logger::instance().log(sip_processor::LogLevel::kError, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_FATAL(fmt, ...) \
    sip_processor::Logger::instance().log(sip_processor::LogLevel::kFatal, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_SLOW(fmt, ...) \
    sip_processor::Logger::instance().log_slow(__FILE__, __LINE__, fmt, ##__VA_ARGS__)

} // namespace sip_processor
#endif // COMMON_LOGGER_H
