#pragma once
// Logging. Macros are for cold paths only (std::format allocates). Thread-safe.
//   LAM_LOG_INFO("loaded {} ({} ms)", path, ms);
#include <cstdint>
#include <format>
#include <string_view>
namespace lam {
enum class LogLevel : std::uint8_t { Trace, Debug, Info, Warn, Error };
class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(LogLevel level, std::string_view msg) = 0;   // may be called from any thread
};
// Installs the sink (null -> stderr). The sink must outlive its installation.
void set_log_sink(LogSink *sink);
LogSink *log_sink();
void set_log_level(LogLevel min);
void log_write(LogLevel level, std::string_view msg);
bool log_enabled(LogLevel level);
const char *log_level_name(LogLevel level);
}
#define LAM_DETAIL_LOG(level, ...)                                                                 \
    do {                                                                                           \
        if (::lam::log_enabled(level)) ::lam::log_write(level, std::format(__VA_ARGS__));          \
    } while (false)
#define LAM_LOG_DEBUG(...) LAM_DETAIL_LOG(::lam::LogLevel::Debug, __VA_ARGS__)
#define LAM_LOG_INFO(...) LAM_DETAIL_LOG(::lam::LogLevel::Info, __VA_ARGS__)
#define LAM_LOG_WARN(...) LAM_DETAIL_LOG(::lam::LogLevel::Warn, __VA_ARGS__)
#define LAM_LOG_ERROR(...) LAM_DETAIL_LOG(::lam::LogLevel::Error, __VA_ARGS__)
