#pragma once
// Tilesets from Tiled .tsj or native .tileset.json. The image is packed into an atlas at finalize
// unless "atlas" is "none". Tile properties are listed in the tilemap README.
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <lam/core/asset_id.h>
#include <lam/core/id.h>
#include <lam/core/result.h>
#include <lam/math/vec2.h>
#include <lam/physics/tile_world.h>
#include <lam/render/texture.h>
namespace lam {
class App;
class AssetServer;
using TileId = std::uint32_t;                                   // Tiled GID: 0 = empty
inline constexpr TileId kTileFlipH = 0x80000000u;               // Tiled flip bits kept in a GID: horizontal,
inline constexpr TileId kTileFlipV = 0x40000000u;               // vertical,
inline constexpr TileId kTileFlipD = 0x20000000u;               // diagonal (transpose; applied first)
inline constexpr TileId kTileIdMask = 0x1FFFFFFFu;              // the GID without its flip bits
// Per-tile properties, by local id.
struct TileProps {
    TileShape shape = TileShape::Empty;                         // collision shape (fill_tile_world)
    std::uint8_t flags = 0;                                     // TileFlags (hazard, ladder, water, opaque)
    std::uint16_t cost = 10;                                    // pathfinding; 0 = impassable
    StrId kind;                                                 // game-defined kind ("wall", "door"); none = 0
    std::uint16_t anim_first = 0, anim_count = 0, anim_ms = 0;  // animated tiles (local ids)
};
// Square tiles cut from one image (or properties only), plus the regions draw_tiles uses.
struct Tileset {
    AssetId<Texture> texture;                                   // atlas page or standalone texture
    std::string image_path;                                     // relative to the tileset file; "" = props only
    IVec2 image_size;                                           // pixels (0 for a props-only tileset)
    int tile_size = 16;                                         // square tiles; others -> Error{Unsupported}
    int columns = 0, count = 0, margin = 0, spacing = 0;        // Tiled geometry: tiles per row, tiles, pixels
    std::vector<TileProps> props;                               // by local id
    std::vector<TextureRegion> regions;                         // by local id (finalize)
    ImageData pending_image;                                    // decode -> finalize; cleared
    StrId name;                                                 // Tiled "name" (informational)
    StrId atlas_group = str_id("default");                      // atlas group packed into at finalize
    bool use_atlas = true;                                      // false: "atlas": "none" (standalone texture)
    // Empty region when out of range or before finalize.
    TextureRegion region(std::uint32_t local) const;
    const TileProps &prop(std::uint32_t local) const;           // defaults if out of range
};
// Parses a Tiled .tsj. Non-square tiles, image collections, .tsx sources and non-consecutive
// animations are Unsupported.
Result<Tileset> parse_tiled_tileset(std::string_view text, std::string_view file_name);
// Parses a native .tileset.json. Strict: unknown keys are errors.
Result<Tileset> parse_native_tileset(std::string_view text, std::string_view file_name);
// Shared by the tileset and map loaders: packs pending_image into its atlas group (or a standalone
// texture) and fills `regions`. Fails if a tile lies outside the image.
Status finalize_tileset(Tileset &t, App &app, AssetServer &assets);
template <class T> class AssetLoader;                           // <lam/assets/asset_server.h>
// Loader for .tsj and .tileset.json, registered by TilemapPlugin.
std::unique_ptr<AssetLoader<Tileset>> make_tileset_loader();
}
