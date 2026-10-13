#pragma once
// Scene flow. The arena is a Scene; pause and game over are engine MenuScenes pushed on top.
// ArenaScene::resumed turns the menu result into a "shooter.restart" intent, so it replays.
#include <memory>
#include <string_view>
#include <lam/core/app.h>
#include <lam/scene/scene.h>
namespace shooter_app {

class ArenaScene final : public lam::Scene {
public:
    std::string_view name() const override { return "arena"; }
    // setup_arena (rules), then the draw-only tilemap, camera, listener and HUD.
    void enter(lam::App &app, lam::World &world) override;
    // "restart" (or "back" after a game over) -> restart intent; "quit" -> App::quit.
    void resumed(lam::App &app, const lam::SceneMessage *msg) override;
};

// Input (tick stage): the pause action pushes the pause menu. Scene ops from tick stages apply
// at the next tick boundary, so pausing replays too.
void flow_pause(lam::Ctx &ctx);
// TickEnd: a GameOver event (PostPhysics, this tick) pushes the game-over menu.
void flow_game_over(lam::Ctx &ctx);
}
