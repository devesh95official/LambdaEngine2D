// Strict JSON reading (DataReader) and the canonical DataWriter.
#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <lam/core/data.h>

using namespace lam;

namespace {

bool contains(const std::string &haystack, std::string_view needle) {
    return haystack.find(needle) != std::string::npos;
}

// Parses `json` and runs `read` on the root; returns the finish() status (or the parse error).
template <class Read>
Status read_doc(std::string_view json, Read read, std::string_view file = "t.json") {
    auto doc = DataDocument::parse(json, file);
    if (!doc) return doc.error();
    DataReader root = doc->root();
    read(root);
    return doc->finish();
}

struct Enemy {
    int hp = 0;
    float speed = 0;
};

} // namespace

// ---- reader basics ---------------------------------------------------------------------------

TEST_CASE("json reader: a misspelt key is an error that names the file and the path") {
    const auto parsed =
        parse_data<float>(R"({"speed": 3, "sped": 4})", "player.json",
                          [](DataReader &root) { return root.number("speed", 0.0f, 10.0f); });
    REQUIRE_FALSE(parsed.ok());
    CHECK(parsed.error().code == ErrorCode::Parse);
    CHECK(contains(parsed.error().message, "player.json"));
    CHECK(contains(parsed.error().message, "sped"));
    const auto whole = parse_data<int>(R"({"lives": 2.5})", "game.json", [](DataReader &root) {
        return root.integer("lives", 0, 9);
    });
    REQUIRE_FALSE(whole.ok());
    CHECK(contains(whole.error().message, "game.json"));
    CHECK(contains(whole.error().message, "/lives"));
    CHECK(contains(whole.error().message, "whole number"));
}

TEST_CASE("json reader: object lists are strict and name their place") {
    Status ok = read_doc(R"({"a": [1, 2], "b": [0.5], "c": ["x", "y"]})", [](DataReader &r) {
        DataArray a = r.array("a");
        REQUIRE(a.size() == 2);
        CHECK(a.integer(0, 0, 5) == 1);
        CHECK(a.integer(1, 0, 5) == 2);
        CHECK(r.array("b").number(0, 0.0f, 1.0f) == 0.5f);
        DataArray c = r.array("c");
        CHECK(c.string(1) == "y");
        CHECK(c.type(0) == DataType::String);
        CHECK(c.type(9) == DataType::Missing);
    });
    CHECK(ok.ok());
    Status bad = read_doc(R"({"a": [1, 2.5]})", [](DataReader &r) {
        DataArray a = r.array("a");
        for (std::size_t i = 0; i < a.size(); ++i)
            (void)a.integer(i, 0, 5);
    });
    REQUIRE_FALSE(bad.ok());
    CHECK(contains(bad.error().message, "/a/1"));
    Status count = read_doc(R"({"a": [1]})", [](DataReader &r) {
        DataArray a = r.array("a");
        if (a.size() != 2) a.fail("expected exactly 2 values");
    });
    REQUIRE_FALSE(count.ok());
    CHECK(contains(count.error().message, "exactly 2"));
    CHECK(contains(count.error().message, "/a"));
    Status range =
        read_doc(R"({"a": [1]})", [](DataReader &r) { (void)r.array("a").integer(3, 0, 5); });
    REQUIRE_FALSE(range.ok());
    CHECK(contains(range.error().message, "/a/3"));
}

// ---- strictness ------------------------------------------------------------------------------

TEST_CASE("data: finish() catches unread keys in nested objects and array elements") {
    Status nested = read_doc(R"({"enemy": {"hp": 3, "hq": 4}})", [](DataReader &r) {
        DataReader e = r.object("enemy");
        CHECK(e.path() == "/enemy");
        CHECK(e.integer("hp", 0, 10) == 3);
    });
    REQUIRE_FALSE(nested.ok());
    CHECK(contains(nested.error().message, "/enemy"));
    CHECK(contains(nested.error().message, "hq"));

    Status listed = read_doc(R"({"list": [{"x": 1}, {"x": 2, "z": 3}]})", [](DataReader &r) {
        DataArray list = r.array("list");
        for (std::size_t i = 0; i < list.size(); ++i)
            (void)list.object(i).integer("x", 0, 9);
    });
    REQUIRE_FALSE(listed.ok());
    CHECK(contains(listed.error().message, "/list/1"));
    CHECK(contains(listed.error().message, "z"));

    Status deep = read_doc(R"({"a": {"b": {"c": 1, "d": 2}}})", [](DataReader &r) {
        (void)r.object("a").object("b").integer("c", 0, 9);
    });
    REQUIRE_FALSE(deep.ok());
    CHECK(contains(deep.error().message, "/a/b"));

    // ignore() and ignore_unknown() mark keys as read.
    CHECK(read_doc(R"({"enemy": {"hp": 3, "hq": 4}})", [](DataReader &r) {
              DataReader e = r.object("enemy");
              (void)e.integer("hp", 0, 10);
              e.ignore("hq");
          }).ok());
    CHECK(read_doc(R"({"tiled": {"a": 1, "b": [2]}, "keep": 1})", [](DataReader &r) {
              r.object("tiled").ignore_unknown();
              (void)r.integer("keep", 0, 9);
          }).ok());
    // An object that is never handed out is checked as one unread key of its parent.
    CHECK_FALSE(read_doc(R"({"x": {"y": 1}})", [](DataReader &) {}).ok());
}

TEST_CASE("data: wrong types record the first error and return neutral values") {
    auto doc = DataDocument::parse(
        R"({"hp": "five", "flag": 1, "name": 3, "pos": [1], "obj": 4, "arr": {}})", "types.json");
    REQUIRE(doc.ok());
    DataReader r = doc->root();
    CHECK(r.integer("hp", 0, 10) == 0);
    CHECK(r.failed());
    CHECK(doc->failed());
    // Later reads are no-ops returning neutral values; the first error is kept.
    CHECK(r.boolean("flag") == false);
    CHECK(r.string("name").empty());
    CHECK(r.vec2("pos") == Vec2{});
    CHECK_FALSE(r.object("obj").valid());
    CHECK(r.array("arr").size() == 0);
    const Status s = doc->finish();
    REQUIRE_FALSE(s.ok());
    CHECK(contains(s.error().message, "types.json"));
    CHECK(contains(s.error().message, "/hp"));
    CHECK_FALSE(contains(s.error().message, "/flag"));

    // Each mismatch on its own names its key.
    struct Case {
        const char *json;
        void (*read)(DataReader &);
        const char *where;
    };
    const Case cases[] = {
        {R"({"k": 1})", [](DataReader &r2) { (void)r2.boolean("k"); }, "/k"},
        {R"({"k": 1})", [](DataReader &r2) { (void)r2.string("k"); }, "/k"},
        {R"({"k": "s"})", [](DataReader &r2) { (void)r2.number("k", 0, 1); }, "/k"},
        {R"({"k": [1, 2, 3]})", [](DataReader &r2) { (void)r2.vec2("k"); }, "/k"},
        {R"({"k": [1, 2]})", [](DataReader &r2) { (void)r2.rect("k"); }, "/k"},
        {R"({"k": 4})", [](DataReader &r2) { (void)r2.object("k"); }, "/k"},
        {R"({"k": 4})", [](DataReader &r2) { (void)r2.array("k"); }, "/k"},
        {R"({"k": ""})", [](DataReader &r2) { (void)r2.string("k"); }, "/k"},
        {R"({"k": ""})", [](DataReader &r2) { (void)r2.id("k"); }, "/k"},
        {R"({"k": -1})", [](DataReader &r2) { (void)r2.u64("k"); }, "/k"},
        {R"({})", [](DataReader &r2) { (void)r2.integer("k", 0, 1); }, "/k"},
    };
    for (const Case &c : cases) {
        CAPTURE(c.json);
        const Status st = read_doc(c.json, c.read);
        REQUIRE_FALSE(st.ok());
        CHECK(contains(st.error().message, c.where));
    }
}

TEST_CASE("data: ranges, optional keys and whole numbers") {
    CHECK(
        read_doc(R"({"hp": 3.0})", [](DataReader &r) { CHECK(r.integer("hp", 1, 9) == 3); }).ok());
    CHECK_FALSE(
        read_doc(R"({"hp": 3.5})", [](DataReader &r) { (void)r.integer("hp", 1, 9); }).ok());
    CHECK_FALSE(
        read_doc(R"({"hp": 1000})", [](DataReader &r) { (void)r.integer("hp", 1, 999); }).ok());
    CHECK_FALSE(
        read_doc(R"({"hp": 0})", [](DataReader &r) { (void)r.integer("hp", 1, 999); }).ok());
    CHECK_FALSE(read_doc(R"({"s": 10.5})", [](DataReader &r) { (void)r.number("s", 0, 10); }).ok());
    CHECK_FALSE(read_doc(R"({"s": 1e39})", [](DataReader &r) {
                    (void)r.number("s", -std::numeric_limits<float>::max(),
                                   std::numeric_limits<float>::max());
                }).ok());
    CHECK(read_doc(R"({"s": -2.5})", [](DataReader &r) {
              CHECK(r.number("s", -5, 5) == -2.5f);
          }).ok());
    // Optional reads: absent -> fallback without error; present -> checked like the strict ones.
    CHECK(read_doc(R"({})", [](DataReader &r) {
              CHECK(r.number_or("speed", 1.5f, 0, 10) == 1.5f);
              CHECK(r.integer_or("lives", 3, 0, 9) == 3);
              CHECK(r.boolean_or("loop", true));
              CHECK(r.text_or("title", "untitled") == "untitled");
          }).ok());
    CHECK(read_doc(R"({"speed": 2, "lives": 4, "loop": false, "title": ""})", [](DataReader &r) {
              CHECK(r.number_or("speed", 1.5f, 0, 10) == 2.0f);
              CHECK(r.integer_or("lives", 3, 0, 9) == 4);
              CHECK_FALSE(r.boolean_or("loop", true));
              CHECK(r.text_or("title", "untitled").empty());
          }).ok());
    CHECK_FALSE(read_doc(R"({"speed": 20})", [](DataReader &r) {
                    (void)r.number_or("speed", 1, 0, 10);
                }).ok());
    CHECK_FALSE(read_doc(R"({"lives": "x"})", [](DataReader &r) {
                    (void)r.integer_or("lives", 1, 0, 9);
                }).ok());
    // A missing required key is an error; the neutral value stays inside the range.
    auto doc = DataDocument::parse("{}", "m.json");
    REQUIRE(doc.ok());
    DataReader r = doc->root();
    CHECK(r.number("missing", 2.0f, 5.0f) == 2.0f);
    REQUIRE_FALSE(doc->finish().ok());
    CHECK(contains(doc->finish().error().message, "missing"));
}

TEST_CASE("data: 64-bit integers are exact") {
    CHECK(read_doc(R"({"seed": 18446744073709551615, "big": 9007199254740993})", [](DataReader &r) {
              CHECK(r.u64("seed") == std::numeric_limits<std::uint64_t>::max());
              CHECK(r.u64("big") == 9007199254740993ULL);
          }).ok());
    CHECK(read_doc(R"({"a": 9223372036854775807, "b": -9223372036854775808})", [](DataReader &r) {
              CHECK(r.integer64("a", 0, std::numeric_limits<std::int64_t>::max()) ==
                    std::numeric_limits<std::int64_t>::max());
              CHECK(r.integer64("b", std::numeric_limits<std::int64_t>::min(), 0) ==
                    std::numeric_limits<std::int64_t>::min());
          }).ok());
    CHECK_FALSE(
        read_doc(R"({"a": 11})", [](DataReader &r) { (void)r.integer64("a", 0, 10); }).ok());
    CHECK(read_doc(R"({"gids": [2147483648, 3221225473]})", [](DataReader &r) {
              DataArray g = r.array("gids");
              CHECK(g.integer64(0, 0, 4294967295LL) == 2147483648LL);
              CHECK(g.integer64(1, 0, 4294967295LL) == 3221225473LL);
          }).ok());
}

// Regression: nlohmann's is_number_integer() is true for unsigned numbers too, so values above
// INT64_MAX used to wrap (2^64 - 1 read as -1).
TEST_CASE("data: integers above INT64_MAX are range errors, not wrapped") {
    CHECK_FALSE(read_doc(R"({"n": 18446744073709551615})", [](DataReader &r) {
                    (void)r.integer("n", -10, 10);
                }).ok());
    CHECK_FALSE(read_doc(R"({"n": 9223372036854775808})", [](DataReader &r) {
                    (void)r.integer64("n", std::numeric_limits<std::int64_t>::min(),
                                      std::numeric_limits<std::int64_t>::max());
                }).ok());
}

// ---- value forms -----------------------------------------------------------------------------

TEST_CASE("data: colour, vector and rectangle forms") {
    CHECK(read_doc(R"({"a": "#ff8000", "b": "#FF800080", "c": [1, 2, 3], "d": [1, 2, 3, 4]})",
                   [](DataReader &r) {
                       CHECK(r.color("a") == Color{255, 128, 0, 255});
                       CHECK(r.color("b") == Color{255, 128, 0, 128});
                       CHECK(r.color("c") == Color{1, 2, 3, 255});
                       CHECK(r.color("d") == Color{1, 2, 3, 4});
                   })
              .ok());
    for (const char *bad :
         {R"({"c": "ff8000"})", R"({"c": "#ff80"})", R"({"c": "#gg8000"})", R"({"c": [256, 0, 0]})",
          R"({"c": [1, 2]})", R"({"c": [1.5, 0, 0]})", R"({"c": 7})"}) {
        CAPTURE(bad);
        CHECK_FALSE(read_doc(bad, [](DataReader &r) { (void)r.color("c"); }).ok());
    }
    CHECK(read_doc(R"({"v": [1.5, -2], "i": [3, -4], "r": [1, 2, 3.5, 4], "ir": [1, 2, 3, 4]})",
                   [](DataReader &r) {
                       CHECK(r.vec2("v") == Vec2{1.5f, -2});
                       CHECK(r.ivec2("i") == IVec2{3, -4});
                       CHECK(r.rect("r") == Rect{1, 2, 3.5f, 4});
                       CHECK(r.irect("ir") == IRect{1, 2, 3, 4});
                   })
              .ok());
    CHECK_FALSE(read_doc(R"({"i": [3.5, 4]})", [](DataReader &r) { (void)r.ivec2("i"); }).ok());
    CHECK_FALSE(read_doc(R"({"ir": [1, 2, 3]})", [](DataReader &r) { (void)r.irect("ir"); }).ok());
    CHECK_FALSE(read_doc(R"({"v": ["a", 1]})", [](DataReader &r) { (void)r.vec2("v"); }).ok());
    CHECK(read_doc(R"({"pts": [[1, 2], [3, 4]]})", [](DataReader &r) {
              CHECK(r.array("pts").vec2(1) == Vec2{3, 4});
          }).ok());
}

TEST_CASE("data: ids are names (interned) or #<16 hex digits>") {
    constexpr const char *kIds = R"({"a": "test_data.goblin", "b": "#00000000000000ff",
                                     "c": ["x.y", "#0000000000000001"]})";
    CHECK(read_doc(kIds, [](DataReader &r) {
              const StrId a = r.id("a");
              CHECK(a == str_id("test_data.goblin"));
              CHECK(id_name(a) == "test_data.goblin");
              CHECK(r.id("b") == StrId{0xFF});
              DataArray c = r.array("c");
              CHECK(c.id(0) == str_id("x.y"));
              CHECK(c.id(1) == StrId{1});
          }).ok());
}

TEST_CASE("data: has, type, keys, path, fail and bool reads") {
    CHECK(read_doc(R"({"t": {"grass": 1, "water": 2, "lava": 3}, "n": null, "b": true, "s": "x"})",
                   [](DataReader &r) {
                       CHECK(r.has("t"));
                       CHECK_FALSE(r.has("zz"));
                       CHECK(r.type("t") == DataType::Object);
                       CHECK(r.type("n") == DataType::Null);
                       CHECK(r.type("b") == DataType::Bool);
                       CHECK(r.type("s") == DataType::String);
                       CHECK(r.type("zz") == DataType::Missing);
                       DataReader t = r.object("t");
                       const std::vector<std::string_view> keys = t.keys();
                       REQUIRE(keys.size() == 3);
                       CHECK(keys[0] == "grass"); // file order
                       CHECK(keys[2] == "lava");
                       for (std::string_view k : keys)
                           (void)t.integer(k, 0, 9);
                       CHECK(r.boolean("b"));
                       CHECK(r.text("s") == "x");
                       r.ignore("n");
                   })
              .ok());
    const Status custom = read_doc(R"({"t": {"grass": 1}})", [](DataReader &r) {
        DataReader t = r.object("t");
        (void)t.integer("grass", 0, 9);
        t.fail("grass needs a texture");
    });
    REQUIRE_FALSE(custom.ok());
    CHECK(contains(custom.error().message, "/t"));
    CHECK(contains(custom.error().message, "grass needs a texture"));
    // A default reader is invalid and harmless.
    DataReader none;
    CHECK_FALSE(none.valid());
    CHECK(none.integer("x", 0, 5) == 0);
    CHECK(none.keys().empty());
}

TEST_CASE("data: parse errors, bad roots and exceptions become Errors") {
    const auto bad = DataDocument::parse("{", "broken.json");
    REQUIRE_FALSE(bad.ok());
    CHECK(bad.error().code == ErrorCode::Parse);
    CHECK(contains(bad.error().message, "broken.json"));
    CHECK_FALSE(DataDocument::parse("{\"a\": \"\xFF\"}", "utf.json").ok()); // invalid UTF-8 input
    CHECK_FALSE(DataDocument::parse("", "empty.json").ok());
    CHECK_FALSE(DataDocument::parse("{} trailing", "t.json").ok());
    const Status root = read_doc("[1, 2]", [](DataReader &r) { (void)r.integer("a", 0, 1); });
    REQUIRE_FALSE(root.ok());
    CHECK(contains(root.error().message, "t.json"));
    const auto thrown = parse_data<int>(
        "{}", "throw.json", [](DataReader &) -> int { throw std::runtime_error("boom"); });
    REQUIRE_FALSE(thrown.ok());
    CHECK(contains(thrown.error().message, "throw.json"));
    const auto good =
        parse_data<Enemy>(R"({"hp": 5, "speed": 2.5})", "enemy.json", [](DataReader &in) {
            return Enemy{in.integer("hp", 1, 999), in.number_or("speed", 1, 0, 10)};
        });
    REQUIRE(good.ok());
    CHECK(good->hp == 5);
    CHECK(good->speed == 2.5f);
    // Documents are movable and keep their readers' state.
    auto doc = DataDocument::parse(R"({"x": 1})", "move.json");
    REQUIRE(doc.ok());
    DataDocument moved = std::move(*doc);
    DataReader r = moved.root();
    CHECK(r.integer("x", 0, 5) == 1);
    CHECK(moved.finish().ok());
}

// ---- DataWriter ------------------------------------------------------------------------------

TEST_CASE("data writer: canonical output round-trips through the reader") {
    const StrId named = intern("test_data.sword");
    DataWriter w;
    w.begin_object();
    w.key("b").value(true);
    w.key("a").value(-42);
    w.key("u").value(std::numeric_limits<std::uint64_t>::max());
    w.key("i64").value(std::numeric_limits<std::int64_t>::min());
    w.key("f").value(0.1f);
    w.key("d").value(2.5);
    w.key("s").value("quote\" back\\ nl\n tab\t ctl\x01 \xC3\xA9");
    w.key("id").value(named);
    w.key("raw_id").value(StrId{0x1234});
    w.key("v").value(Vec2{1.5f, -2});
    w.key("iv").value(IVec2{3, 4});
    w.key("r").value(Rect{1, 2, 3, 4});
    w.key("ir").value(IRect{5, 6, 7, 8});
    w.key("c").value(Color{255, 128, 0, 255});
    w.key("ca").value(Color{1, 2, 3, 4});
    w.key("n").null();
    w.key("list")
        .begin_array()
        .value(1)
        .value(2)
        .begin_object()
        .key("x")
        .value(3)
        .end_object()
        .end_array();
    w.key("raw").raw("[7,8]");
    w.end_object();
    const Result<std::string> out = w.finish();
    REQUIRE(out.ok());
    CHECK(out->find("\"b\"") < out->find("\"a\"")); // insertion order
    CHECK(out->find('\n') == std::string::npos);    // compact

    CHECK(read_doc(*out, [&](DataReader &r) {
              CHECK(r.boolean("b"));
              CHECK(r.integer("a", -100, 0) == -42);
              CHECK(r.u64("u") == std::numeric_limits<std::uint64_t>::max());
              CHECK(r.integer64("i64", std::numeric_limits<std::int64_t>::min(), 0) ==
                    std::numeric_limits<std::int64_t>::min());
              CHECK(r.number("f", 0, 1) == 0.1f);
              CHECK(r.number("d", 0, 5) == 2.5f);
              CHECK(r.text("s") == "quote\" back\\ nl\n tab\t ctl\x01 \xC3\xA9");
              CHECK(r.id("id") == named);
              CHECK(r.id("raw_id") == StrId{0x1234});
              CHECK(r.vec2("v") == Vec2{1.5f, -2});
              CHECK(r.ivec2("iv") == IVec2{3, 4});
              CHECK(r.rect("r") == Rect{1, 2, 3, 4});
              CHECK(r.irect("ir") == IRect{5, 6, 7, 8});
              CHECK(r.color("c") == Color{255, 128, 0, 255});
              CHECK(r.color("ca") == Color{1, 2, 3, 4});
              CHECK(r.type("n") == DataType::Null);
              r.ignore("n");
              DataArray list = r.array("list");
              REQUIRE(list.size() == 3);
              CHECK(list.integer(1, 0, 9) == 2);
              CHECK(list.object(2).integer("x", 0, 9) == 3);
              CHECK(r.array("raw").integer(1, 0, 9) == 8);
          }).ok());

    DataWriter small;
    small.begin_object()
        .key("a")
        .value(1)
        .key("b")
        .begin_array()
        .value(1)
        .value(2)
        .end_array()
        .end_object();
    CHECK(small.finish().value() == R"({"a":1,"b":[1,2]})");
}

TEST_CASE("data writer: pretty output parses to the same document") {
    DataWriter w;
    w.begin_object().key("z").value(1).key("a").begin_array().value("x").end_array().end_object();
    const auto pretty = w.finish(true);
    REQUIRE(pretty.ok());
    CHECK(pretty->find('\n') != std::string::npos);
    CHECK(pretty->find("\"z\"") < pretty->find("\"a\"")); // order kept
    CHECK(read_doc(*pretty, [](DataReader &r) {
              CHECK(r.integer("z", 0, 9) == 1);
              CHECK(r.array("a").string(0) == "x");
          }).ok());
}

TEST_CASE("data writer: invalid UTF-8 and non-finite numbers are Errors") {
    DataWriter a;
    a.begin_object().key("s").value(std::string_view("bad \xFF byte")).end_object();
    CHECK_FALSE(a.finish().ok());
    DataWriter b;
    b.begin_object().key("bad\xC3").value(1).end_object();
    CHECK_FALSE(b.finish().ok());
    DataWriter c;
    c.begin_array().value(std::numeric_limits<double>::quiet_NaN()).end_array();
    CHECK_FALSE(c.finish().ok());
    DataWriter d;
    d.begin_array().value(std::numeric_limits<float>::infinity()).end_array();
    CHECK_FALSE(d.finish().ok());
    DataWriter e;
    e.begin_object().key("v").value(Vec2{std::numeric_limits<float>::quiet_NaN(), 0}).end_object();
    CHECK_FALSE(e.finish().ok());
}

TEST_CASE("data writer: structural mistakes are Errors") {
    DataWriter open;
    open.begin_object().key("a").value(1);
    CHECK_FALSE(open.finish().ok());
    DataWriter dangling;
    dangling.begin_object().key("a");
    CHECK_FALSE(dangling.finish().ok());
    DataWriter no_key;
    no_key.begin_object().value(1).end_object();
    CHECK_FALSE(no_key.finish().ok());
    DataWriter two_keys;
    two_keys.begin_object().key("a").key("b").value(1).end_object();
    CHECK_FALSE(two_keys.finish().ok());
    DataWriter mismatch;
    mismatch.begin_object().end_array();
    CHECK_FALSE(mismatch.finish().ok());
    DataWriter two_roots;
    two_roots.value(1).value(2);
    CHECK_FALSE(two_roots.finish().ok());
    DataWriter empty;
    CHECK_FALSE(empty.finish().ok());
    DataWriter scalar;
    scalar.value("just a string");
    CHECK(scalar.finish().value() == "\"just a string\"");
    DataWriter moved_from;
    moved_from.begin_array().value(1).end_array();
    DataWriter moved = std::move(moved_from);
    CHECK(moved.finish().value() == "[1]");
}

TEST_CASE("data writer: copy and copy_rest carry unread keys through a migration") {
    const char *v1 = R"({"version": 1, "hp": 3, "inv": {"a": [1, 2]}, "name": "x"})";
    auto doc = DataDocument::parse(v1, "save.json");
    REQUIRE(doc.ok());
    DataReader in = doc->root();
    REQUIRE(in.integer("version", 1, 1) == 1);
    DataWriter w;
    w.begin_object();
    w.key("version").value(2);
    w.copy(in, "name");    // moved to the front
    w.copy(in, "missing"); // absent keys are skipped
    w.copy_rest(in);
    w.end_object();
    CHECK(doc->finish().ok()); // everything was read or copied
    const auto out = w.finish();
    REQUIRE(out.ok());
    CHECK(out->find("\"name\"") < out->find("\"hp\""));
    CHECK(out->find("\"hp\"") < out->find("\"inv\""));
    CHECK(read_doc(*out, [](DataReader &r) {
              CHECK(r.integer("version", 0, 9) == 2);
              CHECK(r.integer("hp", 0, 9) == 3);
              CHECK(r.text("name") == "x");
              DataArray a = r.object("inv").array("a");
              REQUIRE(a.size() == 2);
              CHECK(a.integer(1, 0, 9) == 2);
          }).ok());
}

TEST_CASE("data: number64 keeps double precision; arrays read text, u64 and number64") {
    CHECK(read_doc(R"({"d": 0.1, "big": 123456789.125, "a": ["", "x"], "u": [18446744073709551615, 7], "f": [0.1]})",
                   [](DataReader &r) {
                       CHECK(r.number64("d", -1.0, 1.0) == 0.1);
                       CHECK(r.number64("big", 0.0, 1e12) == 123456789.125);
                       DataArray a = r.array("a");
                       CHECK(a.text(0).empty());
                       CHECK(a.text(1) == "x");
                       DataArray u = r.array("u");
                       CHECK(u.u64(0) == 18446744073709551615ULL);
                       CHECK(u.u64(1) == 7);
                       CHECK(r.array("f").number64(0, 0.0, 1.0) == 0.1);
                   }).ok());
    CHECK_FALSE(read_doc(R"({"d": 2.5})", [](DataReader &r) { (void)r.number64("d", 0.0, 1.0); }).ok());
    CHECK_FALSE(read_doc(R"({"u": [-1]})", [](DataReader &r) { (void)r.array("u").u64(0); }).ok());
}
