// Aseprite sheet parsing: array and hash frames, trim, tags in all four directions, slices.
// Strict about the values it reads, but ignores keys it doesn't, since exporters add plenty.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <lam/animation/sprite_sheet.h>
#include <lam/core/data.h>

namespace lam {
namespace {

// Pixel coordinates far beyond any accepted image, so sums never overflow an int.
constexpr int kMaxCoord = 1 << 24;

// A frame, tag or slice name with its index, kept only to report duplicates.
struct Named {
    std::string name;
    std::size_t index = 0;
};

// The index of the first entry whose name already appeared earlier, or -1.
long first_duplicate(std::vector<Named> names) {
    std::stable_sort(names.begin(), names.end(),
                     [](const Named &a, const Named &b) { return a.name < b.name; });
    long found = -1;
    for (std::size_t i = 1; i < names.size(); ++i) {
        if (names[i].name == names[i - 1].name) {
            const long at = static_cast<long>(names[i].index);
            if (found < 0 || at < found) found = at;
        }
    }
    return found;
}

std::string size_text(int w, int h) {
    return std::to_string(w) + " x " + std::to_string(h);
}

// Reads one frame object (array entry or hash value). Errors are recorded in the document.
SheetFrame read_frame(DataReader &f, IVec2 image) {
    f.ignore_unknown();
    SheetFrame frame;
    DataReader rect = f.object("frame");
    rect.ignore_unknown();
    const int x = rect.integer("x", 0, kMaxCoord);
    const int y = rect.integer("y", 0, kMaxCoord);
    const int w = rect.integer("w", 1, kMaxCoord);
    const int h = rect.integer("h", 1, kMaxCoord);
    if (f.failed()) return frame;
    frame.source = Rect{static_cast<float>(x), static_cast<float>(y), static_cast<float>(w),
                        static_cast<float>(h)};
    // A rotated frame would have to be drawn turned by 90 degrees; nothing does that.
    if (f.boolean_or("rotated", false)) {
        f.fail("rotated frames are not supported (export without rotation)");
        return frame;
    }
    if (x + w > image.x || y + h > image.y) {
        f.fail("frame " + size_text(w, h) + " at (" + std::to_string(x) + ", " +
               std::to_string(y) + ") is outside the image (" + size_text(image.x, image.y) +
               ")");
        return frame;
    }
    int source_w = w, source_h = h, offset_x = 0, offset_y = 0;
    if (f.has("sourceSize")) {
        DataReader s = f.object("sourceSize");
        s.ignore_unknown();
        source_w = s.integer("w", 1, kMaxCoord);
        source_h = s.integer("h", 1, kMaxCoord);
    }
    if (f.has("spriteSourceSize")) {
        DataReader p = f.object("spriteSourceSize");
        p.ignore_unknown();
        offset_x = p.integer("x", 0, kMaxCoord);
        offset_y = p.integer("y", 0, kMaxCoord);
        const int pw = p.integer("w", 1, kMaxCoord);
        const int ph = p.integer("h", 1, kMaxCoord);
        if (!f.failed() && (pw != w || ph != h)) {
            p.fail("size " + size_text(pw, ph) + " differs from the frame's size " +
                   size_text(w, h));
        }
    }
    if (f.failed()) return frame;
    if (offset_x + w > source_w || offset_y + h > source_h) {
        f.fail("the trimmed frame does not fit inside its source size");
        return frame;
    }
    frame.trim_offset = {static_cast<float>(offset_x), static_cast<float>(offset_y)};
    frame.source_size = {static_cast<float>(source_w), static_cast<float>(source_h)};
    // Aseprite writes whole milliseconds; other exporters may write fractions: round to 1 ms.
    const double ms = f.number64("duration", -1e12, 1e12);
    if (f.failed()) return frame;
    const double rounded = std::floor(ms + 0.5);
    if (!(rounded >= 1.0)) {
        f.fail("duration must be greater than 0 (milliseconds, at least 1 after rounding)");
        return frame;
    }
    if (rounded > 65535.0) {
        f.fail("duration " + std::to_string(static_cast<long long>(rounded)) +
               " ms is too long (at most 65535)");
        return frame;
    }
    frame.duration_ms = static_cast<std::uint16_t>(rounded);
    return frame;
}

// Aseprite writes "repeat" as a string ("1"); other exporters write a number.
std::uint16_t read_repeat(DataReader &t) {
    if (!t.has("repeat")) return 0;
    if (t.type("repeat") == DataType::String) {
        const std::string text = t.text("repeat");
        bool ok = !text.empty() && text.size() <= 5;
        long value = 0;
        for (char c : text) {
            if (c < '0' || c > '9') {
                ok = false;
                break;
            }
            value = value * 10 + (c - '0');
        }
        if (!ok || value > 65535) {
            t.fail("repeat \"" + text + "\" must be a whole number of plays, 0..65535");
            return 0;
        }
        return static_cast<std::uint16_t>(value);
    }
    return static_cast<std::uint16_t>(t.integer("repeat", 0, 65535));
}

AnimDirection read_direction(DataReader &t) {
    if (!t.has("direction")) return AnimDirection::Forward;
    const std::string d = t.string("direction");
    if (d == "forward") return AnimDirection::Forward;
    if (d == "reverse") return AnimDirection::Reverse;
    if (d == "pingpong") return AnimDirection::PingPong;
    if (d == "pingpong_reverse") return AnimDirection::PingPongReverse;
    if (!t.failed()) {
        t.fail("direction \"" + d +
               "\" is not one of forward, reverse, pingpong, pingpong_reverse");
    }
    return AnimDirection::Forward;
}

void read_tags(DataReader &meta, SpriteSheet &sheet) {
    if (!meta.has("frameTags")) return;
    DataArray tags = meta.array("frameTags");
    std::vector<Named> names;
    const std::size_t count = sheet.frames.size();
    for (std::size_t i = 0; i < tags.size() && !meta.failed(); ++i) {
        DataReader t = tags.object(i);
        t.ignore_unknown();
        SheetTag tag;
        std::string name = t.string("name");
        const int from = t.integer("from", 0, 65535);
        const int to = t.integer("to", 0, 65535);
        tag.dir = read_direction(t);
        tag.repeat = read_repeat(t);
        if (t.failed()) break;
        if (from > to || static_cast<std::size_t>(to) >= count) {
            t.fail("frames " + std::to_string(from) + ".." + std::to_string(to) +
                   " are not inside the frame list (" + std::to_string(count) + " frames)");
            break;
        }
        tag.name = intern(name);
        tag.from = static_cast<std::uint16_t>(from);
        tag.to = static_cast<std::uint16_t>(to);
        sheet.tags.push_back(tag);
        names.push_back(Named{std::move(name), i});
    }
    if (meta.failed()) return;
    if (const long dup = first_duplicate(names); dup >= 0) {
        const auto at = static_cast<std::size_t>(dup);
        DataReader t = tags.object(at);
        t.fail("duplicate tag name \"" + std::string(id_name(sheet.tags[at].name)) + "\"");
    }
}

void read_slices(DataReader &meta, SpriteSheet &sheet) {
    if (!meta.has("slices")) return;
    DataArray slices = meta.array("slices");
    std::vector<Named> names;
    for (std::size_t i = 0; i < slices.size() && !meta.failed(); ++i) {
        DataReader s = slices.object(i);
        s.ignore_unknown();
        SheetSlice slice;
        std::string name = s.string("name");
        DataArray keys = s.array("keys");
        for (std::size_t k = 0; k < keys.size() && !s.failed(); ++k) {
            DataReader key = keys.object(k);
            key.ignore_unknown();
            SliceKey sk;
            const int frame = key.integer("frame", 0, 65535);
            DataReader b = key.object("bounds");
            b.ignore_unknown();
            const int bx = b.integer("x", -kMaxCoord, kMaxCoord);
            const int by = b.integer("y", -kMaxCoord, kMaxCoord);
            const int bw = b.integer("w", 0, kMaxCoord);
            const int bh = b.integer("h", 0, kMaxCoord);
            if (key.has("pivot")) {
                DataReader p = key.object("pivot");
                p.ignore_unknown();
                sk.pivot = {static_cast<float>(p.integer("x", -kMaxCoord, kMaxCoord)),
                            static_cast<float>(p.integer("y", -kMaxCoord, kMaxCoord))};
            }
            if (key.failed()) break;
            if (static_cast<std::size_t>(frame) >= sheet.frames.size()) {
                key.fail("frame " + std::to_string(frame) + " is not in the frame list (" +
                         std::to_string(sheet.frames.size()) + " frames)");
                break;
            }
            sk.frame = static_cast<std::uint16_t>(frame);
            sk.bounds = Rect{static_cast<float>(bx), static_cast<float>(by),
                             static_cast<float>(bw), static_cast<float>(bh)};
            slice.keys.push_back(sk);
        }
        if (s.failed()) break;
        if (slice.keys.empty()) {
            s.fail("a slice needs at least one key");
            break;
        }
        std::stable_sort(slice.keys.begin(), slice.keys.end(),
                         [](const SliceKey &a, const SliceKey &b) { return a.frame < b.frame; });
        for (std::size_t k = 1; k < slice.keys.size(); ++k) {
            if (slice.keys[k].frame == slice.keys[k - 1].frame) {
                s.fail("two keys for frame " + std::to_string(slice.keys[k].frame));
                break;
            }
        }
        if (s.failed()) break;
        slice.name = intern(name);
        sheet.slices.push_back(std::move(slice));
        names.push_back(Named{std::move(name), i});
    }
    if (meta.failed()) return;
    if (const long dup = first_duplicate(names); dup >= 0) {
        const auto at = static_cast<std::size_t>(dup);
        DataReader s = slices.object(at);
        s.fail("duplicate slice name \"" + std::string(id_name(sheet.slices[at].name)) + "\"");
    }
}

// "atlas": "<group>" | "none" (top level or meta).
void read_atlas(DataReader &r, SpriteSheet &sheet) {
    if (r.has("atlas")) sheet.atlas_group = intern(r.string("atlas"));
}

SpriteSheet read_sheet(DataReader &root) {
    SpriteSheet sheet;
    root.ignore_unknown();
    DataReader meta = root.object("meta");
    meta.ignore_unknown();
    sheet.image_path = meta.string("image");
    DataReader size = meta.object("size");
    size.ignore_unknown();
    sheet.image_size.x = size.integer("w", 1, kMaxCoord);
    sheet.image_size.y = size.integer("h", 1, kMaxCoord);
    if (root.failed()) return sheet;
    if (sheet.image_size.x > kMaxSheetImageSide || sheet.image_size.y > kMaxSheetImageSide) {
        size.fail(size_text(sheet.image_size.x, sheet.image_size.y) +
                  " is larger than the limit of " +
                  size_text(kMaxSheetImageSide, kMaxSheetImageSide));
        return sheet;
    }
    read_atlas(meta, sheet);
    read_atlas(root, sheet);

    std::vector<Named> names;
    if (root.type("frames") == DataType::Object) {
        // Hash format: key order is file order (the document keeps insertion order).
        DataReader frames = root.object("frames");
        const std::vector<std::string_view> keys = frames.keys();
        if (keys.empty()) {
            frames.fail("must not be empty");
            return sheet;
        }
        sheet.frames.reserve(keys.size());
        for (std::string_view key : keys) {
            DataReader f = frames.object(key);
            sheet.frames.push_back(read_frame(f, sheet.image_size));
            if (root.failed()) return sheet;
        }
    } else {
        DataArray frames = root.array("frames");
        if (root.failed()) return sheet;
        if (frames.size() == 0) {
            frames.fail("must not be empty");
            return sheet;
        }
        sheet.frames.reserve(frames.size());
        for (std::size_t i = 0; i < frames.size(); ++i) {
            DataReader f = frames.object(i);
            std::string name = f.string("filename");
            sheet.frames.push_back(read_frame(f, sheet.image_size));
            if (root.failed()) return sheet;
            names.push_back(Named{std::move(name), i});
        }
        if (const long dup = first_duplicate(names); dup >= 0) {
            const auto at = static_cast<std::size_t>(dup);
            std::string name;
            for (const Named &n : names) {
                if (n.index == at) name = n.name;
            }
            DataReader f = frames.object(at);
            f.fail("duplicate frame name \"" + name + "\"");
            return sheet;
        }
    }
    if (sheet.frames.size() > 65535) {
        root.fail("more than 65535 frames");
        return sheet;
    }
    read_tags(meta, sheet);
    if (root.failed()) return sheet;
    read_slices(meta, sheet);
    return sheet;
}

} // namespace

int SpriteSheet::find_tag(StrId name) const {
    for (std::size_t i = 0; i < tags.size(); ++i) {
        if (tags[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

int SpriteSheet::find_slice(StrId name) const {
    for (std::size_t i = 0; i < slices.size(); ++i) {
        if (slices[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

Result<SpriteSheet> parse_aseprite_json(std::string_view text, std::string_view file_name) {
    try {
        Result<DataDocument> doc = DataDocument::parse(text, file_name);
        if (!doc) return doc.error();
        DataReader root = doc->root();
        SpriteSheet sheet = read_sheet(root);
        if (Status s = doc->finish(); !s) return s.error();
        return sheet;
    } catch (const std::exception &e) {
        return make_error(ErrorCode::Parse, std::string(file_name) + ": " + e.what());
    } catch (...) {
        return make_error(ErrorCode::Parse, std::string(file_name) + ": unexpected exception");
    }
}

SpriteSheet make_single_frame_sheet(IVec2 size, std::string_view image_path) {
    SpriteSheet sheet;
    sheet.image_path = std::string(image_path);
    sheet.image_size = size;
    SheetFrame frame;
    frame.source = Rect{0, 0, static_cast<float>(size.x), static_cast<float>(size.y)};
    frame.source_size = {static_cast<float>(size.x), static_cast<float>(size.y)};
    sheet.frames.push_back(frame);
    return sheet;
}

TextureRegion frame_region(const SpriteSheet &sheet, std::size_t frame) {
    LAM_ASSERT(frame < sheet.frames.size(), "frame_region: frame index out of range");
    if (frame >= sheet.frames.size()) return {};
    if (frame < sheet.regions.size()) return sheet.regions[frame];
    const SheetFrame &f = sheet.frames[frame];
    TextureRegion r;
    r.texture = sheet.texture;
    r.source = f.source;
    r.size = f.source_size;
    r.trim_offset = f.trim_offset;
    return r;
}

} // namespace lam
