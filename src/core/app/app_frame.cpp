// The frame loop: FrameBegin (Once < 0, PerWorld, Once >= 0), n fixed ticks (Once TickBegin,
// each ticking world through TickBegin..TickEnd, Once TickEnd), then Update, Render, RenderUI and
// FrameEnd. Also event windows and retention. No allocation in steady state.
#include <lam/core/app.h>

#include <algorithm>
#include <limits>

#include <lam/core/assert.h>
#include <lam/core/debug.h>
#include <lam/core/log.h>
#include <lam/core/platform.h>
#include <lam/core/profiler.h>
#include <lam/ecs/commands.h>

#include "detail/app_impl.h"

namespace lam::detail {
namespace {

constexpr std::size_t idx(Stage s) {
    return static_cast<std::size_t>(s);
}

// The tick (tick stages) or frame + 1 (frame stages) a pass of `s` belongs to.
std::uint64_t occurrence_of(const AppInternals::Impl &m, Stage s) {
    return is_tick_stage(s) ? m.app_tick : m.frames_done + 1;
}

// Starts a new read window for stage `s` the first time it runs in this occurrence.
// Windows never overlap (each starts at or after the previous end), so nothing is read twice.
void roll(StageMarks &marks, TickSkip *skip, const EventBus &bus, Stage s,
          std::uint64_t occurrence) {
    const std::size_t i = idx(s);
    if (marks.occurrence[i] == occurrence) return;
    marks.occurrence[i] = occurrence;
    const std::uint32_t seq = bus.sequence();
    std::uint32_t from = marks.cur[i];
    if (skip && skip->next_from[i] != 0) {
        from = std::max(from, skip->next_from[i]);
        skip->next_from[i] = 0;
    }
    marks.prev[i] = from;
    const std::uint32_t bit = 1u << i;
    if (skip && (skip->pending & bit) != 0) {
        // First run after the world could not tick: deliver what was pending before it stopped,
        // and continue after what was emitted meanwhile.
        skip->pending &= ~bit;
        marks.cur[i] = std::max(from, std::min(skip->skip_from, seq));
        skip->next_from[i] = std::max(marks.cur[i], skip->skip_to);
        return;
    }
    marks.cur[i] = seq;
}

void roll_world(WorldRecord &r, Stage s, std::uint64_t occurrence) {
    roll(r.marks, &r.skip, r.world->events(), s, occurrence);
}

// Oldest sequence any stage still needs, called at FrameBegin (only FrameBegin may be mid-run).
std::uint32_t oldest_needed(const StageMarks &marks, std::uint64_t frame_occurrence) {
    std::uint32_t needed = std::numeric_limits<std::uint32_t>::max();
    for (std::size_t i = 0; i < kStageCount; ++i) {
        const bool running = i == idx(Stage::FrameBegin) && marks.occurrence[i] == frame_occurrence;
        needed = std::min(needed, running ? marks.prev[i] : marks.cur[i]);
    }
    return needed;
}

void run_list(const std::vector<SystemEntry> &list, Ctx &ctx, std::uint32_t world_id,
              OrderRange range) {
    for (const SystemEntry &e : list) {
        const int order = e.desc.order;
        if (range == OrderRange::Negative && order >= 0) break;
        if (range == OrderRange::NonNegative && order < 0) continue;
        if (e.desc.only_world != 0 && e.desc.only_world != world_id) continue;
        if (e.desc.run_if && !e.desc.run_if(ctx)) continue;
        LAM_PROFILE_SCOPE(e.desc.name);
        e.desc.fn(ctx);
    }
}

void run_once(App &app, Stage s, OrderRange range) {
    AppInternals::Impl &m = AppInternals::impl(app);
    WorldRecord &r = AppInternals::ctx_record(app);
    const std::uint64_t occ = occurrence_of(m, s);
    roll_world(r, s, occ);
    roll(m.app_marks, nullptr, AppInternals::bus(app), s, occ);
    ++m.pass_depth;
    {
        LAM_PROFILE_SCOPE(stage_name(s));
        Ctx ctx = AppInternals::make_ctx(app, r, &m.app_marks, s, SystemScope::Once,
                                         r.world.get() == app.focused_world(), false);
        run_list(m.once[idx(s)], ctx, r.world->id(), range);
    }
    --m.pass_depth;
    r.world->commands().apply();
    if (m.pass_depth == 0) AppInternals::apply_pending_ops(app);
}

void run_world(App &app, Stage s, WorldRecord &r, bool focused) {
    AppInternals::Impl &m = AppInternals::impl(app);
    const std::uint64_t occ = occurrence_of(m, s);
    roll_world(r, s, occ);
    roll(m.app_marks, nullptr, AppInternals::bus(app), s, occ);
    ++m.pass_depth;
    {
        LAM_PROFILE_SCOPE(stage_name(s));
        Ctx ctx =
            AppInternals::make_ctx(app, r, &m.app_marks, s, SystemScope::PerWorld, focused, false);
        run_list(m.per_world[idx(s)], ctx, r.world->id(), OrderRange::All);
    }
    --m.pass_depth;
    r.world->commands().apply();
    if (m.pass_depth == 0) AppInternals::apply_pending_ops(app);
}

// True while ticks are held for startup assets: until pending() first reaches 0.
bool startup_assets_hold(AppInternals::Impl &m) {
    if (m.startup_assets_ready) return false;
    if (!m.config.wait_for_startup_assets || !m.resolver || m.resolver->pending() == 0) {
        m.startup_assets_ready = true;
        return false;
    }
    return true;
}

void warn_event_growth(AppInternals::Impl &m) {
    const std::uint64_t grown = event_grow_count();
    if (grown == m.grow_seen) return;
    m.grow_seen = grown;
    if constexpr (LAM_DEV) {
        LAM_LOG_WARN(
            "event channel grew past its capacity (raise the add_event capacity or call "
            "App::reserve_event); {} growths so far",
            grown);
    }
}

} // namespace

void AppInternals::start(App &app) {
    Impl &m = impl(app);
    if (!m.started) {
        finish_plugins(app); // finish() may still call set_seed
        m.started = true;
    }
    run_startup(app);
}

void AppInternals::run_startup(App &app) {
    Impl &m = impl(app);
    while (m.startup_done < m.startup.size()) {
        const StartupEntry e = m.startup[m.startup_done++];
        WorldRecord &r = *m.worlds.front();
        ++m.pass_depth;
        {
            LAM_PROFILE_SCOPE(e.name);
            Ctx ctx = make_ctx(app, r, &m.no_marks, Stage::FrameBegin, SystemScope::Once,
                               r.world.get() == app.focused_world(), true);
            e.fn(ctx);
        }
        --m.pass_depth;
        r.world->commands().apply();
        if (m.pass_depth == 0) {
            apply_pending_ops(app);
            if (!m.in_frame) apply_pending_destroys(app);
        }
    }
}

void AppInternals::frame(App &app, bool force_one_tick) {
    Impl &m = impl(app);
    LAM_CHECK(!m.in_frame && m.pass_depth == 0,
              "App frame re-entered (run_frames/step_ticks/run called from a system)");
    start(app);
    if (Profiler *p = resources(app).try_get<Profiler>()) set_current_profiler(p);
    apply_pending_ops(app);
    apply_pending_destroys(app);
    m.in_frame = true;

    // Frame time and the provisional tick count.
    const double now = m.platform->now_seconds();
    if (!m.have_last) { // first frame or a new platform
        m.have_last = true;
        m.last_now = now;
        m.start_now = now - m.app_time.real_seconds;
    }
    const double dt = std::max(0.0, now - m.last_now);
    m.last_now = now;
    const bool forced = force_one_tick || m.options.step || m.options.headless;
    int n = 0;
    if (m.paused) {
        m.clock.reset();
    } else if (forced) {
        n = 1;
    } else {
        n = m.clock.advance(dt);
    }
    m.app_time.frame = m.frames_done;
    // --step: frame time is one tick too, so cameras, tweens and particles keep pace with the
    // simulation and step screenshots are reproducible. Headless gets this from NullPlatform.
    m.app_time.frame_dt = m.options.step ? m.app_time.tick_dt : static_cast<float>(dt);
    m.app_time.real_seconds = now - m.start_now;
    m.app_time.ticks_this_frame = n;
    m.app_time.paused = m.paused;
    for (auto &rec : m.worlds) {
        Time &t = rec->time;
        t.frame = m.app_time.frame;
        t.frame_dt = m.app_time.frame_dt;
        t.real_seconds = m.app_time.real_seconds;
        t.ticks_this_frame = n;
        t.paused = m.paused;
        t.app_tick = m.app_tick;
        t.tick = rec->world->tick();
        rec->ticked_this_frame = false;
    }
    warn_event_growth(m);

    // ---- FrameBegin --------------------------------------------------------------------------
    const std::uint64_t frame_occ = m.frames_done + 1;
    bus(app).trim(oldest_needed(m.app_marks, frame_occ));
    run_once(app, Stage::FrameBegin, OrderRange::Negative);
    apply_pending_destroys(app);
    refresh_stack(app);
    for (std::size_t i = 0; i < m.active_count; ++i) {
        WorldRecord &r = *m.active_records[i];
        roll_world(r, Stage::FrameBegin, frame_occ);
        r.world->events().trim(oldest_needed(r.marks, frame_occ));
        run_world(app, Stage::FrameBegin, r, r.world.get() == app.focused_world());
    }
    for (auto &rec : m.worlds) { // worlds outside the stack
        const auto *begin = m.active_records.data();
        if (std::find(begin, begin + m.active_count, rec.get()) != begin + m.active_count) continue;
        rec->world->events().trim(oldest_needed(rec->marks, frame_occ));
    }
    run_once(app, Stage::FrameBegin, OrderRange::NonNegative);

    // ---- Ticks -------------------------------------------------------------------------------
    const bool held = n > 0 && startup_assets_hold(m);
    if (held) {
        n = 0;
        m.clock.reset();
    }
    m.app_time.ticks_this_frame = n;
    for (auto &rec : m.worlds)
        rec->time.ticks_this_frame = 0;
    for (int t = 0; t < n; ++t) {
        ++m.app_tick;
        m.app_time.tick = m.app_tick;
        m.app_time.app_tick = m.app_tick;
        for (auto &rec : m.worlds)
            rec->time.app_tick = m.app_tick;
        run_once(app, Stage::TickBegin, OrderRange::All);
        apply_pending_destroys(app);
        refresh_stack(app);
        for (std::size_t i = 0; i < m.active_count; ++i) {
            if (!m.active[i].ticks) continue;
            WorldRecord &r = *m.active_records[i];
            World &w = *r.world;
            const bool focused = &w == app.focused_world();
            w.advance_tick();
            r.time.tick = w.tick();
            ++r.time.ticks_this_frame;
            r.ticked_this_frame = true;
            for (std::size_t s = idx(Stage::TickBegin); s <= idx(Stage::TickEnd); ++s)
                run_world(app, static_cast<Stage>(s), r, focused);
        }
        run_once(app, Stage::TickEnd, OrderRange::All);
    }
    if (n > 0) {
        // App-scope windows of tick stages that no world ran (nothing ticked them) catch up.
        const std::uint32_t seq = bus(app).sequence();
        for (std::size_t s = idx(Stage::TickBegin); s <= idx(Stage::TickEnd); ++s) {
            if (m.app_marks.occurrence[s] == m.app_tick) continue;
            m.app_marks.prev[s] = seq;
            m.app_marks.cur[s] = seq;
        }
    }

    // ---- Presentation ------------------------------------------------------------------------
    const float clock_alpha = (forced || m.paused || held) ? 1.0f : m.clock.alpha();
    m.app_time.alpha = clock_alpha;
    for (auto &rec : m.worlds)
        rec->time.alpha = 1.0f;
    for (std::size_t i = 0; i < m.active_count; ++i)
        if (m.active[i].ticks) m.active_records[i]->time.alpha = clock_alpha;
    for (Stage s : {Stage::Update, Stage::Render, Stage::RenderUI}) {
        run_once(app, s, OrderRange::Negative);
        for (std::size_t i = 0; i < m.active_count; ++i) {
            if (!m.active[i].visible) continue;
            WorldRecord &r = *m.active_records[i];
            run_world(app, s, r, r.world.get() == app.focused_world());
        }
        run_once(app, s, OrderRange::NonNegative);
    }
    run_once(app, Stage::FrameEnd, OrderRange::All);

    // Catch-up. Frame stages that did not run on a world this frame skip what was emitted
    // meanwhile. Worlds that could not tick skip, for their tick stages, what was emitted while
    // they could not (a frame that ran 0 ticks for a ticking world loses nothing).
    for (auto &rec : m.worlds) {
        WorldRecord &r = *rec;
        const std::uint32_t seq = r.world->events().sequence();
        for (Stage s :
             {Stage::FrameBegin, Stage::Update, Stage::Render, Stage::RenderUI, Stage::FrameEnd}) {
            const std::size_t i = idx(s);
            if (r.marks.occurrence[i] == frame_occ) continue;
            r.marks.prev[i] = seq;
            r.marks.cur[i] = seq;
        }
        bool eligible = r.ticked_this_frame;
        for (std::size_t i = 0; i < m.active_count && !eligible; ++i)
            eligible = m.active_records[i] == &r && m.active[i].ticks;
        if (eligible) {
            if (r.skip.pending == 0) r.skip.eligible_end_seq = seq;
            continue;
        }
        if (r.skip.pending == 0) r.skip.skip_from = r.skip.eligible_end_seq;
        r.skip.skip_to = seq;
        for (std::size_t i = idx(Stage::TickBegin); i <= idx(Stage::TickEnd); ++i)
            r.skip.pending |= 1u << i;
    }

    m.in_frame = false;
    apply_pending_destroys(app);
    if (Profiler *p = resources(app).try_get<Profiler>()) p->end_frame();
    if (DebugOverlay *o = resources(app).try_get<DebugOverlay>()) o->clear_frame();
    ++m.frames_done;
}

} // namespace lam::detail
