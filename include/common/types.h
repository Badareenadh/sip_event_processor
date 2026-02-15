// =============================================================================
// FILE: include/common/types.h
// =============================================================================
#ifndef COMMON_TYPES_H
#define COMMON_TYPES_H

#include <cstdint>
#include <chrono>
#include <string>
#include <memory>
#include <functional>

namespace sip_processor {

using Clock     = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration  = Clock::duration;
using Millisecs = std::chrono::milliseconds;
using Seconds   = std::chrono::seconds;
using EventId   = uint64_t;
using TenantId  = std::string;

enum class Result {
    kOk, kError, kTimeout, kNotFound, kAlreadyExists,
    kCapacityExceeded, kInvalidArgument, kShuttingDown,
    kConnectionLost, kParseError, kPersistenceError
};

inline const char* result_to_string(Result r) {
    switch (r) {
        case Result::kOk:               return "OK";
        case Result::kError:            return "Error";
        case Result::kTimeout:          return "Timeout";
        case Result::kNotFound:         return "NotFound";
        case Result::kAlreadyExists:    return "AlreadyExists";
        case Result::kCapacityExceeded: return "CapacityExceeded";
        case Result::kInvalidArgument:  return "InvalidArgument";
        case Result::kShuttingDown:     return "ShuttingDown";
        case Result::kConnectionLost:   return "ConnectionLost";
        case Result::kParseError:       return "ParseError";
        case Result::kPersistenceError: return "PersistenceError";
        default:                        return "Unknown";
    }
}

// Scoped timer for measuring operation durations
class ScopedTimer {
public:
    ScopedTimer() : start_(Clock::now()) {}
    Millisecs elapsed_ms() const {
        return std::chrono::duration_cast<Millisecs>(Clock::now() - start_);
    }
    double elapsed_sec() const {
        return std::chrono::duration<double>(Clock::now() - start_).count();
    }
private:
    TimePoint start_;
};

} // namespace sip_processor
#endif // COMMON_TYPES_H
