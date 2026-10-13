// Ctx accessors and the cold-path failures behind Ctx::res/local/emit/read (the message names
// the right call).
#include <lam/core/app.h>

#include <cstdio>

#include <lam/core/assert.h>
#include <lam/core/log.h>

namespace lam {

App &Ctx::app() const {
    return *app_;
}
World &Ctx::world() const {
    return *world_;
}
Commands &Ctx::commands() const {
    return world_->commands();
}
const Time &Ctx::time() const {
    return *time_;
}
bool Ctx::focused() const {
    return focused_;
}
Stage Ctx::stage() const {
    return stage_;
}
SystemScope Ctx::scope() const {
    return scope_;
}

Rng &Ctx::rng(StrId stream) const {
    LAM_ASSERT(is_tick_stage(stage_) || startup_,
               "Ctx::rng() is for tick stages (and startup) only; presentation uses ctx.fx_rng()");
    return world_->rng(stream);
}

Rng &Ctx::fx_rng() const {
    return app_->fx_rng();
}

namespace detail {

void ctx_resource_fail(const char *type_name, bool wanted_app, bool in_other_scope) {
    char buf[512];
    if (wanted_app && in_other_scope) {
        std::snprintf(
            buf, sizeof buf,
            "res<%s>(): this is a World resource, use ctx.local<T>() (or world.resources())",
            type_name);
    } else if (!wanted_app && in_other_scope) {
        std::snprintf(buf, sizeof buf,
                      "local<%s>(): this is an App resource, use ctx.res<T>() (or app.res<T>())",
                      type_name);
    } else if (wanted_app) {
        std::snprintf(buf, sizeof buf,
                      "res<%s>(): App resource missing; insert it with app.insert_resource<T>() in "
                      "a plugin build() (or add the plugin that provides it)",
                      type_name);
    } else {
        std::snprintf(buf, sizeof buf,
                      "local<%s>(): World resource missing on this world; insert it in "
                      "Plugin::init_world() (or guard the system with run_if = has_local<T>)",
                      type_name);
    }
    assert_fail(__FILE__, __LINE__, "resource present", buf);
}

void event_not_registered(const char *type_name, const char *where) {
    char buf[384];
    std::snprintf(buf, sizeof buf,
                  "%s<%s>: event type not registered; call app.add_event<T>() in a plugin build()",
                  where, type_name);
    assert_fail(__FILE__, __LINE__, "event registered", buf);
}

} // namespace detail
} // namespace lam
