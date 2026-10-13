// core_demo: console tour of lam_core. Strict data loading, a Catalog keyed by StrId, JSON
// writing, the settings store, FixedString HUD text, the fixed-step clock and event windows.
//   ./core_demo
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <lam/core/catalog.h>
#include <lam/core/clock.h>
#include <lam/core/data.h>
#include <lam/core/events.h>
#include <lam/core/fixed_string.h>
#include <lam/core/fs.h>
#include <lam/core/log.h>
#include <lam/core/settings.h>

using namespace lam;
using namespace lam::literals;

namespace {

struct EnemyDef {
    StrId id;
    int hp = 0;
    float speed = 0;
    Color tint;
};

constexpr const char *kEnemies = R"({
  "enemies": [
    {"id": "goblin", "hp": 12, "speed": 1.5, "tint": "#7fbf3f"},
    {"id": "wolf",   "hp": 20, "tint": [200, 200, 210]}
  ]
})";

// Load-time parsing: every key is checked, and unread keys are errors (a typo cannot hide).
Result<std::vector<EnemyDef>> load_enemies(std::string_view text, std::string_view file) {
    return parse_data<std::vector<EnemyDef>>(text, file, [](DataReader &root) {
        std::vector<EnemyDef> out;
        DataArray list = root.array("enemies");
        for (std::size_t i = 0; i < list.size(); ++i) {
            DataReader e = list.object(i);
            out.push_back({e.id("id"), e.integer("hp", 1, 999),
                           e.number_or("speed", 1.0f, 0.0f, 10.0f), e.color("tint")});
        }
        return out;
    });
}

void data_and_catalog() {
    std::printf("-- data and catalog\n");
    auto enemies = load_enemies(kEnemies, "enemies.json");
    if (!enemies) {
        std::printf("error: %s\n", enemies.error().message.c_str());
        return;
    }
    Catalog<EnemyDef> catalog;
    for (const EnemyDef &e : *enemies)
        catalog.add(e.id, e);
    const std::uint32_t wolf = catalog.index_of("wolf"_id); // resolve once at load...
    const EnemyDef &def = catalog.at(wolf);                 // ...index per tick
    std::printf("wolf: index %u, hp %d, speed %.1f, tint #%02x%02x%02x\n", wolf, def.hp, def.speed,
                def.tint.r, def.tint.g, def.tint.b);

    auto typo = load_enemies(
        R"({"enemies": [{"id": "bat", "hp": 3, "tint": "#000000", "sped": 4}]})", "bats.json");
    std::printf("a typo is reported: %s\n", typo ? "no" : typo.error().message.c_str());

    DataWriter w;
    w.begin_object()
        .key("enemy")
        .value("goblin"_id)
        .key("pos")
        .value(Vec2{12.5f, 4})
        .key("hp")
        .value(9);
    w.end_object();
    auto json = w.finish();
    std::printf("written: %s\n", json ? json->c_str() : json.error().message.c_str());
}

void settings() {
    std::printf("-- settings\n");
    const std::string path =
        (std::filesystem::temp_directory_path() / "lam_core_demo_settings.json").string();
    (void)write_file_atomic(path, R"({"audio.music": 3.0, "video.scale": 2, "mod.extra": "kept"})");
    SettingsStore s;
    s.define_float("audio.music", 0.8f, 0.0f, 1.0f); // 3.0 is out of range: the default stays
    s.define_int("video.scale", 1, 1, 4);
    if (Status st = s.load(path); !st) std::printf("load failed: %s\n", st.error().message.c_str());
    std::printf("music %.2f, scale %d\n", s.get_float("audio.music"), s.get_int("video.scale"));
    s.set_int("video.scale", 9); // clamped to 4
    std::printf("scale after set_int(9): %d (revision %u)\n", s.get_int("video.scale"),
                s.revision());
    if (Status st = s.save(path); st) {
        auto text = read_file(path);
        std::printf("saved:\n%s\n", text ? text->c_str() : "?");
    }
    (void)remove_file(path);
}

void hud_clock_events() {
    std::printf("-- hud, clock and events\n");
    struct Damage {
        int amount;
    };
    EventBus bus;
    Events<Damage> &damage = bus.add<Damage>(16);
    FixedClock clock(60);
    FixedString<32> hud;
    int hp = 100;
    std::uint32_t read_from = bus.sequence();
    const double frame_times[] = {1.0 / 60, 1.0 / 30, 0.004, 0.5}; // the last one is a hitch
    for (double dt : frame_times) {
        const int ticks = clock.advance(dt);
        for (int t = 0; t < ticks; ++t)
            damage.emit({3});
        const std::uint32_t read_to = bus.sequence();
        for (const Damage &d : damage.window(read_from, read_to))
            hp -= d.amount;
        read_from = read_to;
        bus.trim(read_from);
        format_to(hud, "HP {} | ticks {} | a={:.2f}", hp, ticks, clock.alpha()); // no allocation
        std::printf("%s\n", hud.c_str());
    }
}

} // namespace

int main() {
    LAM_LOG_INFO("core_demo starting");
    data_and_catalog();
    settings();
    hud_clock_events();
    return 0;
}
