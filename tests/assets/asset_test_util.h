#pragma once
// lam_assets fixtures: four asset types sharing one scriptable loader. One directive per line:
//   value: <text>              the asset's value
//   node: | root: | mid: | leaf: <relative path>   LoadContext::depend<Node|RootDoc|MidDoc|LeafDoc>
//   read: <relative path>      LoadContext::read into Doc::extra
//   fail: decode | finalize    decode or finalize returns an error
//   gate                       decode waits until g_gate opens
//   sleep: <ms>                decode sleeps
//   slowfin: <ms>              finalize sleeps (finalize budget tests)
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <lam/assets/asset_server.h>
#include <lam/core/app.h>
#include <lam/core/fs.h>
#include <lam/test/support.h>

namespace assets_test {

inline std::atomic<int> g_live{0};                       // Doc objects alive (leak checks)
inline std::atomic<int> g_off_main_decodes{0};           // decodes that ran off the main thread
inline std::thread::id g_main_thread = std::this_thread::get_id();

template <int Kind> struct Doc {
    std::string value;
    std::vector<std::uint64_t> deps;                     // bits of each declared dependency
    std::string extra;                                   // LoadContext::read content
    bool fail_finalize = false;
    int finalize_sleep_ms = 0;
    int finalized = 0;
    Doc() { ++g_live; }
    Doc(const Doc &o)
        : value(o.value), deps(o.deps), extra(o.extra), fail_finalize(o.fail_finalize),
          finalize_sleep_ms(o.finalize_sleep_ms), finalized(o.finalized) {
        ++g_live;
    }
    Doc(Doc &&o) noexcept
        : value(std::move(o.value)), deps(std::move(o.deps)), extra(std::move(o.extra)),
          fail_finalize(o.fail_finalize), finalize_sleep_ms(o.finalize_sleep_ms),
          finalized(o.finalized) {
        ++g_live;
    }
    Doc &operator=(const Doc &) = default;
    Doc &operator=(Doc &&) noexcept = default;
    ~Doc() { --g_live; }
};
using Node = Doc<0>;
using RootDoc = Doc<1>;
using MidDoc = Doc<2>;
using LeafDoc = Doc<3>;

// Loader calls: "D:<path>" (decode, any thread), "F:<path>" (finalize), "U:<path>" (unload).
struct Journal {
    mutable std::mutex mutex;
    std::vector<std::string> lines;
    void add(std::string s) {
        const std::lock_guard<std::mutex> lock(mutex);
        lines.push_back(std::move(s));
    }
    std::vector<std::string> with(char kind) const {
        const std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::string> out;
        for (const std::string &l : lines)
            if (!l.empty() && l[0] == kind) out.push_back(l.substr(2));
        return out;
    }
    std::size_t count(char kind) const { return with(kind).size(); }
    void clear() {
        const std::lock_guard<std::mutex> lock(mutex);
        lines.clear();
    }
};
inline Journal g_journal;

// Decodes of files with a "gate" line block in pass() while the gate is closed.
struct Gate {
    std::mutex mutex;
    std::condition_variable cv;
    bool closed = false;
    int waiting = 0;
    void close() {
        const std::lock_guard<std::mutex> lock(mutex);
        closed = true;
    }
    void open() {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            closed = false;
        }
        cv.notify_all();
    }
    void pass() {
        std::unique_lock<std::mutex> lock(mutex);
        ++waiting;
        cv.notify_all();
        cv.wait(lock, [&] { return !closed; });
        --waiting;
    }
    // True once `n` decodes wait at the gate (within timeout).
    bool wait_waiting(int n, int timeout_ms = 5000) {
        std::unique_lock<std::mutex> lock(mutex);
        return cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                           [&] { return waiting >= n; });
    }
};
inline Gate g_gate;
// Opens the gate when it goes out of scope; declare it after the App so it runs first.
struct GateGuard {
    GateGuard() { g_gate.close(); }
    ~GateGuard() { g_gate.open(); }
};

inline constexpr std::string_view kNodeExt[] = {".node"};
inline constexpr std::string_view kRootExt[] = {".root"};
inline constexpr std::string_view kMidExt[] = {".mid"};
inline constexpr std::string_view kLeafExt[] = {".leaf"};

template <int Kind> class DocLoader final : public lam::AssetLoader<Doc<Kind>> {
public:
    DocLoader(std::span<const std::string_view> extensions, std::string tag = {})
        : extensions_(extensions), tag_(std::move(tag)) {}
    std::span<const std::string_view> extensions() const override { return extensions_; }
    lam::Result<Doc<Kind>> decode(std::span<const std::byte> bytes, lam::LoadContext &ctx) override {
        if (std::this_thread::get_id() != g_main_thread) ++g_off_main_decodes;
        g_journal.add("D:" + std::string(ctx.path()));
        const std::string text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        Doc<Kind> d;
        std::string_view rest = text;
        while (!rest.empty()) {
            const std::size_t nl = rest.find('\n');
            std::string_view line = rest.substr(0, nl);
            rest = nl == std::string_view::npos ? std::string_view{} : rest.substr(nl + 1);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            const auto arg = [&](std::string_view key) -> std::string_view {
                return line.substr(key.size());
            };
            if (line.starts_with("value: ")) {
                d.value = tag_ + std::string(arg("value: "));
            } else if (line.starts_with("node: ")) {
                d.deps.push_back(ctx.template depend<Node>(arg("node: ")).bits());
            } else if (line.starts_with("root: ")) {
                d.deps.push_back(ctx.template depend<RootDoc>(arg("root: ")).bits());
            } else if (line.starts_with("mid: ")) {
                d.deps.push_back(ctx.template depend<MidDoc>(arg("mid: ")).bits());
            } else if (line.starts_with("leaf: ")) {
                d.deps.push_back(ctx.template depend<LeafDoc>(arg("leaf: ")).bits());
            } else if (line.starts_with("read: ")) {
                lam::Result<std::vector<std::byte>> r = ctx.read(arg("read: "));
                if (!r) return r.error();
                d.extra.assign(reinterpret_cast<const char *>(r->data()), r->size());
            } else if (line == "fail: decode") {
                return lam::make_error(lam::ErrorCode::Parse, "bad content");
            } else if (line == "fail: named") {                 // a parser that names its file
                return lam::make_error(lam::ErrorCode::Parse, std::string(ctx.path()) + ": bad content");
            } else if (line == "fail: finalize") {
                d.fail_finalize = true;
            } else if (line == "gate") {
                g_gate.pass();
            } else if (line.starts_with("sleep: ")) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(std::stoi(std::string(arg("sleep: ")))));
            } else if (line.starts_with("slowfin: ")) {
                d.finalize_sleep_ms = std::stoi(std::string(arg("slowfin: ")));
            }
        }
        return d;
    }
    lam::Status finalize(Doc<Kind> &d, lam::FinalizeContext &ctx) override {
        g_journal.add("F:" + std::string(ctx.path));
        if (d.finalize_sleep_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(d.finalize_sleep_ms));
        if (d.fail_finalize) return lam::make_error(lam::ErrorCode::Invalid, "finalize refused");
        ++d.finalized;
        return {};
    }
    void unload(Doc<Kind> &, lam::FinalizeContext &ctx) override {
        g_journal.add("U:" + std::string(ctx.path));
    }

private:
    std::span<const std::string_view> extensions_;
    std::string tag_;
};

inline void add_doc_loaders(lam::AssetServer &s) {
    s.register_loader<Node>(std::make_unique<DocLoader<0>>(kNodeExt));
    s.register_loader<RootDoc>(std::make_unique<DocLoader<1>>(kRootExt));
    s.register_loader<MidDoc>(std::make_unique<DocLoader<2>>(kMidExt));
    s.register_loader<LeafDoc>(std::make_unique<DocLoader<3>>(kLeafExt));
}

// Writes `content` to dir/rel (creating folders). Returns the absolute path.
inline std::string write(const lam::test::TempDir &dir, std::string_view rel,
                         std::string_view content) {
    const std::string path = dir.file(rel);
    REQUIRE(lam::make_directories(lam::parent_path(path)).ok());
    REQUIRE(lam::write_file_atomic(path, content).ok());
    return path;
}
// Writes dir/rel (folder must exist) without fsync. F_FULLFSYNC is too slow for bulk fixtures.
inline void write_fast(const lam::test::TempDir &dir, std::string_view rel,
                       std::string_view content) {
    std::ofstream out(dir.file(rel), std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    REQUIRE(out.good());
}
// Rewrites dir/rel and moves its mtime `seconds` past the previous one, so the change is seen
// even on file systems with coarse timestamps.
inline void rewrite(const lam::test::TempDir &dir, std::string_view rel, std::string_view content,
                    int seconds = 10) {
    const std::filesystem::path p(dir.file(rel));
    std::error_code ec;
    const auto before = std::filesystem::last_write_time(p, ec);
    write(dir, rel, content);
    if (!ec) std::filesystem::last_write_time(p, before + std::chrono::seconds(seconds), ec);
}

inline lam::AssetsConfig config_for(const lam::test::TempDir &dir) {
    lam::AssetsConfig c;
    c.root = dir.path();
    c.hot_reload = false;
    return c;
}

// An App that is not deterministic (no --headless): asynchronous assets, NullPlatform.
inline std::unique_ptr<lam::App> make_async_app() {
    lam::AppConfig c;
    c.settings_file = "";
    return std::make_unique<lam::App>(std::move(c));
}

// Asynchronous App with AssetsPlugin rooted at a fresh TempDir and the Doc loaders registered.
struct AsyncFixture {
    lam::test::TempDir dir;
    std::unique_ptr<lam::App> app = make_async_app();
    lam::AssetServer *server = nullptr;
    explicit AsyncFixture(lam::AssetsConfig c = {}) {
        g_journal.clear();
        if (c.root.empty()) c.root = dir.path();
        app->add_plugin<lam::AssetsPlugin>(c);
        server = &app->res<lam::AssetServer>();
        add_doc_loaders(*server);
    }
    lam::AssetServer &s() { return *server; }
};

// Asynchronous mode without hot reload and with the given finalize budget.
inline lam::AssetsConfig async_config(float budget_ms = 4.0f) {
    lam::AssetsConfig c;
    c.hot_reload = false;
    c.finalize_budget_ms = budget_ms;
    return c;
}

// A synchronous (--headless) App with AssetsPlugin rooted at a fresh TempDir and the Doc loaders.
struct SyncFixture {
    lam::test::TempDir dir;
    lam::test::HeadlessApp h;
    lam::AssetServer *server = nullptr;
    SyncFixture() {
        g_journal.clear();
        h.add<lam::AssetsPlugin>(config_for(dir));
        server = &h.app().res<lam::AssetServer>();
        add_doc_loaders(*server);
    }
    lam::AssetServer &s() { return *server; }
};

// Blocks (without update()) until the worker has decoded `n` files in total, or 5 s passed.
inline bool wait_decodes(std::size_t n, int timeout_ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (g_journal.count('D') < n) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

// Calls server.update() until pred() holds (true) or timeout (false).
template <class Pred> bool pump_until(lam::AssetServer &server, Pred pred, int timeout_ms = 5000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        server.update();
        if (pred()) return true;
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// Every event of type T still retained on the App bus, as (kind, id).
template <class T>
std::vector<std::pair<typename lam::AssetEvent<T>::Kind, lam::AssetId<T>>> events(lam::App &app) {
    std::vector<std::pair<typename lam::AssetEvent<T>::Kind, lam::AssetId<T>>> out;
    if (auto *ch = app.events().try_get<lam::AssetEvent<T>>())
        for (const lam::AssetEvent<T> &e : ch->all())
            out.emplace_back(e.kind, e.id);
    return out;
}

} // namespace assets_test
