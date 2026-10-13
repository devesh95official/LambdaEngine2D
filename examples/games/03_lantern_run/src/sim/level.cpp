#include "sim/level.h"

#include <algorithm>
#include <span>
#include <lam/core/id.h>
#include <lam/ecs/prefab.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/physics/tile_world.h>

#include "sim/components.h"

namespace lantern {

bool Level::solid_at(lam::IVec2 t) const {
    if (t.x < 0 || t.y < 0 || t.x >= width || t.y >= height) return true;
    return solid[static_cast<std::size_t>(t.y * width + t.x)] != 0;
}

lam::Result<Level> parse_level(std::string_view text) {
    std::vector<std::string_view> rows;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view row = text.substr(0, end);
        if (!row.empty() && row.back() == '\r') row.remove_suffix(1);
        rows.push_back(row);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    }
    while (!rows.empty() && rows.back().empty()) rows.pop_back();
    if (rows.empty()) return lam::make_error(lam::ErrorCode::Parse, "forest level: no rows");

    Level level;
    level.height = static_cast<int>(rows.size());
    for (std::string_view row : rows) level.width = std::max(level.width, static_cast<int>(row.size()));
    level.solid.assign(static_cast<std::size_t>(level.width * level.height), 0);
    bool has_start = false;
    for (int y = 0; y < level.height; ++y) {
        const std::string_view row = rows[static_cast<std::size_t>(y)];
        for (int x = 0; x < level.width; ++x) {
            // Short rows are padded with forest edge, so the map is always closed.
            const char c = x < static_cast<int>(row.size()) ? row[static_cast<std::size_t>(x)] : '#';
            const lam::IVec2 t{x, y};
            switch (c) {
            case 'T': level.trees.push_back(t); break;
            case 'R': level.rocks.push_back(t); break;
            case 'W': level.dens.push_back(t); break;
            case 'P':
                if (!has_start) level.start = t;
                has_start = true;
                break;
            default: break;
            }
            if (c == '#' || c == 'T' || c == 'R') level.solid[static_cast<std::size_t>(y * level.width + x)] = 1;
        }
    }
    // A level without a den gets one near the far corner.
    if (level.dens.empty()) level.dens.push_back({level.width - 3, level.height - 3});
    return level;
}

lam::Vec2 tile_feet(lam::IVec2 tile) {
    return {static_cast<float>(tile.x * kTile) + kTile * 0.5f, static_cast<float>(tile.y * kTile + kTile) - 1.0f};
}

void fill_tile_world(const Level &level, lam::TileWorld &out) {
    out.resize(level.width, level.height, kTile, lam::Vec2{});
    out.outside_solid = true;
    for (int y = 0; y < level.height; ++y) {
        for (int x = 0; x < level.width; ++x) {
            if (!level.solid_at({x, y})) continue;
            // Trees, rocks and the edge block movement, sight (kTileOpaque: can_see) and paths.
            out.set({x, y}, lam::TileShape::Solid, lam::kTileOpaque);
            out.cost(x, y) = 0;
        }
    }
}

namespace {

void spawn_scenery(lam::World &w, std::span<const lam::IVec2> cells, lam::StrId prefab, bool tree) {
    const lam::PrefabLibrary *prefabs = w.prefabs();
    for (const lam::IVec2 cell : cells) {
        const lam::Vec2 feet = tile_feet(cell);
        lam::Entity e;
        if (prefabs != nullptr && prefabs->has(prefab)) {
            e = prefabs->spawn(w, prefab, feet);                // sprite, canopy child, fade
        } else {
            e = w.create();
            w.add<lam::Transform>(e, lam::Transform{.position = feet});
        }
        if (tree) w.add<Tree>(e);
        else w.add<Rock>(e);
    }
}

} // namespace

lam::Status load_level(lam::World &w, std::string_view text) {
    LAM_TRY_ASSIGN(Level level, parse_level(text));
    lam::TileWorld *tiles = w.resources().try_get<lam::TileWorld>();
    if (tiles == nullptr) tiles = &w.resources().insert<lam::TileWorld>();   // no PhysicsPlugin (tests)
    fill_tile_world(level, *tiles);
    spawn_scenery(w, level.trees, lam::str_id("tree"), true);
    spawn_scenery(w, level.rocks, lam::str_id("rock"), false);
    if (Level *old = w.resources().try_get<Level>()) *old = std::move(level);
    else w.resources().insert<Level>(std::move(level));
    return {};
}

} // namespace lantern
