// Asset root resolution: config, LAM_ASSET_ROOT, the <exe>.assetroot sidecar, <exe_dir>/assets,
// <exe_dir>/../Resources/assets, then the fallback.
#include <cstdlib>
#include <optional>
#include <string>

#include <lam/assets/asset_root.h>
#include <lam/assets/asset_server.h>
#include <lam/core/fs.h>
#include <lam/test/support.h>

#include "asset_test_util.h"

using namespace lam;
using namespace assets_test;

namespace {

// Sets or clears LAM_ASSET_ROOT for one test and restores the previous value.
class EnvRoot {
public:
    explicit EnvRoot(const char *value) {
        if (const char *old = std::getenv("LAM_ASSET_ROOT")) old_ = old;
        if (value != nullptr) {
            ::setenv("LAM_ASSET_ROOT", value, 1);
        } else {
            ::unsetenv("LAM_ASSET_ROOT");
        }
    }
    ~EnvRoot() {
        if (old_) {
            ::setenv("LAM_ASSET_ROOT", old_->c_str(), 1);
        } else {
            ::unsetenv("LAM_ASSET_ROOT");
        }
    }
    EnvRoot(const EnvRoot &) = delete;
    EnvRoot &operator=(const EnvRoot &) = delete;

private:
    std::optional<std::string> old_;
};

} // namespace

TEST_CASE("root: the resolution order with explicit inputs") {
    test::TempDir t;
    const std::string exe = t.file("Game.app/Contents/MacOS");
    const std::string beside = exe + "/assets";
    const std::string bundle = exe + "/../Resources/assets";
    const std::string source = t.file("src/assets");
    REQUIRE(make_directories(exe).ok());
    REQUIRE(make_directories(source).ok());

    AssetRootResult r = resolve_asset_root("", "", exe, "game", true);
    CHECK(r.source == AssetRootSource::Fallback);
    CHECK(r.path == beside);

    REQUIRE(make_directories(t.file("Game.app/Contents/Resources/assets")).ok());
    r = resolve_asset_root("", "", exe, "game", true);
    CHECK(r.source == AssetRootSource::Bundle);
    CHECK(r.path == bundle);

    REQUIRE(make_directories(beside).ok());
    r = resolve_asset_root("", "", exe, "game", true);
    CHECK(r.source == AssetRootSource::BesideExecutable);
    CHECK(r.path == beside);

    REQUIRE(write_file_atomic(exe + "/game.assetroot", "  " + source + "\n").ok());
    r = resolve_asset_root("", "", exe, "game", true);
    CHECK(r.source == AssetRootSource::Sidecar);
    CHECK(r.path == source);
    // Release builds skip the sidecar step; other executables have their own sidecar.
    CHECK(resolve_asset_root("", "", exe, "game", false).source == AssetRootSource::BesideExecutable);
    CHECK(resolve_asset_root("", "", exe, "tool", true).source == AssetRootSource::BesideExecutable);

    r = resolve_asset_root("", "/env/root", exe, "game", true);
    CHECK(r.source == AssetRootSource::Environment);
    CHECK(r.path == "/env/root");
    r = resolve_asset_root("cfg/root", "/env/root", exe, "game", true);
    CHECK(r.source == AssetRootSource::Config);
    CHECK(r.path == "cfg/root");
}

TEST_CASE("root: empty or dangling sidecars are skipped; an unknown exe dir falls back") {
    test::TempDir t;
    const std::string exe = t.file("bin");
    REQUIRE(make_directories(exe + "/assets").ok());
    REQUIRE(write_file_atomic(exe + "/game.assetroot", "").ok()); // Release builds write this
    CHECK(resolve_asset_root("", "", exe, "game", true).source == AssetRootSource::BesideExecutable);
    REQUIRE(write_file_atomic(exe + "/game.assetroot", t.file("gone")).ok());
    CHECK(resolve_asset_root("", "", exe, "game", true).source == AssetRootSource::BesideExecutable);
    const AssetRootResult r = resolve_asset_root("", "", "", "", true);
    CHECK(r.source == AssetRootSource::Fallback);
    CHECK(r.path == "assets");
}

TEST_CASE("root: source names") {
    CHECK(asset_root_source_name(AssetRootSource::Config) == "config");
    CHECK(asset_root_source_name(AssetRootSource::Environment) == "env");
    CHECK(asset_root_source_name(AssetRootSource::Sidecar) == "sidecar");
    CHECK(asset_root_source_name(AssetRootSource::BesideExecutable) == "exe");
    CHECK(asset_root_source_name(AssetRootSource::Bundle) == "bundle");
    CHECK(asset_root_source_name(AssetRootSource::Fallback) == "fallback");
}

TEST_CASE("root: AssetServer resolves its root once: config, then LAM_ASSET_ROOT, then the exe") {
    test::TempDir env_dir;
    test::TempDir cfg_dir;
    write(env_dir, "a.txt", "from env");
    write(cfg_dir, "a.txt", "from config");
    {
        const EnvRoot env(env_dir.path().c_str());
        test::HeadlessApp h;
        h.add<AssetsPlugin>(AssetsConfig{});
        AssetServer &s = h.app().res<AssetServer>();
        CHECK(s.root() == env_dir.path());
        CHECK(s.get(s.load<TextAsset>("a.txt"))->text == "from env");
        CHECK(s.read_text("a.txt").value() == "from env");
        CHECK(s.resolve_path("x/y.png") == env_dir.path() + "/x/y.png");
    }
    {
        const EnvRoot env(env_dir.path().c_str());
        test::HeadlessApp h;
        h.add<AssetsPlugin>(config_for(cfg_dir));
        AssetServer &s = h.app().res<AssetServer>();
        CHECK(s.root() == cfg_dir.path());
        CHECK(s.get(s.load<TextAsset>("a.txt"))->text == "from config");
    }
    {
        const EnvRoot env(nullptr);
        test::HeadlessApp h;
        h.add<AssetsPlugin>(AssetsConfig{});
        const std::string &root = h.app().res<AssetServer>().root();
        const AssetRootResult expected = resolve_asset_root("");
        CHECK(root == expected.path);
        CHECK(root.size() >= 6);
        CHECK(root.substr(root.size() - 6) == "assets");
    }
}
