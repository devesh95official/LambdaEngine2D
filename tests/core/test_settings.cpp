// SettingsStore: lenient loading, clamping, unknown keys and the revision counter.
#include <doctest/doctest.h>

#include <cmath>
#include <limits>
#include <string>

#include <lam/core/data.h>
#include <lam/core/fs.h>
#include <lam/core/settings.h>
#include <lam/test/support.h>

using namespace lam;

namespace {

void define_volume_schema(SettingsStore &s) {
    s.define_int("master_volume", 5, 0, 10);
    s.define_int("music_volume", 7, 0, 10);
    s.define_bool("fullscreen", false);
    s.define_int("sfx_volume", 5, 0, 10);
    s.define_int("ui_scale", 1, 1, 4);
}

void check_bad_values_kept_defaults(const SettingsStore &s) {
    CHECK(s.get_int("master_volume") == 5);   // 99 is out of range
    CHECK(s.get_int("music_volume") == 7);    // "loud" is not a number
    CHECK(s.get_bool("fullscreen") == false); // 1 is not a bool
    CHECK(s.get_int("sfx_volume") == 2);      // valid
    CHECK(s.get_int("ui_scale") == 1);        // 2.5 is not whole
}

constexpr const char *kBadValues =
    R"({"master_volume": 99, "music_volume": "loud", "fullscreen": 1, "sfx_volume": 2,
        "ui_scale": 2.5})";

} // namespace

TEST_CASE("settings ignore bad values and keep defaults") {
    // Keys defined before load.
    lam::test::TempDir dir;
    const std::string path = dir.file("settings.json");
    REQUIRE(write_file_atomic(path, kBadValues).ok());
    lam::test::CaptureLog log;
    SettingsStore s;
    define_volume_schema(s);
    REQUIRE(s.load(path).ok());
    check_bad_values_kept_defaults(s);
    CHECK(log.count(LogLevel::Warn) >= 1);
}

TEST_CASE("settings: values loaded before their definition are validated when defined") {
    lam::test::TempDir dir;
    const std::string path = dir.file("settings.json");
    REQUIRE(write_file_atomic(path, kBadValues).ok());
    lam::test::CaptureLog log;
    SettingsStore s;
    REQUIRE(s.load(path).ok());
    CHECK_FALSE(s.defined("master_volume"));
    define_volume_schema(s);
    check_bad_values_kept_defaults(s);

    REQUIRE(write_file_atomic(path,
                              R"({"audio.music": 0.5, "video.vsync": "yes", "player.name": "Ann"})")
                .ok());
    SettingsStore t;
    REQUIRE(t.load(path).ok());
    t.define_float("audio.music", 0.8f, 0.0f, 1.0f);
    t.define_bool("video.vsync", true);
    t.define_string("player.name", "Hero");
    CHECK(t.get_float("audio.music") == doctest::Approx(0.5f));
    CHECK(t.get_bool("video.vsync"));
    CHECK(t.get_string("player.name") == "Ann");
}

TEST_CASE("settings: a missing or broken file keeps every default") {
    lam::test::TempDir dir;
    SettingsStore s;
    define_volume_schema(s);
    CHECK(s.load(dir.file("absent.json")).ok());
    CHECK(s.get_int("master_volume") == 5);
    const std::string path = dir.file("broken.json");
    REQUIRE(write_file_atomic(path, "{ not json").ok());
    lam::test::CaptureLog log;
    CHECK(s.load(path).ok());
    CHECK(s.get_int("music_volume") == 7);
    CHECK(log.count(LogLevel::Warn) >= 1);
}

TEST_CASE("settings: setters clamp to the schema; reset restores the default") {
    SettingsStore s;
    s.define_int("volume", 5, 0, 10);
    s.define_float("gamma", 1.0f, 0.5f, 2.0f);
    s.define_int("clamped_default", 50, 0, 10);
    CHECK(s.get_int("clamped_default") == 10);
    s.set_int("volume", 99);
    CHECK(s.get_int("volume") == 10);
    s.set_int("volume", -3);
    CHECK(s.get_int("volume") == 0);
    s.set_float("gamma", 3.0f);
    CHECK(s.get_float("gamma") == 2.0f);
    s.set_float("gamma", 0.1f);
    CHECK(s.get_float("gamma") == 0.5f);
    s.set_float("gamma", 1.25f);
    s.set_float("gamma", std::numeric_limits<float>::quiet_NaN()); // ignored
    CHECK(s.get_float("gamma") == 1.25f);
    s.reset("gamma");
    CHECK(s.get_float("gamma") == 1.0f);
    s.reset("unknown.key"); // ignored
    CHECK(s.defined("volume"));
    CHECK_FALSE(s.defined("unknown.key"));
}

TEST_CASE("settings: revision counts changes only") {
    SettingsStore s;
    const std::uint32_t r0 = s.revision();
    s.define_bool("vsync", true);
    s.define_string("name", "a");
    const std::uint32_t r1 = s.revision();
    CHECK(r1 > r0);
    s.set_bool("vsync", true); // same value
    s.set_string("name", "a");
    CHECK(s.revision() == r1);
    s.set_bool("vsync", false);
    CHECK(s.revision() > r1);
    const std::uint32_t r2 = s.revision();
    s.set_string("name", "b");
    CHECK(s.revision() > r2);
    const std::uint32_t r3 = s.revision();
    s.reset("vsync");
    CHECK(s.revision() > r3);
    CHECK(s.get_bool("vsync"));
}

TEST_CASE("settings: save and load round-trip; unknown keys are written back unchanged") {
    lam::test::TempDir dir;
    const std::string path = dir.file("settings.json");
    REQUIRE(write_file_atomic(path, R"({"future.option": 3, "future.flag": true, "future.name": "x",
                                        "audio.music": 0.25})")
                .ok());
    SettingsStore s;
    s.define_float("audio.music", 0.8f, 0.0f, 1.0f);
    s.define_int("video.scale", 2, 1, 4);
    s.define_bool("video.fullscreen", false);
    s.define_string("player.name", "Hero");
    REQUIRE(s.load(path).ok());
    CHECK(s.get_float("audio.music") == 0.25f);
    s.set_float("audio.music", 0.6f);
    s.set_int("video.scale", 3);
    s.set_bool("video.fullscreen", true);
    s.set_string("player.name", "Zoë \"Z\"");
    const std::string out = dir.file("out.json");
    REQUIRE(s.save(out).ok());

    SettingsStore back;
    back.define_float("audio.music", 0.8f, 0.0f, 1.0f);
    back.define_int("video.scale", 2, 1, 4);
    back.define_bool("video.fullscreen", false);
    back.define_string("player.name", "Hero");
    REQUIRE(back.load(out).ok());
    CHECK(back.get_float("audio.music") == 0.6f);
    CHECK(back.get_int("video.scale") == 3);
    CHECK(back.get_bool("video.fullscreen"));
    CHECK(back.get_string("player.name") == "Zoë \"Z\"");

    auto text = read_file(out);
    REQUIRE(text.ok());
    auto doc = DataDocument::parse(*text, "out.json");
    REQUIRE(doc.ok());
    DataReader root = doc->root();
    CHECK(root.integer("future.option", 0, 10) == 3);
    CHECK(root.boolean("future.flag"));
    CHECK(root.text("future.name") == "x");
    root.ignore_unknown();
    CHECK(doc->finish().ok());
}

// Regression: unknown objects, arrays and nulls used to be dropped on save.
TEST_CASE("settings: unknown object and array values survive a save") {
    lam::test::TempDir dir;
    const std::string path = dir.file("settings.json");
    REQUIRE(write_file_atomic(path, R"({"future.table": {"a": 1}, "future.list": [1, 2]})").ok());
    SettingsStore s;
    REQUIRE(s.load(path).ok());
    REQUIRE(s.save(path).ok());
    const std::string text = read_file(path).value_or("");
    CHECK(text.find("future.table") != std::string::npos);
    CHECK(text.find("future.list") != std::string::npos);
}

// Regression: numbers used to load as float, so 123456789 came back as 123456792.
TEST_CASE("settings: large integers round-trip exactly") {
    lam::test::TempDir dir;
    const std::string path = dir.file("settings.json");
    SettingsStore s;
    s.define_int("seed", 0, 0, 2000000000);
    s.set_int("seed", 123456789);
    REQUIRE(s.save(path).ok());
    SettingsStore back;
    back.define_int("seed", 0, 0, 2000000000);
    REQUIRE(back.load(path).ok());
    CHECK(back.get_int("seed") == 123456789);
}

// Regression: 1e39 fails as a float, and since reader errors are sticky every later key used to
// read as 0.
TEST_CASE("settings: one out-of-range number does not clobber the other keys") {
    lam::test::TempDir dir;
    const std::string path = dir.file("settings.json");
    REQUIRE(write_file_atomic(path, R"({"a": 1e39, "b": 7, "c": true})").ok());
    lam::test::CaptureLog log;
    SettingsStore s;
    s.define_int("a", 3, 0, 10);
    s.define_int("b", 5, 0, 10);
    s.define_bool("c", false);
    REQUIRE(s.load(path).ok());
    CHECK(s.get_int("a") == 3);
    CHECK(s.get_int("b") == 7);
    CHECK(s.get_bool("c"));
}

TEST_CASE("settings: unknown keys are written back verbatim, including a huge number") {
    lam::test::TempDir dir;
    const std::string path = dir.file("settings.json");
    REQUIRE(write_file_atomic(path, R"({"x": 1e39, "n": null, "v": 0.8})").ok());
    SettingsStore s;
    REQUIRE(s.load(path).ok());
    REQUIRE(s.save(path).ok());
    const std::string text = read_file(path).value_or("");
    CHECK(text.find("1e+39") != std::string::npos);
    CHECK(text.find("null") != std::string::npos);
    CHECK(text.find("0.8") != std::string::npos);
    CHECK(text.find("0.800000") == std::string::npos);
}
