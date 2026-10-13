#pragma once
// Item and monster tables loaded from JSON into lam::Catalog, plus the prefab file. Read-only after
// load.
#include <cstdint>
#include <lam/core/catalog.h>
#include <lam/core/fixed_string.h>
#include <lam/core/id.h>
#include <lam/core/result.h>
#include <string_view>

namespace lam {
class App;
}

namespace rl {
enum class ItemEffect : std::uint8_t { Heal, Lightning, Mapping, Teleport };
struct ItemDef {       // one row of assets/data/items.json
    lam::StrId prefab; // spawned on the floor (prefab name = item id)
    lam::FixedString<32> name;
    ItemEffect effect = ItemEffect::Heal;
    int power = 0;  // hp healed / damage dealt
    int weight = 1; // spawn weight
    int min_depth = 1;
};
struct MonsterDef { // one row of assets/data/monsters.json
    lam::StrId prefab;
    int weight = 1;
    int min_depth = 1, max_depth = 99;
};
struct GameData {                      // App resource
    lam::Catalog<ItemDef> items;       // keyed by item id ("potion_heal")
    lam::Catalog<MonsterDef> monsters; // keyed by prefab name ("goblin")
};

// Strict parsers (unknown keys and bad values are errors that name the file and JSON path).
lam::Result<lam::Catalog<ItemDef>> parse_items(std::string_view json, std::string_view file_name);
lam::Result<lam::Catalog<MonsterDef>> parse_monsters(std::string_view json,
                                                     std::string_view file_name);

// Loads prefabs.json into the PrefabLibrary and data/*.json into a new GameData App resource.
// Reads through the App's AssetResolver (asset-root relative, no chdir). Startup only.
lam::Status load_game_data(lam::App &app);
} // namespace rl
