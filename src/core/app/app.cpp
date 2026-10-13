// App construction, resources, services, settings, seeds, pause and run/quit. The frame loop
// itself is in app_frame.cpp.
#include <lam/core/app.h>

#include <algorithm>
#include <cstdlib>
#include <utility>

#include <lam/core/assert.h>
#include <lam/core/debug.h>
#include <lam/core/fs.h>
#include <lam/core/log.h>
#include <lam/core/platform.h>
#include <lam/core/profiler.h>
#include <lam/core/settings.h>
#include <lam/ecs/prefab.h>
#include <lam/ecs/transform.h>

#include "detail/app_impl.h"

namespace lam {
namespace {
// Seed stream of App::fx_rng (never hashed or snapshotted).
constexpr std::uint64_t kFxStream = str_id("lam.fx").value;

AppConfig sanitized(AppConfig c) {
    c.tick_rate = std::max(1, c.tick_rate);
    c.max_ticks_per_frame = std::max(1, c.max_ticks_per_frame);
    if (!(c.max_frame_seconds > 0)) c.max_frame_seconds = 0.25;
    return c;
}
} // namespace

App::Impl::Impl(AppConfig c, LaunchOptions o)
    : config(sanitized(std::move(c))), options(std::move(o)),
      clock(config.tick_rate, config.max_frame_seconds, config.max_ticks_per_frame) {}

App::App(AppConfig config, int argc, const char *const *argv)
    : impl_(std::make_unique<Impl>(std::move(config), parse_launch_options(argc, argv))) {
    Impl &m = *impl_;
    m.seed = m.options.seed != 0 ? m.options.seed : m.config.seed;
    m.fx_rng.reseed(derive_seed(m.seed, kFxStream));
    m.platform = std::make_unique<NullPlatform>(m.config.tick_rate);
    m.app_time.tick_rate = m.config.tick_rate;
    m.app_time.tick_dt = 1.0f / static_cast<float>(m.config.tick_rate);
    for (auto &list : m.once)
        list.reserve(32);
    for (auto &list : m.per_world)
        list.reserve(32);
    m.pending_ops.reserve(16);
    m.pending_destroys.reserve(kMaxWorlds);
    m.worlds.reserve(16);

    // App resources. Scripted runs don't load or save the player's settings, so their outcome
    // can't depend on them and bots leave the data folder alone.
    SettingsStore &settings = resources_.insert<SettingsStore>();
    if (!m.options.scripted() && !m.config.settings_file.empty()) {
        Result<std::string> dir = user_data_dir(m.config.title);
        if (dir) {
            m.settings_path = join_path(*dir, m.config.settings_file);
            if (Status s = settings.load(m.settings_path); !s)
                LAM_LOG_WARN("settings: {}", s.error().message);
        } else {
            LAM_LOG_WARN("settings: {}", dir.error().message);
        }
    }
    resources_.insert<PrefabLibrary>();
    detail::set_current_profiler(&resources_.insert<Profiler>());
    resources_.insert<DebugOverlay>();

    // Engine components, events and systems.
    register_engine_components();
    add_event<WorldRestored>(EventScope::World, 8);
    add_event<LifecycleChanged>(EventScope::App, 8);
    add_event<WindowResized>(EventScope::App, 8);
    detail::AppInternals::create_record(*this, "main");
    detail::AppInternals::refresh_stack(*this);
    detail::AppInternals::register_engine_systems(*this);
}

App::~App() {
    Impl &m = *impl_;
    // Plugins see every world, then themselves, in reverse build order.
    for (std::size_t w = m.worlds.size(); w-- > 0;) {
        World &world = *m.worlds[w]->world;
        for (std::size_t p = m.plugins.size(); p-- > 0;)
            m.plugins[p]->shutdown_world(*this, world);
    }
    for (std::size_t p = m.plugins.size(); p-- > 0;)
        m.plugins[p]->shutdown(*this);
    m.shut_down = true;
    m.active_count = 0;
    m.requested_count = 0;
    while (!m.worlds.empty())
        m.worlds.pop_back();
    if (Profiler *p = resources_.try_get<Profiler>(); p && detail::current_profiler() == p)
        detail::set_current_profiler(nullptr);
    resources_.clear();
    while (!m.plugins.empty())
        m.plugins.pop_back();
    m.platform.reset();
}

// ---- Plugins -----------------------------------------------------------------------------------

void App::add_plugin_erased(std::unique_ptr<Plugin> plugin) {
    Impl &m = *impl_;
    LAM_CHECK(plugin != nullptr, "add_plugin: null plugin");
    const std::string_view name = plugin->name();
    if (has_plugin(name)) {
        LAM_LOG_ERROR("add_plugin: plugin '{}' was already added", name);
        LAM_CHECK(false, "add_plugin: duplicate plugin name (see log)");
    }
    Plugin &p = *plugin;
    m.building.push_back(name);
    p.build(*this);
    // Worlds that exist now (a world created inside build() is included; one created inside
    // init_world() already ran this plugin's init_world through create_world()).
    m.plugins.push_back(std::move(plugin));
    const std::size_t world_count = m.worlds.size();
    for (std::size_t i = 0; i < world_count && i < m.worlds.size(); ++i)
        p.init_world(*this, *m.worlds[i]->world);
    m.building.pop_back();
    if (m.finished && !m.finishing) p.finish(*this);
}

bool App::has_plugin(std::string_view name) const {
    const Impl &m = *impl_;
    for (const auto &p : m.plugins)
        if (p->name() == name) return true;
    return std::find(m.building.begin(), m.building.end(), name) != m.building.end();
}

void App::require_plugin(std::string_view needed, std::string_view by) {
    impl_->requirements.emplace_back(std::string(needed), std::string(by));
}

namespace detail {

void AppInternals::finish_plugins(App &app) {
    Impl &m = impl(app);
    if (m.finished) return;
    m.finished = true;
    m.finishing = true;
    for (std::size_t i = 0; i < m.plugins.size(); ++i)
        m.plugins[i]->finish(app);
    m.finishing = false;
    bool missing = false;
    for (const auto &[needed, by] : m.requirements) {
        if (app.has_plugin(needed)) continue;
        LAM_LOG_ERROR("plugin '{}' requires plugin '{}', which was never added", by, needed);
        missing = true;
    }
    LAM_CHECK(!missing, "App::run: a required plugin is missing (see log)");
}

void AppInternals::save_settings(App &app) {
    Impl &m = impl(app);
    if (m.settings_path.empty()) return;
    if (Status s = app.resources_.get<SettingsStore>().save(m.settings_path); !s)
        LAM_LOG_WARN("settings: {}", s.error().message);
}

} // namespace detail

// ---- Data --------------------------------------------------------------------------------------

void App::register_event(TypeId id, EventScope scope, std::size_t capacity,
                         void (*add)(EventBus &bus, std::size_t capacity)) {
    Impl &m = *impl_;
    const std::uint8_t code = scope == EventScope::App ? 2 : 1;
    if (id >= event_scopes_.size()) event_scopes_.resize(id + 1, 0);
    if (event_scopes_[id] != 0) {
        LAM_ASSERT(event_scopes_[id] == code,
                   "add_event<T>: already registered with another EventScope");
        return;
    }
    event_scopes_[id] = code;
    const std::size_t cap = std::max<std::size_t>(capacity, 1) *
                            static_cast<std::size_t>(m.config.max_ticks_per_frame + 1);
    if (scope == EventScope::App) {
        add(events_, cap);
        return;
    }
    m.world_events.push_back({id, cap, add});
    for (auto &r : m.worlds)
        add(r->world->events(), cap);
}

void App::reserve_event_erased(TypeId id, std::size_t capacity,
                               void (*reserve)(EventBus &bus, std::size_t capacity)) {
    Impl &m = *impl_;
    const std::uint8_t code = event_scope_of(id);
    LAM_ASSERT(code != 0, "reserve_event<T>: event type not registered; call add_event<T>() first");
    if (code == 0) return;
    const std::size_t cap = std::max<std::size_t>(capacity, 1) *
                            static_cast<std::size_t>(m.config.max_ticks_per_frame + 1);
    if (code == 2) {
        reserve(events_, cap);
        return;
    }
    for (detail::EventRegistration &ev : m.world_events)
        if (ev.id == id) ev.capacity = std::max(ev.capacity, cap);   // worlds created later
    for (auto &r : m.worlds)
        reserve(r->world->events(), cap);
}

Resources &App::resources() {
    return resources_;
}

EventBus &App::events() {
    return events_;
}

// ---- Services ----------------------------------------------------------------------------------

void App::set_platform(std::unique_ptr<Platform> platform) {
    LAM_ASSERT(platform != nullptr, "App::set_platform: null platform");
    if (!platform) return;
    impl_->platform = std::move(platform);
    impl_->have_last = false; // new clock origin
}

Platform &App::platform() {
    return *impl_->platform;
}

void App::set_asset_resolver(AssetResolver *resolver) {
    impl_->resolver = resolver;
}

AssetResolver *App::asset_resolver() const {
    return impl_->resolver;
}

SerializeContext App::serialize_context() const {
    return SerializeContext{impl_->resolver, {}};
}

const AppConfig &App::config() const {
    return impl_->config;
}

const LaunchOptions &App::options() const {
    return impl_->options;
}

std::string App::data_dir() const {
    const Impl &m = *impl_;
    const char *env = std::getenv("LAM_DATA_DIR");
    if (m.options.scripted() && !(env && *env)) return {};
    Result<std::string> dir = user_data_dir(m.config.title);   // honours LAM_DATA_DIR; creates
    if (!dir) {
        LAM_LOG_WARN("data_dir: {}", dir.error().message);
        return {};
    }
    return std::move(*dir);
}

void App::set_seed(std::uint64_t seed) {
    Impl &m = *impl_;
    LAM_ASSERT(!m.started, "App::set_seed: only allowed before run()");
    m.seed = seed;
    m.fx_rng.reseed(derive_seed(seed, kFxStream));
    for (auto &r : m.worlds)
        r->world->reseed(derive_seed(seed, r->stream));
}

std::uint64_t App::seed() const {
    return impl_->seed;
}

std::uint64_t App::tick() const {
    return impl_->app_tick;
}

const Time &App::time() const {
    return impl_->app_time;
}

Rng &App::fx_rng() {
    return impl_->fx_rng;
}

void App::set_paused(bool paused) {
    Impl &m = *impl_;
    if (m.paused == paused) return;
    m.paused = paused;
    m.clock.reset();
    m.app_time.paused = paused;
    for (auto &r : m.worlds)
        r->time.paused = paused;
}

bool App::paused() const {
    return impl_->paused;
}

// ---- Running -----------------------------------------------------------------------------------

int App::run() {
    Impl &m = *impl_;
    detail::AppInternals::finish_plugins(*this);
    if (m.options.print_schedule) {
        LAM_LOG_INFO("schedule:\n{}", schedule_text());
        m.quitting = true;
        return 0;
    }
    detail::AppInternals::start(*this);
    int frames = 0;
    while (!m.quitting) {
        if (m.platform->should_close()) {
            quit(0);
            break;
        }
        detail::AppInternals::frame(*this, false);
        ++frames;
        if (m.options.frames > 0 && frames >= m.options.frames) quit(0);
    }
    return m.exit_code;
}

void App::run_frames(int frames) {
    detail::AppInternals::start(*this);
    for (int i = 0; i < frames; ++i)
        detail::AppInternals::frame(*this, false);
}

void App::step_ticks(int ticks) {
    detail::AppInternals::start(*this);
    for (int i = 0; i < ticks; ++i)
        detail::AppInternals::frame(*this, true);
}

void App::quit(int exit_code) {
    Impl &m = *impl_;
    m.quitting = true;
    m.exit_code = exit_code;
    detail::AppInternals::save_settings(*this);
}

bool App::quitting() const {
    return impl_->quitting;
}

} // namespace lam
