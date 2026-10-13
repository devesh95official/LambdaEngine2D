#pragma once
// Roguelike presentation (executable only). Changes the simulation only through queue_intent.
//   Update    +0  view_sync     monster sprites hidden outside the FOV, floor items until explored
//   Update    +5  camera_frame  CameraBounds from the map so the HUD never covers the hero
//   Update   +10  hud_update    status line and pack buttons (UiTree::set_text, no allocation)
//   Update   +20  hud_input     pack clicks / number keys -> kUseItem intents, F5 save,
//                               F9 -> kLoadGame intent
//   Update   +30  hit_fx        rl::Hit events -> BurstAt sparks + CameraShake
//   RenderUI  +0  draw_log      MessageLog lines with their colours (draw_text over the log panel)
// plus two game-flow tick systems on the run's world:
//   Input        +0  load_intent  kLoadGame intent -> SaveSlots::load -> rl::load_envelope
//   PostPhysics  +0  watch_death  permadeath: erase the save, push the game-over menu
#include <cstdint>
#include <lam/core/app.h>
#include <lam/core/asset_id.h>
#include <lam/ecs/entity.h>
#include <lam/ui/ui.h>
#include <string_view>

namespace lam {
struct EmitterConfig;
}

namespace game {
// App resource: which world holds the current run (for the headless summary) and how many runs
// were started (new runs derive their seed from it).
struct RunTracker {
    lam::World *world = nullptr;
    std::uint64_t base_seed = 1;
    std::uint32_t runs = 0;
};

// World resource of the run's world (presentation only, never saved): HUD node ids.
struct Hud {
    lam::UiId status = 0, pack_title = 0, log_panel = 0;
    lam::UiId slots[8] = {};
    bool game_over_shown = false; // the death menu was pushed
};

class ViewPlugin final : public lam::Plugin {
public:
    std::string_view name() const override { return "game.roguelike.view"; }
    void build(lam::App &app) override; // FxAssets, RunTracker, systems
};

// Camera, fog style and HUD for a freshly set-up run (GameScene::enter, after rl::setup_run).
void build_view(lam::App &app, lam::World &world);
} // namespace game
