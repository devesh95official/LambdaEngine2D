#pragma once
// Physics plugin. System order is listed in src/physics/README.md.
#include <cstddef>
#include <string_view>
#include <lam/core/app.h>
#include <lam/math/vec2.h>
namespace lam {
// Read every tick, so it can be changed at run time. The capacities size each world's buffers;
// pairs past pair_capacity are dropped and counted in PhysicsStats.
struct PhysicsConfig {                                          // App resource
    Vec2 gravity{0, 0};                                         // Dynamic bodies; motors have their own
    float cell_size = 64;                                       // broadphase cell, px (> 0)
    std::size_t body_capacity = 16384;                          // colliders reserved per world
    std::size_t pair_capacity = 65536;                          // candidate pairs per tick
    int solver_iterations = 4;                                  // velocity passes; position gets twice that
};
// Adding RigidBody or a motor also adds Velocity.
class PhysicsPlugin final : public Plugin {
public:
    // Ignored if a PhysicsConfig was inserted before build().
    explicit PhysicsPlugin(PhysicsConfig c = {});
    std::string_view name() const override { return "lam.physics"; }
    void build(App &app) override;
    // Adds the per-world resources, keeping any already there. After a restore the grids are
    // rebuilt so the next tick matches the saved run.
    void init_world(App &app, World &world) override;
private:
    PhysicsConfig config_;
};
}
