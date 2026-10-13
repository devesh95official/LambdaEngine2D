#pragma once
// How a Pathar world looks and sounds. These systems read the sim and write only presentation
// state (Sprite, Shadow, Camera2D, Tilemap, toasts, audio).
//   look_board   Update    on Board::serial change: autotiled Tilemap, slab sprites, camera
//   look_pieces  Update    sprites for new stones/player; stone frame (plain, lit, sinking, sunk)
//   animate_hero Simulate  (tick, after puzzle_move) walk/idle clip and Animator::facing
//   feedback     Update    PuzzleEvent -> ToastQueue messages and solve stingers
//   draw_hud     RenderUI  top/bottom bars and counters through RenderQueue + the default font
// All run only on worlds holding a Board (run_if = has_local<Board>).
#include <string>
#include <string_view>
#include <vector>
#include <lam/animation/sprite_sheet.h>
#include <lam/audio/audio.h>
#include <lam/core/app.h>
#include <lam/core/asset_id.h>
#include <lam/ecs/entity.h>
#include <lam/math/vec2.h>
#include <lam/tilemap/tilemap.h>
#include <lam/tilemap/tileset.h>

namespace pathar {

// Asset ids loaded once at startup (App resource; root asset scope).
struct Art {
    lam::AssetId<lam::Tileset> tileset;        // tiles/pathar_tiles.tsj (Tiled tileset over the png)
    lam::AssetId<lam::SpriteSheet> hero, props;
    lam::AssetId<lam::MusicTrack> music, discover, victory;
};

// The tile choices of tiles/pathar_tiles.json (the terrain table), read with
// DataReader: per terrain, base tiles (random variants), details, or the first of 16 edge tiles.
struct Terrain {
    std::string name;
    std::vector<int> base, details;            // local tile ids
    int detail_percent = 0;
    int edges = -1;                            // first local id of an Edge4 set (-1: none)
};
struct TileTable {
    std::vector<Terrain> terrains;
    const Terrain *find(std::string_view name) const;
    int single(std::string_view name) const;   // base[0] of a one-tile terrain, -1 if missing
};

// Per-world presentation state (World resource, not snapshotted).
struct BoardLook {
    std::uint32_t serial = 0;                  // Board::serial last built
    lam::AssetId<lam::TilemapAsset> map_asset; // runtime asset, refilled per level (mark_modified)
    lam::Entity map, camera;
    std::vector<lam::Entity> slabs;
};
struct SlabLook {};                            // tag on slab sprites (unreflected: never hashed or saved)
struct TitleLook {                             // World resource: this is the title's demo board
    lam::Vec2 board_offset{56, 26};            // screen offset of the board (room for logo and menu)
};

// Builds the look of `world`'s board now (map, slabs, camera, piece sprites) if the art is Ready;
// look_board/look_pieces call it every frame and it only rebuilds what is missing or stale. Call
// it from Scene::enter: the TilemapAsset is then created in the scene's asset scope (freed with
// the scene), and the look exists before the world's first tick, so no undo snapshot predates it.
void refresh_look(lam::App &app, lam::World &world);
// The theme of global level `index` ("village" when unknown).
std::string theme_of(lam::App &app, int index);

class PatharLookPlugin final : public lam::Plugin {
public:
    std::string_view name() const override { return "pathar.look"; }
    void build(lam::App &app) override;        // Art, TileTable, PatharFx, sound bank, systems
};

}  // namespace pathar
