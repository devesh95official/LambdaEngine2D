// InputPlugin: resources, systems, --record-input and --replay. The raylib poll is in lam_input_rl.
#include <lam/input/input_plugin.h>

#include <lam/core/build_info.h>
#include <lam/core/log.h>
#include <lam/core/platform.h>
#include <lam/core/settings.h>
#include <lam/input/actions.h>
#include <lam/input/input_frame.h>
#include <lam/render/camera.h>
#include <lam/render/virtual_screen.h>

#include "detail/input_state.h"

namespace lam {
namespace {

std::uint32_t focus_id(const Ctx &ctx) {
    const World *w = ctx.app().focused_world();
    return w != nullptr ? w->id() : 0u;
}

// Once systems run with ctx.world() set to the focused world.
const CameraView *focused_view(const Ctx &ctx) {
    return ctx.focused() ? ctx.try_local<CameraView>() : nullptr;
}

void sys_input_frame_update(Ctx &ctx) {
    InputSystem &sys = ctx.res<InputSystem>();
    ActionMap &map = ctx.res<ActionMap>();
    FrameEnv env;
    env.layout = ctx.try_res<VirtualLayout>();
    env.view = focused_view(ctx);
    env.frame_dt = ctx.time().frame_dt;
    env.tick_dt = ctx.time().tick_dt;
    env.paused = ctx.app().paused();
    env.focus_id = focus_id(ctx);
    for (const LifecycleChanged &e : ctx.read<LifecycleChanged>())
        if (e.event == LifecycleEvent::FocusLost || e.event == LifecycleEvent::Background) env.lost_focus = true;
    detail::input_frame_update(sys, map, ctx.res<FrameInput>(), env);

    // Mirror changed bindings into SettingsStore; the App saves it on quit and on Background.
    detail::InputAccess::SysImpl &s = detail::InputAccess::sys(sys);
    if (!s.mirrored_once || s.mirrored_revision != map.revision()) {
        if (SettingsStore *settings = ctx.try_res<SettingsStore>()) detail::sync_settings(map, *settings);
        s.mirrored_revision = map.revision();
        s.mirrored_once = true;
    }
}

void sys_input_publish(Ctx &ctx) {
    detail::input_publish(ctx.res<InputSystem>(), ctx.res<ActionMap>(), ctx.res<ActionState>(), focused_view(ctx),
                          focus_id(ctx));
}

void sys_input_driver(Ctx &ctx) {
    InputSystem &sys = ctx.res<InputSystem>();
    InputDriver *driver = sys.driver();
    if (driver == nullptr) return;
    driver->fill(ctx, std::span<InputFrame, kMaxPlayers>(ctx.res<ActionState>().players));
}

void sys_input_record(Ctx &ctx) {
    InputSystem &sys = ctx.res<InputSystem>();
    InputRecorder *rec = sys.recorder();
    if (rec == nullptr) return;
    detail::InputAccess::SysImpl &s = detail::InputAccess::sys(sys);
    if (s.record_pending) {
        s.record_pending = false;
        // Record every player slot (~1.5 KiB/tick) in case someone joins mid-run.
        const ReplayHeader h =
            current_replay_header(ctx.app().seed(), ctx.app().config().tick_rate, kMaxPlayers);
        if (Status st = rec->open(s.record_path, h); !st) {
            LAM_LOG_ERROR("input: --record-input: {}", st.error().message);
            ctx.app().quit(1);
            return;
        }
        LAM_LOG_INFO("input: recording to {} ({} player(s))", s.record_path, static_cast<int>(h.players));
    }
    if (!rec->is_open()) return;
    rec->record(ctx.app().tick(), std::span<const InputFrame, kMaxPlayers>(ctx.res<ActionState>().players));
}

void add_once(App &app, Stage stage, SystemFn fn, std::string_view name, int order) {
    SystemDesc d;
    d.stage = stage;
    d.fn = fn;
    d.name = name;
    d.order = order;
    d.scope = SystemScope::Once;
    app.add_system(d);
}

} // namespace

void InputPlugin::build(App &app) {
    InputSystem &sys = app.insert_resource<InputSystem>();
    app.insert_resource<ActionMap>();
    app.insert_resource<ActionState>();
    app.insert_resource<FrameInput>();
    add_once(app, Stage::FrameBegin, &sys_input_frame_update, "input_frame_update", -4000);
    add_once(app, Stage::TickBegin, &sys_input_publish, "input_publish", -1500);
    add_once(app, Stage::TickBegin, &sys_input_driver, "input_driver", -1400);
    add_once(app, Stage::TickBegin, &sys_input_record, "input_record", -1300);

    const LaunchOptions &o = app.options();
    if (o.replay.empty()) return;
    auto opened = ReplayDriver::open(o.replay);
    if (!opened) {
        LAM_LOG_ERROR("input: --replay {}: {}", o.replay, opened.error().message);
        app.quit(1);
        return;
    }
    const ReplayHeader &h = (*opened)->header();
    if (Status ok = replay_compatible(h, app.config().tick_rate, o.replay_any_build); !ok) {
        LAM_LOG_ERROR("input: --replay {} refused: {}", o.replay, ok.error().message);
        app.quit(1);
        return;
    }
    if (o.replay_any_build && (h.build_id != build_id() || h.os != os_description()))
        LAM_LOG_WARN("input: replaying a file from build '{}' on '{}' (--replay-any-build); results may differ",
                     h.build_id, h.os);
    app.set_seed(h.seed);
    replay_ = std::move(*opened);
    sys.set_driver(replay_.get());
    LAM_LOG_INFO("input: replaying {} ({} ticks, seed {})", o.replay, replay_->tick_count(), h.seed);
}

void InputPlugin::finish(App &app) {
    ActionMap &map = app.res<ActionMap>();
    if (SettingsStore *settings = app.try_res<SettingsStore>()) {
        map.define_settings(*settings);
        (void)map.load(*settings);
    }
    const LaunchOptions &o = app.options();
    if (o.record_input.empty() || recorder_) return;
    // Opened by input_record at the first tick, so players assigned by startup systems count.
    recorder_ = std::make_unique<InputRecorder>();
    InputSystem &sys = app.res<InputSystem>();
    sys.set_recorder(recorder_.get());
    detail::InputAccess::SysImpl &s = detail::InputAccess::sys(sys);
    s.record_path = o.record_input;
    s.record_pending = true;
}

void InputPlugin::shutdown(App &app) {
    InputSystem *sys = app.try_res<InputSystem>();
    if (recorder_ && recorder_->is_open()) {
        if (Status s = recorder_->close(); !s) LAM_LOG_ERROR("input: --record-input: {}", s.error().message);
        else LAM_LOG_INFO("input: recorded {} ticks", recorder_->ticks_recorded());
    }
    if (recorder_) {
        if (sys != nullptr && sys->recorder() == recorder_.get()) sys->set_recorder(nullptr);
    }
    if (replay_ && sys != nullptr && sys->driver() == replay_.get()) sys->set_driver(nullptr);
    ActionMap *map = app.try_res<ActionMap>();
    SettingsStore *settings = app.try_res<SettingsStore>();
    if (map != nullptr && settings != nullptr) (void)map->save(*settings);
}

} // namespace lam
