// DialogueRunner, the .dialogue.json parser and its loader.
#include <string>

#include <lam/assets/asset_server.h>
#include <lam/test/assets.h>
#include <lam/test/support.h>
#include <lam/ui/dialogue.h>
#include <lam/ui/ui_plugin.h>

using namespace lam;
using namespace lam::literals;

namespace {

DialogueScript shop_script() {
    DialogueScript s;
    DialogueLine hello;
    hello.id = intern("hello");
    hello.speaker = intern("Shopkeeper");
    hello.text = "Hello there";
    hello.next = intern("buy");
    DialogueLine buy;
    buy.id = intern("buy");
    buy.speaker = intern("Shopkeeper");
    buy.text = "Buy?";
    buy.choices[0] = {"Yes", intern("thanks")};
    buy.choices[1] = {"No", StrId{}};
    buy.choice_count = 2;
    DialogueLine thanks;
    thanks.id = intern("thanks");
    thanks.text = "Thanks!";
    s.lines = {hello, buy, thanks};
    return s;
}

} // namespace

TEST_CASE("dialogue types, pages and picks a choice") {
    DialogueRunner d;
    CHECK_FALSE(d.active());
    d.start(shop_script(), "hello"_id);
    CHECK(d.active());
    d.update(0.5f, 10.0f);
    CHECK(d.visible_chars() == 5);
    CHECK(d.typed_last_update() == 5);
    CHECK(d.visible_text() == "Hello");
    CHECK(d.advance());                                         // finish the page
    CHECK(d.typing_done());
    CHECK(d.visible_text() == "Hello there");
    CHECK(d.advance());                                         // next page
    REQUIRE(d.line() != nullptr);
    CHECK(d.line()->id == "buy"_id);
    CHECK(d.visible_chars() == 0);
    CHECK_FALSE(d.choose(0));                                   // still typing
    d.update(1.0f, 10.0f);
    CHECK(d.typing_done());
    CHECK(d.advance());                                         // waits for a choice
    CHECK(d.line()->id == "buy"_id);
    CHECK_FALSE(d.choose(2));                                   // out of range
    CHECK_FALSE(d.choose(-1));
    CHECK(d.choose(1));                                         // "No": ends
    CHECK(d.last_choice() == 1);
    CHECK_FALSE(d.active());
    CHECK(d.line() == nullptr);
    CHECK_FALSE(d.advance());
}

TEST_CASE("dialogue: a choice jumps to its line; the end returns false") {
    DialogueRunner d;
    d.start(shop_script(), StrId{});                            // none: the first line
    CHECK(d.line()->id == "hello"_id);
    d.update(0, 0);                                             // <= 0 cps: whole line at once
    CHECK(d.typing_done());
    CHECK(d.advance());
    d.update(10.0f);
    CHECK(d.choose(0));
    REQUIRE(d.active());
    CHECK(d.line()->id == "thanks"_id);
    d.update(10.0f);
    CHECK_FALSE(d.advance());                                   // last line, no next: the end
    CHECK_FALSE(d.active());
    d.start(shop_script(), "nowhere"_id);
    CHECK_FALSE(d.active());
    d.start(DialogueScript{}, StrId{});
    CHECK_FALSE(d.active());
    d.start(shop_script(), "buy"_id);
    d.stop();
    CHECK_FALSE(d.active());
}

TEST_CASE("dialogue: typing counts code points, not bytes") {
    DialogueScript s;
    DialogueLine l;
    l.id = intern("u");
    l.text = "\xC3\xA9t\xC3\xA9";                               // "ete" with accents: 3 code points, 5 bytes
    s.lines = {l};
    DialogueRunner d;
    d.start(s, StrId{});
    d.update(0.1f, 20.0f);                                      // 2 code points
    CHECK(d.visible_chars() == 2);
    CHECK(d.visible_text() == "\xC3\xA9t");
    d.update(1.0f, 20.0f);
    CHECK(d.visible_chars() == 3);
    CHECK(d.typing_done());
}

TEST_CASE("dialogue: the runner copies the script") {
    DialogueRunner d;
    {
        DialogueScript temp = shop_script();
        d.start(temp, "hello"_id);
        temp.lines.clear();
    }
    REQUIRE(d.line() != nullptr);
    CHECK(d.line()->text == "Hello there");
}

TEST_CASE("dialogue json: defaults, choices and ends") {
    const char *json = R"({"lines": [
      {"id": "hi", "speaker": "Shopkeeper", "text": "Hello!"},
      {"text": "Anything else?"},
      {"id": "buy", "text": "Buy?", "choices": [{"text": "Yes", "next": "yes"}, {"text": "No"}]},
      {"id": "yes", "text": "Thanks!", "end": true},
      {"id": "unused", "text": "", "next": "hi"}
    ]})";
    Result<DialogueScript> r = parse_dialogue(json, "shop.dialogue.json");
    REQUIRE_MESSAGE(r.ok(), (r.ok() ? "" : r.error().message));
    const auto &lines = r->lines;
    REQUIRE(lines.size() == 5);
    CHECK(lines[0].speaker == "Shopkeeper"_id);
    CHECK(id_name(lines[0].speaker) == "Shopkeeper");
    CHECK(lines[0].next == lines[1].id);                        // default: the following line
    CHECK(lines[1].id == "line1"_id);                           // default id
    CHECK(lines[1].next == "buy"_id);
    CHECK(lines[2].choice_count == 2);
    CHECK(lines[2].choices[0].next == "yes"_id);
    CHECK_FALSE(lines[2].choices[1].next);
    CHECK_FALSE(lines[2].next);                                 // choices: no default next
    CHECK_FALSE(lines[3].next);                                 // "end"
    CHECK(lines[4].text.empty());
    CHECK(lines[4].next == "hi"_id);
    DialogueRunner d;
    d.start(*r, StrId{});
    d.update(1, 0);
    CHECK(d.advance());
    CHECK(d.line()->id == "line1"_id);
}

TEST_CASE("dialogue json: errors name the file and the place") {
    struct Case { const char *json; const char *needle; };
    const Case cases[] = {
        {R"({"lines": []})", "at least one line"},
        {R"({"lines": [{"id": "a", "text": "x"}, {"id": "a", "text": "y"}]})", "duplicate id 'a'"},
        {R"({"lines": [{"text": "x", "next": "nope"}]})", "no line 'nope'"},
        {R"({"lines": [{"text": "x", "choices": [{"text": "a", "next": "zz"}]}]})", "choices/0/next"},
        {R"({"lines": [{"text": "x", "choices": [{"text": "1"}, {"text": "2"}, {"text": "3"}, {"text": "4"}, {"text": "5"}]}]})",
         "at most 4"},
        {R"({"lines": [{"text": "x", "next": "x", "end": true}]})", "both"},
        {R"({"lines": [{"text": "x", "choices": [{"text": "a"}], "next": "line0"}]})", "own"},
        {R"({"lines": [{"txt": "x"}]})", "/lines/0/text: missing required key"},
        {R"({"lines": [{"text": "x", "txt": "y"}]})", "txt"},          // strict: unknown keys
        {R"({"lines": [{"text": "x", "choices": [{"text": "0123456789012345678901234567890123456789012345678"}]}]})",
         "47 bytes"},
        {R"({"lines": [{"text": "x", "choices": []}]})", "must not be empty"},
    };
    for (const Case &c : cases) {
        const std::string json = c.json;
        CAPTURE(json);
        Result<DialogueScript> r = parse_dialogue(json, "bad.dialogue.json");
        REQUIRE_FALSE(r.ok());
        const std::string message = r.error().message;
        CAPTURE(message);
        CHECK(r.error().message.find("bad.dialogue.json") != std::string::npos);
        CHECK(r.error().message.find(c.needle) != std::string::npos);
    }
}

TEST_CASE("dialogue loader: '.dialogue.json' assets load through the asset server") {
    test::TempDir dir;
    test::write_test_file(dir, "talk/shop.dialogue.json",
                          R"({"lines": [{"id": "a", "text": "Hi"}, {"id": "b", "text": "Bye"}]})");
    test::HeadlessApp h;
    AssetsConfig cfg;
    cfg.root = dir.path();
    h.add<AssetsPlugin>(cfg);
    h.add<UiPlugin>();
    AssetServer &assets = h.app().res<AssetServer>();
    Result<AssetId<DialogueScript>> id = assets.load_now<DialogueScript>("talk/shop.dialogue.json");
    REQUIRE_MESSAGE(id.ok(), (id.ok() ? "" : id.error().message));
    const DialogueScript *s = assets.get(*id);
    REQUIRE(s != nullptr);
    CHECK(s->lines.size() == 2);
    CHECK(s->lines[0].next == "b"_id);
}
