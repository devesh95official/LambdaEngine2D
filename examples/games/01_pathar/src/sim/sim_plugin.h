#pragma once
// Pathar's simulation plugin: input actions, the intents UI may send, the puzzle event, and the
// two tick systems that run on every world holding a Board (run_if = has_local<Board>):
//   puzzle_input  (Input)    InputFrame -> buffered direction / walk route / undo / restart / hint
//   puzzle_move   (Simulate) when nothing glides: one step(), then PlaySound/BurstAt/PuzzleEvent
// Effects go out as PlaySound and BurstAt; presentation reads PuzzleEvent for toasts and stingers.
#include <cstdint>
#include <string_view>
#include <lam/core/app.h>
#include <lam/core/asset_id.h>
#include <lam/core/id.h>
#include <lam/input/actions.h>
#include <lam/input/input_frame.h>
#include <lam/math/vec2.h>
#include "sim/rules.h"

namespace lam {
struct EmitterConfig;
}

namespace pathar {

// The game's actions (App resource). Ids are resolved once at startup.
struct Controls {
    lam::ButtonAction up, down, left, right;
    lam::ButtonAction undo, restart, hint, pause;
    lam::ButtonAction point;                   // left mouse: click a cell to walk or push
    lam::ButtonAction dir(Dir d) const;
};
// Defines every action with its default keys and pad buttons, and the key repeat that makes
// held keys walk / undo / hint again (ActionMap::set_repeat -> InputFrame::is_repeated).
Controls define_controls(lam::ActionMap &map);

// Intents (UI -> simulation): queued with InputSystem::queue_intent from frame code or
// scene callbacks, delivered in the next tick's InputFrame, recorded with it for replays.
inline constexpr lam::StrId kIntentRestart = lam::str_id("pathar.restart");  // undoable restart
inline constexpr lam::StrId kIntentReplay = lam::str_id("pathar.replay");    // fresh start, history cleared
inline constexpr lam::StrId kIntentUndo = lam::str_id("pathar.undo");

// What happened in the puzzle (World event, emitted at Simulate/Input, read by presentation).
struct PuzzleEvent {
    enum class Kind : std::uint8_t { Moved, Pushed, Sank, OntoGoal, OffGoal, Bump, Undo, NothingToUndo,
                                     Restart, Solved, Stuck, NoHint };
    Kind kind = Kind::Moved;
    lam::IVec2 cell;                           // where (player cell, or the stone's target)
};

// Particle configs the simulation may name in BurstAt (App resource, optional: absent -> no
// bursts). The presentation fills it with AssetServer::add<EmitterConfig>(...) ids.
struct PatharFx {
    lam::AssetId<lam::EmitterConfig> dust, splash, sparkle;
};

// Registers components, events and systems. Requires lam.tilemap (GridIndex, grid_movers),
// lam.physics (TileWorld) and lam.input.
class PatharSimPlugin final : public lam::Plugin {
public:
    std::string_view name() const override { return "pathar.sim"; }
    void build(lam::App &app) override;
};

// The systems, public so tests and tools can register them by hand.
void puzzle_input(lam::Ctx &ctx);
void puzzle_move(lam::Ctx &ctx);

}  // namespace pathar
