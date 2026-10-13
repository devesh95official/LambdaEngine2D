#pragma once
// Tile maps from Tiled .tmj or native .map.json. tilemap_sync copies a map's layers into a
// per-world TilemapInstance, fills TileWorld and spawns objects as prefabs; set_tile and set_layer
// edit the instance, never the shared asset.
//
// Tiled: orthogonal, finite, square tiles. Layer properties "collision" and "render_layer" are
// read, group layers are flattened and object layers become MapObjects.
//
// Native maps are strict JSON: an unknown key is an error naming its path.
// A complete example (the tests parse this exact text):
//   {
//     "tile_size": 16,
//     "tilesets": ["tiles.tileset.json", {"source": "deco.tsj", "first_gid": 65}],
//     "terrains": {
//       "wall":  {"shape": "solid", "flags": ["opaque"], "cost": 0, "kind": "wall", "tile": 3},
//       "grass": {"tile": 1},
//       "water": {"cost": 30, "flags": ["water"], "tile": 9}
//     },
//     "legend": {"#": "wall", ".": "grass", "~": "water",
//                "T": {"terrain": "grass", "object": "tree", "tile": 2}},
//     "rows": ["#######",
//              "#..T..#",
//              "#.~~..#",
//              "#######"],
//     "layers": [{"name": "deco", "collision": false, "render_layer": 20,
//                 "data": [0, 0, 0, 0, 0, 0, 0,
//                          0, 0, 0, 0, 0, 66, 0,
//                          0, 65, 0, 0, 0, 0, 0,
//                          0, 0, 0, 0, 0, 0, 0]}],
//     "objects": [{"type": "hero", "name": "start", "x": 1, "y": 1},
//                 {"type": "door", "x": 5, "y": 2, "w": 1, "h": 1, "to": "level2", "locked": true}],
//     "properties": {"music": "day", "par_time": 90}
//   }
// Keys (all optional unless marked required):
//   tile_size          pixels per square tile, 1..4096 (default 16).
//   rows               array of strings of equal length, top row first, one character per cell;
//                      it sets the map size. Every character, a space included, must be a legend
//                      key. Without rows, "width" and "height" (cells) are required and the map has
//                      only "layers"; with rows they may be given but must match. At most 2^26
//                      cells (also for ASCII text).
//   legend             one-character key -> terrain name (a string, never a number), or
//                      {"terrain": name (required), "object": type, "tile": GID}. "object" adds a
//                      1x1-cell MapObject of that type on every such cell; "tile" overrides the
//                      terrain's visual tile there.
//   terrains           terrain name -> {"shape": TileShape name (list in tileset.h), "flags":
//                      ["hazard", "ladder", "water", "opaque"], "cost": 0..65535 (0 = impassable,
//                      default 10), "kind": name, "tile": visual GID}. A terrain named only by the
//                      legend has the defaults (Empty, no flags, cost 10).
//   tilesets           first entry: a path or {"source": path, "first_gid": n (default 1)}; later
//                      entries {"source", "first_gid"} with increasing first_gid. Paths are
//                      relative to the map file (".tsj" or ".tileset.json"); the loader loads them.
//   terrain_first_gid  first GID of the generated terrain tileset (default 1 without "tilesets",
//                      else 1 << 24); must be above every tileset's first_gid, and the last
//                      terrain's GID (first + terrains - 1) at most kTileIdMask.
//   autotile           {"layer": name (default "ground"), "seed": uint32, "details_layer": name,
//                       "rules": [{"terrain": name (required), "first": GID (required), "mode":
//                       "edge4" (default) | "blob47" | "plain", "variants": 1..255, "blends":
//                       [names], "edge_outside": bool (true), "details": [GIDs],
//                       "detail_percent": 0..100}]}. Needs rows (tile order: autotile.h). A
//                      rule's GIDs, first .. first + (47 blob47, 16 edge4, 1 plain) * variants - 1,
//                      must stay at most kTileIdMask (below the flip bits).
//   layers             [{"name" (required), "data" (required: width * height GIDs, row by row,
//                      Tiled flip bits allowed), "collision" (true), "visible" (true), "opacity"
//                      (1), "render_layer" (kLayerTiles), "parallax" ([1, 1])}]. GID layers only:
//                      ASCII art goes in the top-level "rows".
//   objects            [{"type" (required), "x", "y" (required; cells, inside the map), "w", "h"
//                      (cells, default 1; 0 x 0 = a point), "name", any other key with a scalar
//                      value -> MapObject::props as text}]. MapObject::rect = cells * tile_size.
//   properties         name -> scalar (string, number, bool), into TilemapAsset::properties as text.
// Layer order: a hidden "terrain" collision layer, the "ground" layer (autotile or legend tiles),
// the details layer, then each "layers" entry. Plain text (.txt) is ASCII rows; '#' is a wall.
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <lam/core/asset_id.h>
#include <lam/core/data.h>
#include <lam/core/hash.h>
#include <lam/core/id.h>
#include <lam/core/result.h>
#include <lam/ecs/component.h>
#include <lam/ecs/entity.h>
#include <lam/math/grid.h>
#include <lam/math/rect.h>
#include <lam/physics/tile_world.h>
#include <lam/render/sprite.h>
#include <lam/tilemap/autotile.h>
#include <lam/tilemap/tileset.h>
namespace lam {
class World;
class AssetServer;
// Most tilesets one map may reference (fixed arrays in the per-frame systems).
inline constexpr int kMaxMapTilesets = 32;
// One grid of GIDs. Order in a map = draw order (later layers on top) and collision order.
struct TileLayer {
    StrId name;                                                 // Tiled/native layer name (may be none)
    Grid<TileId> tiles;                                         // GIDs with flip bits; 0 = empty
    std::uint8_t render_layer = kLayerTiles;                    // RenderQueue layer of its quads
    bool collision = false;                                     // feeds TileWorld; parsed maps default to true
    bool visible = true;                                        // drawn by draw_tiles
    float opacity = 1;                                          // 0..1, the quads' alpha
    Vec2 parallax{1, 1};                                        // scroll factor against the camera (1 = with the map)
};
// A Tiled object or native "objects" entry. tilemap_sync spawns `type` as a prefab when the
// PrefabLibrary has one; other objects are data for the game.
struct MapObject {
    StrId type;                                                 // prefab name
    StrId name;                                                 // object name; spawned prefabs get it as Name
    Rect rect;                                                  // pixels, relative to the map origin
    std::vector<std::pair<StrId, std::string>> props;           // custom properties as text (prop_int & co.)
};
// One tileset of a map and the first GID it covers.
struct TilesetRef {
    AssetId<Tileset> tileset;                                   // set by the loader for external tilesets
    TileId first_gid = 1;                                       // GIDs [first_gid, next first_gid) are its tiles
    std::string source;                                         // external file, relative to the map ("" if embedded)
    std::int32_t embedded = -1;                                 // index into TilemapAsset::embedded_tilesets
};
// A map asset (".tmj", ".map.json", ASCII ".txt"): shared and read-only at run time; a world edits
// its TilemapInstance copy instead. Values; copying allocates.
struct TilemapAsset {
    int width = 0, height = 0, tile_size = 16;                  // cells; pixels per (square) tile
    std::vector<TilesetRef> tilesets;                           // ascending first_gid
    std::vector<TileLayer> layers;                              // tile layers, bottom first
    std::vector<MapObject> objects;                             // every object of every object layer
    std::vector<std::pair<StrId, std::string>> properties;      // map custom properties as text
    std::vector<Tileset> embedded_tilesets;                     // Tiled embedded tilesets, the native terrain set
    Grid<std::uint8_t> terrain;                                 // native rows: terrain value per cell (0 = none)
    std::vector<StrId> terrains;                                // value v is terrains[v - 1]
    std::vector<AutotileRule> autotile;                         // native "autotile" rules (by terrain value)
    StrId autotile_layer;                                       // layer the rules fill
    std::uint32_t autotile_seed = 0;
};
// spawn_objects spawns each object whose type names a prefab once, at the map origin + its rect
// centre (bottom centre with the property "anchor": "bottom"), and gives it a MapObjectRef.
struct Tilemap {                                                // component; Transform = top-left of cell (0, 0)
    AssetId<TilemapAsset> map;                                  // the map asset (shared; never edited per world)
    // The first Tilemap with build_collision fills the world's TileWorld. With none, the game owns
    // TileWorld and has to rebuild it in its own restore hook.
    bool build_collision = true;
    bool spawn_objects = true;                                  // spawn the map's objects as prefabs, once
};
// Added to spawned object entities; read the object's properties through map_object().
struct MapObjectRef {                                           // component
    Entity map;                                                 // the Tilemap entity that spawned it
    std::uint32_t object = 0;                                   // index into TilemapAsset::objects
};
struct TilemapInstance {                                        // component (engine-added)
    std::vector<TileLayer> layers;                              // this world's copy of the asset's layers
    std::uint32_t asset_version = 0;                            // AssetServer version copied (Modified -> recopy)
    std::uint32_t version = 0;                                  // ++ on set_tile
    bool objects_spawned = false;                               // set once the objects were spawned
    AssetId<TilemapAsset> map;                                  // the asset the layers were copied from
};
// Layers are saved run-length encoded. asset_version is process-local: not saved or hashed.
Status lam_read(TilemapInstance &m, DataReader &in, const SerializeContext &ctx);
void lam_write(const TilemapInstance &m, DataWriter &out, const SerializeContext &ctx);
void lam_hash(const TilemapInstance &m, Hasher &h);
// A resolved GID: its tileset (null if none or not loaded), local id and flip bits.
struct TileRef { const Tileset *set = nullptr; std::uint32_t local = 0; TileId flip = 0; };
// The tileset (largest first_gid <= the id) and local id of `gid`, with its flip bits; {} for 0.
// `set` is null when sets[i] is. No allocation.
TileRef resolve_gid(const TilemapAsset &m, std::span<const Tileset *const> sets, TileId gid);   // sets[i] ~ m.tilesets[i]
// The GID at `cell` of the named layer; 0 if there is no such layer or the cell is outside.
TileId tile_at(const TilemapInstance &m, StrId layer, IVec2 cell);
// Sets the instance cell (version + 1) and, when `map` is the world's collision map and the layer
// collides, recomputes that TileWorld cell from every collision layer. No allocation.
void set_tile(World &w, Entity map, StrId layer, IVec2 cell, TileId id);
// Replaces a whole layer of the instance with one version bump and at most one TileWorld refill.
// False, changing nothing, if there's no instance yet, no such layer, or the size differs.
bool set_layer(World &w, Entity map, StrId layer, const Grid<TileId> &tiles);
// The object `r` refers to, or null if the map is gone, not Ready, or the index is out of range.
const MapObject *map_object(const World &w, const AssetServer &assets, const MapObjectRef &r);
// Parses a Tiled .tmj. Compressed or chunked layers, other orientations, non-square tiles and .tsx
// tilesets are Unsupported. External tilesets are left in `source` for the loader.
Result<TilemapAsset> parse_tiled_json(std::string_view text, std::string_view file_name);
// Parses a native map (strict JSON or ASCII rows). Errors name the file and JSON path.
Result<TilemapAsset> parse_native_map(std::string_view text, std::string_view file_name);
// Resizes `out` to the map and fills it from the collision layers: last non-Empty shape wins,
// flags are OR'd, cost is 0 for Solid or any cost-0 tile, else the highest tile cost.
void fill_tile_world(const TilemapAsset &m, std::span<const TileLayer> layers, std::span<const Tileset *const> sets,
                     Vec2 origin, TileWorld &out);
// The path cost of every cell of one layer (fill_tile_world's rule for that layer alone); `out`
// is resized to the map. An unknown layer gives the default cost everywhere.
void build_cost_grid(const TilemapAsset &m, std::span<const TileLayer> layers, std::span<const Tileset *const> sets,
                     StrId layer, Grid<std::uint16_t> &out);
// Fills out[i] with the Tileset of m.tilesets[i] (embedded ones from the asset itself; null if not
// Ready) and returns how many were written (at most out.size()). No allocation.
int map_tilesets(const AssetServer &assets, const TilemapAsset &m, std::span<const Tileset *> out);
// The layer named `name`, or null.
const TileLayer *find_layer(std::span<const TileLayer> layers, StrId name);
// Terrain value of a terrain name (native maps), 0 if the map has none by that name.
std::uint8_t terrain_value(const TilemapAsset &m, StrId name);
// Every cell of `g` holding `v`, row by row. Allocates into `out`.
template <class T> void find_cells(const Grid<T> &g, T v, std::vector<IVec2> &out) {
    out.clear();
    for (int y = 0; y < g.height(); ++y)
        for (int x = 0; x < g.width(); ++x)
            if (g(x, y) == v) out.push_back({x, y});
}
// Property lookups on MapObject::props and TilemapAsset::properties.
std::string_view prop_or(std::span<const std::pair<StrId, std::string>> props, StrId key, std::string_view fallback = {});
int prop_int(std::span<const std::pair<StrId, std::string>> props, StrId key, int fallback = 0);
float prop_float(std::span<const std::pair<StrId, std::string>> props, StrId key, float fallback = 0);
bool prop_bool(std::span<const std::pair<StrId, std::string>> props, StrId key, bool fallback = false);
template <class T> class AssetLoader;                           // <lam/assets/asset_server.h>
// Loader for .tmj, .map.json, .map.txt and .txt. JSON with a "tiledversion" key is read as Tiled.
std::unique_ptr<AssetLoader<TilemapAsset>> make_tilemap_loader();
}
LAM_REFLECT(lam::Tilemap, map, build_collision, spawn_objects)
LAM_REFLECT(lam::MapObjectRef, map, object)
LAM_REFLECT_CUSTOM(lam::TilemapInstance, lam::kDefaultFlags)
