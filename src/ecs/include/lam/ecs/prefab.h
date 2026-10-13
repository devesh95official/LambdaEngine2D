#pragma once
// Prefabs, compiled from JSON at load so spawning just copies bytes. "extends" copies a base and
// overrides fields; an Entity field may be a child index (0 = root). Asset fields resolve at load,
// so load prefabs after DefaultPlugins. A failed load changes nothing.
//   {"prefabs": {"goblin": {"extends": "enemy", "tags": ["Enemy"],
//     "components": {"Transform": {}, "Animator": {"sheet": "goblin.json", "clip": "idle"}},
//     "children": [{"prefab": "torch", "offset": [4, -6]}]}}}
#include <memory>
#include <span>
#include <string_view>
#include <lam/core/id.h>
#include <lam/core/result.h>
#include <lam/ecs/component.h>
#include <lam/ecs/entity.h>
#include <lam/math/vec2.h>
namespace lam {
class Commands;
class World;
// App resource shared by every world. Load on the main thread; spawning is const and cheap.
class PrefabLibrary {
public:
    PrefabLibrary();                                            // empty
    ~PrefabLibrary();
    PrefabLibrary(const PrefabLibrary &) = delete;
    PrefabLibrary &operator=(const PrefabLibrary &) = delete;
    // Reads through ctx.assets when set, else from disk.
    Status load(std::string_view path, const SerializeContext &ctx);
    // Same-named prefabs are replaced.
    Status load_text(std::string_view json_text, std::string_view file_name, const SerializeContext &ctx);
    bool has(StrId name) const;                                 // O(1)
    // Spawns now (not while iterating) and returns the root. Null for an unknown name.
    Entity spawn(World &w, StrId name, Vec2 position) const;
    // Same, recorded through `c` for an entity from c.spawn().
    void spawn_into(Commands &c, Entity e, StrId name, Vec2 position) const;
    std::span<const StrId> names() const;                       // load order
    void clear();                                               // forget every prefab
private:
    struct Impl;
    // World/Commands internals (PrefabLibrary is their friend; Impl reaches them through these).
    static void *add_to_world(World &w, Entity e, ComponentId id, void *src, bool move);
    static void *add_to_commands(Commands &c, Entity e, ComponentId id);
    std::unique_ptr<Impl> impl_;
};
}
