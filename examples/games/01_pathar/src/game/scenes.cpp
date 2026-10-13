// Title, play and menu scenes (see scenes.h).
#include "game/scenes.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include <lam/core/fixed_string.h>
#include <lam/core/log.h>
#include <lam/core/settings.h>
#include <lam/ecs/world.h>
#include <lam/input/driver.h>
#include <lam/input/input_frame.h>
#include <lam/ui/menu_scenes.h>
#include <lam/ui/ui.h>
#include "game/game.h"
#include "game/look.h"
#include "sim/board.h"
#include "sim/sim_plugin.h"

namespace pathar {

namespace {

using lam::App;
using lam::SceneMessage;
using lam::StrId;
using lam::World;

// Menu item ids. A menu pops with SceneMessage{kind = the chosen item's id, value = its value}.
constexpr StrId kPlay = lam::str_id("play"), kLevels = lam::str_id("levels"), kSettings = lam::str_id("settings"),
                kHelp = lam::str_id("help"), kCredits = lam::str_id("credits"), kQuit = lam::str_id("quit"),
                kResume = lam::str_id("resume"), kRestart = lam::str_id("restart"), kTitle = lam::str_id("title"),
                kNext = lam::str_id("next"), kAgain = lam::str_id("again"), kLevel = lam::str_id("level"),
                kChapter = lam::str_id("chapter"), kBack = lam::str_id("back"), kInfo = lam::str_id("info");

constexpr std::uint16_t kSolvedDelayTicks = 54;  // 0.9 s of celebration before the menu
constexpr int kDemo[] = {4, 10, 16, 6};           // the title's demo levels

lam::Transition fade() { return lam::Transition{.kind = lam::TransitionKind::Fade, .ticks = 15}; }

lam::MenuItem item(StrId id, std::string_view label, float value = 0, bool enabled = true,
                   lam::UiKind kind = lam::UiKind::Button) {
    lam::MenuItem m;
    m.id = id;
    m.label = label;
    m.kind = kind;
    m.value = value;
    m.max = std::max(1.0f, value);
    m.enabled = enabled;
    return m;
}

void set_footstep(World &w, int level, App &app) {
    w.resources().get<Board>().footstep =
        lam::str_id(theme_of(app, level) == "village" ? "footstep_grass" : "footstep_stone");
}

// ---- menus --------------------------------------------------------------------------------------

// The title menu leaves the demo below it ticking (MenuScene blocks lower ticks by default) and
// sits in the bottom-left corner, under the logo and beside the demo board (TitleLook::board_offset).
// MenuSpec has no position, so enter() moves the panel node build_menu made.
class TitleMenu final : public lam::MenuScene {
public:
    using lam::MenuScene::MenuScene;
    bool blocks_ticks() const override { return false; }
    void enter(App &app, World &world) override {
        lam::MenuScene::enter(app, world);
        const lam::MenuState &st = world.resources().get<lam::MenuState>();
        if (lam::UiNode *panel = world.resources().get<lam::UiTree>().get(st.panel)) {
            panel->anchor = panel->pivot = {0.0f, 1.0f};
            panel->rect.x = 6;
            panel->rect.y = -4;
        }
    }
};

std::unique_ptr<lam::Scene> make_title_menu(const PatharGame &g) {
    lam::MenuSpec spec;
    spec.dim = lam::kTransparent;  // the HUD dims the demo and draws the logo
    spec.back_pops = false;
    lam::FixedString<48> play;
    if (g.progress.solved_count() == 0) play = "Play";
    else lam::format_to(play, "Continue - level {}", g.progress.next_unsolved() + 1);
    spec.items = {item(kPlay, play.view(), static_cast<float>(g.progress.next_unsolved())),
                  item(kLevels, "Choose a level"), item(kSettings, "Settings"), item(kHelp, "How to play"),
                  item(kCredits, "Credits"), item(kQuit, "Quit")};
    return std::make_unique<TitleMenu>(std::move(spec));
}

std::unique_ptr<lam::Scene> make_pause_menu() {
    lam::MenuSpec spec;
    spec.title = "Paused";
    spec.items = {item(kResume, "Resume"), item(kRestart, "Start the level again"), item(kLevels, "Choose a level"),
                  item(kSettings, "Settings"), item(kHelp, "How to play"), item(kTitle, "To the title")};
    return std::make_unique<lam::MenuScene>(std::move(spec));
}

std::string stars_text(int n) { return std::string(static_cast<std::size_t>(n), '*') + std::string(static_cast<std::size_t>(3 - n), '-'); }

std::unique_ptr<lam::Scene> make_solved_menu(const PatharGame &g, int level, int moves, int stars, bool helped, bool best) {
    const bool last = level + 1 >= g.catalogue.total();
    lam::MenuSpec spec;
    if (last) spec.title = "Every stone is home!";
    else lam::format_to(spec.title, "Solved!  {}", stars_text(stars));
    spec.back_pops = false;  // a solved level is left through the menu
    spec.dim = lam::Color{0, 0, 0, 90};
    lam::FixedString<48> line;
    lam::format_to(line, "{} moves, par {}{}", moves, g.catalogue.at(level).level.par, helped ? " (hints)" : "");
    spec.items.push_back(item(kInfo, line.view(), 0, false, lam::UiKind::Label));
    if (best && !helped) spec.items.push_back(item(kInfo, "A new best!", 0, false, lam::UiKind::Label));
    if (!last) spec.items.push_back(item(kNext, "Next level", static_cast<float>(level + 1), g.progress.unlocked(level + 1)));
    spec.items.push_back(item(kAgain, "Play it again"));
    spec.items.push_back(item(kLevels, "Choose a level"));
    return std::make_unique<lam::MenuScene>(std::move(spec));
}

// One chapter's levels; chapter changes pop with "chapter" and the scene below opens the next.
std::unique_ptr<lam::Scene> make_level_select(const PatharGame &g, int chapter) {
    const int chapters = static_cast<int>(g.catalogue.chapters.size());
    chapter = std::clamp(chapter, 0, chapters - 1);
    const ChapterInfo &ch = g.catalogue.chapters[static_cast<std::size_t>(chapter)];
    lam::MenuSpec spec;
    lam::format_to(spec.title, "{}. {} - {}", chapter + 1, ch.name, ch.english);
    for (int i = 0; i < static_cast<int>(ch.levels.size()); ++i) {
        const int global = g.catalogue.global(chapter, i);
        const bool open = g.progress.unlocked(global);
        lam::FixedString<48> label;
        if (open) lam::format_to(label, "{:>2}. {:<15} {}", global + 1, ch.levels[static_cast<std::size_t>(i)].name,
                                 stars_text(g.progress.record(global).stars));
        else lam::format_to(label, "{:>2}. (solve more to open)", global + 1);
        spec.items.push_back(item(kLevel, label.view(), static_cast<float>(global), open));
    }
    if (chapters > 1) {
        spec.items.push_back(item(kChapter, "Next chapter >", static_cast<float>((chapter + 1) % chapters)));
        spec.items.push_back(item(kChapter, "< Previous chapter", static_cast<float>((chapter + chapters - 1) % chapters)));
    }
    spec.items.push_back(item(kBack, "Back"));
    return std::make_unique<lam::MenuScene>(std::move(spec));
}

std::unique_ptr<lam::Scene> make_settings(App &app) {
    // The engine settings page (bus volumes, fullscreen, key rebinding) plus one game toggle. Its
    // id names a defined SettingsStore key, so the page writes the key itself when it changes; the
    // page finds the key by id_name(), so the id must be interned (lam::intern, not str_id).
    const lam::SettingsStore &s = app.res<lam::SettingsStore>();
    const std::vector<lam::MenuItem> extra{item(lam::intern(kHudKeysSetting), "Key hints in the HUD",
                                                s.get_bool(kHudKeysSetting) ? 1.0f : 0.0f, true, lam::UiKind::Toggle)};
    return lam::make_settings_scene(extra);
}

// Messages both the title and the play scene understand. True if it opened another scene.
bool open_common(App &app, const SceneMessage &m) {
    lam::SceneStack &stack = app.res<lam::SceneStack>();
    const PatharGame &g = app.res<PatharGame>();
    if (m.kind == kLevel) {
        stack.replace(make_play_scene(static_cast<int>(m.value)), fade());
    } else if (m.kind == kChapter) {
        stack.push(make_level_select(g, static_cast<int>(m.value)));
    } else if (m.kind == kSettings) {
        stack.push(make_settings(app));
    } else if (m.kind == kHelp) {
        stack.push(lam::make_text_scene("How to play", help_text()));
    } else {
        return false;
    }
    return true;
}

// ---- title --------------------------------------------------------------------------------------

struct TitleDemo {           // World resource of the title world
    int demo = 0;
    int wait = 48;           // ticks before the next step
    std::vector<Dir> plan;
    std::size_t next = 0;
};

// Tick system on the title world: plays the demo levels with the solver, one after another.
// It writes PuzzleState::queued directly: the title world takes no input (the menu is focused).
void title_demo(lam::Ctx &ctx) {
    World &w = ctx.world();
    TitleDemo &d = ctx.local<TitleDemo>();
    PuzzleState &ps = ctx.local<PuzzleState>();
    if (d.wait > 0) {
        --d.wait;
        return;
    }
    if (busy(w) || ps.queued != 0) return;
    if (ps.solved) {
        d.demo = (d.demo + 1) % static_cast<int>(std::size(kDemo));
        const int level = kDemo[d.demo];
        load_level(w, ctx.res<PatharGame>().catalogue.at(level).level, level);
        set_footstep(w, level, ctx.app());
        d.plan.clear();
        d.next = 0;
        d.wait = 84;
        return;
    }
    if (d.next >= d.plan.size()) {
        const Solution sol = solve(ctx.local<Board>().level, read_state(w));
        d.plan = sol.moves;
        d.next = 0;
        if (!sol.found) {  // cannot happen with the shipped levels; start over rather than search every tick
            restart(w, false);
            return;
        }
    }
    ps.queued = static_cast<std::uint8_t>(1 + static_cast<int>(d.plan[d.next++]));
    d.wait = 4;
}

class TitleScene final : public lam::Scene {
public:
    std::string_view name() const override { return "pathar.title"; }
    void enter(App &app, World &world) override {
        const PatharGame &g = app.res<PatharGame>();
        world.resources().insert<TitleLook>();
        world.resources().insert<TitleDemo>();
        load_level(world, g.catalogue.at(kDemo[0]).level, kDemo[0]);
        world.resources().get<Board>().quiet = true;  // the demo makes no sound
        set_footstep(world, kDemo[0], app);
        refresh_look(app, world);
        add_system(app, lam::Stage::Simulate, title_demo, "pathar.title_demo", -10);
        app.res<lam::SceneStack>().push(make_title_menu(g));
    }
    void resumed(App &app, const SceneMessage *msg) override {
        lam::SceneStack &stack = app.res<lam::SceneStack>();
        PatharGame &g = app.res<PatharGame>();
        if (msg != nullptr) {
            if (msg->kind == kPlay) {
                stack.replace(make_play_scene(static_cast<int>(msg->value)), fade());
                return;
            }
            if (msg->kind == kLevels) {
                stack.push(make_level_select(g, g.catalogue.locate(g.progress.last_played).first));
                return;
            }
            if (msg->kind == kCredits) {
                stack.push(lam::make_text_scene("Credits", credits_text()));
                return;
            }
            if (msg->kind == kQuit) {
                app.quit();
                return;
            }
            if (open_common(app, *msg)) return;
        }
        stack.push(make_title_menu(g));  // back from a page: the menu again (with fresh labels)
    }
};

// ---- play ---------------------------------------------------------------------------------------

struct Flow {                // World resource of a play world: what the flow already did
    bool reported = false;   // progress recorded for this solve
    bool menu_shown = false;
    bool best = false;
    int stars = 0;
};

// Tick system on the play world (after the simulation): pause key, recording a solve, then the
// solved menu (or, for the bot, the next level).
void play_flow(lam::Ctx &ctx) {
    lam::SceneStack &stack = ctx.res<lam::SceneStack>();
    if (stack.transitioning() || stack.queued() > 0) return;
    World &w = ctx.world();
    Flow &f = ctx.local<Flow>();
    const PuzzleState &ps = ctx.local<PuzzleState>();
    PatharGame &g = ctx.res<PatharGame>();
    if (!ps.solved) {
        f = Flow{};
        if (lam::input_frame(ctx).is_pressed(ctx.res<Controls>().pause)) stack.push(make_pause_menu());
        return;
    }
    const int par = g.catalogue.at(ps.level).level.par;
    if (!f.reported) {
        f.reported = true;
        f.best = g.progress.finish(ps.level, ps.moves, par, ps.helped);
        f.stars = ps.helped ? 1 : stars(ps.moves, par);
        g.save_progress();
        if (g.bot) {
            ++g.bot_solves;
            g.bot_moves += ps.moves;
            LAM_LOG_INFO("pathar bot: level {} '{}' solved in {} moves, {} pushes (par {}) at tick {}", ps.level + 1,
                         g.catalogue.at(ps.level).name, ps.moves, ps.pushes, par, w.tick());
        }
    }
    if (f.menu_shown || busy(w) || w.tick() < ps.solved_tick + kSolvedDelayTicks) return;
    f.menu_shown = true;
    stack.push(make_solved_menu(g, ps.level, ps.moves, f.stars, ps.helped, f.best));  // the bot answers it too
}

class PlayScene final : public lam::Scene {
public:
    explicit PlayScene(int level) : level_(level) {}
    std::string_view name() const override { return "pathar.play"; }
    void enter(App &app, World &world) override {
        PatharGame &g = app.res<PatharGame>();
        level_ = std::clamp(level_, 0, g.catalogue.total() - 1);
        load_level(world, g.catalogue.at(level_).level, level_);
        set_footstep(world, level_, app);
        world.resources().insert<Flow>();
        refresh_look(app, world);
        add_system(app, lam::Stage::Simulate, play_flow, "pathar.flow", 100);
        g.progress.last_played = level_;
        g.save_progress();
    }
    void resumed(App &app, const SceneMessage *msg) override {
        if (msg == nullptr) return;
        lam::SceneStack &stack = app.res<lam::SceneStack>();
        const PatharGame &g = app.res<PatharGame>();
        lam::InputSystem &input = app.res<lam::InputSystem>();
        // Puzzle changes go back as intents: delivered in the next tick's InputFrame and recorded.
        if (msg->kind == kRestart) {
            input.queue_intent(lam::Intent{.kind = kIntentRestart});
        } else if (msg->kind == kAgain) {
            input.queue_intent(lam::Intent{.kind = kIntentReplay});
        } else if (msg->kind == kNext) {
            stack.replace(make_play_scene(level_ + 1), fade());
        } else if (msg->kind == kLevels) {
            stack.push(make_level_select(g, g.catalogue.locate(level_).first));
        } else if (msg->kind == kTitle) {
            stack.replace(make_title_scene(), fade());
        } else if (!open_common(app, *msg) && world().resources().get<PuzzleState>().solved) {
            world().resources().get<Flow>().menu_shown = false;  // back over a solved board: its menu again
        }
    }

private:
    int level_;
};

// The item a bot picks in `spec`: Next level, else level 1 in a level select, else the first
// chapter, else "Choose a level"; -1 for any other menu (pause, settings: never opened by the bot).
int bot_choice(const lam::MenuSpec &spec) {
    auto find = [&](StrId id, bool want_zero) {
        for (std::size_t i = 0; i < spec.items.size(); ++i) {
            const lam::MenuItem &m = spec.items[i];
            if (m.id == id && m.enabled && (!want_zero || m.value == 0.0f)) return static_cast<int>(i);
        }
        return -1;
    };
    for (const auto &[id, zero] : {std::pair{kNext, false}, std::pair{kLevel, true}, std::pair{kChapter, true},
                                   std::pair{kLevels, false}}) {
        if (const int i = find(id, zero); i >= 0) return i;
    }
    return -1;
}

}  // namespace

void PatharBot::fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) {
    World &w = ctx.world();
    const lam::MenuState *menu = w.resources().try_get<lam::MenuState>();
    if (menu == nullptr) {
        const PuzzleState *ps = w.resources().try_get<PuzzleState>();
        if (undo_next_) {
            frames[0].press(controls_.undo);
            undo_next_ = false;
            return;
        }
        if (undo_every_ > 0 && ps != nullptr && !ps->solved && ps->moves > 0 && ps->queued == 0 && !busy(w) &&
            solver_.presses() - last_undo_at_ >= undo_every_) {
            last_undo_at_ = solver_.presses();
            if (++undos_ % 3 == 0) {
                frames[0].press(controls_.restart);
                undo_next_ = true;
            } else {
                frames[0].press(controls_.undo);
            }
            return;
        }
        solver_.fill(ctx, frames);
        return;
    }
    if (menu->closed) return;  // already answered; it pops at the next tick boundary
    const int i = bot_choice(menu->spec);
    if (i < 0) return;
    const lam::MenuItem &item = menu->spec.items[static_cast<std::size_t>(i)];
    // a = index into MenuSpec::items (labels count), b = the button's value, as a click sends.
    frames[0].push_intent(lam::Intent{.kind = lam::kMenuSelectIntent, .a = i, .b = std::llround(item.value)});
    ++menu_picks_;
}

std::unique_ptr<lam::Scene> make_title_scene() { return std::make_unique<TitleScene>(); }
std::unique_ptr<lam::Scene> make_play_scene(int level) { return std::make_unique<PlayScene>(level); }

}  // namespace pathar
