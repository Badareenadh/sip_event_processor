// =============================================================================
// FILE: src/common/logger.cpp
// =============================================================================
#include "common/logger.h"
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>

namespace sip_processor {

// =============================================================================
// LogSink
// =============================================================================

LogSink::LogSink(const LogSinkConfig& config) : config_(config) {
    if (!config_.file_path.empty()) {
        open_file();
    }
}

LogSink::~LogSink() {
    std::lock_guard<std::mutex> lk(mu_);
    if (fp_ && fp_ != stderr && fp_ != stdout) {
        fflush(fp_);
        fclose(fp_);
        fp_ = nullptr;
    }
}

void LogSink::open_file() {
    if (config_.file_path.empty()) return;

    fp_ = fopen(config_.file_path.c_str(), "a");
    if (!fp_) {
        fprintf(stderr, "LOGGER: failed to open log file '%s': %s\n",
                config_.file_path.c_str(), strerror(errno));
        fp_ = stderr;
        return;
    }

    // Get current file size
    struct stat st;
    if (fstat(fileno(fp_), &st) == 0) {
        current_size_ = static_cast<size_t>(st.st_size);
    }
}

std::string LogSink::rotated_path(int index) const {
    return config_.file_path + "." + std::to_string(index);
}

bool LogSink::needs_rotation() const {
    return config_.max_file_size_bytes > 0 &&
           current_size_ >= config_.max_file_size_bytes;
}

void LogSink::rotate() {
    if (!fp_ || fp_ == stderr || fp_ == stdout) return;

    fflush(fp_);
    fclose(fp_);
    fp_ = nullptr;

    // Rotate files: .9 -> .10, .8 -> .9, ... .1 -> .2, current -> .1
    // Delete oldest if exceeds max_rotated_files
    std::string oldest = rotated_path(config_.max_rotated_files);
    remove(oldest.c_str());

    for (int i = config_.max_rotated_files - 1; i >= 1; --i) {
        std::string src = rotated_path(i);
        std::string dst = rotated_path(i + 1);
        rename(src.c_str(), dst.c_str());
    }

    // Current -> .1
    rename(config_.file_path.c_str(), rotated_path(1).c_str());

    // Open fresh file
    current_size_ = 0;
    open_file();
}

void LogSink::write(LogLevel level, const char* formatted_msg, size_t len) {
    if (level < config_.min_level) return;

    std::lock_guard<std::mutex> lk(mu_);

    if (!fp_) return;

    // Check rotation before write
    if (needs_rotation()) {
        rotate();
        if (!fp_) return;
    }

    size_t written = fwrite(formatted_msg, 1, len, fp_);
    current_size_ += written;

    // Also write to stderr if configured
    if (config_.also_stderr && fp_ != stderr) {
        fwrite(formatted_msg, 1, len, stderr);
    }

    // Flush on WARN and above for timely visibility
    if (level >= LogLevel::kWarn) {
        fflush(fp_);
    }
}

void LogSink::flush() {
    std::lock_guard<std::mutex> lk(mu_);
    if (fp_) fflush(fp_);
}


// =============================================================================
// Logger
// =============================================================================

Logger::Logger() : level_(LogLevel::kInfo) {}

Logger::~Logger() {
    flush_all();
}

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

void Logger::configure(const std::string& log_dir,
                       const std::string& base_name,
                       LogLevel console_level,
                       size_t max_file_size_bytes,
                       int max_rotated_files) {
    std::lock_guard<std::mutex> lk(configure_mu_);

    sinks_.clear();
    slow_event_sink_.reset();

    // Create log directory if needed
    if (!log_dir.empty()) {
        mkdir(log_dir.c_str(), 0755);
    }

    std::string prefix = log_dir.empty() ? base_name : log_dir + "/" + base_name;

    // 1. Main log file - INFO and above
    {
        LogSinkConfig cfg;
        cfg.file_path = prefix + ".log";
        cfg.max_file_size_bytes = max_file_size_bytes;
        cfg.max_rotated_files = max_rotated_files;
        cfg.min_level = LogLevel::kInfo;
        cfg.also_stderr = (console_level <= LogLevel::kInfo);
        sinks_.push_back(std::make_unique<LogSink>(cfg));
    }

    // 2. Debug log file - DEBUG and TRACE
    {
        LogSinkConfig cfg;
        cfg.file_path = prefix + "_debug.log";
        cfg.max_file_size_bytes = max_file_size_bytes;
        cfg.max_rotated_files = max_rotated_files / 2;
        cfg.min_level = LogLevel::kTrace;
        cfg.also_stderr = false;
        sinks_.push_back(std::make_unique<LogSink>(cfg));
    }

    // 3. Error log file - ERROR and FATAL only
    {
        LogSinkConfig cfg;
        cfg.file_path = prefix + "_error.log";
        cfg.max_file_size_bytes = max_file_size_bytes;
        cfg.max_rotated_files = max_rotated_files;
        cfg.min_level = LogLevel::kError;
        cfg.also_stderr = true;
        sinks_.push_back(std::make_unique<LogSink>(cfg));
    }

    // 4. Slow event log file - dedicated
    {
        LogSinkConfig cfg;
        cfg.file_path = prefix + "_slow.log";
        cfg.max_file_size_bytes = max_file_size_bytes;
        cfg.max_rotated_files = max_rotated_files;
        cfg.min_level = LogLevel::kTrace;
        cfg.also_stderr = false;
        slow_event_sink_ = std::make_unique<LogSink>(cfg);
    }

    stderr_fallback_ = false;
    configured_ = true;

    fprintf(stderr, "Logger configured: dir=%s base=%s max_size=%zu max_files=%d\n",
            log_dir.c_str(), base_name.c_str(), max_file_size_bytes, max_rotated_files);
}

void Logger::add_sink(std::unique_ptr<LogSink> sink) {
    std::lock_guard<std::mutex> lk(configure_mu_);
    sinks_.push_back(std::move(sink));
}

size_t Logger::format_message(char* buf, size_t buf_size,
                              LogLevel level, const char* file, int line,
                              const char* fmt, va_list args) {
    // Timestamp
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);

    // Thread ID
    auto tid = static_cast<unsigned long>(pthread_self());

    // Extract filename
    const char* base = strrchr(file, '/');
    base = base ? base + 1 : file;

    // Format prefix
    int prefix_len = snprintf(buf, buf_size,
        "%04d-%02d-%02d %02d:%02d:%02d.%03d [%s] [tid:%lu] [%s:%d] ",
        tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
        static_cast<int>(ms.count()),
        log_level_name(level), tid, base, line);

    if (prefix_len < 0 || static_cast<size_t>(prefix_len) >= buf_size) {
        return 0;
    }

    // Format user message
    int msg_len = vsnprintf(buf + prefix_len,
                            buf_size - static_cast<size_t>(prefix_len),
                            fmt, args);

    if (msg_len < 0) return static_cast<size_t>(prefix_len);

    size_t total = static_cast<size_t>(prefix_len) + static_cast<size_t>(msg_len);
    if (total >= buf_size - 1) total = buf_size - 2;

    // Ensure newline
    buf[total] = '\n';
    buf[total + 1] = '\0';
    return total + 1;
}

void Logger::log(LogLevel level, const char* file, int line, const char* fmt, ...) {
    if (level < level_.load(std::memory_order_relaxed)) return;

    char buf[4096];
    va_list args;
    va_start(args, fmt);
    size_t len = format_message(buf, sizeof(buf), level, file, line, fmt, args);
    va_end(args);

    if (len == 0) return;

    if (stderr_fallback_ || !configured_) {
        fwrite(buf, 1, len, stderr);
        if (level >= LogLevel::kWarn) fflush(stderr);
        return;
    }

    // Write to all matching sinks
    for (auto& sink : sinks_) {
        sink->write(level, buf, len);
    }

    if (level == LogLevel::kFatal) {
        flush_all();
    }
}

void Logger::log_slow(const char* file, int line, const char* fmt, ...) {
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    size_t len = format_message(buf, sizeof(buf), LogLevel::kWarn, file, line, fmt, args);
    va_end(args);

    if (len == 0) return;

    // Write to dedicated slow event log
    if (slow_event_sink_) {
        slow_event_sink_->write(LogLevel::kWarn, buf, len);
    }

    // Also write to main sinks as WARN
    for (auto& sink : sinks_) {
        sink->write(LogLevel::kWarn, buf, len);
    }
}

void Logger::flush_all() {
    for (auto& sink : sinks_) {
        sink->flush();
    }
    if (slow_event_sink_) slow_event_sink_->flush();
}

} // namespace sip_processor
