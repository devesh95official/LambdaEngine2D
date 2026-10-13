// Tilesets: Tiled ".tsj" and native ".tileset.json" parsing, the shared parse helpers, the
// finalize step that packs the image into the atlas, and the Tileset loader.
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lam/assets/asset_server.h>
#include <lam/core/app.h>
#include <lam/core/log.h>
#include <lam/render/atlas.h>
#include <lam/tilemap/tileset.h>

#include "detail/parse_util.h"

namespace lam {

namespace detail {

void unsupported(ParseState &st, DataReader &in, std::string_view message) {
    if (!in.failed()) st.code = ErrorCode::Unsupported;
    in.fail(message);
}

std::string number_text(double v) {
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 9007199254740992.0) {
        return std::to_string(static_cast<long long>(v));
    }
    return std::format("{}", v);
}

std::optional<std::string> scalar_text(DataReader &in, std::string_view key) {
    switch (in.type(key)) {
    case DataType::Bool:
        return in.boolean(key) ? std::string("true") : std::string("false");
    case DataType::Number:
        return number_text(in.number64(key, -1.7976931348623157e308, 1.7976931348623157e308));
    case DataType::String:
        return in.text(key);
    default:
        in.ignore(key);
        return std::nullopt;
    }
}

std::optional<TileShape> shape_from_name(std::string_view name) {
    static constexpr std::pair<std::string_view, TileShape> kNames[] = {
        {"empty", TileShape::Empty},
        {"solid", TileShape::Solid},
        {"one_way", TileShape::OneWay},
        {"slope_up_right", TileShape::SlopeUpRight},
        {"slope_up_left", TileShape::SlopeUpLeft},
        {"half_slope_up_right_low", TileShape::HalfSlopeUpRightLow},
        {"half_slope_up_right_high", TileShape::HalfSlopeUpRightHigh},
        {"half_slope_up_left_low", TileShape::HalfSlopeUpLeftLow},
        {"half_slope_up_left_high", TileShape::HalfSlopeUpLeftHigh},
    };
    for (const auto &[n, s] : kNames) {
        if (n == name) return s;
    }
    return std::nullopt;
}

std::optional<std::uint8_t> flag_from_name(std::string_view name) {
    if (name == "hazard") return std::uint8_t{kTileHazard};
    if (name == "ladder") return std::uint8_t{kTileLadder};
    if (name == "water") return std::uint8_t{kTileWater};
    if (name == "opaque") return std::uint8_t{kTileOpaque};
    return std::nullopt;
}

void read_tiled_properties(DataReader &owner, std::vector<std::pair<StrId, std::string>> &out) {
    if (!owner.has("properties")) return;
    DataArray arr = owner.array("properties");
    for (std::size_t i = 0; i < arr.size(); ++i) {
        DataReader p = arr.object(i);
        p.ignore_unknown();
        const std::string name = p.string("name");
        std::optional<std::string> value = scalar_text(p, "value");
        if (!value || name.empty()) continue; // class-typed and null values are not kept
        out.emplace_back(intern(name), std::move(*value));
    }
}

void settle_tileset_geometry(Tileset &t) {
    const int step = t.tile_size + t.spacing;
    if (t.columns <= 0 && t.image_size.x > 0 && step > 0) {
        t.columns = std::max(1, (t.image_size.x - 2 * t.margin + t.spacing) / step);
    }
    if (t.count <= 0 && t.image_size.y > 0 && t.columns > 0 && step > 0) {
        // In 64 bits (columns x rows of a huge image overflows an int), at most kMaxTilesetTiles
        // like an explicit "count"/"tilecount".
        const std::int64_t rows = std::max(0, (t.image_size.y - 2 * t.margin + t.spacing) / step);
        t.count = static_cast<int>(
            std::min<std::int64_t>(std::int64_t{t.columns} * rows, kMaxTilesetTiles));
    }
    if (static_cast<int>(t.props.size()) < t.count)
        t.props.resize(static_cast<std::size_t>(t.count));
}

namespace {

// A Tiled tile "animation": consecutive tile ids only (TileProps holds first, count, ms).
void read_tiled_animation(DataReader &tile, TileProps &p, ParseState &st) {
    DataArray frames = tile.array("animation");
    if (frames.size() == 0) return;
    int first = 0;
    int ms = 0;
    for (std::size_t k = 0; k < frames.size(); ++k) {
        DataReader f = frames.object(k);
        f.ignore_unknown();
        const int id = f.integer("tileid", 0, 0xFFFF);
        const int duration = f.integer("duration", 0, 0xFFFF);
        if (k == 0) {
            first = id;
            ms = duration;
        } else if (id != first + static_cast<int>(k)) {
            unsupported(st, f, "animation frames must be consecutive tile ids");
            return;
        }
    }
    p.anim_first = static_cast<std::uint16_t>(first);
    p.anim_count = static_cast<std::uint16_t>(std::min<std::size_t>(frames.size(), 0xFFFF));
    p.anim_ms = static_cast<std::uint16_t>(std::max(ms, 1));
}

void apply_tile_property(TileProps &p, StrId name, const std::string &value, DataReader &at) {
    using namespace literals;
    const auto truthy = [](const std::string &v) { return v == "true" || v == "1"; };
    if (name == "shape"_id) {
        const auto s = shape_from_name(value);
        if (!s)
            at.fail("unknown tile shape \"" + value + "\"");
        else
            p.shape = *s;
    } else if (name == "cost"_id) {
        int c = -1;
        const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), c);
        if (ec != std::errc{} || ptr != value.data() + value.size() || c < 0 || c > 0xFFFF) {
            at.fail("tile cost must be a whole number 0..65535");
        } else {
            p.cost = static_cast<std::uint16_t>(c);
        }
    } else if (name == "kind"_id) {
        p.kind = value.empty() ? StrId{} : intern(value);
    } else {
        std::uint8_t bit = 0;
        if (name == "hazard"_id)
            bit = kTileHazard;
        else if (name == "ladder"_id)
            bit = kTileLadder;
        else if (name == "water"_id)
            bit = kTileWater;
        else if (name == "opaque"_id)
            bit = kTileOpaque;
        if (bit == 0) return; // other properties are the game's
        if (truthy(value))
            p.flags = static_cast<std::uint8_t>(p.flags | bit);
        else
            p.flags = static_cast<std::uint8_t>(p.flags & ~bit);
    }
}

} // namespace

void read_tiled_tileset(DataReader &in, Tileset &t, ParseState &st) {
    in.ignore_unknown();
    const int tw = in.integer("tilewidth", 1, 4096);
    const int th = in.integer("tileheight", 1, 4096);
    if (in.failed()) return;
    if (tw != th) {
        unsupported(st, in, "tiles must be square (tilewidth == tileheight)");
        return;
    }
    t.tile_size = tw;
    const std::string name = in.text_or("name", "");
    if (!name.empty()) t.name = intern(name);
    t.margin = in.integer_or("margin", 0, 0, 4096);
    t.spacing = in.integer_or("spacing", 0, 0, 4096);
    t.columns = in.integer_or("columns", 0, 0, 1 << 20);
    t.count = in.integer_or("tilecount", 0, 0, 1 << 24);
    if (in.has("image")) {
        t.image_path = in.string("image");
        t.image_size = {in.integer_or("imagewidth", 0, 0, 1 << 16),
                        in.integer_or("imageheight", 0, 0, 1 << 16)};
    }
    std::vector<std::pair<StrId, std::string>> props;
    read_tiled_properties(in, props);
    for (const auto &[k, v] : props) {
        if (k == str_id("atlas")) {
            if (v == "none")
                t.use_atlas = false;
            else if (!v.empty())
                t.atlas_group = intern(v);
        }
    }
    settle_tileset_geometry(t);
    if (!in.has("tiles")) return;
    DataArray tiles = in.array("tiles");
    for (std::size_t i = 0; i < tiles.size() && !in.failed(); ++i) {
        DataReader tile = tiles.object(i);
        tile.ignore_unknown();
        if (tile.has("image")) {
            unsupported(st, tile,
                        "image-collection tilesets are not supported (use one tileset image)");
            return;
        }
        const int id = tile.integer("id", 0, (1 << 24) - 1);
        if (tile.failed()) return;
        if (t.count > 0 && id >= t.count) {
            tile.fail("tile id " + std::to_string(id) + " is outside the tileset (tilecount " +
                      std::to_string(t.count) + ")");
            return;
        }
        if (static_cast<std::size_t>(id) >= t.props.size())
            t.props.resize(static_cast<std::size_t>(id) + 1);
        TileProps &p = t.props[static_cast<std::size_t>(id)];
        const std::string kind = tile.has("class") ? tile.text("class") : tile.text_or("type", "");
        if (!kind.empty()) p.kind = intern(kind);
        std::vector<std::pair<StrId, std::string>> tp;
        read_tiled_properties(tile, tp);
        for (const auto &[k, v] : tp)
            apply_tile_property(p, k, v, tile);
        if (tile.has("animation")) read_tiled_animation(tile, p, st);
    }
}

} // namespace detail

TextureRegion Tileset::region(std::uint32_t local) const {
    if (local >= regions.size()) return {};
    return regions[local];
}

const TileProps &Tileset::prop(std::uint32_t local) const {
    static const TileProps kDefault{};
    if (local >= props.size()) return kDefault;
    return props[local];
}

Result<Tileset> parse_tiled_tileset(std::string_view text, std::string_view file_name) {
    return detail::parse_json<Tileset>(text, file_name,
                                       [](DataReader &root, detail::ParseState &st) {
                                           Tileset t;
                                           detail::read_tiled_tileset(root, t, st);
                                           return t;
                                       });
}

Result<Tileset> parse_native_tileset(std::string_view text, std::string_view file_name) {
    return detail::parse_json<Tileset>(text, file_name, [](DataReader &root, detail::ParseState &) {
        Tileset t;
        t.tile_size = root.integer_or("tile_size", 16, 1, 4096);
        t.image_path = root.text_or("image", "");
        t.columns = root.integer_or("columns", 0, 0, 1 << 20);
        t.count = root.integer_or("count", 0, 0, 1 << 24);
        t.margin = root.integer_or("margin", 0, 0, 4096);
        t.spacing = root.integer_or("spacing", 0, 0, 4096);
        const std::string name = root.text_or("name", "");
        if (!name.empty()) t.name = intern(name);
        const std::string atlas = root.text_or("atlas", "default");
        if (atlas == "none")
            t.use_atlas = false;
        else
            t.atlas_group = intern(atlas);
        if (t.image_path.empty() && t.count == 0 && !root.failed()) {
            root.fail("a tileset without \"image\" needs \"count\"");
        }
        detail::settle_tileset_geometry(t);
        if (!root.has("tiles")) return t;
        DataReader tiles = root.object("tiles");
        for (const std::string_view key : tiles.keys()) {
            int id = -1;
            const auto [ptr, ec] = std::from_chars(key.data(), key.data() + key.size(), id);
            if (ec != std::errc{} || ptr != key.data() + key.size() || id < 0 || id >= (1 << 24) ||
                (t.count > 0 && id >= t.count)) {
                tiles.fail("tile key \"" + std::string(key) +
                           "\" must be a local tile id below count");
                break;
            }
            if (static_cast<std::size_t>(id) >= t.props.size())
                t.props.resize(static_cast<std::size_t>(id) + 1);
            TileProps &p = t.props[static_cast<std::size_t>(id)];
            DataReader o = tiles.object(key);
            if (o.has("shape")) {
                const std::string s = o.string("shape");
                if (const auto shape = detail::shape_from_name(s))
                    p.shape = *shape;
                else
                    o.fail("unknown tile shape \"" + s + "\"");
            }
            if (o.has("flags")) {
                DataArray flags = o.array("flags");
                for (std::size_t i = 0; i < flags.size(); ++i) {
                    const std::string f = flags.string(i);
                    if (const auto bit = detail::flag_from_name(f))
                        p.flags = static_cast<std::uint8_t>(p.flags | *bit);
                    else
                        flags.fail("unknown tile flag \"" + f +
                                   "\" (hazard, ladder, water, opaque)");
                }
            }
            p.cost = static_cast<std::uint16_t>(o.integer_or("cost", p.cost, 0, 0xFFFF));
            if (o.has("kind")) p.kind = intern(o.string("kind"));
            if (o.has("anim")) {
                DataReader a = o.object("anim");
                p.anim_first = static_cast<std::uint16_t>(a.integer_or("first", id, 0, 0xFFFF));
                p.anim_count = static_cast<std::uint16_t>(a.integer("count", 1, 0xFFFF));
                p.anim_ms = static_cast<std::uint16_t>(a.integer("ms", 1, 0xFFFF));
            }
        }
        return t;
    });
}

Status finalize_tileset(Tileset &t, App &app, AssetServer &assets) {
    ImageData image = std::move(t.pending_image);
    t.pending_image = ImageData{};
    if (image.width <= 0 || image.height <= 0) {
        detail::settle_tileset_geometry(t);
        return {};
    }
    t.image_size = {image.width, image.height};
    detail::settle_tileset_geometry(t);
    // Every region must lie inside the image (64-bit: columns and tile sizes come from the file).
    // Otherwise tiles past the image would sample whatever sits next to it in the atlas.
    if (t.count > 0) {
        const std::int64_t step = std::int64_t{t.tile_size} + t.spacing;
        const std::int64_t columns = std::max(t.columns, 1);
        const std::int64_t used_cols = std::min<std::int64_t>(t.count, columns);
        const std::int64_t used_rows = (t.count + columns - 1) / columns;
        const std::int64_t right = t.margin + (used_cols - 1) * step + t.tile_size;
        const std::int64_t bottom = t.margin + (used_rows - 1) * step + t.tile_size;
        if (right > image.width || bottom > image.height) {
            return make_error(ErrorCode::Parse,
                              std::format("{}: {} tiles of {} px in {} columns (margin {}, spacing "
                                          "{}) need {} x {} px; the image is {} x {}",
                                          t.image_path, t.count, t.tile_size, columns, t.margin,
                                          t.spacing, right, bottom, image.width, image.height));
        }
    }
    IVec2 base{};
    bool packed = false;
    if (AtlasSet *atlas = t.use_atlas ? app.try_res<AtlasSet>() : nullptr) {
        Result<TextureRegion> r = atlas->add(assets, image, t.atlas_group);
        if (r) {
            t.texture = r->texture;
            base = {static_cast<int>(r->source.x), static_cast<int>(r->source.y)};
            packed = true;
        } else {
            LAM_LOG_WARN("tilemap: tileset image {} kept standalone: {}", t.image_path,
                         r.error().message);
        }
    }
    if (!packed) {
        Texture tex;
        tex.width = image.width;
        tex.height = image.height;
        tex.pixels = std::move(image);
        t.texture = assets.add<Texture>(std::move(tex));
    }
    t.regions.assign(static_cast<std::size_t>(std::max(t.count, 0)), TextureRegion{});
    const float size = static_cast<float>(t.tile_size);
    const int columns = std::max(t.columns, 1);
    const std::int64_t step = std::int64_t{t.tile_size} + t.spacing;
    for (int local = 0; local < t.count; ++local) {
        const std::int64_t col = local % columns;
        const std::int64_t row = local / columns;
        TextureRegion &r = t.regions[static_cast<std::size_t>(local)];
        r.texture = t.texture;
        r.source = Rect{static_cast<float>(base.x + t.margin + col * step),
                        static_cast<float>(base.y + t.margin + row * step), size, size};
        r.size = {size, size};
    }
    return {};
}

namespace {

std::string lower_extension(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    const std::string_view file = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const std::size_t dot = file.find_last_of('.');
    std::string ext(dot == std::string_view::npos ? std::string_view{} : file.substr(dot));
    for (char &c : ext) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return ext;
}

bool ends_with(std::string_view s, std::string_view suffix) {
    if (s.size() < suffix.size()) return false;
    for (std::size_t i = 0; i < suffix.size(); ++i) {
        char c = s[s.size() - suffix.size() + i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != suffix[i]) return false;
    }
    return true;
}

class TilesetLoader final : public AssetLoader<Tileset> {
public:
    std::span<const std::string_view> extensions() const override {
        static constexpr std::string_view kExt[] = {".tsj", ".tileset.json"};
        return kExt;
    }
    Result<Tileset> decode(std::span<const std::byte> bytes, LoadContext &ctx) override {
        const std::string_view text(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        Result<Tileset> r = ends_with(ctx.path(), ".tsj") ? parse_tiled_tileset(text, ctx.path())
                                                          : parse_native_tileset(text, ctx.path());
        if (!r) return r.error();
        Tileset &t = r.value();
        if (!t.image_path.empty()) {
            Result<std::vector<std::byte>> image = ctx.read(t.image_path);
            if (!image) return image.error();
            Result<ImageData> decoded = decode_image(*image, lower_extension(t.image_path));
            if (!decoded)
                return make_error(decoded.error().code,
                                  t.image_path + ": " + decoded.error().message);
            t.pending_image = std::move(decoded.value());
            t.image_size = {t.pending_image.width, t.pending_image.height};
            detail::settle_tileset_geometry(t);
        }
        return std::move(r.value());
    }
    Status finalize(Tileset &t, FinalizeContext &ctx) override {
        return finalize_tileset(t, ctx.app, ctx.server);
    }
};

} // namespace

std::unique_ptr<AssetLoader<Tileset>> make_tileset_loader() {
    return std::make_unique<TilesetLoader>();
}

} // namespace lam
