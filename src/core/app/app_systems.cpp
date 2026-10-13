// System registration (allowed scopes, (order, registration index) order, changes queued during
// a pass), the schedule printout, and the lam_app engine systems.
#include <lam/core/app.h>

#include <algorithm>
#include <format>

#include <lam/core/assert.h>
#include <lam/core/log.h>
#include <lam/core/platform.h>
#include <lam/ecs/lifetime.h>
#include <lam/ecs/transform.h>

#include "detail/app_impl.h"

namespace lam {
namespace {

constexpr std::size_t idx(Stage s) {
    return static_cast<std::size_t>(s);
}

// Input..Animate are PerWorld only; FrameEnd is Once only.
bool scope_allowed(Stage s, SystemScope scope) {
    switch (s) {
    case Stage::Input:
    case Stage::Simulate:
    case Stage::Physics:
    case Stage::PostPhysics:
    case Stage::Animate:
        return scope == SystemScope::PerWorld;
    case Stage::FrameEnd:
        return scope == SystemScope::Once;
    default:
        return true;
    }
}

[[maybe_unused]] bool same_key(const SystemDesc &a, const SystemDesc &b) {
    return a.stage == b.stage && a.only_world == b.only_world && a.name == b.name;
}

const char *scope_name(SystemScope s) {
    return s == SystemScope::Once ? "Once" : "PerWorld";
}

// ---- lam_app engine systems ----------------------------------------------------------

void sys_platform_begin_frame(Ctx &ctx) {
    ctx.app().platform().begin_frame();
}

void sys_settings_autosave(Ctx &ctx);

void sys_copy_prev_transforms(Ctx &ctx) {
    copy_prev_transforms(ctx.world());
}

void sys_propagate_parents(Ctx &ctx) {
    propagate_parents(ctx.world());
}

void sys_lifetimes(Ctx &ctx) {
    tick_lifetimes(ctx.world());
}

void sys_settle_spawned(Ctx &ctx) {
    settle_spawned(ctx.world());
}

void sys_hash_log(Ctx &ctx) {
    App &app = ctx.app();
    const int every = app.options().hash_every;
    if (every <= 0 || app.tick() % static_cast<std::uint64_t>(every) != 0) return;
    for (const WorldEntry &e : app.world_stack()) {
        if (!e.ticks) continue;
        LAM_LOG_INFO("hash tick={} world={} {:016x}", app.tick(), id_display(e.world->name()),
                     e.world->state_hash());
    }
}

void sys_platform_end_frame(Ctx &ctx) {
    ctx.app().platform().end_frame();
}

} // namespace

std::string_view stage_name(Stage s) {
    switch (s) {
    case Stage::FrameBegin:
        return "FrameBegin";
    case Stage::TickBegin:
        return "TickBegin";
    case Stage::Input:
        return "Input";
    case Stage::Simulate:
        return "Simulate";
    case Stage::Physics:
        return "Physics";
    case Stage::PostPhysics:
        return "PostPhysics";
    case Stage::Animate:
        return "Animate";
    case Stage::TickEnd:
        return "TickEnd";
    case Stage::Update:
        return "Update";
    case Stage::Render:
        return "Render";
    case Stage::RenderUI:
        return "RenderUI";
    case Stage::FrameEnd:
        return "FrameEnd";
    case Stage::Count:
        break;
    }
    return "?";
}

App &App::add_system(Stage stage, SystemFn fn, std::string_view name, int order) {
    SystemDesc d;
    d.stage = stage;
    d.fn = fn;
    d.name = name;
    d.order = order;
    d.scope = stage == Stage::FrameEnd ? SystemScope::Once : SystemScope::PerWorld;
    return add_system(d);
}

App &App::add_system(const SystemDesc &desc) {
    Impl &m = *impl_;
    LAM_CHECK(desc.fn != nullptr, "add_system: fn is null");
    LAM_CHECK(desc.stage < Stage::Count, "add_system: invalid stage");
    if (!scope_allowed(desc.stage, desc.scope)) {
        LAM_LOG_ERROR("add_system('{}'): {} systems are not allowed in stage {}", desc.name,
                      scope_name(desc.scope), stage_name(desc.stage));
        LAM_CHECK(false, "add_system: scope not allowed in this stage (Input..Animate are PerWorld "
                         "only, FrameEnd is Once only)");
    }
    LAM_ASSERT(!desc.name.empty(), "add_system: a system needs a name (static storage)");
#if LAM_ENABLE_ASSERTS
    bool duplicate = false;
    for (const auto *lists : {&m.once, &m.per_world})
        for (const detail::SystemEntry &e : (*lists)[idx(desc.stage)])
            duplicate = duplicate || same_key(e.desc, desc);
    for (const detail::PendingOp &op : m.pending_ops)
        duplicate =
            duplicate || (op.kind == detail::PendingOp::Kind::Add && same_key(op.entry.desc, desc));
    if (duplicate)
        LAM_LOG_ERROR("add_system: '{}' is already registered in {}", desc.name,
                      stage_name(desc.stage));
    LAM_ASSERT(!duplicate, "add_system: duplicate system name in this stage (see log)");
#endif
    detail::SystemEntry e;
    e.desc = desc;
    e.reg = m.next_reg++;
    e.plugin = m.building.empty() ? std::string_view() : m.building.back();
    if (m.pass_depth > 0) {
        detail::PendingOp op;
        op.kind = detail::PendingOp::Kind::Add;
        op.entry = e;
        m.pending_ops.push_back(op);
        return *this;
    }
    detail::AppInternals::insert_system(*this, e);
    return *this;
}

App &App::add_startup(SystemFn fn, std::string_view name) {
    LAM_CHECK(fn != nullptr, "add_startup: fn is null");
    impl_->startup.push_back({fn, name});
    return *this;
}

void App::remove_systems_of(std::uint32_t world_id) {
    Impl &m = *impl_;
    if (world_id == 0) return;
    if (m.pass_depth > 0) {
        detail::PendingOp op;
        op.kind = detail::PendingOp::Kind::RemoveOf;
        op.world_id = world_id;
        m.pending_ops.push_back(op);
        return;
    }
    detail::AppInternals::remove_systems_now(*this, world_id);
}

bool App::has_system(Stage stage, std::string_view name) const {
    const Impl &m = *impl_;
    for (const auto *lists : {&m.once, &m.per_world})
        for (const detail::SystemEntry &e : (*lists)[idx(stage)])
            if (e.desc.name == name) return true;
    for (const detail::PendingOp &op : m.pending_ops)
        if (op.kind == detail::PendingOp::Kind::Add && op.entry.desc.stage == stage &&
            op.entry.desc.name == name)
            return true;
    return false;
}

std::string App::schedule_text() const {
    const Impl &m = *impl_;
    std::string out;
    auto emit = [&](const detail::SystemEntry &e) {
        out += std::format("{:<11} {:<8} {:>6}  {}", stage_name(e.desc.stage),
                           scope_name(e.desc.scope), e.desc.order, e.desc.name);
        out += std::format("  [{}]", e.plugin.empty() ? std::string_view("game") : e.plugin);
        if (e.desc.only_world != 0) out += std::format(" world={}", e.desc.only_world);
        if (e.desc.run_if) out += " run_if";
        out += '\n';
    };
    auto emit_once = [&](Stage s, detail::OrderRange range) {
        for (const detail::SystemEntry &e : m.once[idx(s)]) {
            if (range == detail::OrderRange::Negative && e.desc.order >= 0) continue;
            if (range == detail::OrderRange::NonNegative && e.desc.order < 0) continue;
            emit(e);
        }
    };
    auto emit_world = [&](Stage s) {
        for (const detail::SystemEntry &e : m.per_world[idx(s)])
            emit(e);
    };
    for (std::size_t i = 0; i < detail::kStageCount; ++i) {
        const auto s = static_cast<Stage>(i);
        switch (s) {
        case Stage::TickBegin:
            emit_once(s, detail::OrderRange::All);
            emit_world(s);
            break;
        case Stage::TickEnd:
            emit_world(s);
            emit_once(s, detail::OrderRange::All);
            break;
        default:
            emit_once(s, detail::OrderRange::Negative);
            emit_world(s);
            emit_once(s, detail::OrderRange::NonNegative);
            break;
        }
    }
    return out;
}

namespace detail {

void AppInternals::insert_system(App &app, const SystemEntry &e) {
    Impl &m = impl(app);
    auto &list = e.desc.scope == SystemScope::Once ? m.once[idx(e.desc.stage)]
                                                   : m.per_world[idx(e.desc.stage)];
    const auto pos = std::upper_bound(
        list.begin(), list.end(), e, [](const SystemEntry &a, const SystemEntry &b) {
            return a.desc.order != b.desc.order ? a.desc.order < b.desc.order : a.reg < b.reg;
        });
    list.insert(pos, e);
}

void AppInternals::remove_systems_now(App &app, std::uint32_t world_id) {
    Impl &m = impl(app);
    for (auto *lists : {&m.once, &m.per_world})
        for (auto &list : *lists)
            std::erase_if(
                list, [world_id](const SystemEntry &e) { return e.desc.only_world == world_id; });
}

void AppInternals::apply_pending_ops(App &app) {
    Impl &m = impl(app);
    if (m.pending_ops.empty()) return;
    // Index loop: nothing below runs systems, so the queue cannot grow while we walk it.
    for (std::size_t i = 0; i < m.pending_ops.size(); ++i) {
        const PendingOp &op = m.pending_ops[i];
        if (op.kind == PendingOp::Kind::Add) {
            insert_system(app, op.entry);
        } else {
            remove_systems_now(app, op.world_id);
        }
    }
    m.pending_ops.clear();
}

void AppInternals::register_engine_systems(App &app) {
    Impl &m = impl(app);
    m.building.push_back("lam.app");
    auto add = [&app](Stage stage, SystemFn fn, std::string_view name, int order,
                      SystemScope scope) {
        SystemDesc d;
        d.stage = stage;
        d.fn = fn;
        d.name = name;
        d.order = order;
        d.scope = scope;
        app.add_system(d);
    };
    add(Stage::FrameBegin, &sys_platform_begin_frame, "platform_begin_frame", -10000,
        SystemScope::Once);
    add(Stage::FrameBegin, &sys_settings_autosave, "settings_autosave", -9500, SystemScope::Once);
    add(Stage::TickBegin, &sys_copy_prev_transforms, "copy_prev_transforms", -2000,
        SystemScope::PerWorld);
    add(Stage::Physics, &sys_propagate_parents, "propagate_parents", -1900, SystemScope::PerWorld);
    add(Stage::TickEnd, &sys_propagate_parents, "propagate_parents", -1000, SystemScope::PerWorld);
    add(Stage::TickEnd, &sys_lifetimes, "lifetimes", -900, SystemScope::PerWorld);
    add(Stage::TickEnd, &sys_settle_spawned, "settle_spawned", 1000, SystemScope::PerWorld);
    add(Stage::TickEnd, &sys_hash_log, "hash_log", 1000, SystemScope::Once);
    add(Stage::FrameEnd, &sys_platform_end_frame, "platform_end_frame", 10000, SystemScope::Once);
    app.align_pools<Transform, PrevTransform>();
    m.building.pop_back();
}

} // namespace detail

namespace {
void sys_settings_autosave(Ctx &ctx) {
    for (const LifecycleChanged &e : ctx.read<LifecycleChanged>()) {
        if (e.event != LifecycleEvent::Background) continue;
        detail::AppInternals::save_settings(ctx.app());
        return;
    }
}
} // namespace

} // namespace lam
