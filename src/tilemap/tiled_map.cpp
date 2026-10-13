// Tiled .tmj import: orthogonal finite maps, uncompressed layer data, embedded or .tsj tilesets,
// object and group layers. Unknown Tiled keys are ignored.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lam/tilemap/tilemap.h>

#include "detail/parse_util.h"

namespace lam {

namespace {

using detail::ParseState;

int base64_value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Decodes standard base64 (padding optional; whitespace skipped). False on a bad character.
bool decode_base64(std::string_view in, std::vector<std::uint8_t> &out) {
    out.clear();
    std::uint32_t acc = 0;
    int bits = 0;
    for (const char c : in) {
        if (c == '=' || c == ' ' || c == '\n' || c == '\r' || c == '\t') continue;
        const int v = base64_value(c);
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<std::uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xFFu));
        }
    }
    return true;
}

// Reads a tile layer's "data" into `tiles` (w x h).
void read_layer_data(DataReader &l, Grid<TileId> &tiles, int w, int h, ParseState &st) {
    const std::string encoding = l.text_or("encoding", "csv");
    const std::string compression = l.text_or("compression", "");
    if (!compression.empty()) {
        detail::unsupported(
            st, l,
            "compressed layer data (\"" + compression +
                "\") is not supported; save the map with CSV or uncompressed base64");
        return;
    }
    tiles.resize(w, h, 0);
    const std::size_t cells = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    const auto out = tiles.cells();
    if (l.type("data") == DataType::Array) {
        DataArray data = l.array("data");
        if (data.size() != cells) {
            data.fail("holds " + std::to_string(data.size()) + " tiles, the layer has " +
                      std::to_string(cells));
            return;
        }
        for (std::size_t i = 0; i < cells; ++i)
            out[i] = static_cast<TileId>(data.integer64(i, 0, 0xFFFFFFFFll));
        return;
    }
    const std::string text = l.string("data");
    if (l.failed()) return;
    if (encoding == "base64") {
        std::vector<std::uint8_t> bytes;
        if (!decode_base64(text, bytes)) {
            l.fail("data is not valid base64");
            return;
        }
        if (bytes.size() != cells * 4) {
            l.fail("base64 data holds " + std::to_string(bytes.size() / 4) +
                   " tiles, the layer has " + std::to_string(cells));
            return;
        }
        for (std::size_t i = 0; i < cells; ++i) {
            out[i] = static_cast<TileId>(bytes[i * 4]) |
                     (static_cast<TileId>(bytes[i * 4 + 1]) << 8) |
                     (static_cast<TileId>(bytes[i * 4 + 2]) << 16) |
                     (static_cast<TileId>(bytes[i * 4 + 3]) << 24);
        }
        return;
    }
    if (encoding != "csv") {
        detail::unsupported(st, l, "layer encoding \"" + encoding + "\" is not supported");
        return;
    }
    std::size_t i = 0;
    std::uint64_t value = 0;
    bool digits = false;
    const auto flush = [&]() {
        if (!digits) return true;
        if (i >= cells || value > 0xFFFFFFFFull) return false;
        out[i++] = static_cast<TileId>(value);
        value = 0;
        digits = false;
        return true;
    };
    for (const char c : text) {
        if (c >= '0' && c <= '9') {
            value = value * 10 + static_cast<std::uint64_t>(c - '0');
            digits = true;
            if (value > 0xFFFFFFFFull) break;
        } else if (c == ',' || c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            if (!flush()) break;
        } else {
            l.fail("CSV data holds a character that is not a digit or a comma");
            return;
        }
    }
    if (!flush() || i != cells) l.fail("CSV data does not hold exactly width x height tiles");
}

struct TiledReader {
    TilemapAsset &m;
    ParseState &st;

    void layers(DataReader &owner, bool visible, float opacity) {
        DataArray arr = owner.array("layers");
        for (std::size_t i = 0; i < arr.size() && !owner.failed(); ++i) {
            DataReader l = arr.object(i);
            l.ignore_unknown();
            const std::string type = l.string("type");
            const bool layer_visible = visible && l.boolean_or("visible", true);
            const float layer_opacity = opacity * l.number_or("opacity", 1, 0, 1);
            if (type == "tilelayer") {
                tile_layer(l, layer_visible, layer_opacity);
            } else if (type == "objectgroup") {
                object_layer(l);
            } else if (type == "group") {
                layers(l, layer_visible, layer_opacity);
            }
            // imagelayer and anything newer: ignored
        }
    }

    void tile_layer(DataReader &l, bool visible, float opacity) {
        if (l.has("chunks")) {
            detail::unsupported(st, l, "infinite (chunked) layers are not supported");
            return;
        }
        TileLayer layer;
        const std::string name = l.text_or("name", "");
        layer.name = name.empty() ? StrId{} : intern(name);
        layer.visible = visible;
        layer.opacity = opacity;
        layer.parallax = {l.number_or("parallaxx", 1, -100, 100),
                          l.number_or("parallaxy", 1, -100, 100)};
        layer.collision = true;
        const int w = l.integer_or("width", m.width, 0, 1 << 16);
        const int h = l.integer_or("height", m.height, 0, 1 << 16);
        if (w != m.width || h != m.height) {
            l.fail("layer size differs from the map size");
            return;
        }
        std::vector<std::pair<StrId, std::string>> props;
        detail::read_tiled_properties(l, props);
        layer.collision = prop_bool(props, str_id("collision"), true);
        const int render_layer = prop_int(props, str_id("render_layer"), kLayerTiles);
        if (render_layer < 0 || render_layer > 255) {
            l.fail("property render_layer must be 0..255");
            return;
        }
        layer.render_layer = static_cast<std::uint8_t>(render_layer);
        read_layer_data(l, layer.tiles, w, h, st);
        m.layers.push_back(std::move(layer));
    }

    void object_layer(DataReader &l) {
        if (!l.has("objects")) return;
        DataArray objects = l.array("objects");
        for (std::size_t i = 0; i < objects.size(); ++i) {
            DataReader o = objects.object(i);
            o.ignore_unknown();
            MapObject obj;
            const std::string type = o.has("class") ? o.text("class") : o.text_or("type", "");
            const std::string name = o.text_or("name", "");
            obj.type = type.empty() ? StrId{} : intern(type);
            obj.name = name.empty() ? StrId{} : intern(name);
            obj.rect.x = o.number_or("x", 0, -1e9f, 1e9f);
            obj.rect.y = o.number_or("y", 0, -1e9f, 1e9f);
            obj.rect.w = o.number_or("width", 0, 0, 1e9f);
            obj.rect.h = o.number_or("height", 0, 0, 1e9f);
            if (o.has("gid")) obj.rect.y -= obj.rect.h; // tile objects are anchored bottom-left
            detail::read_tiled_properties(o, obj.props);
            m.objects.push_back(std::move(obj));
        }
    }

    void tilesets(DataReader &root) {
        if (!root.has("tilesets")) return;
        DataArray arr = root.array("tilesets");
        if (arr.size() > static_cast<std::size_t>(kMaxMapTilesets)) {
            detail::unsupported(st, root,
                                "more than " + std::to_string(kMaxMapTilesets) + " tilesets");
            return;
        }
        TileId last = 0;
        for (std::size_t i = 0; i < arr.size() && !root.failed(); ++i) {
            DataReader t = arr.object(i);
            t.ignore_unknown();
            TilesetRef ref;
            ref.first_gid = static_cast<TileId>(t.integer64("firstgid", 1, kTileIdMask));
            if (ref.first_gid <= last && i > 0) {
                t.fail("firstgid must increase from one tileset to the next");
                return;
            }
            last = ref.first_gid;
            if (t.has("source")) {
                ref.source = t.string("source");
                if (ref.source.size() >= 4 && ref.source.substr(ref.source.size() - 4) == ".tsx") {
                    detail::unsupported(st, t,
                                        "XML tileset \"" + ref.source + "\": export it as .tsj");
                    return;
                }
            } else {
                Tileset embedded;
                detail::read_tiled_tileset(t, embedded, st);
                ref.embedded = static_cast<std::int32_t>(m.embedded_tilesets.size());
                m.embedded_tilesets.push_back(std::move(embedded));
            }
            m.tilesets.push_back(std::move(ref));
        }
    }
};

} // namespace

Result<TilemapAsset> parse_tiled_json(std::string_view text, std::string_view file_name) {
    return detail::parse_json<TilemapAsset>(text, file_name, [](DataReader &root, ParseState &st) {
        TilemapAsset m;
        root.ignore_unknown();
        const std::string orientation = root.text_or("orientation", "orthogonal");
        if (orientation != "orthogonal") {
            detail::unsupported(st, root,
                                "orientation \"" + orientation + "\" (only orthogonal maps)");
            return m;
        }
        if (root.boolean_or("infinite", false)) {
            detail::unsupported(st, root, "infinite maps are not supported");
            return m;
        }
        m.width = root.integer("width", 0, 1 << 16);
        m.height = root.integer("height", 0, 1 << 16);
        const int tw = root.integer("tilewidth", 1, 4096);
        const int th = root.integer("tileheight", 1, 4096);
        if (root.failed()) return m;
        if (tw != th) {
            detail::unsupported(st, root, "tiles must be square (tilewidth == tileheight)");
            return m;
        }
        if (static_cast<std::int64_t>(m.width) * m.height > (1ll << 26)) {
            root.fail("map too large");
            return m;
        }
        m.tile_size = tw;
        TiledReader reader{m, st};
        reader.tilesets(root);
        if (root.has("layers")) reader.layers(root, true, 1.0f);
        detail::read_tiled_properties(root, m.properties);
        // The map's background colour, as Tiled writes it ("#rrggbb" / "#aarrggbb"), unless a
        // custom property already has that name.
        const std::string background = root.text_or("backgroundcolor", "");
        const StrId background_key = intern("backgroundcolor");
        const bool custom = std::any_of(m.properties.begin(), m.properties.end(),
                                        [&](const auto &p) { return p.first == background_key; });
        if (!background.empty() && !custom) m.properties.emplace_back(background_key, background);
        return m;
    });
}

} // namespace lam
