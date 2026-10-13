// Save envelope, migrations and slots (save.h).
#include <string>
#include <vector>

#include <lam/core/data.h>
#include <lam/core/fs.h>
#include <lam/scene/save.h>
#include <lam/test/support.h>

using namespace lam;

namespace {

// Adds "v<from+1>": true and keeps everything else.
Status add_marker(DataReader &in, DataWriter &out, int from) {
    out.key("v" + std::to_string(from + 1)).value(true);
    out.copy_rest(in);
    return {};
}

// Renames "gold" to "coins" (2 -> 3 in the tests below).
Status rename_gold(DataReader &in, DataWriter &out, int) {
    out.key("coins").value(in.integer("gold", 0, 1000000));
    out.copy_rest(in);
    return {};
}

Status failing(DataReader &, DataWriter &, int from) {
    return make_error(ErrorCode::Invalid, "cannot upgrade from " + std::to_string(from));
}

// Forgets copy_rest: every other key would be dropped silently.
Status forgetful(DataReader &, DataWriter &out, int) {
    out.key("only").value(1);
    return {};
}

// Drops "junk" on purpose.
Status drop_junk(DataReader &in, DataWriter &out, int) {
    in.ignore("junk");
    out.copy_rest(in);
    return {};
}

std::string data_value(const std::string &json, std::string_view key) {
    Result<DataDocument> doc = DataDocument::parse(json, "t");
    REQUIRE(doc);
    DataReader r = doc->root();
    if (!r.has(key)) return "<missing>";
    DataWriter w;
    w.begin_object().copy(r, key).end_object();
    Result<std::string> t = w.finish();
    REQUIRE(t);
    return *t;
}

} // namespace

TEST_CASE("a save round-trips and is upgraded step by step") {
    SaveEnvelope e;
    e.game = "farm";
    e.version = 1;
    e.data_json = R"({"gold": 5})";
    const Result<std::string> text = write_save(e);
    REQUIRE(text);
    const SaveMigration steps[] = {add_marker, add_marker};
    const Result<SaveEnvelope> r = read_save(*text, "farm", 3, steps);
    REQUIRE(r);
    CHECK(r->version == 3);
    CHECK(r->game == "farm");
    CHECK(r->data_json == R"({"v3":true,"v2":true,"gold":5})");
    CHECK(r->summary_json == "{}");

    CHECK_FALSE(read_save(*text, "other", 1, {}));
    CHECK(read_save("{not json", "farm", 1, {}).error().code == ErrorCode::Parse);
    e.version = 9;
    const Result<std::string> newer = write_save(e);
    REQUIRE(newer);
    CHECK(read_save(*newer, "farm", 3, steps).error().code == ErrorCode::VersionTooNew);
    CHECK(read_save(*text, "farm", 2, {}).error().code == ErrorCode::MigrationFailed);   // none given
}

TEST_CASE("save envelope: canonical output and strict reading") {
    SaveEnvelope e{.game = "hop", .version = 2, .summary_json = " { \"level\" : 3 } ",
                   .data_json = "{\"b\": [1, 2], \"a\": {\"x\": \"y\"}}"};
    const Result<std::string> text = write_save(e);
    REQUIRE(text);
    CHECK(*text == R"({"format":"lam-save","game":"hop","version":2,"summary":{"level":3},)"
                   R"("data":{"b":[1,2],"a":{"x":"y"}}})");
    // Same input, same bytes; reading and writing again is a fixed point.
    const Result<SaveEnvelope> back = read_save(*text, "hop", 2, {});
    REQUIRE(back);
    const Result<std::string> again = write_save(*back);
    REQUIRE(again);
    CHECK(*again == *text);

    SUBCASE("empty summary and data are {}") {
        const Result<std::string> t = write_save(SaveEnvelope{.game = "g"});
        REQUIRE(t);
        CHECK(*t == R"({"format":"lam-save","game":"g","version":1,"summary":{},"data":{}})");
    }
    SUBCASE("write errors") {
        CHECK(write_save(SaveEnvelope{.game = ""}).error().code == ErrorCode::Invalid);
        CHECK(write_save(SaveEnvelope{.game = "g", .version = 0}).error().code == ErrorCode::Invalid);
        CHECK_FALSE(write_save(SaveEnvelope{.game = "g", .data_json = "[1]"}));
        CHECK_FALSE(write_save(SaveEnvelope{.game = "g", .data_json = "{"}));
        CHECK_FALSE(write_save(SaveEnvelope{.game = "g", .summary_json = "3"}));
        CHECK_FALSE(write_save(SaveEnvelope{.game = "g", .data_json = "{\"bad\": \"\xff\"}"}));
    }
    SUBCASE("read errors") {
        CHECK(read_save("[1]", "g", 1, {}).error().code == ErrorCode::Invalid);
        CHECK(read_save(R"({"format":"other","game":"g","version":1,"data":{}})", "g", 1, {})
                  .error().code == ErrorCode::Invalid);
        CHECK(read_save(R"({"format":"lam-save","game":"g","version":0,"data":{}})", "g", 1, {})
                  .error().code == ErrorCode::Invalid);
        CHECK(read_save(R"({"format":"lam-save","game":"g","version":-4,"data":{}})", "g", 1, {})
                  .error().code == ErrorCode::Invalid);
        CHECK(read_save(R"({"format":"lam-save","game":"g","version":1.5,"data":{}})", "g", 1, {})
                  .error().code == ErrorCode::Invalid);
        CHECK(read_save(R"({"format":"lam-save","game":"g","data":{}})", "g", 1, {}).error().code ==
              ErrorCode::Invalid);
        CHECK(read_save(R"({"format":"lam-save","game":"g","version":1})", "g", 1, {}).error().code ==
              ErrorCode::Invalid);
        CHECK(read_save(R"({"format":"lam-save","game":"g","version":1,"data":[]})", "g", 1, {})
                  .error().code == ErrorCode::Invalid);
        CHECK(read_save(R"({"format":"lam-save","game":"g","version":1,"data":{},"x":1})", "g", 1, {})
                  .error().code == ErrorCode::Invalid);                                   // unknown key
        CHECK(read_save(R"({"format":"lam-save","game":"g","version":1,"data":{}})", "g", 0, {})
                  .error().code == ErrorCode::Invalid);
    }
    SUBCASE("the legacy \"lamda-save\" format tag is accepted") {
        const Result<SaveEnvelope> r = read_save(
            R"({"format":"lamda-save","game":"g","version":1,"summary":{"t":1},"data":{"d":2}})",
            "g", 1, {});
        REQUIRE(r);
        CHECK(r->summary_json == R"({"t":1})");
        CHECK(r->data_json == R"({"d":2})");
        // Older saves have "summary": null and an indent of 1.
        const Result<SaveEnvelope> n = read_save(
            "{\n \"format\": \"lamda-save\",\n \"game\": \"g\",\n \"version\": 1,\n"
            " \"summary\": null,\n \"data\": {\n  \"gold\": 5\n }\n}",
            "g", 1, {});
        REQUIRE(n);
        CHECK(n->summary_json == "{}");
        CHECK(n->data_json == R"({"gold":5})");
        CHECK(read_save(R"({"format":"lam-save","game":"g","version":1,"summary":[],"data":{}})", "g",
                        1, {})
                  .error().code == ErrorCode::Invalid);
    }
}

TEST_CASE("save migrations: one step at a time from a bounded list") {
    const Result<std::string> v1 = write_save(SaveEnvelope{.game = "g", .version = 1,
                                                           .data_json = R"({"gold":7,"junk":0})"});
    REQUIRE(v1);
    SUBCASE("each step sees its from_version, in order") {
        const SaveMigration steps[] = {drop_junk, rename_gold, add_marker};
        const Result<SaveEnvelope> r = read_save(*v1, "g", 4, steps);
        REQUIRE(r);
        CHECK(r->version == 4);
        CHECK(r->data_json == R"({"v4":true,"coins":7})");
        CHECK(data_value(r->data_json, "gold") == "<missing>");
    }
    SUBCASE("a save already at the current version runs no migration") {
        const SaveMigration steps[] = {failing};
        const Result<SaveEnvelope> r = read_save(*v1, "g", 1, steps);
        REQUIRE(r);
        CHECK(r->data_json == R"({"gold":7,"junk":0})");
    }
    SUBCASE("a missing, null or failing step is MigrationFailed and names the step") {
        const SaveMigration short_list[] = {drop_junk};
        const Result<SaveEnvelope> a = read_save(*v1, "g", 3, short_list);
        REQUIRE_FALSE(a);
        CHECK(a.error().code == ErrorCode::MigrationFailed);
        CHECK(a.error().message.find("version 2") != std::string::npos);
        const SaveMigration with_null[] = {drop_junk, nullptr};
        CHECK(read_save(*v1, "g", 3, with_null).error().code == ErrorCode::MigrationFailed);
        const SaveMigration fails[] = {failing};
        const Result<SaveEnvelope> c = read_save(*v1, "g", 2, fails);
        REQUIRE_FALSE(c);
        CHECK(c.error().code == ErrorCode::MigrationFailed);
        CHECK(c.error().message.find("cannot upgrade from 1") != std::string::npos);
    }
    SUBCASE("a step that leaves keys unread fails instead of dropping them") {
        const SaveMigration steps[] = {forgetful};
        const Result<SaveEnvelope> r = read_save(*v1, "g", 2, steps);
        REQUIRE_FALSE(r);
        CHECK(r.error().code == ErrorCode::MigrationFailed);
        CHECK(r.error().message.find("unread") != std::string::npos);
    }
}

TEST_CASE("save slots write, list, load and erase") {
    test::TempDir dir;
    SaveSlots slots(dir.file("saves"), "farm", 1, 3);
    CHECK(slots.count() == 3);
    CHECK_FALSE(slots.exists(1));
    CHECK(slots.latest() == 0);
    REQUIRE(slots.save(2, SaveEnvelope{.summary_json = R"({"title":"Day 4"})",
                                       .data_json = R"({"day":4})"}));
    CHECK(slots.exists(2));
    CHECK(slots.latest() == 2);
    const std::vector<std::string> summaries = slots.summaries();
    REQUIRE(summaries.size() == 3);
    CHECK(summaries[0].empty());
    CHECK(summaries[1] == R"({"title":"Day 4"})");
    CHECK(summaries[2].empty());
    const Result<SaveEnvelope> loaded = slots.load(2);
    REQUIRE(loaded);
    CHECK(loaded->data_json == R"({"day":4})");
    CHECK(loaded->game == "farm");
    CHECK(slots.load(1).error().code == ErrorCode::NotFound);
    CHECK(slots.save(4, {}).error().code == ErrorCode::OutOfRange);
    REQUIRE(slots.erase(2));
    CHECK_FALSE(slots.exists(2));
    CHECK(slots.erase(2));                                      // missing file is fine
    CHECK(slots.latest() == 0);
}

TEST_CASE("save slots: range checks, game and version, atomic files") {
    test::TempDir dir;
    SaveSlots slots(dir.path(), "hop", 2, 2);
    for (int bad : {0, -1, 3, 1000}) {
        CAPTURE(bad);
        CHECK(slots.save(bad, {}).error().code == ErrorCode::OutOfRange);
        CHECK(slots.load(bad).error().code == ErrorCode::OutOfRange);
        CHECK(slots.erase(bad).error().code == ErrorCode::OutOfRange);
        CHECK_FALSE(slots.exists(bad));
        CHECK(slots.slot_path(bad).empty());
    }
    CHECK(slots.slot_path(1) == join_path(dir.path(), "slot1.json"));
    // The envelope takes the SaveSlots' game and version, whatever the caller filled in.
    REQUIRE(slots.save(1, SaveEnvelope{.game = "other", .version = 9, .data_json = R"({"x":1})"}));
    const Result<std::string> raw = read_file(slots.slot_path(1));
    REQUIRE(raw);
    CHECK(raw->find(R"("game":"hop","version":2)") != std::string::npos);
    CHECK_FALSE(file_exists(slots.slot_path(1) + ".tmp"));
    // Overwrite keeps exactly one file with the new content.
    REQUIRE(slots.save(1, SaveEnvelope{.data_json = R"({"x":2})"}));
    CHECK(slots.load(1)->data_json == R"({"x":2})");
    // Slot 2 later than slot 1: latest follows the file times.
    REQUIRE(slots.save(2, SaveEnvelope{.data_json = R"({"x":3})"}));
    CHECK(slots.latest() == 2);
    // A slot written by an older build is upgraded on load; another game's file is not listed.
    SaveSlots old_build(dir.path(), "hop", 1, 2);
    REQUIRE(old_build.save(1, SaveEnvelope{.summary_json = R"({"s":1})", .data_json = R"({"gold":3})"}));
    const SaveMigration steps[] = {rename_gold};
    const Result<SaveEnvelope> up = slots.load(1, steps);
    REQUIRE(up);
    CHECK(up->version == 2);
    CHECK(up->data_json == R"({"coins":3})");
    CHECK(slots.load(1).error().code == ErrorCode::MigrationFailed);
    SaveSlots other(dir.path(), "zap", 1, 2);
    CHECK(other.summaries()[0].empty());
    CHECK(other.load(1).error().code == ErrorCode::Invalid);
    // A corrupt file is reported, not thrown, and summaries skip it.
    REQUIRE(write_file_atomic(slots.slot_path(2), "{corrupt"));
    CHECK(slots.load(2).error().code == ErrorCode::Parse);
    CHECK(slots.summaries()[1].empty());
    // Bad constructor arguments are clamped.
    SaveSlots tiny(dir.path(), "hop", 0, 0);
    CHECK(tiny.count() == 1);
}
