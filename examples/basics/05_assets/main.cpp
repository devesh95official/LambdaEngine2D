// assets_demo: lam_assets in one file.
//  1. A custom asset type (Palette) and its ".palette.json" loader. A palette may name a "base":
//     decode declares it with LoadContext::depend, finalize merges the base colours in.
//  2. A startup system loads "day", on the worker normally and inline under --headless. Ticks
//     wait for it (AppConfig::wait_for_startup_assets).
//  3. A frame system reacts to AssetEvent<Palette>: Loaded, Modified (editing base.palette.json
//     reloads base, then day), Failed, Removed.
//  4. "night" gets its own scope at frame 20; the scope ends at frame 80 and the next
//     asset_update collects it. base survives because day retains it.
//  5. --touch rewrites a scratch copy of base.palette.json at frame 50 to show hot reload (Debug
//     builds) without touching the source tree.
//   ./assets_demo                          window; edit assets/palettes/*.palette.json while it runs
//   ./assets_demo --hidden --frames 120    the same in a hidden window, quits after 120 frames
//   ./assets_demo --touch                  shows a hot reload by itself
//   ./assets_demo --headless [--frames N]  console, deterministic: synchronous loads, no hot reload
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <lam/assets/asset_root.h>
#include <lam/assets/asset_server.h>
#include <lam/core/app.h>
#include <lam/core/data.h>
#include <lam/core/fs.h>
#include <lam/core/raylib_platform.h>

#include "draw_rl.h"

using namespace lam;

namespace {

// 1. The asset type: plain data, stored by the server, read through AssetId<Palette>.
struct Palette {
    std::string name;
    AssetId<Palette> base;                                      // optional dependency
    std::vector<std::pair<std::string, Color>> own;             // decoded
    std::vector<std::pair<std::string, Color>> colors;          // finalize: base, then own
};

class PaletteLoader final : public AssetLoader<Palette> {
public:
    std::span<const std::string_view> extensions() const override {
        static constexpr std::string_view kExtensions[] = {".palette.json"};
        return kExtensions;
    }
    // Worker thread (CPU only): no server or App access except through ctx.
    Result<Palette> decode(std::span<const std::byte> bytes, LoadContext &ctx) override {
        const std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        return parse_data<Palette>(text, ctx.path(), [&ctx](DataReader &in) {
            Palette p;
            p.name = in.string("name");
            if (in.has("base")) p.base = ctx.depend<Palette>(in.string("base")); // next to path()
            DataReader colors = in.object("colors");
            for (std::string_view key : colors.keys())
                p.own.emplace_back(std::string(key), colors.color(key));
            return p;
        });
    }
    // Main thread, every dependency Ready (a GPU upload would go here too).
    Status finalize(Palette &p, FinalizeContext &ctx) override {
        p.colors.clear();
        if (const Palette *base = ctx.server.get(p.base)) p.colors = base->colors;
        for (const auto &[name, color] : p.own) {
            bool replaced = false;
            for (auto &[have, c] : p.colors) {
                if (have == name) {
                    c = color;
                    replaced = true;
                }
            }
            if (!replaced) p.colors.emplace_back(name, color);
        }
        return {};
    }
};

// Demo state (an App resource; systems are plain functions).
struct Demo {
    bool window = false;
    std::string scratch;                                        // --touch: scratch asset root
    AssetId<Palette> day;
    AssetId<Palette> night;
    AssetId<TextAsset> notes;                                   // built-in ".txt" loader
    AssetScope night_scope = kRootScope;
    int frame = 0;
    int first_tick_frame = -1;
    int modified = 0;
    bool failed = false;
    // What the window shows; rebuilt on events only (no per-frame allocation).
    std::vector<DemoSwatch> day_swatches;
    std::vector<DemoSwatch> night_swatches;
    std::vector<DemoRow> rows;
    std::vector<std::string> lines;                             // header + recent events
    std::vector<std::string> recent;
    Color background = kBlack;
};

void log_event(Demo &demo, std::string line) {
    std::printf("[frame %d] %s\n", demo.frame, line.c_str());
    constexpr std::size_t kWindowColumns = 88; // the window keeps a short copy
    if (line.size() > kWindowColumns) line = line.substr(0, kWindowColumns - 3) + "...";
    demo.recent.push_back(std::move(line));
    if (demo.recent.size() > 8) demo.recent.erase(demo.recent.begin());
}

std::vector<DemoSwatch> swatches_of(const Palette &p) {
    std::vector<DemoSwatch> out;
    for (const auto &[name, color] : p.colors)
        out.push_back({name, color});
    return out;
}

std::string describe(const Palette &p) {
    std::string s = "'" + p.name + "':";
    char hex[16];
    for (const auto &[name, c] : p.colors) {
        std::snprintf(hex, sizeof hex, "#%02x%02x%02x", c.r, c.g, c.b);
        s += " " + name + "=" + hex;
    }
    return s;
}

// Rebuilds rows and text from the current assets (cold: called when an event arrives).
void refresh_view(Demo &demo, const AssetServer &assets) {
    demo.day_swatches.clear();
    demo.night_swatches.clear();
    demo.rows.clear();
    demo.background = kBlack;
    if (const Palette *day = assets.get(demo.day)) {
        demo.day_swatches = swatches_of(*day);
        for (const DemoSwatch &s : demo.day_swatches)
            if (s.name == "sky") demo.background = s.color;
    }
    if (const Palette *night = assets.get(demo.night)) demo.night_swatches = swatches_of(*night);
    demo.rows.push_back({"day v" + std::to_string(assets.version(demo.day)), demo.day_swatches});
    if (!demo.night_swatches.empty())
        demo.rows.push_back({"night (scene scope)", demo.night_swatches});
    demo.lines.clear();
    demo.lines.push_back(std::string("lam_assets: ") +
                         (assets.synchronous() ? "synchronous" : "asynchronous") +
                         (LAM_DEV && !assets.synchronous() ? ", hot reload on" : ""));
    if (const TextAsset *notes = assets.get(demo.notes))
        demo.lines.push_back(notes->text.substr(0, notes->text.find('\n')));
    demo.lines.push_back("asset root: see the console; edit palettes/base.palette.json there");
    for (const std::string &l : demo.recent)
        demo.lines.push_back(l);
}

// 2. Startup (root asset scope): request the assets. Returns at once in asynchronous mode.
void load_assets(Ctx &ctx) {
    AssetServer &assets = ctx.res<AssetServer>();
    Demo &demo = ctx.res<Demo>();
    demo.day = assets.load<Palette>("palettes/day.palette.json");
    demo.notes = assets.load<TextAsset>("readme.txt");
    log_event(demo, std::string("requested day (") +
                        (assets.synchronous() ? "synchronous" : "asynchronous") + "), pending " +
                        std::to_string(assets.pending()));
    refresh_view(demo, assets);
}

// 3. React to asset events (Update, Once: App-scope events are read once per frame).
void on_palette_events(Ctx &ctx) {
    AssetServer &assets = ctx.res<AssetServer>();
    Demo &demo = ctx.res<Demo>();
    bool any = false;
    for (const AssetEvent<Palette> &e : ctx.read<AssetEvent<Palette>>()) {
        any = true;
        const Palette *p = assets.get(e.id); // valid until the end of this stage
        const std::string path(assets.path_of(e.id));
        switch (e.kind) {
        case AssetEvent<Palette>::Kind::Loaded:
            log_event(demo, "Loaded " + path + (p ? " " + describe(*p) : ""));
            break;
        case AssetEvent<Palette>::Kind::Modified:
            ++demo.modified;
            log_event(demo, "Modified " + path + " v" + std::to_string(assets.version(e.id)) +
                                (p ? " " + describe(*p) : ""));
            break;
        case AssetEvent<Palette>::Kind::Failed:
            demo.failed = true;
            log_event(demo, "Failed: " + std::string(assets.error(e.id)->message));
            break;
        case AssetEvent<Palette>::Kind::Removed:
            log_event(demo, "Removed palette #" + std::to_string(e.id.index) + " (scope ended)");
            break;
        }
    }
    for (const AssetEvent<TextAsset> &e : ctx.read<AssetEvent<TextAsset>>())
        any = any || e.kind == AssetEvent<TextAsset>::Kind::Loaded;
    if (any) refresh_view(demo, assets);
}

// Checks that ticks waited for the startup assets.
void first_tick(Ctx &ctx) {
    Demo &demo = ctx.app().res<Demo>();
    if (demo.first_tick_frame >= 0) return;
    demo.first_tick_frame = demo.frame;
    const AssetServer &assets = ctx.app().res<AssetServer>();
    const bool ready = assets.state(demo.day) == LoadState::Ready;
    log_event(demo, "first tick at frame " + std::to_string(demo.frame) +
                        ", day ready: " + (ready ? "yes" : "no"));
    refresh_view(demo, assets);
}

// 5. --touch: rewrite the scratch base palette (mtime moved forward explicitly so coarse
// file-system clocks still see a change).
void touch_base(Demo &demo) {
    const std::string path = demo.scratch + "/palettes/base.palette.json";
    const std::string text = R"({"name": "base", "colors": {"ink": "#ff3366", "paper": "#202840"}})";
    if (!write_file_atomic(path, text)) return;
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    if (!ec) std::filesystem::last_write_time(path, t + std::chrono::seconds(2), ec);
    log_event(demo, "touched palettes/base.palette.json (scratch copy)");
}

// 4. and 5.: the scripted part of the run (frame stage, so it is the same in every mode).
void script(Ctx &ctx) {
    Demo &demo = ctx.res<Demo>();
    AssetServer &assets = ctx.res<AssetServer>();
    ++demo.frame;
    if (demo.frame == 20) {
        demo.night_scope = assets.begin_scope();
        demo.night = assets.load<Palette>("palettes/night.palette.json", demo.night_scope);
        log_event(demo, "night requested into scope " + std::to_string(demo.night_scope));
    }
    if (demo.frame == 50 && !demo.scratch.empty()) touch_base(demo);
    if (demo.frame == 80) {
        assets.end_scope(demo.night_scope); // collected by the next asset_update
        demo.night = {};
        log_event(demo, "night scope ended");
        refresh_view(demo, assets);
    }
    if (!demo.window && ctx.app().options().frames == 0 && demo.frame >= 120) ctx.app().quit(0);
}

void draw(Ctx &ctx) {
    const Demo &demo = ctx.res<Demo>();
    demo_draw(DemoView{demo.background, demo.rows, demo.lines});
}

// --touch: copies the asset root to a scratch folder and returns it ("" on failure).
std::string make_scratch_root() {
    const AssetRootResult source = resolve_asset_root("");
    std::error_code ec;
    const std::filesystem::path scratch =
        std::filesystem::temp_directory_path(ec) / "lam_assets_demo_scratch";
    std::filesystem::remove_all(scratch, ec);
    std::filesystem::copy(source.path, scratch, std::filesystem::copy_options::recursive, ec);
    if (ec) {
        std::printf("--touch: cannot copy %s: %s\n", source.path.c_str(), ec.message().c_str());
        return {};
    }
    return scratch.string();
}

} // namespace

int main(int argc, char **argv) {
    AppConfig config;
    config.title = "assets_demo";
    config.window_size = {960, 540};
    config.audio = false;
    config.settings_file = ""; // a demo keeps no settings
    App app(config, argc, argv);
    const bool window = !app.options().headless;
    if (window) app.add_plugin<PlatformPlugin>(); // first, so it shuts down last

    AssetsConfig assets_config;                  // root: default order (sidecar in Debug builds)
    assets_config.watch_interval_seconds = 0.1f; // hot reload polling (LAM_DEV builds)
    std::string scratch;
    if (app.options().has("--touch")) {
        scratch = make_scratch_root();
        assets_config.root = scratch;
    }
    app.add_plugin<AssetsPlugin>(assets_config);
    AssetServer &assets = app.res<AssetServer>();
    assets.register_loader<Palette>(std::make_unique<PaletteLoader>()); // AssetEvent<Palette> too
    std::printf("asset root: %s\n", assets.root().c_str());

    Demo &demo = app.insert_resource<Demo>();
    demo.window = window;
    demo.scratch = scratch;
    app.add_startup(load_assets, "demo.load_assets");
    app.add_system({.stage = Stage::Update, .fn = on_palette_events, .name = "demo.palette_events",
                    .scope = SystemScope::Once});
    app.add_system({.stage = Stage::Update, .fn = script, .name = "demo.script", .order = 10,
                    .scope = SystemScope::Once});
    app.add_system(Stage::Simulate, first_tick, "demo.first_tick");
    if (window) {
        app.add_system({.stage = Stage::Render, .fn = draw, .name = "demo.draw",
                        .scope = SystemScope::Once});
    }
    const int code = app.run();

    if (!scratch.empty()) {
        std::printf("hot reload observed: %s%s\n", demo.modified > 0 ? "yes" : "no",
                    LAM_DEV ? "" : " (compiled out of Release builds)");
        std::error_code ec;
        std::filesystem::remove_all(scratch, ec);
    }
    std::printf("done after %d frames, pending = %zu\n", demo.frame, assets.pending());
    return demo.failed ? 1 : code;
}
