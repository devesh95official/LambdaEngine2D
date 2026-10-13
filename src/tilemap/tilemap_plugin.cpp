// TilemapPlugin: loaders, systems and the world restore hook. Nothing allocates in steady state;
// instances are only copied when a map becomes Ready or Modified.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include <lam/assets/asset_server.h>
#include <lam/core/app.h>
#include <lam/core/log.h>
#include <lam/ecs/commands.h>
#include <lam/ecs/prefab.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/render/camera.h>
#include <lam/render/render_queue.h>
#include <lam/tilemap/fov.h>
#include <lam/tilemap/grid_mover.h>
#include <lam/tilemap/grid_path.h>
#include <lam/tilemap/path_follow.h>
#include <lam/tilemap/tilemap.h>
#include <lam/tilemap/tilemap_plugin.h>

#include "detail/tilemap_state.h"

namespace lam {

namespace {

using detail::TilemapWorldState;

// Tilemap entities handled per world and frame (more are skipped with a warning).
constexpr int kMaxMapsPerWorld = 16;

Vec2 origin_of(const World &w, Entity e) {
    const Transform *t = w.try_get<Transform>(e);
    return t != nullptr ? t->position : Vec2{};
}

std::uint32_t tilesets_stamp(const AssetServer &assets, const TilemapAsset &m) {
    std::uint32_t s = 0x811C9DC5u;
    for (const TilesetRef &ref : m.tilesets)
        s = (s ^ (ref.tileset ? assets.version(ref.tileset) : 1u)) * 16777619u;
    return s;
}

// Fills TileWorld from the instance of `map` (the world's collision map).
void refill_collision(World &w, Entity map, const AssetServer &assets, TilemapWorldState &st) {
    const TilemapInstance *inst = w.try_get<TilemapInstance>(map);
    TileWorld *tw = w.resources().try_get<TileWorld>();
    if (inst == nullptr || tw == nullptr) return;
    const TilemapAsset *asset = assets.get(inst->map);
    if (asset == nullptr) return;
    const Tileset *sets[kMaxMapTilesets] = {};
    const int n = map_tilesets(assets, *asset, sets);
    fill_tile_world(*asset, inst->layers,
                    std::span<const Tileset *const>(sets, static_cast<std::size_t>(n)),
                    origin_of(w, map), *tw);
    st.tilesets_stamp = tilesets_stamp(assets, *asset);
}

// The world's collision map: the first Tilemap (dense order) with build_collision. It depends only
// on the current world, so undo and load pick the same map the original run had.
Entity first_collision_map(World &w) {
    int seen = 0;
    for (auto [e, tm] : w.view<Tilemap>()) {
        if (seen++ >= kMaxMapsPerWorld) break;
        if (tm.build_collision) return e;
    }
    return {};
}

void spawn_objects(World &w, Entity map, const TilemapAsset &asset, Vec2 origin) {
    const PrefabLibrary *lib = w.prefabs();
    if (lib == nullptr) return;
    const float ts = static_cast<float>(asset.tile_size);
    const StrId anchor_key = str_id("anchor");
    for (std::size_t i = 0; i < asset.objects.size(); ++i) {
        const MapObject &o = asset.objects[i];
        if (!o.type || !lib->has(o.type)) continue; // data-only objects are the game's to read
        const Vec2 centre = o.rect.center();
        // "anchor": "bottom" spawns feet-anchored prefabs at the bottom centre.
        const bool bottom = prop_or(o.props, anchor_key) == "bottom";
        const Vec2 local = bottom ? Vec2{centre.x, o.rect.y + o.rect.h} : centre;
        const Entity e = lib->spawn(w, o.type, origin + local);
        if (!w.alive(e)) continue;
        if (o.name && !w.has<Name>(e)) w.add<Name>(e, Name{o.name});
        w.add<MapObjectRef>(e, MapObjectRef{map, static_cast<std::uint32_t>(i)});
        if (GridPosition *gp = w.try_get<GridPosition>(e)) gp->cell = floor_to_ivec(centre / ts);
    }
}

void tilemap_sync(Ctx &ctx) {
    World &w = ctx.world();
    TilemapWorldState *st = ctx.try_local<TilemapWorldState>();
    AssetServer *assets = ctx.try_res<AssetServer>();
    if (st == nullptr || assets == nullptr) return;
    Entity maps[kMaxMapsPerWorld];
    int count = 0;
    for (auto [e, tm] : w.view<Tilemap>()) {
        (void)tm;
        if (count < kMaxMapsPerWorld)
            maps[count++] = e;
        else if (!st->warned_many_maps) {
            LAM_LOG_WARN(
                "tilemap: more than {} Tilemap entities in one world; the rest are ignored",
                kMaxMapsPerWorld);
            st->warned_many_maps = true;
        }
    }
    const Entity previous_owner = st->collision_map;
    st->collision_map = first_collision_map(w);
    for (int i = 0; i < count; ++i) {
        const Entity e = maps[i];
        const Tilemap tm = w.get<Tilemap>(e);
        const TilemapAsset *asset = assets->get(tm.map);
        if (asset == nullptr) continue;
        const std::uint32_t version = assets->version(tm.map);
        bool changed = false;
        TilemapInstance *inst = w.try_get<TilemapInstance>(e);
        if (inst == nullptr) {
            TilemapInstance fresh;
            fresh.layers = asset->layers;
            fresh.asset_version = version;
            fresh.map = tm.map;
            w.add<TilemapInstance>(e, std::move(fresh));
            changed = true;
        } else if (inst->map == tm.map && inst->asset_version == 0) {
            // Restored while the asset wasn't Ready (asset_version 0): the restored layers are
            // the world's state, so adopt them instead of recopying the asset.
            inst->asset_version = version;
            changed = true;
        } else if (inst->map != tm.map || inst->asset_version != version) {
            // Modified (hot reload) or another map: the asset wins over set_tile edits.
            inst->layers = asset->layers;
            if (inst->map != tm.map) inst->objects_spawned = false;
            inst->map = tm.map;
            inst->asset_version = version;
            ++inst->version;
            changed = true;
        }
        // The collision map refills TileWorld when its layers changed, it just took over, a
        // tileset was reloaded, or the map entity moved.
        if (e == st->collision_map) {
            const TileWorld *tw = w.resources().try_get<TileWorld>();
            const bool moved = tw != nullptr && tw->origin != origin_of(w, e);
            if (changed || e != previous_owner || moved ||
                tilesets_stamp(*assets, *asset) != st->tilesets_stamp) {
                refill_collision(w, e, *assets, *st);
            }
        }
        inst = w.try_get<TilemapInstance>(e);
        if (tm.spawn_objects && inst != nullptr && !inst->objects_spawned) {
            inst->objects_spawned = true;
            spawn_objects(w, e, *asset, origin_of(w, e));
        }
    }
}

void grid_index_rebuild_system(Ctx &ctx) {
    grid_index_rebuild(ctx.world());
}

void serve_path_requests(Ctx &ctx) {
    World &w = ctx.world();
    PathService *service = ctx.try_local<PathService>();
    const TileWorld *tw = ctx.try_local<TileWorld>();
    if (service == nullptr || tw == nullptr) return;
    service->finder.begin_tick(service->searches_per_tick);
    const PathSmoothing game_smoothing =
        service->finder.smoothing(); // per request below; restored after
    TilemapWorldState *st = ctx.try_local<TilemapWorldState>();
    for (auto [e, req] : w.view<PathRequest>()) {
        IVec2 start;
        if (const GridPosition *gp = w.try_get<GridPosition>(e))
            start = gp->cell;
        else if (const Transform *t = w.try_get<Transform>(e))
            start = tw->tile_at(t->position + req.start_offset);
        else {
            w.commands().remove<PathRequest>(e); // nowhere to start from
            continue;
        }
        PathRoute *existing = w.try_get<PathRoute>(e);
        PathRoute route = existing != nullptr ? *existing : PathRoute{};
        service->finder.set_smoothing(req.smoothing);
        const Result<PathStatus> r =
            service->finder.find(tw->cost, start, req.goal, req.dirs, route);
        if (r && *r == PathStatus::NoBudget) continue; // served on a later tick
        if (!r) {
            if (st == nullptr || !st->warned_path) {
                LAM_LOG_WARN("tilemap: path request failed: {}", r.error().message);
                if (st != nullptr) st->warned_path = true;
            }
            route = PathRoute{};
            route.goal = req.goal;
        }
        route.computed_tick = w.tick();
        if (existing != nullptr)
            *existing = route;
        else
            w.add<PathRoute>(e, route);
        w.commands().remove<PathRequest>(e);
    }
    service->finder.set_smoothing(game_smoothing);
}

void grid_movers_system(Ctx &ctx) {
    grid_movers(ctx.world());
}

void animated_tiles(Ctx &ctx) {
    if (TilemapWorldState *st = ctx.try_local<TilemapWorldState>()) ++st->anim_ticks;
}

// Corner UVs (tl, tr, br, bl) of `src` after Tiled's flips: diagonal (transpose) first, then
// horizontal, then vertical.
void tile_uvs(const Rect &src, TileId flip, Vec2 (&uv)[4]) {
    uv[0] = {src.x, src.y};
    uv[1] = {src.x + src.w, src.y};
    uv[2] = {src.x + src.w, src.y + src.h};
    uv[3] = {src.x, src.y + src.h};
    if ((flip & kTileFlipD) != 0) std::swap(uv[1], uv[3]);
    if ((flip & kTileFlipH) != 0) {
        std::swap(uv[0], uv[1]);
        std::swap(uv[3], uv[2]);
    }
    if ((flip & kTileFlipV) != 0) {
        std::swap(uv[0], uv[3]);
        std::swap(uv[1], uv[2]);
    }
}

struct CellRange {
    int x0, y0, x1, y1; // [x0, x1) x [y0, y1)
};

// Cells under `vis`, plus `extra` columns to the left and rows below: tiles bigger than a cell
// reach up and right, so they can show from just outside the view.
CellRange visible_cells(const Rect &vis, Vec2 origin, float ts, int width, int height, int extra) {
    const auto lo = [&](float v) { return static_cast<int>(std::floor(v / ts)); };
    const auto hi = [&](float v) { return static_cast<int>(std::ceil(v / ts)); };
    CellRange r{lo(vis.x - origin.x) - extra, lo(vis.y - origin.y), hi(vis.x + vis.w - origin.x),
                hi(vis.y + vis.h - origin.y) + extra};
    r.x0 = std::clamp(r.x0, 0, width);
    r.y0 = std::clamp(r.y0, 0, height);
    r.x1 = std::clamp(r.x1, 0, width);
    r.y1 = std::clamp(r.y1, 0, height);
    return r;
}

void draw_fog(const World &w, RenderQueue &q, const CameraView &view) {
    const FovMemory *fov = w.resources().try_get<FovMemory>();
    const FogStyle *style = w.resources().try_get<FogStyle>();
    const TileWorld *tw = w.resources().try_get<TileWorld>();
    if (fov == nullptr || style == nullptr || tw == nullptr) return;
    const int width = std::min(fov->explored.width(), fov->visible.width());
    const int height = std::min(fov->explored.height(), fov->visible.height());
    const float ts = static_cast<float>(tw->tile_size);
    const CellRange r = visible_cells(view.visible_world, tw->origin, ts, width, height, 0);
    for (int y = r.y0; y < r.y1; ++y) {
        for (int x = r.x0; x < r.x1; ++x) {
            if (fov->visible(x, y) != 0) continue;
            const Color c = fov->explored(x, y) != 0 ? style->explored : style->unseen;
            if (c.a == 0) continue;
            q.rect(tw->tile_rect({x, y}), c, style->layer);
        }
    }
}

void draw_tiles(Ctx &ctx) {
    World &w = ctx.world();
    RenderQueue *q = ctx.try_res<RenderQueue>();
    const CameraView *view = ctx.try_local<CameraView>();
    const AssetServer *assets = ctx.try_res<AssetServer>();
    if (q == nullptr || view == nullptr || !view->valid || assets == nullptr) return;
    const TilemapWorldState *st = ctx.try_local<TilemapWorldState>();
    const std::uint64_t ms =
        st != nullptr
            ? st->anim_ticks * 1000u / static_cast<std::uint64_t>(std::max(st->tick_rate, 1))
            : 0;
    for (auto [e, tm, inst] : w.view<Tilemap, TilemapInstance>()) {
        (void)tm;
        const TilemapAsset *asset = assets->get(inst.map);
        if (asset == nullptr) continue;
        const Tileset *sets[kMaxMapTilesets] = {};
        const int n = map_tilesets(*assets, *asset, sets);
        const std::span<const Tileset *const> set_span(sets, static_cast<std::size_t>(n));
        const Vec2 origin = origin_of(w, e);
        const float ts = static_cast<float>(asset->tile_size);
        // Cells to add left of and below the view for tiles larger than a cell.
        int extra = 0;
        for (int i = 0; i < n && asset->tile_size > 0; ++i) {
            if (sets[i] == nullptr) continue;
            const int cells = (sets[i]->tile_size + asset->tile_size - 1) / asset->tile_size;
            extra = std::max(extra, cells - 1);
        }
        for (std::size_t li = 0; li < inst.layers.size(); ++li) {
            const TileLayer &layer = inst.layers[li];
            if (!layer.visible || !(layer.opacity > 0)) continue;
            const Vec2 shift{view->center.x * (1 - layer.parallax.x),
                             view->center.y * (1 - layer.parallax.y)};
            const Vec2 base = origin + shift;
            const CellRange r = visible_cells(view->visible_world, base, ts, layer.tiles.width(),
                                              layer.tiles.height(), extra);
            const Color tint{
                255, 255, 255,
                static_cast<std::uint8_t>(std::clamp(layer.opacity, 0.0f, 1.0f) * 255.0f + 0.5f)};
            const float depth = static_cast<float>(li);
            for (int y = r.y0; y < r.y1; ++y) {
                for (int x = r.x0; x < r.x1; ++x) {
                    const TileId gid = layer.tiles(x, y);
                    if ((gid & kTileIdMask) == 0) continue;
                    const TileRef ref = resolve_gid(*asset, set_span, gid);
                    if (ref.set == nullptr) continue;
                    std::uint32_t local = ref.local;
                    const TileProps &p = ref.set->prop(local);
                    if (p.anim_count > 1 && p.anim_ms > 0) {
                        local = p.anim_first +
                                static_cast<std::uint32_t>((ms / p.anim_ms) % p.anim_count);
                    }
                    const TextureRegion region = ref.set->region(local);
                    if (!(region.source.w > 0) || !(region.source.h > 0))
                        continue; // props-only tileset
                    // Larger tiles extend up and right from the cell's bottom-left, as in Tiled.
                    const float px = base.x + static_cast<float>(x) * ts;
                    const float py = base.y + static_cast<float>(y + 1) * ts - region.size.y;
                    Vec2 uv[4];
                    tile_uvs(region.source, ref.flip, uv);
                    const QuadVertex v[4] = {
                        {{px, py}, uv[0], tint},
                        {{px + region.size.x, py}, uv[1], tint},
                        {{px + region.size.x, py + region.size.y}, uv[2], tint},
                        {{px, py + region.size.y}, uv[3], tint}};
                    q->push(layer.render_layer, depth, 0, region.texture, v);
                }
            }
        }
    }
    draw_fog(w, *q, *view);
}

// Restore hook: instances are authoritative after a restore (undo, load).
void tilemap_restore(World &w) {
    TilemapWorldState *st = w.resources().try_get<TilemapWorldState>();
    if (st == nullptr || st->app == nullptr) return;
    const AssetServer *assets = st->app->try_res<AssetServer>();
    if (assets != nullptr) {
        // 0 while the asset is not Ready (a load in a fresh process): tilemap_sync then adopts
        // the restored layers when it becomes Ready instead of recopying the asset.
        for (auto [e, inst] : w.view<TilemapInstance>()) {
            (void)e;
            inst.asset_version =
                assets->state(inst.map) == LoadState::Ready ? assets->version(inst.map) : 0u;
        }
        // Same rule as tilemap_sync. Without an instance or a Ready asset there is nothing to
        // fill yet; tilemap_sync refills TileWorld once there is (new instance, adopted layers).
        st->collision_map = first_collision_map(w);
        if (st->collision_map) refill_collision(w, st->collision_map, *assets, *st);
    }
    grid_index_rebuild(w);
}

} // namespace

TilemapPlugin::TilemapPlugin(TilemapConfig c) : config_(c) {}

void TilemapPlugin::build(App &app) {
    app.require_plugin("lam.assets", name());
    app.require_plugin("lam.physics", name());
    app.register_component<Tilemap>();
    app.register_component<TilemapInstance>();
    app.register_component<GridPosition>();
    app.register_component<GridMover>();
    app.register_component<PathRoute>();
    app.register_component<PathRequest>();
    app.register_component<MapObjectRef>();
    if (AssetServer *assets = app.try_res<AssetServer>()) {
        assets->register_loader<Tileset>(make_tileset_loader());
        assets->register_loader<TilemapAsset>(make_tilemap_loader());
    } else {
        LAM_LOG_WARN("tilemap: no AssetServer at build (add AssetsPlugin first); map loaders not "
                     "registered");
    }
    app.add_system(
        {.stage = Stage::FrameBegin, .fn = &tilemap_sync, .name = "tilemap_sync", .order = -1000});
    // Again at each tick start, so a Tilemap added (or a map changed) during tick N is synced
    // before tick N + 1 whatever the frame pacing; nothing to do when nothing changed.
    app.add_system({.stage = Stage::TickBegin,
                    .fn = &tilemap_sync,
                    .name = "tilemap_sync_tick",
                    .order = -1900});
    app.add_system({.stage = Stage::Simulate,
                    .fn = &grid_index_rebuild_system,
                    .name = "grid_index_rebuild",
                    .order = -1500});
    app.add_system({.stage = Stage::Simulate,
                    .fn = &serve_path_requests,
                    .name = "serve_path_requests",
                    .order = -1100});
    app.add_system({.stage = Stage::Animate,
                    .fn = &grid_movers_system,
                    .name = "grid_movers",
                    .order = -1200});
    app.add_system(
        {.stage = Stage::Animate, .fn = &animated_tiles, .name = "animated_tiles", .order = -1100});
    app.add_system(
        {.stage = Stage::Render, .fn = &draw_tiles, .name = "draw_tiles", .order = -1000});
}

void TilemapPlugin::init_world(App &app, World &world) {
    Resources &r = world.resources();
    if (!r.has<TileWorld>()) r.insert<TileWorld>();
    if (!r.has<GridIndex>()) r.insert<GridIndex>();
    if (!r.has<PathService>()) {
        PathService &ps = r.insert<PathService>();
        ps.finder.resize(config_.max_path_cells);
        ps.searches_per_tick = config_.path_searches_per_tick;
    }
    if (!r.has<TilemapWorldState>()) {
        TilemapWorldState &st = r.insert<TilemapWorldState>();
        st.app = &app;
        st.tick_rate = app.config().tick_rate;
    }
    world.add_restore_hook(&tilemap_restore);
}

} // namespace lam
