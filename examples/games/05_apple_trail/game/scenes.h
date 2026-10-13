#pragma once
// The game's phases (title, playing, paused, over) as a scene stack (lam/scene/scene.h):
//   [Play]                  playing
//   [Play, Title overlay]   title: the board shows through, the play world does not tick
//   [Play, Pause overlay]   paused
//   [Play, Over overlay]    bump! -> confirm resets the stack to a fresh [Play] (a new world, so
//                           a new seed derivation and an empty entity table per run)
// Each overlay has one Simulate system (confirm/pause) and one RenderUI system. Scene changes come
// from tick systems, so replays reproduce the menus too.
#include <string_view>
#include <lam/core/app.h>
#include <lam/scene/scene.h>

namespace game {

class PlayScene final : public lam::Scene {
public:
    std::string_view name() const override { return "play"; }
    void enter(lam::App &app, lam::World &world) override;      // trail::start_run + dress_run
};

class TitleScene final : public lam::Scene {
public:
    std::string_view name() const override { return "title"; }
    void enter(lam::App &app, lam::World &world) override;
    bool overlay() const override { return true; }
};

class PauseScene final : public lam::Scene {
public:
    std::string_view name() const override { return "pause"; }
    void enter(lam::App &app, lam::World &world) override;
    bool overlay() const override { return true; }
};

class OverScene final : public lam::Scene {
public:
    std::string_view name() const override { return "over"; }
    void enter(lam::App &app, lam::World &world) override;
    bool overlay() const override { return true; }
};

// Play-world flow systems (pause request, bump -> OverScene, session numbers, state hash), added
// once and gated with run_if = has_local<TrailState>, so a fresh PlayScene needs no re-registration.
class FlowPlugin final : public lam::Plugin {
public:
    std::string_view name() const override { return "game.apple_trail.flow"; }
    void build(lam::App &app) override;
};

} // namespace game
