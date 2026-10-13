#pragma once
// Sprite sheets from Aseprite's "Export Sprite Sheet" JSON, or a bare .png as one frame.
// finalize packs the image into an atlas so sheets batch with tiles and other sprites.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <lam/core/asset_id.h>
#include <lam/core/id.h>
#include <lam/core/result.h>
#include <lam/math/rect.h>
#include <lam/render/texture.h>
namespace lam {
// Aseprite tag direction. Each ping-pong sweep counts as one pass, and end frames aren't shown
// twice at a turn.
enum class AnimDirection : std::uint8_t { Forward, Reverse, PingPong, PingPongReverse };
// One frame. `source` is in sheet texels; a trimmed frame's pixels go at trim_offset inside a
// source_size frame. duration_ms is 1..65535.
struct SheetFrame { Rect source; Vec2 trim_offset; Vec2 source_size; std::uint16_t duration_ms = 100; };
// A named frame range (Aseprite frame tag). from <= to < frames.size(); frames are sheet indices.
struct SheetTag {
    StrId name;                                                 // unique in the sheet
    std::uint16_t from = 0, to = 0;                             // inclusive sheet frame range
    AnimDirection dir = AnimDirection::Forward;                 // playback order
    std::uint16_t repeat = 0;                                   // 0 = loop forever
};
// Slice bounds from sheet frame `frame` until the next key, in untrimmed frame pixels.
struct SliceKey { std::uint16_t frame = 0; Rect bounds; Vec2 pivot; };
// A named Aseprite slice (hitboxes, hurtboxes, attach points). Before its first key it is absent.
struct SheetSlice { StrId name; std::vector<SliceKey> keys; };  // keys sorted by frame
// Largest sheet image side accepted (old mobile GPUs stop at 2048; also the atlas page size).
inline constexpr int kMaxSheetImageSide = 2048;
// After finalize, regions[i] is frame i in its atlas page (or own texture). Look the sheet up by
// AssetId each time rather than keeping a pointer.
struct SpriteSheet {
    AssetId<Texture> texture;                                   // atlas page after finalize
    std::string image_path;                                     // meta.image, relative to the JSON file
    std::vector<SheetFrame> frames;                             // file order; 1..65535 frames
    std::vector<TextureRegion> regions;                         // per frame, atlas-mapped
    std::vector<SheetTag> tags;                                 // file order (Aseprite frameTags)
    std::vector<SheetSlice> slices;                             // file order (Aseprite slices)
    ImageData pending_image;                                    // decode -> finalize; cleared
    IVec2 image_size;                                           // meta.size (texels); frames lie inside it
    StrId atlas_group = str_id("default");                      // "atlas" key; str_id("none") = own texture
    int find_tag(StrId name) const;                             // -1 if missing
    int find_slice(StrId name) const;                           // -1 if missing; linear (load time or rare)
};
// Parses an Aseprite export (array or hash "frames", frameTags, slices). Unknown keys are
// ignored so real exports load unchanged. Reads no image; finalize adds texture and regions.
Result<SpriteSheet> parse_aseprite_json(std::string_view text, std::string_view file_name);
// One-frame sheet for a bare .png: frame 0 is the whole image, 100 ms, no tags.
SpriteSheet make_single_frame_sheet(IVec2 size, std::string_view image_path);
// Region of `frame`: regions[frame] once finalized, else built from frames[frame] and texture.
TextureRegion frame_region(const SpriteSheet &sheet, std::size_t frame);
}
