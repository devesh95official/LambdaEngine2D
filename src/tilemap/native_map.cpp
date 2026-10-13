// Native maps: strict JSON with ASCII terrain rows, or raw ASCII text. The format is documented
// in lam/tilemap/tilemap.h.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lam/tilemap/tilemap.h>

#include "detail/parse_util.h"

namespace lam {

namespace {

using detail::ParseState;

constexpr int kMaxSide = 1 << 16;
// Cells a map may have (every path: ASCII text, rows, width/height), checked before any grid is
// sized, so a small file cannot ask for gigabytes.
constexpr std::int64_t kMaxCells = 1ll << 26;
constexpr TileId kTerrainGidWithTilesets = 1u << 24;

struct TerrainDef {
    StrId name;
    TileProps props;
    TileId tile = 0; // visual GID for the "ground" layer; 0 = none
};

struct LegendEntry {
    std::uint8_t terrain = 0; // value (index + 1)
    StrId object;
    TileId tile = 0;
    bool used = false;
};

// Terrain table in value order; returns the value (index + 1), 0 when there are too many.
std::uint8_t ensure_terrain(std::vector<TerrainDef> &defs, StrId name) {
    for (std::size_t i = 0; i < defs.size(); ++i) {
        if (defs[i].name == name) return static_cast<std::uint8_t>(i + 1);
    }
    if (defs.size() >= 255) return 0;
    defs.push_back(TerrainDef{name, {}, 0});
    return static_cast<std::uint8_t>(defs.size());
}

// The props-only tileset of the terrains, its reference, and the hidden collision layer
// "terrain" (GID = first_gid + value - 1).
void add_terrain_set(TilemapAsset &m, const std::vector<TerrainDef> &defs, TileId first_gid) {
    m.terrains.clear();
    Tileset set;
    set.tile_size = m.tile_size;
    set.count = static_cast<int>(defs.size());
    set.name = str_id("terrain");
    for (const TerrainDef &d : defs) {
        m.terrains.push_back(d.name);
        set.props.push_back(d.props);
    }
    TilesetRef ref;
    ref.first_gid = first_gid;
    ref.embedded = static_cast<std::int32_t>(m.embedded_tilesets.size());
    m.embedded_tilesets.push_back(std::move(set));
    m.tilesets.push_back(std::move(ref));

    TileLayer layer;
    layer.name = intern("terrain");
    layer.collision = true;
    layer.visible = false;
    layer.tiles.resize(m.width, m.height, 0);
    const auto src = m.terrain.cells();
    const auto dst = layer.tiles.cells();
    for (std::size_t i = 0; i < src.size(); ++i)
        dst[i] = src[i] == 0 ? 0 : first_gid + src[i] - 1u;
    m.layers.push_back(std::move(layer));
}

Result<TilemapAsset> parse_ascii(std::string_view text, std::string_view file_name) {
    std::vector<std::string_view> rows;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string_view row = text.substr(start, end - start);
        if (!row.empty() && row.back() == '\r') row.remove_suffix(1);
        rows.push_back(row);
        if (end == text.size()) break;
        start = end + 1;
    }
    const auto blank = [](std::string_view r) {
        return std::all_of(r.begin(), r.end(), [](char c) { return c == ' ' || c == '\t'; });
    };
    while (!rows.empty() && blank(rows.front()))
        rows.erase(rows.begin());
    while (!rows.empty() && blank(rows.back()))
        rows.pop_back();
    const std::string where(file_name);
    if (rows.empty()) return make_error(ErrorCode::Parse, where + ": no rows");
    if (rows.size() > static_cast<std::size_t>(kMaxSide))
        return make_error(ErrorCode::Parse, where + ": too many rows");

    TilemapAsset m;
    m.height = static_cast<int>(rows.size());
    for (const std::string_view r : rows)
        m.width = std::max(m.width, static_cast<int>(r.size()));
    if (m.width > kMaxSide) return make_error(ErrorCode::Parse, where + ": rows too long");
    if (static_cast<std::int64_t>(m.width) * m.height > kMaxCells)
        return make_error(ErrorCode::Parse, where + ": map too large");
    m.terrain.resize(m.width, m.height, 0);
    std::vector<TerrainDef> defs;
    for (int y = 0; y < m.height; ++y) {
        const std::string_view r = rows[static_cast<std::size_t>(y)];
        for (int x = 0; x < static_cast<int>(r.size()); ++x) {
            const char c = r[static_cast<std::size_t>(x)];
            if (c == ' ') continue;
            if (static_cast<unsigned char>(c) < 0x21 || static_cast<unsigned char>(c) > 0x7E) {
                return make_error(ErrorCode::Parse, where + ": row " + std::to_string(y) +
                                                        ", column " + std::to_string(x) +
                                                        ": only printable ASCII characters");
            }
            const std::uint8_t v = ensure_terrain(defs, intern(std::string_view(&c, 1)));
            if (v == 0)
                return make_error(ErrorCode::Parse, where + ": more than 255 distinct characters");
            m.terrain(x, y) = v;
        }
    }
    for (TerrainDef &d : defs) {
        if (d.name == str_id("#")) {
            d.props.shape = TileShape::Solid;
            d.props.flags = kTileOpaque;
            d.props.cost = 0;
        }
    }
    add_terrain_set(m, defs, 1);
    return m;
}

struct NativeReader {
    DataReader &root;
    ParseState &st;
    TilemapAsset &m;
    std::vector<TerrainDef> defs;
    std::vector<std::pair<char, LegendEntry>> legend;
    Grid<std::uint8_t> legend_at; // legend entry index + 1 per cell

    TileId gid(DataReader &in, std::string_view key) {
        return static_cast<TileId>(in.integer64(key, 0, kTileIdMask));
    }

    void tilesets() {
        if (!root.has("tilesets")) return;
        DataArray arr = root.array("tilesets");
        if (arr.size() >= static_cast<std::size_t>(kMaxMapTilesets)) {
            detail::unsupported(st, root,
                                "more than " + std::to_string(kMaxMapTilesets - 1) + " tilesets");
            return;
        }
        TileId last = 0;
        for (std::size_t i = 0; i < arr.size() && !root.failed(); ++i) {
            TilesetRef ref;
            if (arr.type(i) == DataType::String) {
                ref.source = arr.string(i);
                if (i > 0) {
                    arr.fail("only the first tileset may omit first_gid (use {\"source\", "
                             "\"first_gid\"})");
                    return;
                }
            } else {
                DataReader o = arr.object(i);
                ref.source = o.string("source");
                if (o.has("first_gid"))
                    ref.first_gid = gid(o, "first_gid");
                else if (i > 0)
                    o.fail("needs \"first_gid\"");
            }
            if (ref.first_gid == 0 || (i > 0 && ref.first_gid <= last)) {
                arr.fail("first_gid must be >= 1 and increase from one tileset to the next");
                return;
            }
            last = ref.first_gid;
            m.tilesets.push_back(std::move(ref));
        }
    }

    void terrain_props(DataReader &o, TerrainDef &d) {
        if (o.has("shape")) {
            const std::string s = o.string("shape");
            if (const auto shape = detail::shape_from_name(s))
                d.props.shape = *shape;
            else
                o.fail("unknown tile shape \"" + s + "\"");
        }
        if (o.has("flags")) {
            DataArray flags = o.array("flags");
            for (std::size_t i = 0; i < flags.size(); ++i) {
                const std::string f = flags.string(i);
                if (const auto bit = detail::flag_from_name(f))
                    d.props.flags = static_cast<std::uint8_t>(d.props.flags | *bit);
                else
                    flags.fail("unknown tile flag \"" + f + "\" (hazard, ladder, water, opaque)");
            }
        }
        d.props.cost = static_cast<std::uint16_t>(o.integer_or("cost", d.props.cost, 0, 0xFFFF));
        if (o.has("kind")) d.props.kind = intern(o.string("kind"));
        if (o.has("tile")) d.tile = gid(o, "tile");
    }

    void read_legend() {
        if (!root.has("legend")) return;
        DataReader lg = root.object("legend");
        for (const std::string_view key : lg.keys()) {
            if (key.size() != 1) {
                lg.fail("\"" + std::string(key) + "\": keys must be one character");
                return;
            }
            LegendEntry e;
            std::string terrain;
            const DataType type = lg.type(key);
            if (type == DataType::String) {
                terrain = lg.string(key);
            } else if (type == DataType::Object) {
                DataReader o = lg.object(key);
                terrain = o.string("terrain");
                if (o.has("object")) e.object = intern(o.string("object"));
                if (o.has("tile")) e.tile = gid(o, "tile");
            } else if (type == DataType::Number) {
                lg.fail("\"" + std::string(key) +
                        "\": legend values are terrain names (\"#\": \"wall\"), not numbers; a "
                        "tile GID goes in {\"terrain\": name, \"tile\": gid}");
                return;
            } else {
                lg.fail("\"" + std::string(key) +
                        "\": must be a terrain name or {\"terrain\", \"object\", \"tile\"}");
                return;
            }
            if (lg.failed()) return;
            if (terrain.empty()) {
                lg.fail("\"" + std::string(key) + "\": empty terrain name");
                return;
            }
            e.terrain = ensure_terrain(defs, intern(terrain));
            if (e.terrain == 0) {
                lg.fail("more than 255 terrains");
                return;
            }
            legend.emplace_back(key[0], e);
        }
    }

    void read_terrains() {
        if (!root.has("terrains")) return;
        DataReader ts = root.object("terrains");
        for (const std::string_view key : ts.keys()) {
            const std::uint8_t v = ensure_terrain(defs, intern(key));
            if (v == 0) {
                ts.fail("more than 255 terrains");
                return;
            }
            DataReader o = ts.object(key);
            terrain_props(o, defs[v - 1u]);
        }
    }

    bool read_rows() {
        if (!root.has("rows")) return false;
        DataArray rows = root.array("rows");
        if (rows.size() == 0) {
            rows.fail("must be a non-empty array of strings");
            return false;
        }
        std::vector<std::string> text;
        text.reserve(rows.size());
        int width = 0;
        for (std::size_t y = 0; y < rows.size(); ++y) {
            text.push_back(rows.string(y));
            width = std::max(width, static_cast<int>(text.back().size()));
        }
        if (root.failed()) return false;
        if (width > kMaxSide || rows.size() > static_cast<std::size_t>(kMaxSide) ||
            static_cast<std::int64_t>(width) * static_cast<std::int64_t>(rows.size()) > kMaxCells) {
            rows.fail("map too large");
            return false;
        }
        m.width = width;
        m.height = static_cast<int>(rows.size());
        m.terrain.resize(m.width, m.height, 0);
        legend_at.resize(m.width, m.height, 0);
        for (int y = 0; y < m.height; ++y) {
            const std::string &r = text[static_cast<std::size_t>(y)];
            if (static_cast<int>(r.size()) != width) {
                rows.fail("/rows/" + std::to_string(y) + " is " + std::to_string(r.size()) +
                          " wide, the map is " + std::to_string(width));
                return false;
            }
            for (int x = 0; x < width; ++x) {
                const char c = r[static_cast<std::size_t>(x)];
                const auto it = std::find_if(legend.begin(), legend.end(),
                                             [&](const auto &p) { return p.first == c; });
                if (it == legend.end()) {
                    rows.fail("/rows/" + std::to_string(y) + ": '" + std::string(1, c) +
                              "' at column " + std::to_string(x) + " is not in the legend");
                    return false;
                }
                m.terrain(x, y) = it->second.terrain;
                legend_at(x, y) = static_cast<std::uint8_t>((it - legend.begin()) + 1);
                if (it->second.object) {
                    const float s = static_cast<float>(m.tile_size);
                    m.objects.push_back(
                        MapObject{it->second.object,
                                  {},
                                  Rect{static_cast<float>(x) * s, static_cast<float>(y) * s, s, s},
                                  {}});
                }
            }
        }
        return true;
    }

    void read_autotile() {
        DataReader a = root.object("autotile");
        const std::string layer_name = a.text_or("layer", "ground");
        m.autotile_layer = intern(layer_name.empty() ? std::string("ground") : layer_name);
        m.autotile_seed = static_cast<std::uint32_t>(a.integer64("seed", 0, 0xFFFFFFFFll));
        DataArray rules = a.array("rules");
        for (std::size_t i = 0; i < rules.size() && !root.failed(); ++i) {
            DataReader r = rules.object(i);
            AutotileRule rule;
            const std::string terrain = r.string("terrain");
            rule.terrain = intern(terrain);
            rule.terrain_value = terrain_value(m, rule.terrain);
            if (rule.terrain_value == 0 && !r.failed()) {
                r.fail("unknown terrain \"" + terrain + "\"");
                return;
            }
            const std::string mode = r.text_or("mode", "edge4");
            if (mode == "edge4")
                rule.mode = AutotileMode::Edge4;
            else if (mode == "blob47")
                rule.mode = AutotileMode::Blob47;
            else if (mode == "plain")
                rule.mode = AutotileMode::Plain;
            else
                r.fail("mode must be edge4, blob47 or plain");
            rule.first = gid(r, "first");
            rule.variants = static_cast<std::uint8_t>(r.integer_or("variants", 1, 1, 255));
            // Every GID the rule can produce (first + mask * variants + variant) must stay below
            // the flip bits, else a tile would come out flipped or as another tileset's.
            const std::uint64_t masks = rule.mode == AutotileMode::Blob47  ? 47u
                                        : rule.mode == AutotileMode::Edge4 ? 16u
                                                                           : 1u;
            if (!r.failed() &&
                std::uint64_t{rule.first} + masks * rule.variants - 1u > kTileIdMask) {
                r.fail("\"first\" + " + std::to_string(masks * rule.variants) +
                       " tiles of this rule exceed the GID range (" + std::to_string(kTileIdMask) +
                       ")");
                return;
            }
            rule.edge_outside = r.boolean_or("edge_outside", true);
            if (r.has("blends")) {
                DataArray blends = r.array("blends");
                for (std::size_t k = 0; k < blends.size(); ++k) {
                    const StrId b = intern(blends.string(k));
                    const std::uint8_t v = terrain_value(m, b);
                    if (v == 0)
                        blends.fail("unknown terrain in blends");
                    else
                        rule.blends.push_back(v);
                }
            }
            if (r.has("details")) {
                DataArray details = r.array("details");
                for (std::size_t k = 0; k < details.size(); ++k) {
                    rule.details.push_back(
                        static_cast<TileId>(details.integer64(k, 0, kTileIdMask)));
                }
            }
            rule.detail_percent =
                static_cast<std::uint8_t>(r.integer_or("detail_percent", 0, 0, 100));
            m.autotile.push_back(std::move(rule));
        }
        if (root.failed()) return;
        TileLayer ground;
        ground.name = m.autotile_layer;
        autotile(m.terrain, m.autotile, m.autotile_seed, ground.tiles);
        m.layers.push_back(std::move(ground));
        if (a.has("details_layer")) {
            TileLayer deco;
            deco.name = intern(a.string("details_layer"));
            autotile_details(m.terrain, m.autotile, m.autotile_seed, deco.tiles);
            m.layers.push_back(std::move(deco));
        }
    }

    // "ground" from the legend's and terrains' "tile" GIDs (when there are no autotile rules).
    void tile_ground() {
        bool any = false;
        for (const auto &[c, e] : legend)
            any = any || e.tile != 0;
        for (const TerrainDef &d : defs)
            any = any || d.tile != 0;
        if (!any) return;
        TileLayer ground;
        ground.name = intern("ground");
        ground.tiles.resize(m.width, m.height, 0);
        for (int y = 0; y < m.height; ++y) {
            for (int x = 0; x < m.width; ++x) {
                const std::uint8_t li = legend_at(x, y);
                TileId t = li ? legend[li - 1u].second.tile : 0;
                const std::uint8_t v = m.terrain(x, y);
                if (t == 0 && v != 0) t = defs[v - 1u].tile;
                ground.tiles(x, y) = t;
            }
        }
        m.layers.push_back(std::move(ground));
    }

    void read_layers() {
        DataArray arr = root.array("layers");
        for (std::size_t i = 0; i < arr.size() && !root.failed(); ++i) {
            DataReader l = arr.object(i);
            if (l.has("rows")) {
                l.fail("layers hold GID \"data\" arrays; ASCII rows go in the top-level \"rows\" "
                       "with a \"legend\"");
                return;
            }
            TileLayer layer;
            layer.name = intern(l.string("name"));
            layer.collision = l.boolean_or("collision", true);
            layer.visible = l.boolean_or("visible", true);
            layer.opacity = l.number_or("opacity", 1, 0, 1);
            layer.render_layer =
                static_cast<std::uint8_t>(l.integer_or("render_layer", kLayerTiles, 0, 255));
            if (l.has("parallax")) layer.parallax = l.vec2("parallax");
            DataArray data = l.array("data");
            const std::size_t cells =
                static_cast<std::size_t>(m.width) * static_cast<std::size_t>(m.height);
            if (data.size() != cells) {
                data.fail("holds " + std::to_string(data.size()) + " tiles, the map has " +
                          std::to_string(cells));
                return;
            }
            layer.tiles.resize(m.width, m.height, 0);
            const auto out = layer.tiles.cells();
            for (std::size_t k = 0; k < cells; ++k)
                out[k] = static_cast<TileId>(data.integer64(k, 0, 0xFFFFFFFFll));
            m.layers.push_back(std::move(layer));
        }
    }

    void read_objects() {
        DataArray arr = root.array("objects");
        for (std::size_t i = 0; i < arr.size() && !root.failed(); ++i) {
            DataReader o = arr.object(i);
            if (o.has("rect")) {
                o.fail("objects use cell coordinates \"x\", \"y\" (and \"w\", \"h\"), not \"rect\"");
                return;
            }
            MapObject obj;
            obj.type = intern(o.string("type"));
            const int x = o.integer("x", -kMaxSide, kMaxSide);
            const int y = o.integer("y", -kMaxSide, kMaxSide);
            const int w = o.integer_or("w", 1, 0, kMaxSide);
            const int h = o.integer_or("h", 1, 0, kMaxSide);
            if (o.has("name")) obj.name = intern(o.string("name"));
            if (o.failed()) return;
            if (x < 0 || y < 0 || x >= m.width || y >= m.height) {
                o.fail("(" + std::to_string(x) + ", " + std::to_string(y) +
                       ") is outside the map (x and y are cells, not pixels)");
                return;
            }
            const float s = static_cast<float>(m.tile_size);
            obj.rect = {static_cast<float>(x) * s, static_cast<float>(y) * s,
                        static_cast<float>(w) * s, static_cast<float>(h) * s};
            for (const std::string_view key : o.keys()) {
                if (key == "type" || key == "x" || key == "y" || key == "w" || key == "h" ||
                    key == "name")
                    continue;
                if (std::optional<std::string> v = detail::scalar_text(o, key))
                    obj.props.emplace_back(intern(key), *v);
            }
            m.objects.push_back(std::move(obj));
        }
    }

    void read_properties() {
        DataReader p = root.object("properties");
        for (const std::string_view key : p.keys()) {
            if (std::optional<std::string> v = detail::scalar_text(p, key))
                m.properties.emplace_back(intern(key), *v);
        }
    }

    void run() {
        m.tile_size = root.integer_or("tile_size", 16, 1, 4096);
        tilesets();
        read_legend();
        read_terrains();
        const bool has_rows = read_rows();
        if (root.failed()) return;
        if (root.has("width") || root.has("height") || !has_rows) {
            const int w = root.integer("width", 1, kMaxSide);
            const int h = root.integer("height", 1, kMaxSide);
            if (root.failed()) return;
            if (has_rows && (w != m.width || h != m.height)) {
                root.fail("width/height differ from the rows");
                return;
            }
            m.width = w;
            m.height = h;
        }
        if (static_cast<std::int64_t>(m.width) * m.height > kMaxCells) {
            root.fail("map too large");
            return;
        }
        if (!has_rows) m.terrain.resize(m.width, m.height, 0);
        if (!defs.empty()) {
            const TileId first = root.has("terrain_first_gid")
                                     ? gid(root, "terrain_first_gid")
                                     : (m.tilesets.empty() ? 1u : kTerrainGidWithTilesets);
            if (first == 0 || (!m.tilesets.empty() && first <= m.tilesets.back().first_gid)) {
                root.fail("terrain_first_gid must be above every tileset's first_gid");
                return;
            }
            // The last terrain's GID must stay below the flip bits (else it reads as flipped).
            if (std::uint64_t{first} + defs.size() - 1u > kTileIdMask) {
                root.fail("terrain_first_gid + " + std::to_string(defs.size()) +
                          " terrains exceed the GID range (" + std::to_string(kTileIdMask) + ")");
                return;
            }
            add_terrain_set(m, defs, first);
        }
        if (root.has("autotile")) {
            if (!has_rows) {
                root.fail("\"autotile\" needs \"rows\"");
                return;
            }
            read_autotile();
        } else if (has_rows) {
            tile_ground();
        }
        if (root.has("layers")) read_layers();
        if (root.has("objects")) read_objects();
        if (root.has("properties")) read_properties();
    }
};

} // namespace

Result<TilemapAsset> parse_native_map(std::string_view text, std::string_view file_name) {
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3); // UTF-8 BOM
    std::size_t i = 0;
    while (i < text.size() &&
           (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n'))
        ++i;
    if (i < text.size() && text[i] == '[' && DataDocument::parse(text, file_name).ok()) {
        // Valid JSON that is not an object: a mistake, not ASCII art.
        return make_error(ErrorCode::Parse,
                          std::string(file_name) + ": a native map is a JSON object or ASCII rows");
    }
    if (i >= text.size() || text[i] != '{') {
        try {
            return parse_ascii(text, file_name);
        } catch (...) { // no exception leaves a loader (AssetServer does not catch around decode)
            return make_error(ErrorCode::Parse,
                              std::string(file_name) + ": exception while building data");
        }
    }
    return detail::parse_json<TilemapAsset>(text, file_name, [](DataReader &root, ParseState &st) {
        TilemapAsset m;
        NativeReader reader{root, st, m, {}, {}, {}};
        reader.run();
        return m;
    });
}

} // namespace lam
