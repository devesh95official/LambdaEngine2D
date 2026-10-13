// lam_core build identity (recorded in replay headers) and the log layer.
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include <lam/core/build_info.h>
#include <lam/core/log.h>

using namespace lam;

namespace {
struct Collect final : LogSink {
    std::vector<std::pair<LogLevel, std::string>> lines;
    void write(LogLevel level, std::string_view msg) override {
        lines.emplace_back(level, std::string(msg));
    }
};
} // namespace

TEST_CASE("build info: version, build id and OS description") {
    CHECK(engine_version() == "0.1.0");
    CHECK_FALSE(build_id().empty());
    const std::string os = os_description();
    CHECK_FALSE(os.empty());
    CHECK(os != "unknown");
    CHECK(os.find(' ') != std::string::npos); // "<system> <release> <machine>"
}

TEST_CASE("log: macros format into the installed sink and respect the level") {
    Collect sink;
    LogSink *old = log_sink();
    set_log_sink(&sink);
    set_log_level(LogLevel::Info);
    LAM_LOG_INFO("loaded {} ({} ms)", "map.json", 12);
    LAM_LOG_DEBUG("hidden {}", 1);
    LAM_LOG_WARN("careful");
    LAM_LOG_ERROR("broken {}", 2);
    set_log_level(LogLevel::Trace);
    LAM_LOG_DEBUG("shown");
    set_log_level(LogLevel::Info);
    set_log_sink(old);
    REQUIRE(sink.lines.size() == 4);
    CHECK(sink.lines[0].first == LogLevel::Info);
    CHECK(sink.lines[0].second == "loaded map.json (12 ms)");
    CHECK(sink.lines[1].first == LogLevel::Warn);
    CHECK(sink.lines[2].second == "broken 2");
    CHECK(sink.lines[3].first == LogLevel::Debug);
    CHECK(log_enabled(LogLevel::Error));
    CHECK_FALSE(log_enabled(LogLevel::Debug));
    CHECK(std::string(log_level_name(LogLevel::Warn)) == "warn");
}
