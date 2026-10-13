#include <lam/core/assert.h>
#include <lam/core/log.h>
#include <lam/core/result.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace lam {
namespace {
std::atomic<LogSink *> g_sink{nullptr};
std::atomic<int> g_min_level{static_cast<int>(LogLevel::Info)};
std::mutex g_stderr_mutex;
} // namespace

void set_log_sink(LogSink *sink) { g_sink.store(sink); }

LogSink *log_sink() { return g_sink.load(); }

void set_log_level(LogLevel min) { g_min_level.store(static_cast<int>(min)); }

bool log_enabled(LogLevel level) { return static_cast<int>(level) >= g_min_level.load(); }

const char *log_level_name(LogLevel level) {
    switch (level) {
    case LogLevel::Trace: return "trace";
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warn: return "warn";
    case LogLevel::Error: return "error";
    }
    return "?";
}

void log_write(LogLevel level, std::string_view msg) {
    if (LogSink *s = g_sink.load()) {
        s->write(level, msg);
        return;
    }
    std::lock_guard lock(g_stderr_mutex);
    std::fprintf(stderr, "LAM %s: %.*s\n", log_level_name(level), static_cast<int>(msg.size()), msg.data());
}

Error make_error(ErrorCode code, std::string message) { return Error{code, std::move(message)}; }

const char *error_code_name(ErrorCode code) {
    switch (code) {
    case ErrorCode::Unknown: return "Unknown";
    case ErrorCode::NotFound: return "NotFound";
    case ErrorCode::Io: return "Io";
    case ErrorCode::Parse: return "Parse";
    case ErrorCode::Invalid: return "Invalid";
    case ErrorCode::OutOfRange: return "OutOfRange";
    case ErrorCode::Unsupported: return "Unsupported";
    case ErrorCode::VersionTooNew: return "VersionTooNew";
    case ErrorCode::MigrationFailed: return "MigrationFailed";
    case ErrorCode::Busy: return "Busy";
    case ErrorCode::NotImplemented: return "NotImplemented";
    case ErrorCode::Device: return "Device";
    }
    return "?";
}

namespace detail {
void assert_fail(const char *file, int line, const char *expr, const char *msg) {
    char buf[1024];
    std::snprintf(buf, sizeof buf, "%s:%d: check failed: %s: %s", file, line, expr, msg ? msg : "");
    log_write(LogLevel::Error, buf);
    std::fflush(stderr);
    std::abort();
}
} // namespace detail

} // namespace lam
