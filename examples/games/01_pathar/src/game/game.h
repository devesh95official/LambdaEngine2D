#pragma once
// PatharGame (App resource), shared by every scene: the level catalogue, progress and its save
// slot, the run's flags and the --headless --bot summary numbers. Outlives the per-level worlds.
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <lam/core/app.h>
#include <lam/core/result.h>
#include <lam/scene/save.h>
#include "sim/catalogue.h"

namespace pathar {

inline constexpr std::string_view kGameId = "pathar";
inline constexpr int kSaveVersion = 1;
inline constexpr std::string_view kHudKeysSetting = "pathar.hud_keys";  // SettingsStore bool

struct PatharGame {
    Catalogue catalogue;
    Progress progress;
    std::unique_ptr<lam::SaveSlots> saves;  // null in scripted runs: nothing is read or written
    bool scripted = false;                  // --headless, --bot or --frames: no saves
    bool bot = false;                       // --bot: the SolverBot plays, levels advance by themselves
    int start_level = -1;                   // --level N (1-based on the command line); -1 = title

    // Summary of a bot run (written by tick systems; printed after App::run returns).
    int bot_solves = 0;
    std::int64_t bot_moves = 0;
    int last_level = -1;
    std::uint64_t last_hash = 0;            // World::state_hash of the focused puzzle world

    void save_progress() const;             // no-op without saves; logs failures
};

// Reads assets/data/levels.json (AssetServer::read_text, so the asset root rules apply), defines
// the game's settings and loads the saved progress unless the run is scripted.
lam::Status load_game(lam::App &app, PatharGame &game);

std::string help_text();
std::string credits_text();

}  // namespace pathar
