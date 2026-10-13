// Reflection: field types, round trips, hashing, errors and LAM_REFLECT_CUSTOM.
#include "ecs_test.h"
#include <array>
#include <cmath>
#include <cstring>
#include <lam/core/data.h>
#include <lam/ecs/component.h>
#include <lam/ecs/reflect_fsm.h>
#include <lam/ecs/snapshot.h>
#include <lam/ecs/world.h>
#include <limits>
#include <map>
#include <new>
#include <string>
#include <vector>

using namespace lam;
using namespace lam::literals;

namespace rt {
enum class Mood : std::uint8_t { Calm, Angry, Sleepy };
enum class Wide : std::int32_t { A, B, C };
struct Sprite {}; // an asset type (never defined further)
struct Inner {
    std::int16_t a = 0;
    Vec2 v;
    bool operator==(const Inner &) const = default;
};
struct AllFields {
    bool b = false;
    std::int8_t i8 = 0;
    std::uint8_t u8 = 0;
    std::int16_t i16 = 0;
    std::uint16_t u16 = 0;
    std::int32_t i32 = 0;
    std::uint32_t u32 = 0;
    std::int64_t i64 = 0;
    std::uint64_t u64 = 0;
    float f32 = 0;
    double f64 = 0;
    Vec2 v2;
    IVec2 iv2;
    Rect r;
    IRect ir;
    Color c;
    Entity e;
    StrId id;
    FixedString<16> s;
    Mood mood = Mood::Calm;
    Wide wide = Wide::A;
    AssetId<Sprite> asset;
    Inner inner;
    std::array<Inner, 2> inners{};
    std::array<FixedString<5>, 2> names{};
    std::array<Mood, 3> moods{};
    std::array<Color, 2> colors{};
    std::array<IVec2, 2> cells{};
    std::array<Entity, 2> ents{};
    std::array<float, 3> floats{};
    bool operator==(const AllFields &) const = default;
};
// Custom reflection: non-field state.
struct Bag {
    std::vector<int> items;
};
Status lam_read(Bag &b, DataReader &in, const SerializeContext &) {
    DataArray a = in.array("items");
    b.items.clear();
    for (std::size_t i = 0; i < a.size(); ++i)
        b.items.push_back(a.integer(i, -1000, 1000));
    return in.failed() ? make_error(ErrorCode::Parse, "bag") : Status{};
}
void lam_write(const Bag &b, DataWriter &out, const SerializeContext &) {
    out.begin_object().key("items").begin_array();
    for (const int i : b.items)
        out.value(i);
    out.end_array().end_object();
}
void lam_hash(const Bag &b, Hasher &h) {
    h.u64(b.items.size());
    for (const int i : b.items)
        h.u64(static_cast<std::uint64_t>(i));
}
struct Plain {
    int x = 0;
}; // unreflected
// Full-width fields in arrays: doubles, u64 and FixedString elements.
struct Orbit {
    double angle = 0;
    std::array<double, 2> radii{};
    std::array<std::uint64_t, 2> seeds{};
    bool operator==(const Orbit &) const = default;
};
// A state machine as a reflected field (reflect_fsm.h).
enum class Wolf : std::uint8_t { Idle, Chase, Flee };
struct WolfAi {
    Fsm<Wolf> fsm;
    std::int32_t target = 0;
};
// A snapshot World resource reflected with the resource alias.
struct LevelState {
    std::int32_t level = 1;
    std::uint32_t gems = 0;
};

// Asset resolver over an in-memory table.
struct FakeAssets final : AssetResolver {
    std::map<std::string, std::uint64_t> ids;
    std::uint64_t resolve(TypeId, std::string_view path) override {
        if (path.find(".png") == std::string_view::npos) return 0;
        auto [it, inserted] = ids.try_emplace(
            std::string(path), AssetId<Sprite>{std::uint32_t(ids.size() + 1), 1}.bits());
        return it->second;
    }
    std::string_view path_of(TypeId, std::uint64_t bits) const override {
        for (const auto &[p, b] : ids)
            if (b == bits) return p;
        return {};
    }
    Result<std::string> read_text(std::string_view) const override {
        return make_error(ErrorCode::NotFound, "none");
    }
    std::size_t pending() const override { return 0; }
};
} // namespace rt
LAM_REFLECT_ENUM(rt::Mood, "calm", "angry", "sleepy")
LAM_REFLECT_ENUM(rt::Wide, "a", "b", "c")
LAM_REFLECT_STRUCT(rt::Inner, a, v)
LAM_REFLECT(rt::AllFields, b, i8, u8, i16, u16, i32, u32, i64, u64, f32, f64, v2, iv2, r, ir, c, e,
            id, s, mood, wide, asset, inner, inners, names, moods, colors, cells, ents, floats)
LAM_REFLECT_CUSTOM(rt::Bag, lam::kDefaultFlags)
LAM_REFLECT(rt::Orbit, angle, radii, seeds)
LAM_REFLECT_ENUM(rt::Wolf, "idle", "chase", "flee")
LAM_REFLECT(rt::WolfAi, fsm, target)
LAM_REFLECT_RESOURCE(rt::LevelState, level, gems)

namespace {
const FieldInfo &field(const TypeLayout &l, std::string_view name) {
    for (const FieldInfo &f : l.fields)
        if (f.name == name) return f;
    FAIL("no field " << name);
    return l.fields[0];
}

rt::AllFields sample(rt::FakeAssets &assets) {
    rt::AllFields a;
    a.b = true;
    a.i8 = -128;
    a.u8 = 255;
    a.i16 = -32768;
    a.u16 = 65535;
    a.i32 = std::numeric_limits<std::int32_t>::min();
    a.u32 = 0xFFFFFFFFu;
    a.i64 = std::numeric_limits<std::int64_t>::min();
    a.u64 = std::numeric_limits<std::uint64_t>::max();
    a.f32 = 0.1f;
    a.f64 = 0.5;
    a.v2 = {1.5f, -2.25f};
    a.iv2 = {-3, 4};
    a.r = {1, 2, 3, 4};
    a.ir = {-1, -2, 3, 4};
    a.c = Color{1, 2, 3, 4};
    a.e = Entity{7, 3};
    a.id = intern("reflection_id");
    a.s = std::string_view("héllo wörld");
    a.mood = rt::Mood::Sleepy;
    a.wide = rt::Wide::C;
    a.asset = AssetId<rt::Sprite>::from_bits(assets.resolve(type_id<rt::Sprite>(), "hero.png"));
    a.inner = {5, {6, 7}};
    a.inners = {rt::Inner{1, {2, 3}}, rt::Inner{-4, {5, 6}}};
    a.names = {FixedString<5>("ab"), FixedString<5>("wxyz")};
    a.moods = {rt::Mood::Angry, rt::Mood::Calm, rt::Mood::Sleepy};
    a.colors = {Color{255, 0, 0, 255}, Color{0, 0, 255, 128}};
    a.cells = {IVec2{1, 2}, IVec2{-3, -4}};
    a.ents = {Entity{}, Entity{2, 9}};
    a.floats = {0.25f, -1e10f, 3.0f};
    return a;
}

std::string write_json(const ComponentInfo &info, const void *value, const SerializeContext &ctx) {
    DataWriter w;
    w.begin_object().key("v");
    info.write(value, w, ctx);
    w.end_object();
    Result<std::string> s = w.finish();
    REQUIRE(s.ok());
    return *s;
}

Status read_json(const ComponentInfo &info, void *value, std::string_view text,
                 const SerializeContext &ctx) {
    Result<DataDocument> doc = DataDocument::parse(text, "test.json");
    if (!doc) return doc.error();
    DataReader root = doc->root();
    DataReader v = root.object("v");
    const Status st = info.read(value, v, ctx);
    LAM_TRY(doc->finish());
    return st;
}
} // namespace

TEST_CASE("field types are deduced for every supported kind") {
    const TypeLayout &l = Reflect<rt::AllFields>::layout();
    CHECK(l.name == "AllFields");
    CHECK(l.size == sizeof(rt::AllFields));
    CHECK(l.fields.size() == 30);
    const std::pair<const char *, FieldType> expect[] = {
        {"b", FieldType::Bool},        {"i8", FieldType::I8},       {"u8", FieldType::U8},
        {"i16", FieldType::I16},       {"u16", FieldType::U16},     {"i32", FieldType::I32},
        {"u32", FieldType::U32},       {"i64", FieldType::I64},     {"u64", FieldType::U64},
        {"f32", FieldType::F32},       {"f64", FieldType::F64},     {"v2", FieldType::Vec2},
        {"iv2", FieldType::IVec2},     {"r", FieldType::Rect},      {"ir", FieldType::IRect},
        {"c", FieldType::Color},       {"e", FieldType::Entity},    {"id", FieldType::StrId},
        {"s", FieldType::FixedString}, {"mood", FieldType::Enum},   {"asset", FieldType::Asset},
        {"inner", FieldType::Struct},  {"inners", FieldType::Array}};
    for (const auto &[name, type] : expect)
        CHECK_MESSAGE(field(l, name).type == type, name);
    CHECK(field(l, "s").size == 16); // FixedString: N
    CHECK(field(l, "mood").enum_names.size() == 3);
    CHECK(field(l, "mood").enum_names[2] == "sleepy");
    CHECK(field(l, "wide").size == 4);
    CHECK(field(l, "asset").asset_type == type_id<rt::Sprite>());
    CHECK(field(l, "inner").nested == &Reflect<rt::Inner>::layout());
    CHECK(field(l, "inners").element == FieldType::Struct);
    CHECK(field(l, "inners").count == 2);
    CHECK(field(l, "names").element == FieldType::FixedString);
    CHECK(field(l, "moods").element == FieldType::Enum);
    CHECK(field(l, "floats").element == FieldType::F32);
    CHECK(field(l, "f64").offset == offsetof(rt::AllFields, f64));
}

TEST_CASE("write then read reproduces every field exactly") {
    rt::FakeAssets assets;
    SerializeContext ctx{&assets, {}};
    const rt::AllFields a = sample(assets);
    const ComponentInfo &info = ComponentRegistry::global().info<rt::AllFields>();
    const std::string json = write_json(info, &a, ctx);
    CHECK(json.find("\"sleepy\"") != std::string::npos);
    CHECK(json.find("\"hero.png\"") != std::string::npos);
    CHECK(json.find("[7,3]") != std::string::npos); // entities as [index, generation]
    rt::AllFields b;
    REQUIRE(read_json(info, &b, json, ctx).ok());
    CHECK(b == a);
    Hasher ha, hb;
    info.hash(&a, ha);
    info.hash(&b, hb);
    CHECK(ha.value() == hb.value());
}

TEST_CASE("missing keys keep defaults; unknown keys and bad values are errors") {
    rt::FakeAssets assets;
    SerializeContext ctx{&assets, {}};
    const ComponentInfo &info = ComponentRegistry::global().info<rt::AllFields>();
    rt::AllFields x;
    x.i32 = 77;
    REQUIRE(read_json(info, &x, R"({"v": {"u8": 9, "inner": {"a": 3}}})", ctx).ok());
    CHECK(x.u8 == 9);
    CHECK(x.i32 == 77); // untouched
    CHECK(x.inner.a == 3);
    const char *bad[] = {
        R"({"v": {"bogus": 1}})",                    // unknown key
        R"({"v": {"inner": {"zz": 1}}})",            // unknown nested key
        R"({"v": {"u8": 256}})",                     // out of range
        R"({"v": {"i8": 1.5}})",                     // not whole
        R"({"v": {"mood": "furious"}})",             // unknown enum
        R"({"v": {"inners": [{"a": 1}, {}, {}]}})",  // more than N array elements
        R"({"v": {"asset": "missing.wav"}})",        // resolver refuses
        R"({"v": {"e": [1]}})",                      // malformed entity
        R"({"v": {"c": "#12"}})",                    // malformed colour
        R"({"v": {"colors": ["#ff0000", "nope"]}})", // malformed colour element
    };
    for (const char *text : bad) {
        rt::AllFields y;
        CHECK_MESSAGE(!read_json(info, &y, text, ctx).ok(), text);
    }
    rt::AllFields z;
    CHECK(!read_json(info, &z, R"({"v": {"asset": "a.png"}})", SerializeContext{})
               .ok()); // no resolver
    REQUIRE(read_json(info, &z,
                      R"({"v": {"asset": null, "e": null, "colors": [[1,2,3], "#00ff0080"]}})",
                      SerializeContext{})
                .ok());
    CHECK(!z.asset);
    CHECK(z.e.is_null());
    CHECK(z.colors[0] == Color{1, 2, 3, 255});
    CHECK(z.colors[1] == Color{0, 255, 0, 128});
}

TEST_CASE("hash canonicalises floats and ignores padding") {
    const ComponentInfo &info = ComponentRegistry::global().info<rt::AllFields>();
    rt::AllFields a, b;
    std::memset(static_cast<void *>(&b), 0xAB, sizeof b); // garbage in padding bytes
    new (&b) rt::AllFields;                               // default-init keeps the padding garbage
    a.f32 = -0.0f;
    b.f32 = 0.0f;
    a.floats[1] = std::numeric_limits<float>::quiet_NaN();
    b.floats[1] = -std::numeric_limits<float>::quiet_NaN();
    Hasher ha, hb;
    info.hash(&a, ha);
    info.hash(&b, hb);
    CHECK(ha.value() == hb.value());
    b.i16 = 1;
    Hasher hc;
    info.hash(&b, hc);
    CHECK(hc.value() != ha.value());
}

TEST_CASE("entity remap: integers read through the remap and write back as integers") {
    const ComponentInfo &info = ComponentRegistry::global().info<rt::AllFields>();
    const std::array<Entity, 3> remap{Entity{10, 1}, Entity{11, 1}, Entity{12, 4}};
    SerializeContext ctx{nullptr, remap};
    rt::AllFields x;
    REQUIRE(read_json(info, &x, R"({"v": {"e": 2, "ents": [0, [5, 5]]}})", ctx).ok());
    CHECK(x.e == Entity{12, 4});
    CHECK(x.ents[0] == Entity{10, 1});
    CHECK(x.ents[1] == Entity{5, 5});
    rt::AllFields y;
    CHECK(!read_json(info, &y, R"({"v": {"e": 3}})", ctx).ok()); // out of range
    DataWriter w;
    w.begin_object();
    write_fields(Reflect<rt::AllFields>::layout(), &x, w, ctx);
    w.end_object();
    const std::string s = *w.finish();
    CHECK(s.find("\"e\":2") != std::string::npos);
}

TEST_CASE("registry: names, flags, lookups, unreflected and custom types") {
    ComponentRegistry &reg = ComponentRegistry::global();
    const ComponentInfo &all = reg.info<rt::AllFields>();
    CHECK(all.name == "AllFields");
    CHECK(all.flags == kDefaultFlags);
    CHECK(all.size == sizeof(rt::AllFields));
    CHECK(all.layout != nullptr);
    CHECK(reg.find("AllFields") == &all);
    CHECK(reg.find("NoSuchComponent") == nullptr);
    CHECK(&reg.at(all.id) == &all);
    CHECK(reg.id<rt::AllFields>() == all.id);
    CHECK(reg.size() > all.id);
    const ComponentInfo &plain = reg.info<rt::Plain>();
    CHECK(plain.name == "Plain");
    CHECK(plain.flags == 0);
    CHECK(plain.layout == nullptr);
    CHECK(plain.read == nullptr);
    CHECK(reg.trivially_copyable(plain.id));
    const ComponentInfo &frozen = reg.info<ecs_test::Frozen>();
    CHECK(frozen.flags == (kTag | kSerialize | kSnapshot | kHash));
    CHECK(frozen.size == 0);
    CHECK(reg.find("Frozen") == &frozen); // registered at static init
    const ComponentInfo &bag = reg.info<rt::Bag>();
    CHECK(bag.layout == nullptr);
    CHECK(bag.read != nullptr);
    CHECK(!reg.trivially_copyable(bag.id));
    CHECK(ReflectEnum<Ease>::count == 19);
    CHECK(ReflectEnum<Ease>::names[2] == "out_quad");
}

TEST_CASE("custom-reflected types round-trip, hash and snapshot as components and resources") {
    const ComponentInfo &info = ComponentRegistry::global().info<rt::Bag>();
    rt::Bag a;
    a.items = {3, -1, 4};
    const std::string json = write_json(info, &a, {});
    rt::Bag b;
    REQUIRE(read_json(info, &b, json, {}).ok());
    CHECK(b.items == a.items);
    World w;
    const Entity e = w.create();
    w.add<rt::Bag>(e, a);
    w.resources().insert<rt::Bag>(rt::Bag{{9, 9}});
    w.snapshot_resource<rt::Bag>();
    const std::uint64_t h = w.state_hash();
    WorldSnapshot s;
    w.snapshot_into(s);
    w.get<rt::Bag>(e).items.push_back(5);
    w.resources().get<rt::Bag>().items.clear();
    CHECK(w.state_hash() != h);
    w.restore(s);
    CHECK(w.get<rt::Bag>(e).items == a.items);
    CHECK(w.resources().get<rt::Bag>().items == std::vector<int>{9, 9});
    CHECK(w.state_hash() == h);
}

TEST_CASE("arrays may list fewer than N elements; the rest keep their current values") {
    const ComponentInfo &info = ComponentRegistry::global().info<rt::AllFields>();
    rt::AllFields x;
    x.floats = {9.0f, 9.0f, 9.0f};
    x.inners = {rt::Inner{1, {2, 3}}, rt::Inner{4, {5, 6}}};
    x.moods = {rt::Mood::Angry, rt::Mood::Angry, rt::Mood::Angry};
    REQUIRE(read_json(info, &x,
                      R"({"v": {"floats": [1.5], "inners": [{"a": 7}], "moods": [],
                                "names": ["", "ab"]}})",
                      {})
                .ok());
    CHECK(x.floats == std::array<float, 3>{1.5f, 9.0f, 9.0f});
    CHECK(x.inners[0] == rt::Inner{7, {2, 3}}); // an element object merges like any object
    CHECK(x.inners[1] == rt::Inner{4, {5, 6}});
    CHECK(x.moods == std::array<rt::Mood, 3>{rt::Mood::Angry, rt::Mood::Angry, rt::Mood::Angry});
    CHECK(x.names[0].empty()); // FixedString elements may be ""
    CHECK(x.names[1] == std::string_view("ab"));
    REQUIRE(read_json(info, &x, R"({"v": {"floats": [1, 2, 3]}})", {}).ok()); // exactly N
    CHECK(x.floats == std::array<float, 3>{1.0f, 2.0f, 3.0f});
    rt::AllFields y;
    const Status too_many = read_json(info, &y, R"({"v": {"floats": [1, 2, 3, 4]}})", {});
    REQUIRE_FALSE(too_many.ok());
    CHECK(too_many.error().message.find("at most 3") != std::string::npos);
}

TEST_CASE("double fields keep full precision through JSON, so save and load keep state_hash") {
    const ComponentInfo &info = ComponentRegistry::global().info<rt::Orbit>();
    rt::Orbit a;
    a.angle = 0.1;                // not exact as a float
    a.radii = {1.0 / 3.0, 1e300}; // beyond float range
    a.seeds = {std::numeric_limits<std::uint64_t>::max(), 0x8000000000000001ull};
    const std::string json = write_json(info, &a, {});
    rt::Orbit b;
    REQUIRE(read_json(info, &b, json, {}).ok());
    CHECK(b == a);
    CHECK(b.angle != static_cast<double>(static_cast<float>(a.angle)));
    // The same entity table with the saved and the loaded value hashes the same.
    World saved, loaded;
    saved.add<rt::Orbit>(saved.create(), a);
    loaded.add<rt::Orbit>(loaded.create(), b);
    CHECK(saved.state_hash() == loaded.state_hash());
    rt::Orbit c = a;
    c.angle = static_cast<double>(static_cast<float>(a.angle));
    World rounded;
    rounded.add<rt::Orbit>(rounded.create(), c);
    CHECK(rounded.state_hash() != saved.state_hash()); // the hash sees the lost precision
}

TEST_CASE("Fsm<E> is a reflected field: JSON, the prefab form, hash and snapshot") {
    const TypeLayout &l = Reflect<rt::WolfAi>::layout();
    const FieldInfo &f = field(l, "fsm");
    CHECK(f.type == FieldType::Struct);
    REQUIRE(f.nested != nullptr);
    CHECK(f.nested->name == "Fsm");
    REQUIRE(f.nested->fields.size() == 4);
    CHECK(f.nested->fields[0].name == "state");
    CHECK(f.nested->fields[0].type == FieldType::Enum);
    CHECK(f.nested->fields[3].name == "entered");
    const ComponentInfo &info = ComponentRegistry::global().info<rt::WolfAi>();
    rt::WolfAi a;
    a.fsm.set(rt::Wolf::Chase);
    a.fsm.tick();
    a.fsm.tick();
    a.target = 4;
    const std::string json = write_json(info, &a, {});
    CHECK(json.find(R"("state":"chase")") != std::string::npos);
    CHECK(json.find(R"("previous":"idle")") != std::string::npos);
    rt::WolfAi b;
    REQUIRE(read_json(info, &b, json, {}).ok());
    CHECK(b.fsm.state == rt::Wolf::Chase);
    CHECK(b.fsm.previous == rt::Wolf::Idle);
    CHECK(b.fsm.ticks_in_state == 2);
    CHECK_FALSE(b.fsm.entered);
    CHECK(b.target == 4);
    // Prefab form: only the state; the rest keeps its defaults.
    rt::WolfAi c;
    REQUIRE(read_json(info, &c, R"({"v": {"fsm": {"state": "flee"}}})", {}).ok());
    CHECK(c.fsm.state == rt::Wolf::Flee);
    CHECK(c.fsm.ticks_in_state == 0);
    CHECK(c.fsm.entered);
    rt::WolfAi d;
    CHECK_FALSE(read_json(info, &d, R"({"v": {"fsm": {"state": "howl"}}})", {}).ok());
    // Hashed and snapshotted with its owner.
    World w;
    const Entity e = w.create();
    w.add<rt::WolfAi>(e, a);
    const std::uint64_t h = w.state_hash();
    WorldSnapshot s;
    w.snapshot_into(s);
    w.get<rt::WolfAi>(e).fsm.tick();
    CHECK(w.state_hash() != h);
    w.restore(s);
    CHECK(w.get<rt::WolfAi>(e).fsm.ticks_in_state == 2);
    CHECK(w.state_hash() == h);
}

TEST_CASE("LAM_REFLECT_RESOURCE: a snapshot World resource is saved, hashed and restored") {
    ComponentRegistry &reg = ComponentRegistry::global();
    const ComponentInfo &info = reg.info<rt::LevelState>();
    CHECK(info.flags == kDefaultFlags); // the same as LAM_REFLECT
    CHECK(reg.find("LevelState") == &info); // also a component type of that name (harmless)
    World w;
    w.resources().insert<rt::LevelState>();
    w.snapshot_resource<rt::LevelState>();
    const std::uint64_t h = w.state_hash();
    WorldSnapshot s;
    w.snapshot_into(s);
    w.resources().get<rt::LevelState>().gems = 3;
    CHECK(w.state_hash() != h);
    w.restore(s);
    CHECK(w.resources().get<rt::LevelState>().gems == 0);
    CHECK(w.state_hash() == h);
    rt::LevelState saved{4, 9};
    const std::string json = write_json(info, &saved, {});
    rt::LevelState loaded;
    REQUIRE(read_json(info, &loaded, json, {}).ok());
    CHECK(loaded.level == 4);
    CHECK(loaded.gems == 9);
}
