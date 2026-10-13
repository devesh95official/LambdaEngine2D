#pragma once
// TilemapPlugin: map and tileset loaders, grid components and the tilemap systems. The system
// order is listed in src/tilemap/README.md.
#include <string_view>
#include <lam/core/app.h>
namespace lam {
// Defaults for every world. max_path_cells sizes each world's PathFinder (about 20 bytes a cell);
// change searches per tick later through the world's PathService.
struct TilemapConfig { int path_searches_per_tick = 8; int max_path_cells = 65536; };
class TilemapPlugin final : public Plugin {
public:
    explicit TilemapPlugin(TilemapConfig c = {});               // config used by every world's init_world
    std::string_view name() const override { return "lam.tilemap"; }
    // Requires lam.assets and lam.physics.
    void build(App &app) override;
    // Inserts GridIndex, PathService and (if missing) TileWorld, and adds the restore hook.
    void init_world(App &app, World &world) override;
private:
    TilemapConfig config_;
};
}
