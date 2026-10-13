#pragma once
// App: worlds, the stage loop, plugins, resources and events. Systems are plain function
// pointers and all state lives in resources and components. Main thread only; the frame path
// doesn't allocate in steady state.
//   App app({.title = "Hop"}, argc, argv);
//   app.add_plugins(DefaultPlugins{});
//   app.add_system(Stage::Simulate, move_things, "game.move");
//   return app.run();
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>
#include <lam/core/asset_id.h>
#include <lam/core/events.h>
#include <lam/core/id.h>
#include <lam/core/resources.h>
#include <lam/core/time.h>
#include <lam/ecs/commands.h>
#include <lam/ecs/component.h>
#include <lam/ecs/world.h>
#include <lam/math/rng.h>
#include <lam/math/vec2.h>
namespace lam {
class App;
class Commands;
class Platform;
// The twelve fixed stages, in frame order. TickBegin..TickEnd run once per fixed tick.
enum class Stage : std::uint8_t { FrameBegin, TickBegin, Input, Simulate, Physics, PostPhysics,
    Animate, TickEnd, Update, Render, RenderUI, FrameEnd, Count };
// "FrameBegin", "TickBegin", ...; "?" for Count. Static storage.
std::string_view stage_name(Stage s);
// True for TickBegin..TickEnd (the stages that run once per fixed tick).
constexpr bool is_tick_stage(Stage s) { return s >= Stage::TickBegin && s <= Stage::TickEnd; }
// PerWorld runs on every world in the stack taking part in the stage, menus and overlays too, so
// gate game rules with run_if or only_world. Once runs on the focused world (or main).
enum class SystemScope : std::uint8_t { PerWorld, Once };
// Maximum entries in the world stack (App::set_world_stack).
inline constexpr int kMaxWorlds = 8;
namespace detail {
inline constexpr std::size_t kStageCount = static_cast<std::size_t>(Stage::Count);
// Read windows of one event bus: Ctx::read<T>() in stage S returns events whose sequence
// is in [prev[S], cur[S]). `occurrence` is the tick (tick stages) or frame + 1 (frame stages)
// whose start last rolled the window, so a stage that runs several passes rolls once.
struct StageMarks {
    std::array<std::uint32_t, kStageCount> prev{};
    std::array<std::uint32_t, kStageCount> cur{};
    std::array<std::uint64_t, kStageCount> occurrence{};
};
// Cold-path failures with a message that names the right call. Never return.
[[noreturn]] void ctx_resource_fail(const char *type_name, bool wanted_app, bool in_other_scope);
[[noreturn]] void event_not_registered(const char *type_name, const char *where);
struct AppInternals;                                            // lam_app private access (.cpp)
}
// Handed to every system. Cheap to copy; valid only during the call.
class Ctx {
public:
    App &app() const;
    World &world() const;                                       // PerWorld: that world; Once: focused or main
    Commands &commands() const;                                 // world().commands()
    const Time &time() const;                                   // per world (time.h)
    bool focused() const;                                       // this world receives input
    Stage stage() const;
    SystemScope scope() const;
    // App resource; a missing one aborts with a message naming the fix.
    template <class T> T &res() const;
    template <class T> T *try_res() const;                      // null if absent
    // World resource of ctx.world(); aborts the same way if missing.
    template <class T> T &local() const;
    template <class T> T *try_local() const;                    // null if absent
    // Appends to the world or App channel T was registered on. Aborts if T isn't registered:
    // register it in your own build() too (add_event is idempotent) or use try_emit.
    template <class T> void emit(const T &e) const;
    // T events emitted since this stage last ran here, each seen once. Events emitted earlier in
    // the same stage pass show up on the next run. Valid until the system returns.
    template <class T> std::span<const T> read() const;
    template <class T> bool has_event() const;                  // registered in either scope
    // emit() if T is registered; returns whether it did. For events of optional plugins.
    template <class T> bool try_emit(const T &e) const;
    // Per-world stream, snapshotted and hashed. Tick stages and startup only; code without a Ctx
    // uses world.rng(stream) directly.
    Rng &rng(StrId stream) const;
    Rng &fx_rng() const;                                        // presentation only; never hashed or replayed
private:
    friend class App;
    friend struct detail::AppInternals;
    Ctx(App *app, World *world, const Time *time, const detail::StageMarks *world_marks,
        const detail::StageMarks *app_marks, Stage stage, SystemScope scope, bool focused,
        bool startup)
        : app_(app), world_(world), time_(time), world_marks_(world_marks), app_marks_(app_marks),
          stage_(stage), scope_(scope), focused_(focused), startup_(startup) {}
    App *app_;
    World *world_;
    const Time *time_;
    const detail::StageMarks *world_marks_;
    const detail::StageMarks *app_marks_;
    Stage stage_;
    SystemScope scope_;
    bool focused_;
    bool startup_;                                              // add_startup: rng() allowed
};
// run_if predicate: true when ctx.world() holds the World resource T.
template <class T> bool has_local(const Ctx &ctx) { return ctx.try_local<T>() != nullptr; }
using SystemFn = void (*)(Ctx &);
// Checked before each call; false skips that call only. Engine systems keep running, so to freeze
// a world stop its ticks (a blocking Scene, WorldEntry::ticks or App::set_paused).
using RunIf = bool (*)(const Ctx &);
// Everything add_system needs. Allowed scopes: Input..Animate PerWorld only, FrameEnd Once only
// (LAM_CHECK). Order within a pass is (order, registration index).
struct SystemDesc {
    Stage stage = Stage::Simulate;
    SystemFn fn = nullptr;
    std::string_view name;                                      // static storage; unique per (stage, only_world)
    // Engine orders are in docs/GUIDE.md. Game systems use (-1000, 1000), which in Animate is after
    // the animators: set clips in PostPhysics, override what the animators wrote in Animate.
    int order = 0;
    SystemScope scope = SystemScope::PerWorld;
    RunIf run_if = nullptr;                                     // null = always
    std::uint32_t only_world = 0;                               // World::id(); 0 = every world
};
// Plugins build in the order they're added and init_world runs on every world, existing or later.
// finish runs at run(); shutdown_world/shutdown go in reverse build order.
class Plugin {
public:
    virtual ~Plugin() = default;
    virtual std::string_view name() const = 0;                  // unique, e.g. "lam.physics"
    virtual void build(App &app) = 0;                           // components, App resources, events, systems, loaders
    virtual void init_world(App &app, World &world) { (void)app; (void)world; }   // world resources
    virtual void finish(App &app) { (void)app; }                // after every plugin built
    virtual void shutdown_world(App &app, World &world) { (void)app; (void)world; }
    virtual void shutdown(App &app) { (void)app; }              // reverse order
};
// Startup configuration. Designated initialisers must follow declaration order.
struct AppConfig {
    std::string title = "LambdaEngine2D";
    IVec2 window_size{1280, 720};                               // points
    IVec2 virtual_size{480, 270};                               // pixel-art target
    int tick_rate = 60;                                         // fixed ticks per second
    double max_frame_seconds = 0.25;                            // longer frames are capped
    int max_ticks_per_frame = 4;                                // also sizes event channels
    std::uint64_t seed = 1;                                     // every world seed derives from it
    bool resizable = true;
    bool vsync = true;
    bool audio = true;                                          // false: no audio device, sound is silent
    bool wait_for_startup_assets = true;
    std::size_t entity_capacity = 16384;                        // per world (WorldConfig)
    std::string settings_file = "settings.json";                // in user_data_dir(title); "" = none
};
// Command-line flags. Unknown ones are kept in `extra`.
struct LaunchOptions {
    bool hidden = false;                                        // --hidden (not vsync-paced; pair with --step)
    bool headless = false;                                      // --headless: NullPlatform, no backends
    bool step = false;                                          // --step: one tick per frame, frame_dt = tick_dt
    int frames = 0;                                             // --frames N: quit after N frames
    int draw_every = 1;                                         // --draw-every N (FrameEnd still swaps)
    std::uint64_t seed = 0;                                     // --seed S (0 = AppConfig::seed)
    std::string record_input;                                   // --record-input FILE
    std::string replay;                                         // --replay FILE
    bool replay_any_build = false;                              // --replay-any-build
    std::string screenshot;                                     // --screenshot FILE (last frame)
    int hash_every = 0;                                         // --hash-every N
    bool print_schedule = false;                                // --print-schedule: log system order, exit
    bool no_vsync = false;                                      // --no-vsync (benches)
    std::vector<std::string> extra;                             // unknown arguments, in order
    // True if `flag` is in `extra`, or is a known boolean flag that is set ("--headless").
    bool has(std::string_view flag) const;
    // Game flags: "--enemies 2000" or "--enemies=2000" -> "2000". nullopt if absent or last.
    std::optional<std::string_view> value(std::string_view flag) const;
    // value(flag) as a decimal int; `fallback` if absent or not a whole number in int range.
    int int_value(std::string_view flag, int fallback) const;
    bool deterministic() const;                                 // headless || step || record || replay
    // deterministic(), --hidden, --frames or --bot. Scripted runs leave the settings file and the
    // data dir alone.
    bool scripted() const;
};
// Parses argv[1..argc). A value flag without a valid value is kept in `extra`; --seed also takes
// 0x-hex. argv may be null when argc is 0.
LaunchOptions parse_launch_options(int argc, const char *const *argv);
// One world in the frame's stack (bottom first). The world must belong to this App.
struct WorldEntry {
    World *world = nullptr;
    bool ticks = true;                                          // runs tick stages
    bool visible = true;                                        // runs Update/Render/RenderUI
    bool input = true;                                          // Ctx::focused() (topmost input world wins)
};
class App {
public:
    // Parses argv, inserts SettingsStore, PrefabLibrary, Profiler and DebugOverlay, registers the
    // engine components, events and systems, and creates the main world.
    explicit App(AppConfig config = {}, int argc = 0, const char *const *argv = nullptr);
    // Shuts plugins down in reverse build order, then destroys worlds, resources and plugins.
    ~App();
    App(const App &) = delete;
    App &operator=(const App &) = delete;
    // Builds the plugin now and runs init_world on existing worlds. Names must be unique.
    template <class P, class... A> P &add_plugin(A &&...args);
    template <class Bundle> App &add_plugins(const Bundle &bundle);   // bundle.build(*this)
    // True once a plugin with that name was added (also while its build() runs).
    bool has_plugin(std::string_view name) const;
    // run() aborts, naming both plugins, if `needed` was never added.
    void require_plugin(std::string_view needed, std::string_view by);
    // Systems (changes during a pass are queued)
    App &add_system(Stage stage, SystemFn fn, std::string_view name, int order = 0);
    App &add_system(const SystemDesc &desc);
    // Runs once on the main world before frame 1 (or the next frame, if added later). Commands
    // are applied after each one; rng() is allowed.
    App &add_startup(SystemFn fn, std::string_view name);
    // Removes every system whose only_world == world_id (queued during a pass).
    void remove_systems_of(std::uint32_t world_id);
    // Adds a TickEnd +1100 system that keeps the Follow pools in Lead's dense order. Idempotent.
    template <class Lead, class... Follow> App &align_pools();
    // One line per system in run order, for --print-schedule.
    std::string schedule_text() const;
    // Data
    // Only needed for unreflected types; reflected ones register during static init.
    template <class T> App &register_component();
    // Registers channel T on every world or on the App bus, reserving capacity x
    // (max_ticks_per_frame + 1). Repeat calls keep the first capacity. Full channels grow.
    template <class T> App &add_event(EventScope scope = EventScope::World, std::size_t capacity = 256);
    // Raises T's reserve to at least capacity x (max_ticks_per_frame + 1). Never shrinks.
    template <class T> App &reserve_event(std::size_t capacity);
    // True once add_event<T> was called (either scope).
    template <class T> bool has_event() const;
    // App resource, constructed in place; LAM_ASSERT absent.
    template <class T, class... A> T &insert_resource(A &&...args);
    template <class T> T &res();                                // App resource; aborts if absent
    template <class T> T *try_res();                            // null if absent
    Resources &resources();                                     // App resources
    EventBus &events();                                         // App-scope events
    // Worlds
    World &main_world();                                        // from construction; never destroyed
    // New world seeded from seed() and its name; every plugin's init_world runs on it.
    World &create_world(std::string_view name);
    void destroy_world(World &w);                               // queued if a pass is running
    // First live world with this name (scene worlds use Scene::name()), or null. Still works after
    // run() returns, which headless summaries rely on.
    World *find_world(StrId name);
    // Bottom first, at most kMaxWorlds; empty means {main_world}. Picked up after the FrameBegin
    // and TickBegin Once passes.
    void set_world_stack(std::span<const WorldEntry> entries);
    // The stack the loop is running on; {main_world} before the first frame.
    std::span<const WorldEntry> world_stack() const;
    // Topmost world of the active stack with input == true; null if none.
    World *focused_world() const;
    // Services
    void set_platform(std::unique_ptr<Platform> platform);     // default: NullPlatform
    Platform &platform();                                       // valid until set_platform
    void set_asset_resolver(AssetResolver *resolver);           // AssetsPlugin
    AssetResolver *asset_resolver() const;                      // null until AssetsPlugin
    SerializeContext serialize_context() const;                 // {asset_resolver(), {}}
    const AppConfig &config() const;                            // as given (sanitised)
    const LaunchOptions &options() const;                       // parsed from argv
    // Per-user data folder, created on call. Empty in scripted runs unless LAM_DATA_DIR is set, so
    // put saves here and bots and tests won't touch the player's.
    std::string data_dir() const;
    void set_seed(std::uint64_t seed);                          // until run(); reseeds every world
    std::uint64_t seed() const;                                 // --seed or AppConfig::seed
    std::uint64_t tick() const;                                 // app tick
    const Time &time() const;                                   // tick == app_tick
    Rng &fx_rng();                                              // presentation only; never hashed
    // Holds ticks and resets the fixed clock. InputPlugin watches paused() to reset its latch.
    void set_paused(bool paused);
    bool paused() const;
    // Running
    // Runs finish, startup and the loop until quit; returns the exit code. Nothing is destroyed
    // until ~App, so state can still be read afterwards.
    int run();
    void run_frames(int frames);                                // tests: exact number of frames
    // For tests: `ticks` frames of one tick each, unless ticks are held (paused, startup assets).
    void step_ticks(int ticks);
    // Stops run() after the current frame and saves settings (not in headless).
    void quit(int exit_code = 0);
    bool quitting() const;                                      // quit() was called

private:
    friend class Ctx;
    friend struct detail::AppInternals;
    struct Impl;
    void add_plugin_erased(std::unique_ptr<Plugin> plugin);
    void register_event(TypeId id, EventScope scope, std::size_t capacity,
                        void (*add)(EventBus &bus, std::size_t capacity));
    void reserve_event_erased(TypeId id, std::size_t capacity,
                              void (*reserve)(EventBus &bus, std::size_t capacity));
    bool has_system(Stage stage, std::string_view name) const;
    // 0 = not registered, 1 = World, 2 = App.
    std::uint8_t event_scope_of(TypeId id) const {
        return id < event_scopes_.size() ? event_scopes_[id] : std::uint8_t{0};
    }
    std::unique_ptr<Impl> impl_;
    Resources resources_;
    EventBus events_;
    std::vector<std::uint8_t> event_scopes_;
};

namespace detail {
// Per-world state of App::align_pools<Lead, Follow...> (a World resource, inserted on first use).
template <class Lead, class... Follow> struct AlignPoolsState {
    std::array<std::uint32_t, 1 + sizeof...(Follow)> versions{};
    bool valid = false;
};
template <class Lead, class... Follow> void align_pools_system(Ctx &ctx) {
    World &w = ctx.world();
    using State = AlignPoolsState<Lead, Follow...>;
    State *st = w.resources().template try_get<State>();
    if (!st) st = &w.resources().template insert<State>();
    const std::array<std::uint32_t, 1 + sizeof...(Follow)> now{
        w.template pool_version<Lead>(), w.template pool_version<Follow>()...};
    if (st->valid && now == st->versions) return;
    w.template align<Lead, Follow...>();
    st->versions = {w.template pool_version<Lead>(), w.template pool_version<Follow>()...};
    st->valid = true;
}
// "align_pools<Lead,Follow...>" with component names; static storage.
template <class Lead, class... Follow> std::string_view align_pools_name() {
    static const std::string name = [] {
        std::string s = "align_pools<";
        s += ComponentRegistry::global().template info<Lead>().name;
        ((s += ",", s += ComponentRegistry::global().template info<Follow>().name), ...);
        s += ">";
        return s;
    }();
    return name;
}
}

// ---- Ctx templates -----------------------------------------------------------------------------
template <class T> T &Ctx::res() const {
    if (T *p = app_->resources().template try_get<T>()) return *p;
    detail::ctx_resource_fail(typeid(T).name(), true, world_->resources().template has<T>());
}
template <class T> T *Ctx::try_res() const { return app_->resources().template try_get<T>(); }
template <class T> T &Ctx::local() const {
    if (T *p = world_->resources().template try_get<T>()) return *p;
    detail::ctx_resource_fail(typeid(T).name(), false, app_->resources().template has<T>());
}
template <class T> T *Ctx::try_local() const { return world_->resources().template try_get<T>(); }
template <class T> void Ctx::emit(const T &e) const {
    switch (app_->event_scope_of(type_id<T>())) {
    case 1:
        if (Events<T> *ch = world_->events().template try_get<T>()) {
            ch->emit(e);
            return;
        }
        break;
    case 2: app_->events().template get<T>().emit(e); return;
    default: break;
    }
    detail::event_not_registered(typeid(T).name(), "Ctx::emit");
}
template <class T> bool Ctx::has_event() const {
    return app_->event_scope_of(type_id<T>()) != 0;
}
template <class T> bool Ctx::try_emit(const T &e) const {
    switch (app_->event_scope_of(type_id<T>())) {
    case 1:
        if (Events<T> *ch = world_->events().template try_get<T>()) {
            ch->emit(e);
            return true;
        }
        return false;
    case 2: app_->events().template get<T>().emit(e); return true;
    default: return false;
    }
}
template <class T> std::span<const T> Ctx::read() const {
    const auto s = static_cast<std::size_t>(stage_);
    switch (app_->event_scope_of(type_id<T>())) {
    case 1:
        if (const Events<T> *ch = world_->events().template try_get<T>())
            return ch->window(world_marks_->prev[s], world_marks_->cur[s]);
        break;
    case 2: return app_->events().template get<T>().window(app_marks_->prev[s], app_marks_->cur[s]);
    default: break;
    }
    detail::event_not_registered(typeid(T).name(), "Ctx::read");
}

// ---- App templates -----------------------------------------------------------------------------
template <class P, class... A> P &App::add_plugin(A &&...args) {
    static_assert(std::is_base_of_v<Plugin, P>, "add_plugin<P>: P must derive from lam::Plugin");
    auto owned = std::make_unique<P>(std::forward<A>(args)...);
    P &ref = *owned;
    add_plugin_erased(std::move(owned));
    return ref;
}
template <class Bundle> App &App::add_plugins(const Bundle &bundle) {
    bundle.build(*this);
    return *this;
}
template <class Lead, class... Follow> App &App::align_pools() {
    const std::string_view name = detail::align_pools_name<Lead, Follow...>();
    if (has_system(Stage::TickEnd, name)) return *this;
    SystemDesc d;
    d.stage = Stage::TickEnd;
    d.fn = &detail::align_pools_system<Lead, Follow...>;
    d.name = name;
    d.order = 1100;
    d.scope = SystemScope::PerWorld;
    return add_system(d);
}
template <class T> App &App::register_component() {
    (void)ComponentRegistry::global().template id<T>();
    return *this;
}
template <class T> App &App::add_event(EventScope scope, std::size_t capacity) {
    register_event(type_id<T>(), scope, capacity,
                   [](EventBus &bus, std::size_t cap) { (void)bus.template add<T>(cap); });
    return *this;
}
template <class T> App &App::reserve_event(std::size_t capacity) {
    reserve_event_erased(type_id<T>(), capacity, [](EventBus &bus, std::size_t cap) {
        if (Events<T> *ch = bus.template try_get<T>()) ch->reserve(cap);
    });
    return *this;
}
template <class T> bool App::has_event() const {
    return event_scope_of(type_id<T>()) != 0;
}
template <class T, class... A> T &App::insert_resource(A &&...args) {
    return resources_.template insert<T>(std::forward<A>(args)...);
}
template <class T> T &App::res() {
    if (T *p = resources_.template try_get<T>()) return *p;
    detail::ctx_resource_fail(typeid(T).name(), true, false);
}
template <class T> T *App::try_res() { return resources_.template try_get<T>(); }
}
