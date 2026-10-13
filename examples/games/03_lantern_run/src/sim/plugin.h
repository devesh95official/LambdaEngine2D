#pragma once
// The rules plugin; nothing here draws. Add it after DefaultPlugins (or a test's subset). Without
// prefabs, physics or input it falls back to bare entities.
#include <string>
#include <string_view>
#include <utility>
#include <lam/core/app.h>

namespace lantern {

struct SimOptions {                                             // App resource
    std::string level_path = "levels/forest.txt";               // through AssetServer (root-relative)
    std::string level_text;                                     // used instead of level_path when set
    bool start_playing = false;                                 // skip the title (bots, tests)
};

class LanternRunSimPlugin final : public lam::Plugin {
public:
    explicit LanternRunSimPlugin(SimOptions options = {}) : options_(std::move(options)) {}
    std::string_view name() const override { return "lantern_run.sim"; }
    // Components, events, Tuning + SimOptions resources, the startup and rule systems.
    void build(lam::App &app) override;
    // RunState (snapshot + hash + save), restore hook.
    void init_world(lam::App &app, lam::World &world) override;
private:
    SimOptions options_;
};

} // namespace lantern
