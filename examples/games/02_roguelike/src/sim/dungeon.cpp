// Floor generation: procgen.h layout -> set_tile into the TilemapInstance (which also updates the
// TileWorld cell), then stairs, monsters and items from the catalogs. Everything is drawn from a
// local Rng seeded with derive_seed(run_seed, depth), so floor N is the same whatever happened on
// the floors before it (and a replay or a loaded save regenerates nothing).
#include <cstdint>
#include <lam/core/log.h>
#include <lam/ecs/prefab.h>
#include <lam/ecs/transform.h>
#include <lam/math/rng.h>
#include <lam/tilemap/autotile.h>
#include <lam/tilemap/fov.h>
#include <lam/tilemap/grid_mover.h>
#include <lam/tilemap/procgen.h>
#include <lam/tilemap/tilemap.h>
#include <sim/data.h>
#include <sim/rules.h>
#include <sim/run.h>
#include <vector>

namespace rl {
namespace {
const lam::TileLayer *find_layer(const lam::TilemapInstance &inst, lam::StrId name) {
    for (const lam::TileLayer &l : inst.layers)
        if (l.name == name) return &l;
    return nullptr;
}

lam::Vec2 cell_center(const lam::World &w, lam::IVec2 cell) {
    const lam::TileWorld *tiles = w.resources().try_get<lam::TileWorld>();
    return tiles ? tiles->tile_center(cell)
                 : lam::Vec2{static_cast<float>(cell.x * 16 + 8),
                             static_cast<float>(cell.y * 16 + 8)};
}

// Weighted pick among rows whose depth range contains `depth`; kNone if nothing qualifies.
template <class Def, class Ok>
std::uint32_t pick_weighted(const lam::Catalog<Def> &table, lam::Rng &rng, Ok ok) {
    std::uint32_t total = 0;
    for (const Def &d : table.items())
        if (ok(d)) total += static_cast<std::uint32_t>(d.weight);
    if (total == 0) return lam::Catalog<Def>::kNone;
    std::uint32_t r = rng.below(total);
    for (std::uint32_t i = 0; i < table.size(); ++i) {
        const Def &d = table.at(i);
        if (!ok(d)) continue;
        const auto w = static_cast<std::uint32_t>(d.weight);
        if (r < w) return i;
        r -= w;
    }
    return lam::Catalog<Def>::kNone;
}

// A free floor cell at least `min_dist` steps (Manhattan) from `from`, or {-1, -1}.
lam::IVec2 free_cell(const std::vector<lam::IVec2> &floors, lam::Grid<std::uint8_t> &used,
                     lam::Rng &rng, lam::IVec2 from, int min_dist) {
    for (int attempt = 0; attempt < 64; ++attempt) {
        const lam::IVec2 c = floors[rng.below(static_cast<std::uint32_t>(floors.size()))];
        if (used(c.x, c.y) || lam::manhattan(c, from) < min_dist) continue;
        used(c.x, c.y) = 1;
        return c;
    }
    return {-1, -1};
}

lam::Entity spawn_at(lam::World &w, const lam::PrefabLibrary &prefabs, lam::StrId prefab,
                     lam::IVec2 cell) {
    if (!prefabs.has(prefab)) return {};
    const lam::Entity e = prefabs.spawn(w, prefab, cell_center(w, cell));
    lam::set_grid_position(w, e, cell, true); // cell + Transform + GridIndex
    return e;
}

void clear_floor_entities(lam::World &w) {
    std::vector<lam::Entity> doomed; // cold path (once per floor)
    for (auto [e, t] : w.view<lam::Transform>(lam::With<FloorLocal>{})) {
        (void)t;
        doomed.push_back(e);
    }
    for (lam::Entity e : doomed)
        w.destroy(e); // no View alive here
}
} // namespace

bool generate_floor(const lam::Ctx &ctx) {
    lam::World &w = ctx.world();
    RunState &run = ctx.local<RunState>();
    const lam::TilemapInstance *inst = w.try_get<lam::TilemapInstance>(run.map);
    if (!inst) return false; // tilemap_sync has not built it yet
    const lam::TileLayer *layer = find_layer(*inst, kLayer);
    if (!layer) {
        LAM_LOG_ERROR("roguelike: maps/floor.tmj has no tile layer named 'dungeon'");
        run.phase = Phase::Dead;
        return false;
    }
    const int width = layer->tiles.width(), height = layer->tiles.height();
    const std::uint64_t floor_seed = lam::derive_seed(run.run_seed, run.depth);
    lam::Rng rng(floor_seed);
    clear_floor_entities(w);

    // 1. Layout: rooms and corridors, every third floor a cave. 0 = wall, 1 = floor. Both
    // generators clear the grid and keep its border solid (procgen.h).
    lam::Grid<std::uint8_t> grid(width, height, 0);
    std::vector<lam::IRect> rooms;
    if (run.depth % 3 == 0) {
        lam::cellular_caves(grid, rng, 0.45f, 4);
        lam::connect_regions(grid, rng);
    } else {
        lam::bsp_rooms(grid, rng,
                       lam::BspParams{.min_room = 4, .max_room = 9, .depth = 5, .padding = 1},
                       rooms);
    }
    std::vector<lam::IVec2> floors;
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            if (grid(x, y) == 1) floors.push_back({x, y});
    if (floors.size() < 16) { // procgen produced nothing: one plain room
        LAM_LOG_WARN("roguelike: procgen gave {} floor cells; carving a fallback room",
                     floors.size());
        floors.clear();
        rooms.assign(1, lam::IRect{width / 2 - 6, height / 2 - 4, 12, 8});
        for (int y = rooms[0].y; y < rooms[0].y + rooms[0].h; ++y)
            for (int x = rooms[0].x; x < rooms[0].x + rooms[0].w; ++x) {
                grid(x, y) = 1;
                floors.push_back({x, y});
            }
    }

    // 2. Carve: set_tile edits this world's TilemapInstance (saved, hashed) and the TileWorld cell
    // (shape/flags/cost from the tileset's TileProps), never the shared map asset.
    const auto variant_seed = static_cast<std::uint32_t>(floor_seed);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const bool alt = lam::tile_hash(x, y, variant_seed) % 5 == 0; // deterministic variety
            const lam::TileId gid =
                grid(x, y) ? (alt ? kFloorAltGid : kFloorGid) : (alt ? kWallAltGid : kWallGid);
            if (lam::tile_at(*w.try_get<lam::TilemapInstance>(run.map), kLayer, {x, y}) != gid)
                lam::set_tile(w, run.map, kLayer, {x, y}, gid);
        }
    // Movement (try_step), FOV and the chase map read the TileWorld, which set_tile fills from
    // the tileset's per-tile properties (assets/maps/dungeon.tsj). Warn if they didn't take
    // effect; the property names are easy to get wrong.
    if (const lam::TileWorld *tiles = w.resources().try_get<lam::TileWorld>();
        tiles && (tiles->shapes.get_or({0, 0}, lam::TileShape::Empty) != lam::TileShape::Solid ||
                  !tiles->opaque({0, 0})))
        LAM_LOG_WARN("roguelike: wall tiles are not Solid+opaque in the TileWorld; check the "
                     "tile properties in maps/dungeon.tsj");

    // 3. Player start, then the stairs on the reachable floor cell farthest from it.
    lam::IVec2 start = floors[rng.below(static_cast<std::uint32_t>(floors.size()))];
    if (!rooms.empty()) {
        const lam::IVec2 c{rooms[0].x + rooms[0].w / 2, rooms[0].y + rooms[0].h / 2};
        if (grid.get_or(c, 0)) start = c;
    }
    lam::Grid<std::uint16_t> cost(width, height, 0);
    for (lam::IVec2 c : floors)
        cost(c.x, c.y) = 10;
    lam::DijkstraMap dist;
    dist.resize({width, height});
    const lam::IVec2 goals[1] = {start};
    dist.compute(cost, goals, lam::GridDirs::Four);
    lam::IVec2 stairs = start;
    std::uint32_t best = 0;
    for (lam::IVec2 c : floors) {
        const std::uint32_t d = dist.at(c);
        const std::uint32_t score = d == UINT32_MAX ? 0 : d; // unreachable cells never win
        if (score > best) best = score, stairs = c;
    }
    if (stairs == start) // no distances (or a 1-cell map)
        for (lam::IVec2 c : floors)
            if (lam::manhattan(c, start) > lam::manhattan(stairs, start)) stairs = c;
    lam::set_tile(w, run.map, kLayer, stairs, kStairsGid);
    run.stairs = stairs;
    lam::set_grid_position(w, run.player, start, true);

    // 4. Monsters and items from the catalogs, weighted by depth.
    const GameData &data = ctx.res<GameData>();
    const lam::PrefabLibrary &prefabs = ctx.res<lam::PrefabLibrary>();
    lam::Grid<std::uint8_t> used(width, height, 0);
    used(start.x, start.y) = used(stairs.x, stairs.y) = 1;
    const int depth = run.depth;
    const int monster_count = depth * 2 + 4 < 24 ? depth * 2 + 4 : 24;
    for (int i = 0; i < monster_count; ++i) {
        const lam::IVec2 cell = free_cell(floors, used, rng, start, 7);
        const std::uint32_t pick = pick_weighted(data.monsters, rng, [depth](const MonsterDef &d) {
            return depth >= d.min_depth && depth <= d.max_depth;
        });
        if (cell.x < 0 || pick == lam::Catalog<MonsterDef>::kNone) continue;
        const lam::Entity m = spawn_at(w, prefabs, data.monsters.at(pick).prefab, cell);
        if (Stats *st = m ? w.try_get<Stats>(m) : nullptr) {   // deeper = tougher
            const int extra_hp = st->max_hp * (depth - 1) / 8; // +1/8 hp per floor
            st->max_hp = static_cast<std::int16_t>(st->max_hp + extra_hp);
            st->hp = st->max_hp;
            st->attack = static_cast<std::int16_t>(st->attack + (depth - 1) / 4);
        }
    }
    const int item_count = 2 + static_cast<int>(rng.below(3));
    for (int i = 0; i < item_count; ++i) {
        const lam::IVec2 cell = free_cell(floors, used, rng, start, 2);
        const std::uint32_t pick = pick_weighted(
            data.items, rng, [depth](const ItemDef &d) { return depth >= d.min_depth; });
        if (cell.x < 0 || pick == lam::Catalog<ItemDef>::kNone) continue;
        spawn_at(w, prefabs, data.items.at(pick).prefab, cell);
    }

    // 5. Fresh memory for the new floor, then look around.
    lam::FovMemory &fov = ctx.local<lam::FovMemory>();
    fov.visible.resize(width, height, 0);
    fov.explored.resize(width, height, 0);
    ctx.local<ChaseMap>().valid = false;
    run.phase = Phase::Playing;
    run.floor_turns = 0;
    refresh_fov(w);
    lam::FixedString<96> line;
    lam::format_to(line, "Depth {}. {}", depth,
                   depth % 3 == 0 ? "Damp caves stretch out." : "Stone halls.");
    say(w, line.view(), lam::Color{160, 200, 255, 255});
    return true;
}

void generate_floor_system(lam::Ctx &ctx) {
    if (ctx.local<RunState>().phase == Phase::NeedFloor) generate_floor(ctx);
}
} // namespace rl
