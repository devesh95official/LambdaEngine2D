#pragma once
// The whole rule set as one plugin: components, events, Controls and the tick systems, all gated
// on has_local<Arena>. Needs lam.input and lam.physics (add DefaultPlugins first).
#include <string_view>
#include <lam/core/app.h>
namespace shooter {
class ShooterSimPlugin final : public lam::Plugin {
public:
    std::string_view name() const override { return "game.shooter.sim"; }
    void build(lam::App &app) override;
};
}
