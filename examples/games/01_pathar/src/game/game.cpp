// PatharGame: catalogue, progress and saves (see game.h).
#include "game/game.h"

#include <lam/assets/asset_server.h>
#include <lam/core/data.h>
#include <lam/core/fs.h>
#include <lam/core/log.h>
#include <lam/core/settings.h>

namespace pathar {

lam::Status load_game(lam::App &app, PatharGame &g) {
    LAM_TRY_ASSIGN(const std::string text, app.res<lam::AssetServer>().read_text("data/levels.json"));
    LAM_TRY_ASSIGN(g.catalogue, Catalogue::parse(text, "data/levels.json"));
    g.progress = Progress(g.catalogue.total());
    // Settings: typed keys with a schema; the engine loaded settings.json before plugins built,
    // so a saved value is validated now that the key is defined.
    app.res<lam::SettingsStore>().define_bool(kHudKeysSetting, true);
    if (g.scripted) return {};
    // The App's data folder (user_data_dir(AppConfig::title) or LAM_DATA_DIR), where the engine
    // keeps settings.json too; "" under --headless or when it cannot be created.
    const std::string dir = app.data_dir();
    if (dir.empty()) {
        LAM_LOG_WARN("pathar: no data folder; progress will not be saved");
        return {};
    }
    g.saves = std::make_unique<lam::SaveSlots>(dir, std::string(kGameId), kSaveVersion, 1);
    if (g.saves->exists(1)) {
        lam::Result<lam::SaveEnvelope> env = g.saves->load(1);
        if (env) g.progress = Progress::from_json(env->data_json, g.catalogue);
        else LAM_LOG_WARN("pathar: progress not loaded: {}", env.error().message);
    }
    return {};
}

void PatharGame::save_progress() const {
    if (!saves) return;
    lam::DataWriter summary;
    summary.begin_object().key("solved").value(progress.solved_count()).key("stars").value(progress.total_stars()).end_object();
    lam::Result<std::string> summary_json = summary.finish();
    const lam::SaveEnvelope env{.game = std::string(kGameId),
                                .version = kSaveVersion,
                                .summary_json = summary_json ? summary_json.value() : std::string("{}"),
                                .data_json = progress.to_json(catalogue)};
    if (lam::Status s = saves->save(1, env); !s) LAM_LOG_WARN("pathar: progress not saved: {}", s.error().message);
}

std::string help_text() {
    return "THE STONES\n"
           "Push every stone onto a carved slab. The level is solved when no slab is left bare.\n"
           "You can only push, never pull, and only one stone at a time. A stone in a corner stays there.\n"
           "\n"
           "THE RIVER\n"
           "You cannot wade through water, but a stone pushed into it sinks, and you can walk over it after. "
           "Some levels have more stones than slabs for this.\n"
           "\n"
           "KEYS\n"
           "Walk and push: WASD or the arrow keys (hold to keep walking), or click a place to walk there.\n"
           "Undo: Z or Backspace (hold to undo many). Start again: R (that can be undone too).\n"
           "Hint: H plays the next step of a solution. A level solved with hints gives one star.\n"
           "Pause and the level list: Esc.\n"
           "\n"
           "STARS\n"
           "Three stars at par or better, two for a little over, one for solving it at all.\n"
           "Each level solved opens the next. You may leave two unsolved and go on.";
}

std::string credits_text() {
    return "PATHAR\n"
           "Stone puzzles of the courtyard, the ghat and the mountain temple.\n"
           "\n"
           "An example game for LambdaEngine2D: 24 levels, a breadth-first solver for hints,\n"
           "built on the ECS (grid movers, History undo, menu scenes).\n"
           "\n"
           "Art, sound and music by the author.\n"
           "raylib (zlib licence), nlohmann/json (MIT).";
}

}  // namespace pathar
