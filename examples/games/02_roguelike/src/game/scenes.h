#pragma once
// Scenes and game flow. GameScene owns one run in its own World (the SceneStack creates and
// destroys it); death pushes the engine's MenuScene as an overlay (permadeath: the save is
// erased first), and its "new_run" / "quit" choice comes back to GameScene::resumed.
#include <cstdint>
#include <lam/core/app.h>
#include <lam/input/input_frame.h>
#include <lam/scene/save.h>
#include <lam/scene/scene.h>
#include <optional>
#include <string_view>

namespace game {
class GameScene final : public lam::Scene {
public:
    explicit GameScene(std::uint64_t seed) : seed_(seed) {}
    std::string_view name() const override { return "game"; }
    void enter(lam::App &app, lam::World &world) override; // rl::setup_run + build_view
    void exit(lam::App &app, lam::World &world) override;
    void resumed(lam::App &app, const lam::SceneMessage *msg) override; // death menu choice
private:
    std::uint64_t seed_;
};

// The intent that picks "New run" in the game-over menu (engine MenuScene: kMenuSelectIntent, a =
// item index); the bot sends it to start over after a death.
lam::Intent new_run_intent();

// The single save slot of the game, in App::data_dir() (user_data_dir("Roguelike"), or
// LAM_DATA_DIR). Empty under --headless without LAM_DATA_DIR: headless bots and tests never read,
// write or erase the player's save.
std::optional<lam::SaveSlots> save_slots(const lam::App &app);

// Tick systems on the run's world (registered by ViewPlugin):
void load_intent(lam::Ctx &ctx); // Input: kLoadGame -> load slot 1
void watch_death(lam::Ctx &ctx); // PostPhysics: permadeath + menu
} // namespace game
