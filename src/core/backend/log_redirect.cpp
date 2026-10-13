// raylib TraceLog -> LogSink. raylib filters by its own level before calling the
// callback, so that filter is set from the engine's level once at install time. The callback may
// run on raylib's audio thread; log_write is thread-safe and formatting uses only the stack.
#include "log_redirect.h"

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <string_view>

#include <raylib.h>

#include <lam/core/log.h>

namespace lam::detail {
namespace {

constexpr std::size_t kLineBytes = 1024;
constexpr char kPrefix[] = "raylib: ";

LogLevel to_lam_level(int raylib_level) {
    switch (raylib_level) {
    case LOG_TRACE:
        return LogLevel::Trace;
    case LOG_DEBUG:
        return LogLevel::Debug;
    case LOG_INFO:
        return LogLevel::Info;
    case LOG_WARNING:
        return LogLevel::Warn;
    default:
        return LogLevel::Error; // LOG_ERROR, LOG_FATAL
    }
}

// The raylib level matching the lowest engine level that is enabled, so filtered lines are never
// formatted.
int raylib_level_for_engine() {
    if (log_enabled(LogLevel::Trace)) return LOG_TRACE;
    if (log_enabled(LogLevel::Debug)) return LOG_DEBUG;
    if (log_enabled(LogLevel::Info)) return LOG_INFO;
    if (log_enabled(LogLevel::Warn)) return LOG_WARNING;
    return LOG_ERROR;
}

void trace_log_callback(int raylib_level, const char *text, va_list args) {
    const LogLevel level = to_lam_level(raylib_level);
    if (!log_enabled(level) || text == nullptr) return;
    char line[kLineBytes];
    constexpr std::size_t prefix_len = sizeof(kPrefix) - 1;
    for (std::size_t i = 0; i < prefix_len; ++i)
        line[i] = kPrefix[i];
    const int n = std::vsnprintf(line + prefix_len, kLineBytes - prefix_len, text, args);
    if (n < 0) return;
    std::size_t len = prefix_len + static_cast<std::size_t>(n);
    if (len > kLineBytes - 1) len = kLineBytes - 1; // truncated by vsnprintf
    log_write(level, std::string_view(line, len));
}

} // namespace

void install_raylib_log_redirect() {
    SetTraceLogLevel(raylib_level_for_engine());
    SetTraceLogCallback(trace_log_callback);
}

} // namespace lam::detail
