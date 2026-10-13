#pragma once
// Private state of lam::App, shared by the lam_app .cpp files. Not installed.
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <lam/core/app.h>
#include <lam/core/clock.h>
#include <lam/core/platform.h>
#include <lam/core/time.h>
#include <lam/ecs/world.h>
#include <lam/math/rng.h>
namespace lam {
namespace detail {
// One registered system plus its tie-breaker and the plugin whose build() added it.
struct SystemEntry {
    SystemDesc desc;
    std::uint64_t reg = 0;                                      // registration index
    std::string_view plugin;                                    // Plugin::name() or "" (game code)
};
// A system-list change made while a pass was running, applied when the pass ends.
struct PendingOp {
    enum class Kind : std::uint8_t { Add, RemoveOf };
    Kind kind = Kind::Add;
    SystemEntry entry;
    std::uint32_t world_id = 0;
};
// Events emitted into a world while it cannot tick are not delivered to its tick stages, but
// events still pending when it stopped ticking are. At the end of every frame in which the world
// could not tick, [skip_from, skip_to) grows to cover what was emitted meanwhile; the next window
// of each tick stage ends at skip_from and the one after starts at skip_to (next_from).
struct TickSkip {
    std::array<std::uint32_t, kStageCount> next_from{};         // 0 = none
    std::uint32_t eligible_end_seq = 0;                         // sequence after the last tickable frame
    std::uint32_t skip_from = 0;
    std::uint32_t skip_to = 0;
    std::uint32_t pending = 0;                                  // bit per stage: skip not applied
};
// One world owned by the App.
struct WorldRecord {
    std::unique_ptr<World> world;
    std::uint64_t stream = 0;                                   // str_id(name).value ^ counter
    Time time;                                                  // what Ctx::time() returns for it
    StageMarks marks;                                           // read windows of its bus
    TickSkip skip;                                              // events emitted while not ticking
    bool ticked_this_frame = false;
};
// A World-scope event channel, added to every world (also worlds created later).
struct EventRegistration {
    TypeId id = 0;
    std::size_t capacity = 0;                                   // already multiplied
    void (*add)(EventBus &bus, std::size_t capacity) = nullptr;
};
// A startup system (add_startup).
struct StartupEntry {
    SystemFn fn = nullptr;
    std::string_view name;
};
// Which part of a stage's Once list a pass runs.
enum class OrderRange : std::uint8_t { All, Negative, NonNegative };
}

struct App::Impl {
    explicit Impl(AppConfig c, LaunchOptions o);
    AppConfig config;
    LaunchOptions options;
    FixedClock clock;
    std::unique_ptr<Platform> platform;
    AssetResolver *resolver = nullptr;
    std::uint64_t seed = 1;
    Rng fx_rng;
    std::string settings_path;                                  // "" = settings are not persisted

    // Plugins (build order) and requirements.
    std::vector<std::unique_ptr<Plugin>> plugins;
    std::vector<std::string_view> building;                     // plugins whose build() is running
    std::vector<std::pair<std::string, std::string>> requirements;   // (needed, by)
    bool finished = false;
    bool finishing = false;                                     // inside the finish() loop

    // Systems: sorted by (order, reg) per stage and scope.
    std::array<std::vector<detail::SystemEntry>, detail::kStageCount> once;
    std::array<std::vector<detail::SystemEntry>, detail::kStageCount> per_world;
    std::uint64_t next_reg = 1;
    std::vector<detail::PendingOp> pending_ops;
    int pass_depth = 0;
    std::vector<detail::StartupEntry> startup;
    std::size_t startup_done = 0;

    // Events.
    std::vector<detail::EventRegistration> world_events;
    detail::StageMarks app_marks;
    detail::StageMarks no_marks;                                // empty windows (startup)
    std::uint64_t grow_seen = 0;

    // Worlds: creation order, [0] = main.
    std::vector<std::unique_ptr<detail::WorldRecord>> worlds;
    std::uint64_t world_counter = 0;
    std::array<WorldEntry, kMaxWorlds> requested{};
    std::size_t requested_count = 0;
    std::array<WorldEntry, kMaxWorlds> active{};
    std::array<detail::WorldRecord *, kMaxWorlds> active_records{};
    std::size_t active_count = 0;
    std::vector<std::uint32_t> pending_destroys;

    // Time and run state.
    Time app_time;
    std::uint64_t app_tick = 0;
    std::uint64_t frames_done = 0;
    double last_now = 0;
    double start_now = 0;
    bool have_last = false;
    bool paused = false;
    bool started = false;
    bool in_frame = false;
    bool quitting = false;
    bool shut_down = false;
    bool startup_assets_ready = false;
    int exit_code = 0;
};

namespace detail {
// Helpers with access to App's private members (friend of App and Ctx).
struct AppInternals {
    using Impl = App::Impl;
    static Impl &impl(App &a) { return *a.impl_; }
    static const Impl &impl(const App &a) { return *a.impl_; }
    static Resources &resources(App &a) { return a.resources_; }
    static EventBus &bus(App &a) { return a.events_; }
    static Ctx make_ctx(App &app, WorldRecord &r, const StageMarks *app_marks, Stage stage,
                        SystemScope scope, bool focused, bool startup) {
        return Ctx(&app, r.world.get(), &r.time, startup ? app_marks : &r.marks, app_marks, stage,
                   scope, focused, startup);
    }
    // Frame algorithm (app_frame.cpp).
    static void frame(App &app, bool force_one_tick);
    static void start(App &app);                                // finish + requirements + startup
    static void finish_plugins(App &app);
    static void run_startup(App &app);
    // Systems (app_systems.cpp).
    static void insert_system(App &app, const SystemEntry &e);
    static void remove_systems_now(App &app, std::uint32_t world_id);
    static void apply_pending_ops(App &app);
    static void register_engine_systems(App &app);
    // Worlds (app_worlds.cpp).
    static WorldRecord &create_record(App &app, std::string_view name);
    static WorldRecord *record_of(App &app, const World &w);
    static void destroy_now(App &app, std::uint32_t world_id);
    static void apply_pending_destroys(App &app);
    static void refresh_stack(App &app);
    static WorldRecord &ctx_record(App &app);                   // focused or main
    // Settings (app.cpp).
    static void save_settings(App &app);
};
}
}
