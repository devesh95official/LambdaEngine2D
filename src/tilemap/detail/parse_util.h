#pragma once
// Helpers shared by the Tiled and native parsers: error codes, property text, shape and flag
// names, and the Tiled property and tileset readers.
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lam/core/data.h>
#include <lam/core/id.h>
#include <lam/core/result.h>
#include <lam/physics/tile_world.h>
#include <lam/tilemap/tileset.h>

namespace lam::detail {

// Error code to report instead of Parse (set with unsupported()).
struct ParseState {
    std::optional<ErrorCode> code;
};

// Records an Unsupported error at `in`'s path (the first error wins).
void unsupported(ParseState &st, DataReader &in, std::string_view message);

// DataDocument::parse + build(root, state) + finish, no exception leaves. Errors keep the file and
// JSON path; the code is ParseState::code when set, else Parse.
template <class T, class Build>
Result<T> parse_json(std::string_view text, std::string_view file_name, Build build) {
    ParseState st;
    Result<DataDocument> doc = DataDocument::parse(text, file_name);
    if (!doc) return doc.error();
    try {
        DataReader root = doc->root();
        T value = build(root, st);
        Status s = doc->finish();
        if (!s) {
            Error e = s.error();
            if (st.code) e.code = *st.code;
            return e;
        }
        return value;
    } catch (...) {
        return make_error(ErrorCode::Parse,
                          std::string(file_name) + ": exception while building data");
    }
}

// "3", "2.5", "true": a JSON scalar as text (shortest round-trip form for numbers).
std::string number_text(double v);
// The value of `key` (bool, number or string) as text; null/object/array -> nullopt (marked read).
std::optional<std::string> scalar_text(DataReader &in, std::string_view key);
std::optional<TileShape> shape_from_name(std::string_view name);
std::optional<std::uint8_t> flag_from_name(std::string_view name); // hazard, ladder, water, opaque

// Appends a Tiled "properties" array ([{name, type, value}]) of `owner` as (name, text) pairs.
void read_tiled_properties(DataReader &owner, std::vector<std::pair<StrId, std::string>> &out);
// Reads a Tiled tileset object (a .tsj root or a map's embedded tileset) into `t`.
void read_tiled_tileset(DataReader &in, Tileset &t, ParseState &st);
// Tiles a tileset may have (explicit "count"/"tilecount", or derived from the image size).
inline constexpr int kMaxTilesetTiles = 1 << 24;
// Sizes props to `count` and fills columns/count from the image size when missing (the derived
// count is capped at kMaxTilesetTiles).
void settle_tileset_geometry(Tileset &t);

} // namespace lam::detail
