// lam_test_support: the doctest main and the harness from <lam/test/support.h>.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <lam/test/support.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>

#include <lam/core/fs.h>

namespace lam::test {
namespace {

// Point LAM_DATA_DIR at a per-process temp folder (unless already set) so tests never touch the
// user's settings or saves.
struct TestEnvironment {
    TempDir data;
    TestEnvironment() {
        if (const char *v = std::getenv("LAM_DATA_DIR"); !v || !*v)
            ::setenv("LAM_DATA_DIR", data.path().c_str(), 1);
    }
};
const TestEnvironment g_environment;

std::string make_temp_dir() {
    std::error_code ec;
    std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec || base.empty()) base = "/tmp";
    std::string pattern = (base / "lam_test_XXXXXX").string();
    if (::mkdtemp(pattern.data())) return pattern;
    // Fallback: pid + counter (mkdtemp unavailable or failed).
    static std::atomic<unsigned> counter{0};
    for (;;) {
        const std::filesystem::path p =
            base / ("lam_test_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
        if (std::filesystem::create_directory(p, ec)) return p.string();
        if (ec) return p.string(); // give up: callers see IO errors
    }
}

} // namespace

// ---- TempDir -----------------------------------------------------------------------------------

TempDir::TempDir() : path_(make_temp_dir()) {}

TempDir::~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
}

const std::string &TempDir::path() const {
    return path_;
}

std::string TempDir::file(std::string_view name) const {
    return join_path(path_, name);
}

// ---- HeadlessApp -------------------------------------------------------------------------------

HeadlessApp::HeadlessApp(AppConfig config) {
    static const char *const kArgv[] = {"lam_test", "--headless"};
    app_ = std::make_unique<App>(std::move(config), 2, kArgv);
}

App &HeadlessApp::app() {
    return *app_;
}

void HeadlessApp::frames(int n) {
    app_->run_frames(n);
}

void HeadlessApp::ticks(int n) {
    app_->step_ticks(n);
}

void run_ticks(App &app, int n) {
    app.step_ticks(n);
}

void run_frames(App &app, int n) {
    app.run_frames(n);
}

// ---- CaptureLog --------------------------------------------------------------------------------

CaptureLog::CaptureLog() : previous_(log_sink()) {
    lines_.reserve(64);
    set_log_sink(this);
}

CaptureLog::~CaptureLog() {
    set_log_sink(previous_);
}

void CaptureLog::write(LogLevel level, std::string_view msg) {
    const std::lock_guard<std::mutex> lock(mutex_);
    lines_.push_back({level, std::string(msg)});
}

bool CaptureLog::contains(std::string_view text) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (const Line &l : lines_)
        if (l.text.find(text) != std::string::npos) return true;
    return false;
}

std::size_t CaptureLog::count(LogLevel level) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::size_t n = 0;
    for (const Line &l : lines_)
        if (l.level == level) ++n;
    return n;
}

std::string CaptureLog::text() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::string out;
    for (const Line &l : lines_) {
        out += l.text;
        out += '\n';
    }
    return out;
}

void CaptureLog::clear() {
    const std::lock_guard<std::mutex> lock(mutex_);
    lines_.clear();
}

// ---- golden_equal ------------------------------------------------------------------------------

bool golden_equal(std::string_view actual, const std::string &golden_path) {
    std::string path = golden_path;
    if (!path.empty() && path.front() != '/') {
        if (const char *src = std::getenv("LAM_TEST_SOURCE_DIR"); src && *src)
            path = join_path(src, path);
    }
    if (const char *update = std::getenv("LAM_UPDATE_GOLDEN");
        update && std::string_view(update) == "1") {
        if (Status s = make_directories(parent_path(path)); !s) {
            std::fprintf(stderr, "golden: %s\n", s.error().message.c_str());
            return false;
        }
        if (Status s = write_file_atomic(path, actual); !s) {
            std::fprintf(stderr, "golden: %s\n", s.error().message.c_str());
            return false;
        }
        std::fprintf(stderr, "golden updated: %s\n", path.c_str());
        return true;
    }
    Result<std::string> expected = read_file(path);
    if (!expected) {
        std::fprintf(stderr, "golden missing: %s (run with LAM_UPDATE_GOLDEN=1 to create it)\n",
                     path.c_str());
        return false;
    }
    if (*expected == actual) return true;
    // Report the first differing line (1-based).
    const std::string_view e = *expected;
    std::size_t line = 1, start = 0, i = 0;
    while (i < e.size() && i < actual.size() && e[i] == actual[i]) {
        if (e[i] == '\n') {
            ++line;
            start = i + 1;
        }
        ++i;
    }
    auto line_at = [start](std::string_view s) {
        if (start >= s.size()) return std::string_view("<end of text>");
        const std::size_t end = s.find('\n', start);
        return s.substr(start,
                        end == std::string_view::npos ? std::string_view::npos : end - start);
    };
    const std::string_view el = line_at(e), al = line_at(actual);
    std::fprintf(stderr, "golden mismatch: %s, line %zu\n  expected: %.*s\n  actual:   %.*s\n",
                 path.c_str(), line, static_cast<int>(el.size()), el.data(),
                 static_cast<int>(al.size()), al.data());
    return false;
}

// ---- MUST_FAIL ---------------------------------------------------------------------------------

namespace detail {
namespace {
// Swallows everything; counts failed assertions.
struct CaptureReporter final : doctest::IReporter {
    int failed = 0;
    void report_query(const doctest::QueryData &) override {}
    void test_run_start() override {}
    void test_run_end(const doctest::TestRunStats &) override {}
    void test_case_start(const doctest::TestCaseData &) override {}
    void test_case_reenter(const doctest::TestCaseData &) override {}
    void test_case_end(const doctest::CurrentTestCaseStats &) override {}
    void test_case_exception(const doctest::TestCaseException &) override {}
    void subcase_start(const doctest::SubcaseSignature &) override {}
    void subcase_end() override {}
    void log_assert(const doctest::AssertData &ad) override {
        if (ad.m_failed) ++failed;
    }
    void log_message(const doctest::MessageData &) override {}
    void test_case_skipped(const doctest::TestCaseData &) override {}
};
} // namespace

struct MustFailScope::State {
    CaptureReporter reporter;
    std::vector<doctest::IReporter *> saved;
    int failed_before = 0;
    bool active = false;
};

MustFailScope::MustFailScope() : state_(std::make_unique<State>()) {
    doctest::detail::ContextState *cs = doctest::detail::g_cs;
    if (!cs) return;
    state_->saved = cs->reporters_currently_used;
    cs->reporters_currently_used.assign(1, &state_->reporter);
    state_->failed_before = cs->numAssertsFailedCurrentTest_atomic;
    state_->active = true;
}

MustFailScope::~MustFailScope() {
    end();
}

bool MustFailScope::end() {
    State &st = *state_;
    if (st.active) {
        st.active = false;
        doctest::detail::ContextState *cs = doctest::detail::g_cs;
        cs->reporters_currently_used = st.saved;
        const int added = cs->numAssertsFailedCurrentTest_atomic - st.failed_before;
        if (added > 0) cs->numAssertsFailedCurrentTest_atomic.fetch_sub(added);
    }
    return st.reporter.failed > 0;
}

} // namespace detail
} // namespace lam::test
