// Aseprite sprite sheet parsing (parse_aseprite_json), slices and frame regions.
#include <string>

#include <lam/animation/sprite_sheet.h>
#include <lam/core/id.h>

#include "anim_test_util.h"

using anim_test::contains;
using lam::str_id;

namespace {

// The shape Aseprite 1.3 writes for "Export Sprite Sheet" with JSON data, Array frames, tags,
// layers and slices ticked. The third frame is trimmed.
const char *kExport = R"({ "frames": [
   {
    "filename": "hero 0.aseprite",
    "frame": { "x": 0, "y": 0, "w": 32, "h": 32 },
    "rotated": false,
    "trimmed": false,
    "spriteSourceSize": { "x": 0, "y": 0, "w": 32, "h": 32 },
    "sourceSize": { "w": 32, "h": 32 },
    "duration": 200
   },
   {
    "filename": "hero 1.aseprite",
    "frame": { "x": 32, "y": 0, "w": 32, "h": 32 },
    "rotated": false,
    "trimmed": false,
    "spriteSourceSize": { "x": 0, "y": 0, "w": 32, "h": 32 },
    "sourceSize": { "w": 32, "h": 32 },
    "duration": 100
   },
   {
    "filename": "hero 2.aseprite",
    "frame": { "x": 64, "y": 0, "w": 18, "h": 25 },
    "rotated": false,
    "trimmed": true,
    "spriteSourceSize": { "x": 7, "y": 4, "w": 18, "h": 25 },
    "sourceSize": { "w": 32, "h": 32 },
    "duration": 70
   },
   {
    "filename": "hero 3.aseprite",
    "frame": { "x": 96, "y": 0, "w": 32, "h": 32 },
    "rotated": false,
    "trimmed": false,
    "spriteSourceSize": { "x": 0, "y": 0, "w": 32, "h": 32 },
    "sourceSize": { "w": 32, "h": 32 },
    "duration": 125
   }
 ],
 "meta": {
  "app": "https://www.aseprite.org/",
  "version": "1.3.7-arm64",
  "image": "hero.png",
  "format": "RGBA8888",
  "size": { "w": 128, "h": 32 },
  "scale": "1",
  "frameTags": [
   { "name": "idle_down", "from": 0, "to": 1, "direction": "forward", "color": "#000000ff" },
   { "name": "attack1_down", "from": 2, "to": 3, "direction": "forward", "repeat": "1",
     "color": "#fe5b59ff", "data": "hit on 1" },
   { "name": "blink", "from": 1, "to": 1, "direction": "forward", "repeat": 3 }
  ],
  "layers": [
   { "name": "body", "opacity": 255, "blendMode": "normal" }
  ],
  "slices": [
   { "name": "hitbox", "color": "#0000ffff", "keys": [
     { "frame": 3, "bounds": {"x": 20, "y": 8, "w": 10, "h": 6 } },
     { "frame": 2, "bounds": {"x": 18, "y": 10, "w": 8, "h": 4 }, "pivot": {"x": 1, "y": 2} }
   ] }
  ]
 }
})";

// A minimal valid sheet with one piece replaceable, for the rejection cases.
std::string sheet_with(const std::string &frames, const std::string &meta_extra = "",
                       const std::string &size = R"({"w": 64, "h": 32})") {
    return R"({"frames": )" + frames + R"(, "meta": {"image": "s.png", "size": )" + size +
           meta_extra + "}}";
}

std::string frame_json(const std::string &name, int x, const std::string &duration = "100",
                       const std::string &extra = "") {
    return R"({"filename": ")" + name + R"(", "frame": {"x": )" + std::to_string(x) +
           R"(, "y": 0, "w": 32, "h": 32}, "duration": )" + duration + extra + "}";
}

const std::string kTwoFrames = "[" + frame_json("a", 0) + "," + frame_json("b", 32) + "]";

std::string error_of(const std::string &text) {
    const lam::Result<lam::SpriteSheet> parsed = lam::parse_aseprite_json(text, "s.json");
    CHECK_FALSE(parsed.ok());
    if (parsed.ok()) return {};
    CHECK(parsed.error().code == lam::ErrorCode::Parse);
    return parsed.error().message;
}

lam::SpriteSheet parse_ok(const std::string &text) {
    lam::Result<lam::SpriteSheet> parsed = lam::parse_aseprite_json(text, "s.json");
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed.value());
}

} // namespace

TEST_CASE("sprite sheet: a real-shaped Aseprite export parses") {
    const lam::Result<lam::SpriteSheet> parsed = lam::parse_aseprite_json(kExport, "hero.json");
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    const lam::SpriteSheet &sheet = parsed.value();

    CHECK(sheet.image_path == "hero.png");
    CHECK(sheet.image_size == lam::IVec2{128, 32});
    CHECK(sheet.atlas_group == str_id("default"));
    CHECK_FALSE(sheet.texture);
    CHECK(sheet.regions.empty());

    REQUIRE(sheet.frames.size() == 4);
    CHECK(sheet.frames[1].source == lam::Rect{32, 0, 32, 32});
    CHECK(sheet.frames[1].trim_offset == lam::Vec2{0, 0});
    CHECK(sheet.frames[1].source_size == lam::Vec2{32, 32});

    SUBCASE("durations are per frame, in milliseconds") {
        CHECK(sheet.frames[0].duration_ms == 200);
        CHECK(sheet.frames[1].duration_ms == 100);
        CHECK(sheet.frames[2].duration_ms == 70);
        CHECK(sheet.frames[3].duration_ms == 125);
    }
    SUBCASE("a trimmed frame keeps its place inside the untrimmed cell") {
        const lam::SheetFrame &trimmed = sheet.frames[2];
        CHECK(trimmed.source == lam::Rect{64, 0, 18, 25});
        CHECK(trimmed.trim_offset == lam::Vec2{7, 4});
        CHECK(trimmed.source_size == lam::Vec2{32, 32});
    }
    SUBCASE("tags: range, and repeat absent, as a string and as a number") {
        REQUIRE(sheet.tags.size() == 3);
        CHECK(sheet.tags[0].name == str_id("idle_down"));
        CHECK(sheet.tags[0].from == 0);
        CHECK(sheet.tags[0].to == 1);
        CHECK(sheet.tags[0].repeat == 0);
        CHECK(sheet.tags[0].dir == lam::AnimDirection::Forward);
        CHECK(sheet.tags[1].name == str_id("attack1_down"));
        CHECK(sheet.tags[1].from == 2);
        CHECK(sheet.tags[1].to == 3);
        CHECK(sheet.tags[1].repeat == 1);
        CHECK(sheet.tags[2].repeat == 3);
        CHECK(lam::id_name(sheet.tags[1].name) == "attack1_down"); // interned for saves and logs
    }
    SUBCASE("lookup by name") {
        CHECK(sheet.find_tag(str_id("attack1_down")) == 1);
        CHECK(sheet.find_tag(str_id("attack1")) == -1);
        CHECK(sheet.find_tag(lam::StrId{}) == -1);
        CHECK(sheet.find_slice(str_id("hitbox")) == 0);
        CHECK(sheet.find_slice(str_id("hurtbox")) == -1);
    }
    SUBCASE("slices: keys sorted by frame, pivot optional") {
        REQUIRE(sheet.slices.size() == 1);
        const lam::SheetSlice &s = sheet.slices[0];
        REQUIRE(s.keys.size() == 2);
        CHECK(s.keys[0].frame == 2);
        CHECK(s.keys[0].bounds == lam::Rect{18, 10, 8, 4});
        CHECK(s.keys[0].pivot == lam::Vec2{1, 2});
        CHECK(s.keys[1].frame == 3);
        CHECK(s.keys[1].pivot == lam::Vec2{0, 0});
    }
}

TEST_CASE("sprite sheet: the optional parts of a frame default to an untrimmed frame") {
    const lam::SpriteSheet sheet = parse_ok(sheet_with(kTwoFrames));
    CHECK(sheet.frames[1].trim_offset == lam::Vec2{0, 0});
    CHECK(sheet.frames[1].source_size == lam::Vec2{32, 32});
    CHECK(sheet.tags.empty());
    CHECK(sheet.slices.empty());
}

TEST_CASE("sprite sheet: broken JSON is reported with the file name") {
    const std::string error = error_of(R"({"frames": [)");
    CHECK(contains(error, "s.json: "));
    CHECK(contains(error, "parse error"));
    CHECK(contains(error_of("[]"), "s.json: /: the document root must be an object"));
}

TEST_CASE("sprite sheet: the Hash frame format parses in file order") {
    // The JSON document keeps insertion order, so frames come out in file order.
    const lam::SpriteSheet sheet = parse_ok(sheet_with(
        R"({"walk 1": {"frame": {"x": 32, "y": 0, "w": 32, "h": 32}, "duration": 50},
            "walk 0": {"frame": {"x": 0, "y": 0, "w": 32, "h": 32}, "duration": 70}})",
        R"(, "frameTags": [{"name": "walk", "from": 0, "to": 1}])"));
    REQUIRE(sheet.frames.size() == 2);
    CHECK(sheet.frames[0].source.x == 32);
    CHECK(sheet.frames[0].duration_ms == 50);
    CHECK(sheet.frames[1].source.x == 0);
    CHECK(sheet.tags.size() == 1);
    CHECK(contains(error_of(sheet_with("{}")), "s.json: /frames: must not be empty"));
    CHECK(contains(error_of(sheet_with(R"({"a": {"frame": {"x": 0, "y": 0, "w": 32, "h": 32}}})")),
                   "/frames/a/duration: missing required key"));
}

TEST_CASE("sprite sheet: missing parts are named by JSON path") {
    CHECK(contains(error_of(R"({"meta": {"image": "s.png", "size": {"w": 8, "h": 8}}})"),
                   "s.json: /frames: missing required key"));
    CHECK(contains(error_of(R"({"frames": []})"), "s.json: /meta: missing required key"));
    CHECK(contains(error_of(sheet_with("[]")), "s.json: /frames: must not be empty"));
    CHECK(contains(error_of(sheet_with(R"([{"filename": "a", "duration": 100}])")),
                   "s.json: /frames/0/frame: missing required key"));
    CHECK(contains(
        error_of(sheet_with(R"([{"filename": "a", "frame": {"x": 0, "y": 0, "w": 32, "h": 32}}])")),
        "s.json: /frames/0/duration: missing required key"));
    CHECK(contains(error_of(R"({"frames": [], "meta": {"size": {"w": 8, "h": 8}}})"),
                   "s.json: /meta/image: missing required key"));
    CHECK(contains(error_of(R"({"frames": [], "meta": {"image": "s.png"}})"),
                   "s.json: /meta/size: missing required key"));
    CHECK(contains(error_of(sheet_with(R"([{"frame": {"x": 0, "y": 0, "w": 32, "h": 32},
                                           "duration": 100}])")),
                   "s.json: /frames/0/filename: missing required key"));
}

TEST_CASE("sprite sheet: values of the wrong kind are rejected") {
    CHECK(contains(
        error_of(sheet_with(
            R"([{"filename": "a", "frame": {"x": 0.5, "y": 0, "w": 32, "h": 32}, "duration": 1}])")),
        "/frames/0/frame/x: expected a whole number"));
    CHECK(contains(
        error_of(sheet_with(
            R"([{"filename": "a", "frame": {"x": -1, "y": 0, "w": 32, "h": 32}, "duration": 1}])")),
        "/frames/0/frame/x: integer -1 is outside"));
    CHECK(contains(
        error_of(sheet_with(
            R"([{"filename": "a", "frame": {"x": 0, "y": 0, "w": 0, "h": 32}, "duration": 1}])")),
        "/frames/0/frame/w: integer 0 is outside"));
    CHECK(contains(
        error_of(sheet_with(
            R"([{"filename": 7, "frame": {"x": 0, "y": 0, "w": 32, "h": 32}, "duration": 1}])")),
        "/frames/0/filename: expected a string"));
    CHECK(contains(error_of(sheet_with("[" + frame_json("a", 0, "\"100\"") + "]")),
                   "/frames/0/duration: expected a number"));
    CHECK(contains(error_of(sheet_with("[" + frame_json("a", 0) + "]",
                                       R"(, "frameTags": [{"name": "t", "from": 0, "to": 0,
                                           "direction": 3}])")),
                   "/meta/frameTags/0/direction: expected a string"));
}

TEST_CASE("sprite sheet: a duration of zero or less is rejected; fractions round to 1 ms") {
    CHECK(contains(
        error_of(sheet_with("[" + frame_json("a", 0) + "," + frame_json("b", 32, "0") + "]")),
        "s.json: /frames/1: duration must be greater than 0"));
    CHECK(contains(error_of(sheet_with("[" + frame_json("a", 0, "-16") + "]")),
                   "s.json: /frames/0: duration must be greater than 0"));
    CHECK(contains(error_of(sheet_with("[" + frame_json("a", 0, "0.4") + "]")),
                   "duration must be greater than 0"));
    CHECK(contains(error_of(sheet_with("[" + frame_json("a", 0, "65536") + "]")),
                   "duration 65536 ms is too long"));
    const lam::SpriteSheet sheet =
        parse_ok(sheet_with("[" + frame_json("a", 0, "16.5") + "," + frame_json("b", 32, "65535") + "]"));
    CHECK(sheet.frames[0].duration_ms == 17);
    CHECK(sheet.frames[1].duration_ms == 65535);
}

TEST_CASE("sprite sheet: a frame outside the image is rejected, one that touches the edge is not") {
    // 64 x 32 image: a frame at x = 32 ends exactly at the edge, one at x = 33 does not fit.
    CHECK(lam::parse_aseprite_json(sheet_with("[" + frame_json("a", 32) + "]"), "s.json").ok());
    const std::string error =
        error_of(sheet_with("[" + frame_json("a", 0) + "," + frame_json("b", 33) + "]"));
    CHECK(contains(error, "s.json: /frames/1: frame 32 x 32 at (33, 0) is outside the image (64 x 32)"));
    CHECK(contains(error_of(sheet_with(kTwoFrames, "", R"({"w": 64, "h": 31})")),
                   "/frames/0: frame 32 x 32 at (0, 0) is outside the image"));
}

TEST_CASE("sprite sheet: an image larger than 2048 on a side is rejected") {
    CHECK(lam::parse_aseprite_json(sheet_with(kTwoFrames, "", R"({"w": 2048, "h": 2048})"), "s.json").ok());
    CHECK(contains(error_of(sheet_with(kTwoFrames, "", R"({"w": 2049, "h": 64})")),
                   "s.json: /meta/size: 2049 x 64 is larger than the limit of 2048 x 2048"));
    CHECK(contains(error_of(sheet_with(kTwoFrames, "", R"({"w": 64, "h": 4096})")),
                   "is larger than the limit"));
}

TEST_CASE("sprite sheet: rotated frames are rejected") {
    CHECK(contains(
        error_of(sheet_with("[" + frame_json("a", 0, "100", R"(, "rotated": true)") + "]")),
        "s.json: /frames/0: rotated frames are not supported"));
    CHECK(lam::parse_aseprite_json(
              sheet_with("[" + frame_json("a", 0, "100", R"(, "rotated": false)") + "]"), "s.json")
              .ok());
}

TEST_CASE("sprite sheet: a trimmed frame must fit its source size") {
    const std::string trimmed =
        R"(, "trimmed": true, "spriteSourceSize": {"x": 4, "y": 0, "w": 32, "h": 32},
            "sourceSize": {"w": 32, "h": 32})";
    CHECK(contains(error_of(sheet_with("[" + frame_json("a", 0, "100", trimmed) + "]")),
                   "/frames/0: the trimmed frame does not fit inside its source size"));
    const std::string wrong_size =
        R"(, "spriteSourceSize": {"x": 0, "y": 0, "w": 31, "h": 32})";
    CHECK(contains(error_of(sheet_with("[" + frame_json("a", 0, "100", wrong_size) + "]")),
                   "/frames/0/spriteSourceSize: size 31 x 32 differs from the frame's size 32 x 32"));
}

TEST_CASE("sprite sheet: duplicate frame names are rejected") {
    const std::string frames =
        "[" + frame_json("a", 0) + "," + frame_json("b", 32) + "," + frame_json("a", 0) + "]";
    CHECK(contains(error_of(sheet_with(frames)), "s.json: /frames/2: duplicate frame name \"a\""));
}

TEST_CASE("sprite sheet: tags are checked") {
    const auto with_tags = [](const std::string &tags) {
        return sheet_with(kTwoFrames, R"(, "frameTags": )" + tags);
    };
    SUBCASE("a tag over the whole list is fine") {
        const lam::SpriteSheet sheet =
            parse_ok(with_tags(R"([{"name": "all", "from": 0, "to": 1, "direction": "forward"}])"));
        CHECK(sheet.tags[0].to == 1);
    }
    SUBCASE("a range past the last frame") {
        CHECK(contains(error_of(with_tags(R"([{"name": "t", "from": 0, "to": 2}])")),
                       "s.json: /meta/frameTags/0: frames 0..2 are not inside the frame list "
                       "(2 frames)"));
    }
    SUBCASE("a range that runs backwards") {
        CHECK(contains(error_of(with_tags(R"([{"name": "t", "from": 1, "to": 0}])")),
                       "/meta/frameTags/0: frames 1..0 are not inside the frame list"));
    }
    SUBCASE("a negative start") {
        CHECK(contains(error_of(with_tags(R"([{"name": "t", "from": -1, "to": 0}])")),
                       "/meta/frameTags/0/from: integer -1 is outside"));
    }
    SUBCASE("every Aseprite direction is supported; others are rejected") {
        const char *names[] = {"forward", "reverse", "pingpong", "pingpong_reverse"};
        const lam::AnimDirection dirs[] = {lam::AnimDirection::Forward, lam::AnimDirection::Reverse,
                                           lam::AnimDirection::PingPong,
                                           lam::AnimDirection::PingPongReverse};
        for (int i = 0; i < 4; ++i) {
            const std::string tags = std::string(R"([{"name": "t", "from": 0, "to": 1, )") +
                                     R"("direction": ")" + names[i] + R"("}])";
            CHECK(parse_ok(with_tags(tags)).tags[0].dir == dirs[i]);
        }
        CHECK(contains(error_of(with_tags(R"([{"name": "t", "from": 0, "to": 1,
                                              "direction": "sideways"}])")),
                       "s.json: /meta/frameTags/0: direction \"sideways\" is not one of"));
    }
    SUBCASE("duplicate names") {
        CHECK(contains(error_of(with_tags(R"([{"name": "t", "from": 0, "to": 0},
                                              {"name": "u", "from": 0, "to": 0},
                                              {"name": "t", "from": 1, "to": 1}])")),
                       "s.json: /meta/frameTags/2: duplicate tag name \"t\""));
    }
    SUBCASE("a repeat that is not a count") {
        for (const char *repeat : {R"("twice")", R"("")", R"("-1")", R"("1.5")", "-2", "1.5"}) {
            const std::string tags = std::string(R"([{"name": "t", "from": 0, "to": 1, )") +
                                     R"("repeat": )" + repeat + "}]";
            CHECK(contains(error_of(with_tags(tags)), "s.json: /meta/frameTags/0"));
        }
        CHECK(parse_ok(with_tags(R"([{"name": "t", "from": 0, "to": 1, "repeat": "65535"}])"))
                  .tags[0]
                  .repeat == 65535);
        CHECK(contains(error_of(with_tags(R"([{"name": "t", "from": 0, "to": 1, "repeat": "65536"}])")),
                       "must be a whole number of plays"));
    }
    SUBCASE("frameTags that is not an array") {
        CHECK(contains(error_of(with_tags("{}")), "/meta/frameTags: expected an array"));
    }
}

TEST_CASE("sprite sheet: slices are checked") {
    const auto with_slices = [](const std::string &slices) {
        return sheet_with(kTwoFrames, R"(, "slices": )" + slices);
    };
    CHECK(contains(error_of(with_slices(R"([{"name": "h", "keys": []}])")),
                   "/meta/slices/0: a slice needs at least one key"));
    CHECK(contains(error_of(with_slices(
                       R"([{"name": "h", "keys": [{"frame": 2, "bounds": {"x": 0, "y": 0, "w": 1, "h": 1}}]}])")),
                   "/meta/slices/0/keys/0: frame 2 is not in the frame list (2 frames)"));
    CHECK(contains(error_of(with_slices(R"([{"name": "h", "keys": [
                       {"frame": 1, "bounds": {"x": 0, "y": 0, "w": 1, "h": 1}},
                       {"frame": 1, "bounds": {"x": 0, "y": 0, "w": 2, "h": 1}}]}])")),
                   "/meta/slices/0: two keys for frame 1"));
    CHECK(contains(error_of(with_slices(R"([
                       {"name": "h", "keys": [{"frame": 0, "bounds": {"x": 0, "y": 0, "w": 1, "h": 1}}]},
                       {"name": "h", "keys": [{"frame": 1, "bounds": {"x": 0, "y": 0, "w": 1, "h": 1}}]}])")),
                   "/meta/slices/1: duplicate slice name \"h\""));
    CHECK(contains(error_of(with_slices(R"([{"name": "h", "keys": [{"frame": 0}]}])")),
                   "/meta/slices/0/keys/0/bounds: missing required key"));
}

TEST_CASE("sprite sheet: the atlas key names a group or opts out") {
    CHECK(parse_ok(sheet_with(kTwoFrames, R"(, "atlas": "ui")")).atlas_group == str_id("ui"));
    const std::string top = R"({"atlas": "none", "frames": )" + kTwoFrames +
                            R"(, "meta": {"image": "s.png", "size": {"w": 64, "h": 32}}})";
    CHECK(parse_ok(top).atlas_group == str_id("none"));
    CHECK(contains(error_of(sheet_with(kTwoFrames, R"(, "atlas": "")")),
                   "/meta/atlas: string must not be empty"));
}

TEST_CASE("sprite sheet: single-frame sheets and frame regions") {
    lam::SpriteSheet sheet = lam::make_single_frame_sheet({24, 40}, "coin.png");
    CHECK(sheet.image_path == "coin.png");
    REQUIRE(sheet.frames.size() == 1);
    CHECK(sheet.frames[0].source == lam::Rect{0, 0, 24, 40});
    CHECK(sheet.frames[0].source_size == lam::Vec2{24, 40});
    CHECK(sheet.frames[0].duration_ms == 100);
    CHECK(sheet.tags.empty());

    // Before finalize, a region is built from the frame and the sheet texture.
    sheet.texture = lam::AssetId<lam::Texture>{3, 1};
    const lam::TextureRegion r = lam::frame_region(sheet, 0);
    CHECK(r.texture == sheet.texture);
    CHECK(r.source == lam::Rect{0, 0, 24, 40});
    CHECK(r.size == lam::Vec2{24, 40});
    // After finalize, the mapped region wins.
    lam::TextureRegion mapped = r;
    mapped.source = {100, 200, 24, 40};
    sheet.regions.push_back(mapped);
    CHECK(lam::frame_region(sheet, 0).source == lam::Rect{100, 200, 24, 40});
}
