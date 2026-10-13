// World serialisation with the exact ECS layout. Loading builds and validates a WorldSnapshot
// before calling World::restore(), so a failed load leaves the world alone. Cold path.
#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <lam/ecs/snapshot.h>
#include <lam/scene/save.h>

namespace lam {
namespace {

constexpr std::uint64_t kMaxU32 = 0xFFFFFFFFull;

Error world_error(std::string message) {
    return make_error(ErrorCode::Invalid, "world save: " + std::move(message));
}

// Every key of `obj` must be a field of `layout` (recursing into nested structs), so a renamed
// or removed field is caught before anything is restored.
Status check_keys(const TypeLayout &layout, DataReader &obj, std::string_view where) {
    for (const std::string_view key : obj.keys()) {
        const FieldInfo *field = nullptr;
        for (const FieldInfo &f : layout.fields)
            if (f.name == key) field = &f;
        if (!field)
            return world_error(std::string(where) + ": unknown field \"" + std::string(key) + "\"");
        if (field->type == FieldType::Struct && field->nested &&
            obj.type(key) == DataType::Object) {
            DataReader nested = obj.object(key);
            LAM_TRY(check_keys(*field->nested, nested, where));
            nested.ignore_unknown();
        } else if (field->type == FieldType::Array && field->element == FieldType::Struct &&
                   field->nested && obj.type(key) == DataType::Array) {
            DataArray arr = obj.array(key);
            for (std::size_t i = 0; i < arr.size(); ++i) {
                if (arr.type(i) != DataType::Object) continue; // read_fields reports it
                DataReader nested = arr.object(i);
                LAM_TRY(check_keys(*field->nested, nested, where));
                nested.ignore_unknown();
            }
        }
    }
    return {};
}

// Reads [index, generation].
bool read_entity(DataArray &list, std::size_t i, Entity &out) {
    if (list.type(i) != DataType::Array) return false;
    DataArray pair = list.array(i);
    if (pair.size() != 2) return false;
    out.index = static_cast<std::uint32_t>(pair.integer64(0, 0, static_cast<std::int64_t>(kMaxU32)));
    out.generation =
        static_cast<std::uint32_t>(pair.integer64(1, 0, static_cast<std::int64_t>(kMaxU32)));
    return true;
}

struct PoolIn {
    ComponentId id = 0;
    const ComponentInfo *info = nullptr;
    std::vector<Entity> entities;
    DataArray values;
    bool has_values = false;
};

Status read_value(const ComponentInfo &info, void *dst, DataReader &obj, const SerializeContext &ctx,
                  std::string_view where) {
    if (!info.read) return world_error(std::string(where) + ": the type cannot be read");
    if (info.layout) LAM_TRY(check_keys(*info.layout, obj, where));
    Status s = info.read(dst, obj, ctx);
    if (!s) return world_error(std::string(where) + ": " + s.error().message);
    if (obj.failed()) return world_error(std::string(where) + ": malformed value");
    return {};
}

// JSON (serialize_world's object) -> snapshot. Checks shape, names, counts and flags; the entity
// table and pool membership are checked afterwards by WorldSnapshot::validate().
Status build_snapshot(const World &w, DataReader &in, const SerializeContext &ctx,
                      WorldSnapshot &snap) {
    if (!in.valid()) return world_error("the world data is not an object");
    const ComponentRegistry &reg = ComponentRegistry::global();
    if (in.type("layout") != DataType::Number) return world_error("missing \"layout\"");
    const int layout = in.integer("layout", 0, 1 << 20);
    if (in.failed()) return world_error("\"layout\" is not an integer");
    if (layout != kWorldLayoutVersion)
        return make_error(ErrorCode::Unsupported,
                          "world save: layout " + std::to_string(layout) + " (this engine reads " +
                              std::to_string(kWorldLayoutVersion) + ")");
    const std::uint64_t tick = in.u64("tick");
    const std::uint64_t seed = in.u64("seed");
    if (in.failed()) return world_error("\"tick\" and \"seed\" must be non-negative integers");
    snap.reset(tick, seed);

    // Entity table.
    if (in.type("entities") != DataType::Object) return world_error("missing \"entities\" object");
    DataReader ents = in.object("entities");
    DataArray gens = ents.array("generations");
    DataArray states = ents.array("states");
    DataArray free = ents.array("free");
    if (ents.failed()) return world_error("\"entities\" needs generations, states and free arrays");
    if (gens.size() != states.size())
        return world_error("entity generations and states differ in size");
    std::vector<std::uint32_t> generations(gens.size());
    std::vector<EntitySlotState> slot_states(states.size());
    std::vector<std::uint32_t> free_list(free.size());
    for (std::size_t i = 0; i < gens.size(); ++i) {
        const std::uint64_t g = gens.u64(i);
        if (g > kMaxU32) return world_error("entity generation out of range");
        generations[i] = static_cast<std::uint32_t>(g);
        slot_states[i] = static_cast<EntitySlotState>(states.integer(i, 0, 2));
    }
    for (std::size_t i = 0; i < free.size(); ++i)
        free_list[i] =
            static_cast<std::uint32_t>(free.integer64(i, 0, static_cast<std::int64_t>(kMaxU32)));
    if (ents.failed() || in.failed()) return world_error("malformed entity table");
    snap.set_entities(generations, slot_states, free_list);

    // RNG streams.
    DataArray rng = in.array("rng");
    if (in.failed()) return world_error("missing \"rng\" array");
    if (rng.size() > World::kMaxRngStreams) return world_error("more than 32 RNG streams");
    std::vector<RngStreamState> streams(rng.size());
    for (std::size_t i = 0; i < rng.size(); ++i) {
        if (rng.type(i) != DataType::Object) return world_error("an RNG stream is not an object");
        DataReader r = rng.object(i);
        streams[i].stream = r.id("stream");
        streams[i].state = r.u64("state");
        if (r.failed()) return world_error("malformed RNG stream");
        for (std::size_t j = 0; j < i; ++j)
            if (streams[j].stream == streams[i].stream) return world_error("repeated RNG stream");
    }
    snap.set_rng_streams(streams);

    // Pools: parsed by name, added in ascending ComponentId order.
    DataArray pools = in.array("pools");
    if (in.failed()) return world_error("missing \"pools\" array");
    std::vector<PoolIn> in_pools(pools.size());
    for (std::size_t p = 0; p < pools.size(); ++p) {
        if (pools.type(p) != DataType::Object) return world_error("a pool is not an object");
        DataReader obj = pools.object(p);
        const std::string name = obj.string("component");
        if (obj.failed()) return world_error("a pool has no \"component\" name");
        const ComponentInfo *info = reg.find(name);
        if (!info) return world_error("unknown component \"" + name + "\"");
        if (!(info->flags & kSerialize))
            return world_error("component \"" + name + "\" is not serialisable (kSerialize)");
        for (std::size_t q = 0; q < p; ++q)
            if (in_pools[q].id == info->id) return world_error("component \"" + name + "\" repeated");
        PoolIn &pin = in_pools[p];
        pin.id = info->id;
        pin.info = info;
        DataArray list = obj.array("entities");
        if (obj.failed()) return world_error(name + ": missing \"entities\" array");
        pin.entities.resize(list.size());
        for (std::size_t k = 0; k < list.size(); ++k)
            if (!read_entity(list, k, pin.entities[k]))
                return world_error(name + ": an entity must be [index, generation]");
        if (obj.failed()) return world_error(name + ": malformed entity");
        const bool tag = info->size == 0;
        if (obj.has("values")) {
            if (tag) return world_error(name + ": a tag has no values");
            pin.values = obj.array("values");
            pin.has_values = true;
            if (obj.failed()) return world_error(name + ": \"values\" must be an array");
            if (pin.values.size() != pin.entities.size())
                return world_error(name + ": " + std::to_string(pin.values.size()) + " values for " +
                                   std::to_string(pin.entities.size()) + " entities");
        } else if (!tag && !pin.entities.empty()) {
            return world_error(name + ": missing \"values\"");
        }
    }
    std::sort(in_pools.begin(), in_pools.end(),
              [](const PoolIn &a, const PoolIn &b) { return a.id < b.id; });
    for (PoolIn &pin : in_pools) {
        auto *dst = static_cast<std::byte *>(snap.add_pool(pin.id, pin.entities));
        if (!pin.has_values || !dst) continue;
        for (std::size_t k = 0; k < pin.entities.size(); ++k) {
            if (pin.values.type(k) != DataType::Object)
                return world_error(std::string(pin.info->name) + ": a value is not an object");
            DataReader v = pin.values.object(k);
            LAM_TRY(read_value(*pin.info, dst + k * pin.info->size, v, ctx, pin.info->name));
        }
    }

    // Snapshot resources: only types this world holds.
    DataArray res = in.array("resources");
    if (in.failed()) return world_error("missing \"resources\" array");
    std::vector<ComponentId> seen;
    for (std::size_t i = 0; i < res.size(); ++i) {
        if (res.type(i) != DataType::Object) return world_error("a resource is not an object");
        DataReader obj = res.object(i);
        const std::string name = obj.string("resource");
        if (obj.failed()) return world_error("a resource has no \"resource\" name");
        const ComponentInfo *info = reg.find(name);
        if (!info) return world_error("unknown resource \"" + name + "\"");
        if (!(info->flags & kSerialize))
            return world_error("resource \"" + name + "\" is not serialisable (kSerialize)");
        if (w.resources().get_raw(info->type) == nullptr)
            return world_error("resource \"" + name + "\" is not in this world");
        if (std::find(seen.begin(), seen.end(), info->id) != seen.end())
            return world_error("resource \"" + name + "\" repeated");
        seen.push_back(info->id);
        if (obj.type("value") != DataType::Object)
            return world_error(name + ": \"value\" must be an object");
        DataReader v = obj.object("value");
        void *dst = snap.add_resource(info->id, kSerialize | kSnapshot);
        LAM_TRY(read_value(*info, dst, v, ctx, info->name));
    }
    if (in.failed()) return world_error("malformed world data");
    return {};
}

struct Named {
    std::string_view name;
    std::size_t index = 0;
};

// Commit, once every check passed. Clear first so runtime-only components of replaced entities
// can't survive on a handle that matches a loaded one.
void commit(World &w, const WorldSnapshot &snap) {
    w.clear();
    w.restore(snap);
}

} // namespace

Status serialize_world(const World &w, DataWriter &out, const SerializeContext &ctx,
                       const WorldSaveOptions &o) {
    (void)o; // pretty printing is decided by the caller's DataWriter::finish
    const ComponentRegistry &reg = ComponentRegistry::global();
    WorldSnapshot snap;
    w.snapshot_into(snap);

    std::vector<Named> pools;
    for (std::size_t i = 0; i < snap.pool_count(); ++i) {
        const ComponentInfo &info = reg.at(snap.pool_component(i));
        if (!(info.flags & kSerialize) || snap.pool_entities(i).empty()) continue;
        if (info.size != 0 && !info.write)
            return world_error(std::string(info.name) + ": kSerialize without a writer");
        // A load finds the type by its short name (ComponentRegistry::find): another reflected
        // type of that name would be read in its place, so refuse the save now.
        if (reg.find(info.name) != &info)
            return world_error("component \"" + std::string(info.name) +
                               "\" shares its name with another reflected type; the save could not be loaded");
        pools.push_back({info.name, i});
    }
    std::vector<Named> resources;
    for (std::size_t i = 0; i < snap.resource_count(); ++i) {
        if (!(snap.resource_flags(i) & kSerialize)) continue;
        const ComponentInfo &info = reg.at(snap.resource_component(i));
        if (!info.write) return world_error(std::string(info.name) + ": resource without a writer");
        if (reg.find(info.name) != &info)
            return world_error("resource \"" + std::string(info.name) +
                               "\" shares its name with another reflected type; the save could not be loaded");
        resources.push_back({info.name, i});
    }
    auto by_name = [](const Named &a, const Named &b) { return a.name < b.name; };
    std::sort(pools.begin(), pools.end(), by_name);
    std::sort(resources.begin(), resources.end(), by_name);
    for (std::size_t i = 1; i < pools.size(); ++i)
        if (pools[i].name == pools[i - 1].name)
            return world_error("two components are named \"" + std::string(pools[i].name) + "\"");

    out.begin_object();
    out.key("layout").value(kWorldLayoutVersion);
    out.key("tick").value(snap.tick());
    out.key("seed").value(snap.seed());
    out.key("entities").begin_object();
    out.key("generations").begin_array();
    for (const std::uint32_t g : snap.generations())
        out.value(g);
    out.end_array();
    out.key("states").begin_array();
    for (const EntitySlotState s : snap.slot_states())
        out.value(static_cast<int>(s));
    out.end_array();
    out.key("free").begin_array();
    for (const std::uint32_t f : snap.free_list())
        out.value(f);
    out.end_array();
    out.end_object();
    out.key("rng").begin_array();
    for (const RngStreamState &r : snap.rng_streams())
        out.begin_object().key("stream").value(r.stream).key("state").value(r.state).end_object();
    out.end_array();
    out.key("pools").begin_array();
    for (const Named &p : pools) {
        const ComponentInfo &info = reg.at(snap.pool_component(p.index));
        const std::span<const Entity> ents = snap.pool_entities(p.index);
        out.begin_object();
        out.key("component").value(info.name);
        out.key("entities").begin_array();
        for (const Entity e : ents)
            out.begin_array().value(e.index).value(e.generation).end_array();
        out.end_array();
        if (info.size != 0) {
            const auto *data = static_cast<const std::byte *>(snap.pool_data(p.index));
            out.key("values").begin_array();
            for (std::size_t k = 0; k < ents.size(); ++k)
                info.write(data + k * info.size, out, ctx);
            out.end_array();
        }
        out.end_object();
    }
    out.end_array();
    out.key("resources").begin_array();
    for (const Named &r : resources) {
        const ComponentInfo &info = reg.at(snap.resource_component(r.index));
        out.begin_object();
        out.key("resource").value(info.name);
        out.key("value");
        info.write(snap.resource_data(r.index), out, ctx);
        out.end_object();
    }
    out.end_array();
    out.end_object();
    return {};
}

Status deserialize_world(World &w, DataReader &in, const SerializeContext &ctx) {
    WorldSnapshot snap;
    LAM_TRY(build_snapshot(w, in, ctx, snap));
    if (Status v = snap.validate(); !v) return world_error(v.error().message);
    commit(w, snap);
    return {};
}

Result<std::string> serialize_world_text(const World &w, const SerializeContext &ctx,
                                         const WorldSaveOptions &o) {
    DataWriter out;
    LAM_TRY(serialize_world(w, out, ctx, o));
    return out.finish(o.pretty);
}

Status deserialize_world_text(World &w, std::string_view json, const SerializeContext &ctx,
                              std::string_view file_name) {
    Result<DataDocument> doc = DataDocument::parse(json, file_name);
    if (!doc) return doc.error();
    DataReader root = doc->root();
    WorldSnapshot snap;
    if (Status s = build_snapshot(w, root, ctx, snap); !s) {
        // The document holds the precise sticky error (key and path) when one was recorded.
        if (doc->failed()) return world_error(doc->finish().error().message);
        return s;
    }
    if (Status fin = doc->finish(); !fin) return world_error(fin.error().message);
    if (Status v = snap.validate(); !v) return world_error(v.error().message);
    commit(w, snap);
    return {};
}

} // namespace lam
