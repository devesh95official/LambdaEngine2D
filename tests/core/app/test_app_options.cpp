// parse_launch_options and LaunchOptions queries.
#include <lam/test/support.h>

using namespace lam;

namespace {
template <std::size_t N>
LaunchOptions parse(const char *const (&argv)[N]) {
    return parse_launch_options(static_cast<int>(N), argv);
}
} // namespace

TEST_CASE("options: defaults with no arguments") {
    const LaunchOptions o = parse_launch_options(0, nullptr);
    CHECK_FALSE(o.hidden);
    CHECK_FALSE(o.headless);
    CHECK_FALSE(o.step);
    CHECK(o.frames == 0);
    CHECK(o.draw_every == 1);
    CHECK(o.seed == 0);
    CHECK(o.record_input.empty());
    CHECK(o.replay.empty());
    CHECK_FALSE(o.replay_any_build);
    CHECK(o.screenshot.empty());
    CHECK(o.hash_every == 0);
    CHECK_FALSE(o.print_schedule);
    CHECK_FALSE(o.no_vsync);
    CHECK(o.extra.empty());
    CHECK_FALSE(o.deterministic());
    const char *const only_exe[] = {"game"};
    CHECK(parse(only_exe).extra.empty());
}

TEST_CASE("options: every documented flag") {
    const char *const argv[] = {"game",
                                "--hidden",
                                "--headless",
                                "--step",
                                "--frames",
                                "120",
                                "--draw-every",
                                "3",
                                "--seed",
                                "0x1F",
                                "--record-input",
                                "in.rec",
                                "--replay",
                                "old.rec",
                                "--replay-any-build",
                                "--screenshot",
                                "shot.png",
                                "--hash-every",
                                "10",
                                "--print-schedule",
                                "--no-vsync"};
    const LaunchOptions o = parse(argv);
    CHECK(o.hidden);
    CHECK(o.headless);
    CHECK(o.step);
    CHECK(o.frames == 120);
    CHECK(o.draw_every == 3);
    CHECK(o.seed == 31);
    CHECK(o.record_input == "in.rec");
    CHECK(o.replay == "old.rec");
    CHECK(o.replay_any_build);
    CHECK(o.screenshot == "shot.png");
    CHECK(o.hash_every == 10);
    CHECK(o.print_schedule);
    CHECK(o.no_vsync);
    CHECK(o.extra.empty());
    CHECK(o.deterministic());
    CHECK(o.has("--headless"));
    CHECK(o.has("--no-vsync"));
}

TEST_CASE("options: unknown flags are kept in order; has() finds them") {
    const char *const argv[] = {"game", "--level", "3", "--god-mode", "--step"};
    const LaunchOptions o = parse(argv);
    CHECK(o.extra == std::vector<std::string>{"--level", "3", "--god-mode"});
    CHECK(o.has("--god-mode"));
    CHECK_FALSE(o.has("--nope"));
    CHECK(o.step);
}

TEST_CASE("options: values are clamped; bad or missing values stay in extra") {
    const char *const clamp[] = {"game", "--frames", "-5", "--draw-every", "0", "--seed", "42"};
    const LaunchOptions a = parse(clamp);
    CHECK(a.frames == 0);
    CHECK(a.draw_every == 1);
    CHECK(a.seed == 42);
    const char *const bad[] = {"game", "--frames", "ten", "--seed"};
    const LaunchOptions b = parse(bad);
    CHECK(b.frames == 0);
    CHECK(b.seed == 0);
    CHECK(b.extra == std::vector<std::string>{"--frames", "ten", "--seed"});
}

TEST_CASE("options: deterministic() is headless, step, record or replay") {
    const char *const h[] = {"g", "--headless"};
    const char *const s[] = {"g", "--step"};
    const char *const r[] = {"g", "--record-input", "x"};
    const char *const p[] = {"g", "--replay", "x"};
    const char *const n[] = {"g", "--hidden", "--no-vsync"};
    CHECK(parse(h).deterministic());
    CHECK(parse(s).deterministic());
    CHECK(parse(r).deterministic());
    CHECK(parse(p).deterministic());
    CHECK_FALSE(parse(n).deterministic());
}

TEST_CASE("options: scripted() is deterministic, --hidden, --frames N or --bot") {
    const char *const h[] = {"g", "--headless"};
    const char *const s[] = {"g", "--step"};
    const char *const r[] = {"g", "--replay", "x"};
    const char *const hid[] = {"g", "--hidden"};
    const char *const f[] = {"g", "--frames", "300"};
    const char *const b[] = {"g", "--bot"};
    const char *const player[] = {"g", "--no-vsync", "--seed", "7", "--level", "3"};
    for (const LaunchOptions &o : {parse(h), parse(s), parse(r), parse(hid), parse(f), parse(b)})
        CHECK(o.scripted());
    CHECK_FALSE(parse(player).scripted());
    CHECK_FALSE(parse_launch_options(0, nullptr).scripted());
}

TEST_CASE("options: the App parses argv and exposes it") {
    const char *const argv[] = {"game", "--headless", "--hash-every", "5", "--custom"};
    App app({}, 5, argv);
    CHECK(app.options().headless);
    CHECK(app.options().hash_every == 5);
    CHECK(app.options().has("--custom"));
}

TEST_CASE("options: value() and int_value() read game flags from extra") {
    const char *const argv[] = {"game",    "--enemies", "2000",  "--level=forest", "--name",
                                "Ada",     "--speed",   "fast",  "--huge",         "99999999999",
                                "--neg",   "-5",        "--seed", "7",             "--last"};
    const LaunchOptions o = parse(argv);
    CHECK(o.seed == 7); // engine flags are consumed as before
    REQUIRE(o.value("--enemies"));
    CHECK(*o.value("--enemies") == "2000");
    CHECK(o.int_value("--enemies", 5) == 2000);
    CHECK(o.value("--level") == std::optional<std::string_view>("forest")); // flag=value form
    CHECK_FALSE(o.value("--lev"));                                          // no prefix match
    CHECK(o.int_value("--level", 7) == 7);                                  // not a number
    CHECK(o.value("--name") == std::optional<std::string_view>("Ada"));
    CHECK(o.int_value("--speed", 3) == 3);
    CHECK(o.int_value("--huge", -1) == -1); // out of int range
    CHECK(o.int_value("--neg", 0) == -5);
    CHECK_FALSE(o.value("--last")); // last argument: no value
    CHECK(o.int_value("--last", 4) == 4);
    CHECK(o.has("--last"));
    CHECK_FALSE(o.value("--missing"));
    CHECK(o.int_value("--missing", 11) == 11);
    CHECK_FALSE(o.value(""));
    CHECK_FALSE(o.value("--seed")); // engine flags are not in extra
}
