// JSON tables -> Catalog. The DataReader pattern: typed reads with ranges, sticky errors, and
// DataDocument::finish() (inside parse_data) rejecting any key nobody read.
#include <sim/data.h>

#include <lam/core/app.h>
#include <lam/core/data.h>
#include <lam/ecs/prefab.h>
#include <string>

namespace rl {
namespace {
ItemEffect effect_from(lam::DataReader &row, std::string_view name) {
    if (name == "heal") return ItemEffect::Heal;
    if (name == "lightning") return ItemEffect::Lightning;
    if (name == "mapping") return ItemEffect::Mapping;
    if (name == "teleport") return ItemEffect::Teleport;
    row.fail("unknown effect (heal, lightning, mapping, teleport)");
    return ItemEffect::Heal;
}
} // namespace

lam::Result<lam::Catalog<ItemDef>> parse_items(std::string_view json, std::string_view file_name) {
    return lam::parse_data<lam::Catalog<ItemDef>>(json, file_name, [](lam::DataReader &root) {
        lam::Catalog<ItemDef> out;
        lam::DataArray rows = root.array("items");
        for (std::size_t i = 0; i < rows.size(); ++i) {
            lam::DataReader row = rows.object(i);
            const lam::StrId id = row.id("id"); // interned: saves write the name
            ItemDef def;
            def.prefab = id;
            def.name = row.string("name");
            def.effect = effect_from(row, row.string("effect"));
            def.power = row.integer("power", 0, 999);
            def.weight = row.integer_or("weight", 1, 1, 100);
            def.min_depth = row.integer_or("min_depth", 1, 1, 99);
            if (id && out.index_of(id) == lam::Catalog<ItemDef>::kNone)
                out.add(id, def);
            else
                row.fail("duplicate or empty item id");
        }
        return out;
    });
}

lam::Result<lam::Catalog<MonsterDef>> parse_monsters(std::string_view json,
                                                     std::string_view file_name) {
    return lam::parse_data<lam::Catalog<MonsterDef>>(json, file_name, [](lam::DataReader &root) {
        lam::Catalog<MonsterDef> out;
        lam::DataArray rows = root.array("monsters");
        for (std::size_t i = 0; i < rows.size(); ++i) {
            lam::DataReader row = rows.object(i);
            MonsterDef def;
            def.prefab = row.id("prefab");
            def.weight = row.integer_or("weight", 1, 1, 100);
            def.min_depth = row.integer_or("min_depth", 1, 1, 99);
            def.max_depth = row.integer_or("max_depth", 99, 1, 99);
            if (def.prefab && out.index_of(def.prefab) == lam::Catalog<MonsterDef>::kNone)
                out.add(def.prefab, def);
            else
                row.fail("duplicate or empty monster prefab");
        }
        return out;
    });
}

lam::Status load_game_data(lam::App &app) {
    lam::AssetResolver *assets = app.asset_resolver();
    if (assets == nullptr)
        return lam::make_error(lam::ErrorCode::Invalid, "load_game_data: no AssetsPlugin");
    LAM_TRY(app.res<lam::PrefabLibrary>().load("prefabs.json", app.serialize_context()));
    GameData data;
    {
        LAM_TRY_ASSIGN(std::string text, assets->read_text("data/items.json"));
        LAM_TRY_ASSIGN(data.items, parse_items(text, "data/items.json"));
    }
    {
        LAM_TRY_ASSIGN(std::string text, assets->read_text("data/monsters.json"));
        LAM_TRY_ASSIGN(data.monsters, parse_monsters(text, "data/monsters.json"));
    }
    if (GameData *old = app.try_res<GameData>())
        *old = std::move(data);
    else
        app.insert_resource<GameData>(std::move(data));
    return {};
}
} // namespace rl
