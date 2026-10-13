// Tilemap data operations: GID resolution, TileWorld filling, cost grids, property lookups, the
// TilemapInstance save/hash ops, set_tile, and the TilemapAsset loader.
#include <algorithm>
#include <clocale>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#if defined(__APPLE__)
#include <xlocale.h> // strtof_l, newlocale
#endif

#include <lam/assets/asset_server.h>
#include <lam/core/app.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>
#include <lam/tilemap/tilemap.h>

#include "detail/grid_codec.h"
#include "detail/parse_util.h"
#include "detail/tilemap_state.h"

namespace lam {

namespace detail {

namespace {

// A horizontal flip mirrors a slope's direction.
TileShape flip_shape(TileShape s, TileId flip) {
    if ((flip & kTileFlipH) == 0) return s;
    switch (s) {
    case TileShape::SlopeUpRight:
        return TileShape::SlopeUpLeft;
    case TileShape::SlopeUpLeft:
        return TileShape::SlopeUpRight;
    case TileShape::HalfSlopeUpRightLow:
        return TileShape::HalfSlopeUpLeftLow;
    case TileShape::HalfSlopeUpRightHigh:
        return TileShape::HalfSlopeUpLeftHigh;
    case TileShape::HalfSlopeUpLeftLow:
        return TileShape::HalfSlopeUpRightLow;
    case TileShape::HalfSlopeUpLeftHigh:
        return TileShape::HalfSlopeUpRightHigh;
    default:
        return s;
    }
}

} // namespace

CellProps tile_world_cell(const TilemapAsset &m, std::span<const TileLayer> layers,
                          std::span<const Tileset *const> sets, int x, int y,
                          const TileLayer *only) {
    static const TileProps kDefault{};
    CellProps out;
    bool any = false;
    bool blocked = false;
    std::uint16_t highest = 0;
    for (const TileLayer &layer : layers) {
        if (only != nullptr ? &layer != only : !layer.collision) continue;
        const TileId gid = layer.tiles.get_or(x, y, 0);
        if ((gid & kTileIdMask) == 0) continue;
        const TileRef ref = resolve_gid(m, sets, gid);
        const TileProps &p = ref.set != nullptr ? ref.set->prop(ref.local) : kDefault;
        if (p.shape != TileShape::Empty) out.shape = flip_shape(p.shape, ref.flip);
        out.flags = static_cast<std::uint8_t>(out.flags | p.flags);
        if (p.cost == 0) blocked = true;
        highest = std::max(highest, p.cost);
        any = true;
    }
    if (blocked || out.shape == TileShape::Solid)
        out.cost = 0;
    else if (any)
        out.cost = highest;
    return out;
}

} // namespace detail

TileRef resolve_gid(const TilemapAsset &m, std::span<const Tileset *const> sets, TileId gid) {
    const TileId id = gid & kTileIdMask;
    if (id == 0) return {};
    std::size_t best = m.tilesets.size();
    for (std::size_t i = 0; i < m.tilesets.size(); ++i) {
        if (m.tilesets[i].first_gid <= id)
            best = i;
        else
            break;
    }
    if (best == m.tilesets.size()) return {};
    TileRef r;
    r.set = best < sets.size() ? sets[best] : nullptr;
    r.local = id - m.tilesets[best].first_gid;
    r.flip = gid & ~kTileIdMask;
    return r;
}

const TileLayer *find_layer(std::span<const TileLayer> layers, StrId name) {
    for (const TileLayer &l : layers) {
        if (l.name == name) return &l;
    }
    return nullptr;
}

TileId tile_at(const TilemapInstance &m, StrId layer, IVec2 cell) {
    const TileLayer *l = find_layer(m.layers, layer);
    return l != nullptr ? l->tiles.get_or(cell, 0) : 0;
}

void fill_tile_world(const TilemapAsset &m, std::span<const TileLayer> layers,
                     std::span<const Tileset *const> sets, Vec2 origin, TileWorld &out) {
    out.resize(m.width, m.height, m.tile_size, origin);
    for (int y = 0; y < m.height; ++y) {
        for (int x = 0; x < m.width; ++x) {
            const detail::CellProps c = detail::tile_world_cell(m, layers, sets, x, y);
            out.shapes(x, y) = c.shape;
            out.flags(x, y) = c.flags;
            out.cost(x, y) = c.cost;
        }
    }
    // resize() already counted this change (TileWorld::version + 1 per fill).
}

void build_cost_grid(const TilemapAsset &m, std::span<const TileLayer> layers,
                     std::span<const Tileset *const> sets, StrId layer, Grid<std::uint16_t> &out) {
    if (out.width() != m.width || out.height() != m.height) out.resize(m.width, m.height, 10);
    const TileLayer *only = find_layer(layers, layer);
    if (only == nullptr) {
        out.fill(10);
        return;
    }
    for (int y = 0; y < m.height; ++y) {
        for (int x = 0; x < m.width; ++x)
            out(x, y) = detail::tile_world_cell(m, layers, sets, x, y, only).cost;
    }
}

int map_tilesets(const AssetServer &assets, const TilemapAsset &m, std::span<const Tileset *> out) {
    const std::size_t n = std::min(out.size(), m.tilesets.size());
    for (std::size_t i = 0; i < n; ++i) {
        const TilesetRef &ref = m.tilesets[i];
        if (ref.embedded >= 0 &&
            static_cast<std::size_t>(ref.embedded) < m.embedded_tilesets.size()) {
            out[i] = &m.embedded_tilesets[static_cast<std::size_t>(ref.embedded)];
        } else {
            out[i] = assets.get(ref.tileset);
        }
    }
    return static_cast<int>(n);
}

std::uint8_t terrain_value(const TilemapAsset &m, StrId name) {
    for (std::size_t i = 0; i < m.terrains.size() && i < 255; ++i) {
        if (m.terrains[i] == name) return static_cast<std::uint8_t>(i + 1);
    }
    return 0;
}

std::string_view prop_or(std::span<const std::pair<StrId, std::string>> props, StrId key,
                         std::string_view fallback) {
    for (const auto &[k, v] : props) {
        if (k == key) return v;
    }
    return fallback;
}

int prop_int(std::span<const std::pair<StrId, std::string>> props, StrId key, int fallback) {
    for (const auto &[k, v] : props) {
        if (k != key) continue;
        char *end = nullptr;
        const long n = std::strtol(v.c_str(), &end, 10);
        if (end == v.c_str() || *end != '\0' || n < -2147483647L - 1 || n > 2147483647L)
            return fallback;
        return static_cast<int>(n);
    }
    return fallback;
}

namespace {

// The "C" locale, made once (C allocation, never freed).
locale_t c_locale() {
    static const locale_t c = newlocale(LC_ALL_MASK, "C", locale_t{});
    return c;
}

} // namespace

float prop_float(std::span<const std::pair<StrId, std::string>> props, StrId key, float fallback) {
    for (const auto &[k, v] : props) {
        if (k != key) continue;
        // Parse in the "C" locale: strtof follows the process locale, so "2.5" would stop at the
        // '.' under de_DE. Float std::from_chars needs macOS 26, newer than our target.
        char *end = nullptr;
        const locale_t c = c_locale();
        const float f =
            c != locale_t{} ? strtof_l(v.c_str(), &end, c) : std::strtof(v.c_str(), &end);
        if (end == v.c_str() || *end != '\0') return fallback;
        return f;
    }
    return fallback;
}

bool prop_bool(std::span<const std::pair<StrId, std::string>> props, StrId key, bool fallback) {
    for (const auto &[k, v] : props) {
        if (k != key) continue;
        if (v == "true" || v == "1") return true;
        if (v == "false" || v == "0") return false;
        return fallback;
    }
    return fallback;
}

// ---- TilemapInstance ops ----------------------------------------------------------------------

Status lam_read(TilemapInstance &m, DataReader &in, const SerializeContext &ctx) {
    m.layers.clear();
    m.version = static_cast<std::uint32_t>(in.integer64("version", 0, 0xFFFFFFFFll));
    m.objects_spawned = in.boolean("objects_spawned");
    m.asset_version = 0; // process-local; the restore hook re-stamps it
    m.map = {};
    if (in.type("map") == DataType::String) {
        const std::string path = in.string("map");
        if (ctx.assets != nullptr)
            m.map = AssetId<TilemapAsset>::from_bits(
                ctx.assets->resolve(type_id<TilemapAsset>(), path));
    } else {
        in.ignore("map");
    }
    DataArray layers = in.array("layers");
    for (std::size_t i = 0; i < layers.size() && !in.failed(); ++i) {
        DataReader l = layers.object(i);
        TileLayer layer;
        layer.name = l.id("name");
        layer.render_layer = static_cast<std::uint8_t>(l.integer("render_layer", 0, 255));
        layer.collision = l.boolean("collision");
        layer.visible = l.boolean("visible");
        layer.opacity = l.number("opacity", 0, 1);
        layer.parallax = l.vec2("parallax");
        detail::read_grid(l.object("tiles"), layer.tiles);
        m.layers.push_back(std::move(layer));
    }
    if (in.failed())
        return make_error(ErrorCode::Parse, "TilemapInstance: " + in.path() + ": invalid instance");
    return {};
}

void lam_write(const TilemapInstance &m, DataWriter &out, const SerializeContext &ctx) {
    out.begin_object();
    out.key("map");
    const std::string_view path = (ctx.assets != nullptr && m.map)
                                      ? ctx.assets->path_of(type_id<TilemapAsset>(), m.map.bits())
                                      : std::string_view{};
    if (path.empty())
        out.null();
    else
        out.value(path);
    out.key("version").value(m.version);
    out.key("objects_spawned").value(m.objects_spawned);
    out.key("layers").begin_array();
    for (const TileLayer &l : m.layers) {
        out.begin_object();
        out.key("name").value(l.name);
        out.key("render_layer").value(l.render_layer);
        out.key("collision").value(l.collision);
        out.key("visible").value(l.visible);
        out.key("opacity").value(l.opacity);
        out.key("parallax").value(l.parallax);
        out.key("tiles");
        detail::write_grid(l.tiles, out);
        out.end_object();
    }
    out.end_array();
    out.end_object();
}

void lam_hash(const TilemapInstance &m, Hasher &h) {
    h.u64(m.version);
    h.u64(m.objects_spawned ? 1u : 0u);
    h.u64(m.layers.size());
    for (const TileLayer &l : m.layers) {
        h.u64(l.name.value);
        h.u64((std::uint64_t(l.render_layer) << 2) | (l.collision ? 2u : 0u) |
              (l.visible ? 1u : 0u));
        h.f32(l.opacity);
        h.f32(l.parallax.x);
        h.f32(l.parallax.y);
        detail::hash_grid(l.tiles, h);
    }
}

void set_tile(World &w, Entity map, StrId layer, IVec2 cell, TileId id) {
    TilemapInstance *inst = w.try_get<TilemapInstance>(map);
    if (inst == nullptr) return;
    TileLayer *target = nullptr;
    for (TileLayer &l : inst->layers) {
        if (l.name == layer) {
            target = &l;
            break;
        }
    }
    if (target == nullptr || !target->tiles.inside(cell)) return;
    if (target->tiles(cell.x, cell.y) == id) return;
    target->tiles(cell.x, cell.y) = id;
    ++inst->version;
    if (!target->collision) return;
    const detail::TilemapWorldState *st = w.resources().try_get<detail::TilemapWorldState>();
    TileWorld *tw = w.resources().try_get<TileWorld>();
    if (st == nullptr || st->app == nullptr || tw == nullptr || st->collision_map != map) return;
    const AssetServer *assets = st->app->try_res<AssetServer>();
    const TilemapAsset *asset = assets != nullptr ? assets->get(inst->map) : nullptr;
    if (asset == nullptr || !tw->shapes.inside(cell)) return;
    const Tileset *sets[kMaxMapTilesets] = {};
    const int n = map_tilesets(*assets, *asset, sets);
    const detail::CellProps c = detail::tile_world_cell(
        *asset, inst->layers, std::span<const Tileset *const>(sets, static_cast<std::size_t>(n)),
        cell.x, cell.y);
    tw->set(cell, c.shape, c.flags, c.cost);                    // one version bump
}

bool set_layer(World &w, Entity map, StrId layer, const Grid<TileId> &tiles) {
    TilemapInstance *inst = w.try_get<TilemapInstance>(map);
    if (inst == nullptr) return false;
    TileLayer *target = nullptr;
    for (TileLayer &l : inst->layers) {
        if (l.name == layer) {
            target = &l;
            break;
        }
    }
    if (target == nullptr || target->tiles.width() != tiles.width() ||
        target->tiles.height() != tiles.height())
        return false;
    // Element-wise copy: same size, so no allocation.
    if (&tiles != &target->tiles)
        std::copy(tiles.cells().begin(), tiles.cells().end(), target->tiles.cells().begin());
    ++inst->version;
    if (!target->collision) return true;
    const detail::TilemapWorldState *st = w.resources().try_get<detail::TilemapWorldState>();
    TileWorld *tw = w.resources().try_get<TileWorld>();
    if (st == nullptr || st->app == nullptr || tw == nullptr || st->collision_map != map)
        return true;
    const AssetServer *assets = st->app->try_res<AssetServer>();
    const TilemapAsset *asset = assets != nullptr ? assets->get(inst->map) : nullptr;
    if (asset == nullptr) return true;
    const Tileset *sets[kMaxMapTilesets] = {};
    const int n = map_tilesets(*assets, *asset, sets);
    const Transform *t = w.try_get<Transform>(map);
    fill_tile_world(*asset, inst->layers,
                    std::span<const Tileset *const>(sets, static_cast<std::size_t>(n)),
                    t != nullptr ? t->position : Vec2{}, *tw);
    return true;
}

const MapObject *map_object(const World &w, const AssetServer &assets, const MapObjectRef &r) {
    if (!w.alive(r.map)) return nullptr;
    AssetId<TilemapAsset> id;
    if (const TilemapInstance *inst = w.try_get<TilemapInstance>(r.map))
        id = inst->map;
    else if (const Tilemap *tm = w.try_get<Tilemap>(r.map))
        id = tm->map;
    const TilemapAsset *asset = assets.get(id);
    if (asset == nullptr || r.object >= asset->objects.size()) return nullptr;
    return &asset->objects[r.object];
}

// ---- loader ------------------------------------------------------------------------------------

namespace {

bool has_suffix(std::string_view s, std::string_view suffix) {
    if (s.size() < suffix.size()) return false;
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        char c = s[s.size() - suffix.size() + i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != suffix[i]) return false;
    }
    return true;
}

std::string image_extension(std::string_view path) {
    const std::size_t dot = path.find_last_of('.');
    const std::size_t slash = path.find_last_of("/\\");
    if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash))
        return {};
    std::string ext(path.substr(dot));
    for (char &c : ext) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return ext;
}

class TilemapLoader final : public AssetLoader<TilemapAsset> {
public:
    std::span<const std::string_view> extensions() const override {
        static constexpr std::string_view kExt[] = {".tmj", ".map.json", ".map.txt", ".txt"};
        return kExt;
    }
    Result<TilemapAsset> decode(std::span<const std::byte> bytes, LoadContext &ctx) override {
        const std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        const bool tiled = has_suffix(ctx.path(), ".tmj") ||
                           (!has_suffix(ctx.path(), ".txt") &&
                            text.find("\"tiledversion\"") != std::string_view::npos);
        Result<TilemapAsset> r =
            tiled ? parse_tiled_json(text, ctx.path()) : parse_native_map(text, ctx.path());
        if (!r) return r.error();
        TilemapAsset &m = r.value();
        for (TilesetRef &ref : m.tilesets) {
            if (!ref.source.empty()) ref.tileset = ctx.depend<Tileset>(ref.source);
        }
        for (Tileset &t : m.embedded_tilesets) {
            if (t.image_path.empty()) continue;
            Result<std::vector<std::byte>> image = ctx.read(t.image_path);
            if (!image) return image.error();
            Result<ImageData> decoded = decode_image(*image, image_extension(t.image_path));
            if (!decoded)
                return make_error(decoded.error().code,
                                  t.image_path + ": " + decoded.error().message);
            t.pending_image = std::move(decoded.value());
            t.image_size = {t.pending_image.width, t.pending_image.height};
            detail::settle_tileset_geometry(t);
        }
        return std::move(r.value());
    }
    Status finalize(TilemapAsset &m, FinalizeContext &ctx) override {
        for (Tileset &t : m.embedded_tilesets)
            LAM_TRY(finalize_tileset(t, ctx.app, ctx.server));
        return {};
    }
};

} // namespace

std::unique_ptr<AssetLoader<TilemapAsset>> make_tilemap_loader() {
    return std::make_unique<TilemapLoader>();
}

} // namespace lam
