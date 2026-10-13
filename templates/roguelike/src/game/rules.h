#pragma once
// Roguelike simulation (roguelike_sim): state, the rules plugin and the bot. Rule state is
// reflected at the bottom so prefabs, saves, undo and the state hash see it.
#include <cstdint>
#include <span>
#include <lam/core/app.h>
#include <lam/core/fixed_string.h>
#include <lam/ecs/component.h>
#include <lam/input/driver.h>
#include <lam/tilemap/grid_maps.h>

namespace game {
// ---- Components ----------------------------------------------------------------------------------
struct Player {};     // tag: the hero (prefab "player" also has the engine's PlayerTurn tag)
struct FloorLocal {}; // tag: destroyed when a new floor is built (monsters; TODO: items, doors)
struct Stats {        // TODO(rules): defense, xp, level... add the field here and in LAM_REFLECT
    std::int16_t hp = 10, max_hp = 10, attack = 2;
};

// ---- World resource: one run (start_run registers it with snapshot_resource) ---------------------
enum class Phase : std::uint8_t { Playing, Dead };
struct RunState {
    std::uint64_t seed = 1; // floor layout = f(seed, depth), so it is rebuilt, never saved
    std::int32_t depth = 1, kills = 0, score = 0;
    Phase phase = Phase::Playing;
    lam::Entity player;
    lam::IVec2 stairs;             // stepping here goes one floor deeper
    lam::FixedString<48> message;  // the last thing that happened (the HUD shows it)
};

// ---- Event: presentation turns it into sparks and shake --------------------------------------------
struct Hit {
    lam::IVec2 cell;
    bool on_player = false, killed = false;
};

// ---- App resource, built by RulesPlugin from the ActionMap -----------------------------------------
struct Controls {
    lam::ButtonAction up, down, left, right, wait; // wait also starts a new run after death
};

// Add after DefaultPlugins: needs lam.input and lam.tilemap.
class RulesPlugin final : public lam::Plugin {
public:
    std::string_view name() const override { return "game.rules"; }
    void build(lam::App &app) override;
    void init_world(lam::App &app, lam::World &world) override; // restore hook (undo / load)
};

// New run on `w`: RunState, FovMemory, TurnScheduler, the "player" prefab, then floor 1. Needs
// prefabs.json loaded.
void start_run(lam::World &w, std::uint64_t seed);

// Fog of war queries for presentation and the bot.
bool cell_visible(const lam::World &w, lam::IVec2 cell); // in the hero's view this turn
bool cell_explored(const lam::World &w, lam::IVec2 cell);

// Autoplay for --bot and the test. Presses the same buttons a player would.
class BotDriver final : public lam::InputDriver {
public:
    void fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) override;

private:
    lam::DijkstraMap map_; // scratch distance field toward the stairs
};
} // namespace game

LAM_REFLECT_TAG(game::Player)
LAM_REFLECT_TAG(game::FloorLocal)
LAM_REFLECT(game::Stats, hp, max_hp, attack)
LAM_REFLECT_ENUM(game::Phase, "playing", "dead")
LAM_REFLECT(game::RunState, seed, depth, kills, score, phase, player, stairs, message)
