// Scene stack. A fade is a small state machine in ticks: Out, the operation at full cover, Wait
// (for the new scene's assets), In. Fades only advance in apply_tick, or in apply while paused,
// so when an operation takes effect doesn't depend on frame pacing.
#include <lam/scene/scene.h>

#include <algorithm>
#include <utility>

#include <lam/core/assert.h>
#include <lam/core/log.h>

namespace lam {

// ---- Scene -------------------------------------------------------------------------------------

World &Scene::world() const {
    LAM_CHECK(world_ != nullptr, "Scene::world(): the scene is not entered (no world yet or exited)");
    return *world_;
}

AssetScope Scene::asset_scope() const {
    return scope_;
}

void Scene::add_system(App &app, Stage stage, SystemFn fn, std::string_view name, int order) const {
    LAM_CHECK(world_ != nullptr, "Scene::add_system: the scene is not entered");
    SystemDesc d;
    d.stage = stage;
    d.fn = fn;
    d.name = name;
    d.order = order;
    d.scope = stage == Stage::FrameEnd ? SystemScope::Once : SystemScope::PerWorld;
    d.only_world = world_->id();
    app.add_system(d);
}

// ---- SceneStack: queueing ----------------------------------------------------------------------

SceneStack::SceneStack() {
    scenes_.reserve(kMaxWorlds);
    released_.reserve(kMaxWorlds);
}

SceneStack::~SceneStack() = default;

void SceneStack::enqueue(OpKind kind, std::unique_ptr<Scene> s, std::optional<SceneMessage> m,
                         Transition t) {
    Op op;
    op.kind = kind;
    op.scene = std::move(s);
    op.message = m;
    op.transition = t;
    ops_.push_back(std::move(op));
}

void SceneStack::push(std::unique_ptr<Scene> s, Transition t) {
    LAM_ASSERT(s != nullptr, "SceneStack::push: null scene");
    if (!s) return;
    enqueue(OpKind::Push, std::move(s), std::nullopt, t);
}

void SceneStack::pop(std::optional<SceneMessage> result, Transition t) {
    enqueue(OpKind::Pop, nullptr, result, t);
}

void SceneStack::replace(std::unique_ptr<Scene> s, Transition t) {
    LAM_ASSERT(s != nullptr, "SceneStack::replace: null scene");
    if (!s) return;
    enqueue(OpKind::Replace, std::move(s), std::nullopt, t);
}

void SceneStack::reset(std::unique_ptr<Scene> s, Transition t) {
    enqueue(OpKind::Reset, std::move(s), std::nullopt, t);
}

// ---- SceneStack: queries -----------------------------------------------------------------------

bool SceneStack::transitioning() const {
    return phase_ != Phase::Idle;
}

std::size_t SceneStack::queued() const {
    return ops_.size() + (active_has_op_ ? 1u : 0u);
}

Scene *SceneStack::top() const {
    return scenes_.empty() ? nullptr : scenes_.back().scene.get();
}

std::size_t SceneStack::size() const {
    return scenes_.size();
}

Scene *SceneStack::at(std::size_t i) const {
    LAM_ASSERT(i < scenes_.size(), "SceneStack::at: index out of range");
    return i < scenes_.size() ? scenes_[i].scene.get() : nullptr;
}

float SceneStack::fade_alpha() const {
    const float ticks = static_cast<float>(active_.transition.ticks);
    switch (phase_) {
    case Phase::Idle:
        return 0.0f;
    case Phase::Out:
        return ticks > 0 ? std::clamp(static_cast<float>(elapsed_) / ticks, 0.0f, 1.0f) : 1.0f;
    case Phase::Wait:
        return 1.0f;
    case Phase::In:
        return ticks > 0 ? std::clamp(1.0f - static_cast<float>(elapsed_) / ticks, 0.0f, 1.0f)
                         : 0.0f;
    }
    return 0.0f;
}

Color SceneStack::fade_color() const {
    return active_.transition.color;
}

bool SceneStack::waiting_for_assets() const {
    return !scenes_.empty() && scenes_.back().waiting;
}

std::span<const WorldEntry> SceneStack::world_entries() const {
    return {entries_.data(), entry_count_};
}

// ---- SceneStack: applying ----------------------------------------------------------------------

void SceneStack::apply(App &app) {
    if (applying_) return; // a scene callback called apply(): the outer call continues the queue
    applying_ = true;
    update_waiting(app);
    drain(app);
    if (app.paused()) advance(app); // ticks are held: frames stand in for ticks
    sync_world_stack(app);
    applying_ = false;
}

void SceneStack::apply_tick(App &app) {
    if (applying_) return;
    applying_ = true;
    update_waiting(app);
    drain(app);
    advance(app);
    sync_world_stack(app);
    applying_ = false;
}

// Runs queued operations in FIFO order until one starts a fade (it then holds the rest).
void SceneStack::drain(App &app) {
    while (phase_ == Phase::Idle && !ops_.empty()) {
        Op op = std::move(ops_.front());
        ops_.pop_front();
        const bool fade = op.transition.kind == TransitionKind::Fade && op.transition.ticks > 0;
        if (!fade) {
            perform(app, op);
            continue;
        }
        active_ = std::move(op);
        active_has_op_ = true;
        phase_ = Phase::Out;
        elapsed_ = 0;
    }
}

// One step of the running fade. The operation runs at full cover; the fade-in starts once the new
// scene's assets are Ready (wait_for_assets) and ends after `ticks` more steps.
void SceneStack::advance(App &app) {
    switch (phase_) {
    case Phase::Idle:
        return;
    case Phase::Out:
        if (++elapsed_ < active_.transition.ticks) return;
        {
            Op op = std::move(active_);
            active_has_op_ = false;
            active_.transition = op.transition; // keep the colour and length for the fade-in
            active_.scene = nullptr;
            const bool entered = op.scene != nullptr;
            perform(app, op);
            elapsed_ = 0;
            update_waiting(app);
            phase_ = (entered && waiting_for_assets()) ? Phase::Wait : Phase::In;
        }
        return;
    case Phase::Wait:
        update_waiting(app);
        if (!waiting_for_assets()) phase_ = Phase::In;
        return;
    case Phase::In:
        if (++elapsed_ < active_.transition.ticks) return;
        phase_ = Phase::Idle;
        elapsed_ = 0;
        active_ = Op{};
        return;
    }
}

void SceneStack::perform(App &app, Op &op) {
    switch (op.kind) {
    case OpKind::Push:
        if (Scene *t = top()) t->covered(app);
        enter_scene(app, std::move(op.scene), op.transition.wait_for_assets);
        break;
    case OpKind::Pop:
        if (scenes_.empty()) {
            LAM_LOG_WARN("SceneStack::pop: the stack is empty");
            break;
        }
        exit_top(app);
        if (Scene *t = top()) {
            t->resumed(app, op.message ? &*op.message : nullptr);
            // As for enter(): records made in resumed() land before the next stage.
            if (t->entered()) t->world_->commands().apply();
        }
        break;
    case OpKind::Replace:
        if (!scenes_.empty()) exit_top(app);
        enter_scene(app, std::move(op.scene), op.transition.wait_for_assets);
        break;
    case OpKind::Reset:
        while (!scenes_.empty())
            exit_top(app);
        if (op.scene) enter_scene(app, std::move(op.scene), op.transition.wait_for_assets);
        break;
    }
    // Scopes end after the new scene entered, so assets both scenes load are never reloaded.
    release_scopes(app);
}

void SceneStack::enter_scene(App &app, std::unique_ptr<Scene> s, bool wait_for_assets) {
    LAM_CHECK(s != nullptr, "SceneStack: null scene");
    std::string_view name = s->name();
    if (name.empty()) name = "scene";
    World &w = app.create_world(name);
    used_ = true;
    AssetServer *assets = app.try_res<AssetServer>();
    AssetScope previous = kRootScope;
    s->world_ = &w;
    s->scope_ = kRootScope;
    if (assets) {
        previous = assets->current_scope();
        s->scope_ = assets->begin_scope();
        assets->set_current_scope(s->scope_);
    }
    Scene &scene = *s;
    Entry e;
    e.scene = std::move(s);
    e.waiting = wait_for_assets;
    scenes_.push_back(std::move(e));
    scene.enter(app, w);
    // Flush enter-time records now so the new world's first stage sees them, whichever boundary
    // applied the operation. The scene's scope is still current for the hooks.
    w.commands().apply();
    if (assets) assets->set_current_scope(previous);
    // Readiness is decided after enter() queued its loads (synchronous mode: Ready already).
    for (Entry &x : scenes_)
        if (x.scene.get() == &scene) x.waiting = wait_for_assets && !assets_ready(app, scene);
}

void SceneStack::exit_top(App &app) {
    Entry e = std::move(scenes_.back());
    scenes_.pop_back();
    Scene &s = *e.scene;
    if (s.world_ != nullptr) {
        World &w = *s.world_;
        s.exit(app, w);
        s.world_ = nullptr; // detached first: ScenePlugin::shutdown_world will not exit it again
        app.destroy_world(w); // queued until the running pass ends
    }
    s.world_ = nullptr;
    if (s.scope_ != kRootScope) released_.push_back(s.scope_);
    s.scope_ = kRootScope;
}

void SceneStack::release_scopes(App &app) {
    if (released_.empty()) return;
    if (AssetServer *assets = app.try_res<AssetServer>()) {
        for (const AssetScope sc : released_)
            assets->end_scope(sc);
        assets->collect_garbage();
    }
    released_.clear();
}

// A scene waits for the loads its own asset scope retains (directly or as dependencies), not for
// unrelated background loads of other scenes; synchronous mode is always ready.
bool SceneStack::assets_ready(App &app, const Scene &scene) const {
    const AssetServer *assets = app.try_res<AssetServer>();
    return assets == nullptr || assets->pending(scene.asset_scope()) == 0;
}

void SceneStack::update_waiting(App &app) {
    for (Entry &e : scenes_)
        if (e.waiting && assets_ready(app, *e.scene))
            e.waiting = false; // once Ready, a world is never held again
}

// Derives the world stack from the scenes (top down) and hands it to the App when it changed.
void SceneStack::sync_world_stack(App &app) {
    if (scenes_.empty() && phase_ != Phase::Idle) {
        // No world gets input during a transition, even with an empty stack.
        if (owns_stack_ && !used_) return;                      // already holding the App's stack, deaf
        std::array<WorldEntry, kMaxWorlds> next{};
        std::size_t n = 0;
        if (!used_) {
            for (const WorldEntry &e : app.world_stack())
                if (n < next.size()) next[n++] = WorldEntry{e.world, e.ticks, e.visible, false};
        }
        if (n == 0) next[n++] = WorldEntry{&app.main_world(), true, true, false};
        bool same = owns_stack_ && n == entry_count_;
        for (std::size_t i = 0; same && i < n; ++i)
            same = next[i].world == entries_[i].world && next[i].ticks == entries_[i].ticks &&
                   next[i].visible == entries_[i].visible && next[i].input == entries_[i].input;
        if (same) return;
        entries_ = next;
        entry_count_ = n;
        owns_stack_ = true;
        app.set_world_stack({entries_.data(), entry_count_});
        return;
    }
    if (scenes_.empty()) {
        if (owns_stack_) {
            app.set_world_stack({});
            owns_stack_ = false;
        }
        entry_count_ = 0;
        return;
    }
    std::array<WorldEntry, kMaxWorlds> next{};
    std::size_t n = 0;           // filled from the back of `next` (top first)
    bool hidden = false;         // a non-overlay scene is above
    bool held = false;           // a blocks_ticks scene is above
    bool deaf = phase_ != Phase::Idle;   // a blocks_input scene is above, or a transition runs
    bool dropped = false;
    for (std::size_t i = scenes_.size(); i-- > 0;) {
        const Entry &e = scenes_[i];
        const Scene &s = *e.scene;
        if (s.world_ == nullptr) continue;
        WorldEntry we;
        we.world = s.world_;
        we.visible = !hidden;
        we.ticks = !held && !e.waiting;
        we.input = !deaf;
        if (we.visible || we.ticks || we.input) {
            if (n < next.size())
                next[next.size() - 1 - n++] = we;
            else
                dropped = true;
        }
        hidden = hidden || !s.overlay();
        held = held || s.blocks_ticks();
        deaf = deaf || s.blocks_input();
    }
    // Bottom first.
    std::array<WorldEntry, kMaxWorlds> ordered{};
    for (std::size_t i = 0; i < n; ++i)
        ordered[i] = next[next.size() - n + i];
    bool same = owns_stack_ && n == entry_count_;
    for (std::size_t i = 0; same && i < n; ++i) {
        const WorldEntry &a = ordered[i];
        const WorldEntry &b = entries_[i];
        same = a.world == b.world && a.ticks == b.ticks && a.visible == b.visible &&
               a.input == b.input;
    }
    if (same) return;
    if (dropped) // logged when the stack changes, not every boundary
        LAM_LOG_WARN("SceneStack: more than {} scenes take part; the lowest are dropped", kMaxWorlds);
    entries_ = ordered;
    entry_count_ = n;
    owns_stack_ = true;
    app.set_world_stack({entries_.data(), entry_count_});
}

// ---- SceneStack: shutdown ----------------------------------------------------------------------

// The scene's world is being destroyed: exit the scene, drop it from the stack and end its
// asset scope. Runs before any Plugin::shutdown, so the AssetServer is still alive.
void SceneStack::exit_world(App &app, World &w) {
    for (std::size_t i = scenes_.size(); i-- > 0;) {
        if (scenes_[i].scene->world_ != &w) continue;
        Entry e = std::move(scenes_[i]);
        scenes_.erase(scenes_.begin() + static_cast<std::ptrdiff_t>(i));
        Scene &s = *e.scene;
        s.exit(app, w);
        s.world_ = nullptr;
        if (s.scope_ != kRootScope) released_.push_back(s.scope_);
        s.scope_ = kRootScope;
        release_scopes(app);
        // The world leaves the derived stack; the next sync hands the App the rest.
        std::size_t n = 0;
        for (std::size_t k = 0; k < entry_count_; ++k)
            if (entries_[k].world != &w) entries_[n++] = entries_[k];
        entry_count_ = n;
        return;
    }
}

void SceneStack::shutdown(App &app) {
    ops_.clear();
    active_ = Op{};
    active_has_op_ = false;
    phase_ = Phase::Idle;
    elapsed_ = 0;
    AssetServer *assets = app.try_res<AssetServer>();
    while (!scenes_.empty()) {
        Entry e = std::move(scenes_.back());
        scenes_.pop_back();
        Scene &s = *e.scene;
        if (s.world_ != nullptr) s.exit(app, *s.world_);
        s.world_ = nullptr;
        if (assets && s.scope_ != kRootScope) assets->end_scope(s.scope_);
        s.scope_ = kRootScope;
    }
    released_.clear();
    entry_count_ = 0;
    owns_stack_ = false;
}

// ---- ScenePlugin -------------------------------------------------------------------------------

namespace {

void scene_apply(Ctx &ctx) {
    if (SceneStack *s = ctx.try_res<SceneStack>()) s->apply(ctx.app());
}

void scene_apply_tick(Ctx &ctx) {
    if (SceneStack *s = ctx.try_res<SceneStack>()) s->apply_tick(ctx.app());
}

} // namespace

namespace detail {
// Defined in scene_fade.cpp (the only file that touches lam_render).
void scene_fade_system(Ctx &ctx);
} // namespace detail

void ScenePlugin::build(App &app) {
    if (!app.try_res<SceneStack>()) app.insert_resource<SceneStack>();
    SystemDesc d;
    d.scope = SystemScope::Once;
    d.stage = Stage::FrameBegin;
    d.fn = &scene_apply;
    d.name = "scene_apply";
    d.order = -2000;
    app.add_system(d);
    d.stage = Stage::TickBegin;
    d.fn = &scene_apply_tick;
    d.name = "scene_apply_tick";
    app.add_system(d);
    d.stage = Stage::RenderUI;
    d.fn = &detail::scene_fade_system;
    d.name = "scene_fade";
    d.order = 1000;
    app.add_system(d);
}

void ScenePlugin::shutdown_world(App &app, World &world) {
    if (SceneStack *s = app.try_res<SceneStack>()) s->exit_world(app, world);
}

void ScenePlugin::shutdown(App &app) {
    if (SceneStack *s = app.try_res<SceneStack>()) s->shutdown(app);
}

} // namespace lam
