#include "trail_map.h"

#include <array>
#include <string>
#include <vector>
#include <lam/core/id.h>
#include <lam/tilemap/tilemap.h>
#include "trail_components.h"

namespace trail {

lam::Result<Orchard> parse_orchard(std::string_view text, std::string_view file_name) {
    lam::Result<lam::TilemapAsset> parsed = lam::parse_native_map(text, file_name);
    if (!parsed) return parsed.error();
    return orchard_from_map(*parsed, file_name);
}

lam::Result<Orchard> orchard_from_map(const lam::TilemapAsset &map, std::string_view file_name) {
    if (map.width <= 0 || map.height <= 0)
        return lam::Error{lam::ErrorCode::Invalid, std::string(file_name) + ": empty board"};

    // Embedded tilesets only (the native terrain set): sets[i] belongs to map.tilesets[i].
    std::array<const lam::Tileset *, lam::kMaxMapTilesets> sets{};
    for (std::size_t i = 0; i < map.tilesets.size() && i < sets.size(); ++i) {
        const std::int32_t e = map.tilesets[i].embedded;
        if (e >= 0 && static_cast<std::size_t>(e) < map.embedded_tilesets.size())
            sets[i] = &map.embedded_tilesets[static_cast<std::size_t>(e)];
    }
    Orchard orchard;
    lam::fill_tile_world(map, map.layers, std::span<const lam::Tileset *const>(sets.data(), map.tilesets.size()),
                         lam::Vec2{0, 0}, orchard.tiles);

    for (const lam::MapObject &o : map.objects) {
        if (o.type == lam::str_id("start")) {
            orchard.start = orchard.tiles.tile_at(o.rect.center());   // native objects: rect = cells * tile_size
            break;
        }
    }
    if (orchard.start.x < 0) {
        if (const std::uint8_t p = lam::terrain_value(map, lam::str_id("P")); p != 0) {
            std::vector<lam::IVec2> cells;                      // cold path (once per load)
            lam::find_cells(map.terrain, p, cells);
            if (!cells.empty()) orchard.start = cells.front();
        }
    }
    if (orchard.start.x < 0) orchard.start = {orchard.width() / 2, orchard.height() / 2};
    if (orchard.wall(orchard.start))
        return lam::Error{lam::ErrorCode::Invalid, std::string(file_name) + ": the start cell is a wall"};
    return orchard;
}

} // namespace trail
