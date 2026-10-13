// Tests for the shared test harness in tests/support.
#include <lam/test/support.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <lam/core/fs.h>
#include <lam/core/platform.h>

using namespace lam;

namespace {
// Makes test allocations observable so Release builds cannot elide them.
void *volatile g_escape = nullptr;
} // namespace

TEST_CASE("support: TempDir is unique, usable and always removed") {
    std::string kept;
    {
        test::TempDir a;
        test::TempDir b;
        CHECK(a.path() != b.path());
        CHECK(std::filesystem::is_directory(a.path()));
        CHECK(a.file("x.txt") == join_path(a.path(), "x.txt"));
        REQUIRE(write_file_atomic(a.file("x.txt"), "hello").ok());
        REQUIRE(make_directories(a.file("sub/deeper")).ok());
        REQUIRE(write_file_atomic(a.file("sub/deeper/y.txt"), "y").ok());
        kept = a.path();
    }
    CHECK_FALSE(std::filesystem::exists(kept));
}

TEST_CASE("support: tests never point LAM_DATA_DIR at the user's real folder") {
    const char *dir = std::getenv("LAM_DATA_DIR");
    REQUIRE(dir != nullptr);
    CHECK(std::string(dir).find("lam_test_") != std::string::npos);
}

TEST_CASE("support: HeadlessApp is --headless with a NullPlatform, one tick per frame") {
    test::HeadlessApp h;
    CHECK(h.app().options().headless);
    CHECK(h.app().options().deterministic());
    CHECK_FALSE(h.app().platform().has_gpu());
    h.frames(3);
    CHECK(h.app().tick() == 3);
    h.ticks(2);
    CHECK(h.app().tick() == 5);
    test::run_ticks(h.app(), 1);
    test::run_frames(h.app(), 1);
    CHECK(h.app().tick() == 7);
}

TEST_CASE("support: CaptureLog captures by level and restores the previous sink") {
    LogSink *before = log_sink();
    {
        test::CaptureLog log;
        CHECK(log_sink() == &log);
        LAM_LOG_INFO("hello {}", 42);
        LAM_LOG_WARN("careful");
        LAM_LOG_WARN("again");
        CHECK(log.contains("hello 42"));
        CHECK_FALSE(log.contains("nope"));
        CHECK(log.count(LogLevel::Warn) == 2);
        CHECK(log.count(LogLevel::Info) == 1);
        CHECK(log.text() == "hello 42\ncareful\nagain\n");
        {
            test::CaptureLog inner;
            LAM_LOG_ERROR("inner");
            CHECK(inner.contains("inner"));
        }
        CHECK(log_sink() == &log);
        CHECK_FALSE(log.contains("inner"));
        log.clear();
        CHECK(log.count(LogLevel::Warn) == 0);
    }
    CHECK(log_sink() == before);
}

TEST_CASE("support: golden_equal compares, reports, and rewrites with LAM_UPDATE_GOLDEN=1") {
    const char *was = std::getenv("LAM_UPDATE_GOLDEN");
    const std::string saved = was ? was : "";
    ::unsetenv("LAM_UPDATE_GOLDEN");
    test::TempDir dir;
    const std::string path = dir.file("golden/out.txt");
    CHECK_FALSE(test::golden_equal("a\nb\n", path)); // missing file
    ::setenv("LAM_UPDATE_GOLDEN", "1", 1);
    CHECK(test::golden_equal("a\nb\n", path)); // written
    ::unsetenv("LAM_UPDATE_GOLDEN");
    CHECK(test::golden_equal("a\nb\n", path));
    CHECK_FALSE(test::golden_equal("a\nc\n", path));
    MUST_FAIL(CHECK(test::golden_equal("different", path)));
    if (was) ::setenv("LAM_UPDATE_GOLDEN", saved.c_str(), 1);
}

TEST_CASE("support: CHECK_NO_ALLOC passes on allocation-free code and fails on allocation") {
    std::vector<int> v;
    v.reserve(16);
    CHECK_NO_ALLOC(v.push_back(1); v.push_back(2));
    CHECK(v.size() == 2);
    if (allocation_counting_enabled()) {
        MUST_FAIL(CHECK_NO_ALLOC(std::vector<int> big(100); g_escape = big.data()));
    } else {
        MESSAGE("allocation counting is off (sanitizer build): canary skipped");
    }
}

TEST_CASE("support: MUST_FAIL passes when its check fails, fails when it passes") {
    MUST_FAIL(CHECK(1 + 1 == 3));
    MUST_FAIL(CHECK_EQ(2, 3));
    // The canary of the canary: a passing inner check makes MUST_FAIL fail.
    test::detail::MustFailScope outer;
    MUST_FAIL(CHECK(true));
    CHECK(outer.end());
}
