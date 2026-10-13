#pragma once
// Player input. Movement, waiting and the stairs are ButtonActions; UI decisions (use an item, load
// a save) arrive as Intents so replays reproduce them.
#include <lam/core/id.h>
#include <lam/input/input_frame.h>

namespace lam {
class ActionMap;
}

namespace rl {
struct Controls {                            // App resource (define_controls)
    lam::ButtonAction up, down, left, right; // step or bump-attack (key repeat)
    lam::ButtonAction wait;                  // skip a turn (rest)
    lam::ButtonAction descend;               // take the stairs
    lam::ButtonAction use[8];                // number keys -> kUseItem intents (HUD)
    lam::ButtonAction save, load;            // F5 / F9 (HUD)
};
// Intent kinds. kUseItem: a = inventory slot. kLoadGame: no payload.
inline constexpr lam::StrId kUseItem = lam::str_id("use_item");
inline constexpr lam::StrId kLoadGame = lam::str_id("load_game");

// Registers the actions in the ActionMap (with key repeat for movement) and returns them.
Controls define_controls(lam::ActionMap &map);

} // namespace rl
