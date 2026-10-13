#pragma once
// Pathar's screens as lam::Scene objects on the SceneStack. Each scene owns a World, so a
// level is a fresh world: replacing the play scene is how levels change.
//   TitleScene   a demo board solving itself (quiet) under the title menu
//   PlayScene    one level; its flow system pushes the pause and solved menus
//   menus        engine MenuScenes built from MenuSpec (title, pause, solved, level select);
//                settings and text pages are the engine's own make_settings_scene/make_text_scene.
// Menus pop with SceneMessage{kind = item id, value = item value}; the scene below acts on it in
// resumed(). Anything that changes the puzzle goes back to the simulation as an intent.
#include <memory>
#include <span>
#include <lam/input/driver.h>
#include <lam/scene/scene.h>
#include "sim/bot.h"

namespace pathar {

std::unique_ptr<lam::Scene> make_title_scene();
std::unique_ptr<lam::Scene> make_play_scene(int level);

// --bot: the SolverBot plays every puzzle. When a menu is focused this driver picks what a player
// would ("Next level"; after the last level "Choose a level", the first chapter, level 1) by
// pushing lam::kMenuSelectIntent as a click does, so a replay of the run needs no bot.
// undo_every > 0 (--bot-undo N): after every N solver steps it presses Undo instead, and every
// third time Restart then Undo, to exercise History with the presentation alive. The solver
// replans from the restored position, so every level still ends at par.
class PatharBot final : public lam::InputDriver {
public:
    explicit PatharBot(Controls controls, int undo_every = 0)
        : controls_(controls), solver_(controls), undo_every_(undo_every) {}
    void fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) override;
    int menu_picks() const { return menu_picks_; }
    int undos() const { return undos_; }

private:
    Controls controls_;
    SolverBot solver_;
    int undo_every_ = 0;
    int last_undo_at_ = 0;                 // solver_.presses() at the last undo
    int undos_ = 0;
    bool undo_next_ = false;               // a restart was pressed: undo it on the next tick
    int menu_picks_ = 0;
};

}  // namespace pathar
