// Prefab library. Loading compiles each prefab into constructed component values; spawning
// copies them into the world or into Commands, patching prefab-local entity references.
#include <lam/ecs/prefab.h>

#include <cstring>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include <lam/core/assert.h>
#include <lam/core/data.h>
#include <lam/core/fs.h>
#include <lam/ecs/commands.h>
#include <lam/ecs/transform.h>
#include <lam/ecs/world.h>

namespace lam {

namespace {

// One compiled component value (null data for tags).
struct Value {
    const ComponentInfo *info = nullptr;
    bool trivial = true;
    std::byte *data = nullptr;
    bool local_refs = false; // holds prefab-local Entity references

    Value() = default;
    Value(const Value &) = delete;
    Value &operator=(const Value &) = delete;
    Value(Value &&o) noexcept
        : info(o.info), trivial(o.trivial), data(o.data), local_refs(o.local_refs) {
        o.data = nullptr;
    }
    Value &operator=(Value &&o) noexcept {
        if (this != &o) {
            release();
            info = o.info, trivial = o.trivial, data = o.data, local_refs = o.local_refs;
            o.data = nullptr;
        }
        return *this;
    }
    ~Value() { release(); }
    void release() {
        if (!data) return;
        info->destroy(data);
        ::operator delete(data, std::align_val_t(info->align < 16 ? 16 : info->align));
        data = nullptr;
    }
    // Default-constructs a value of `ci` (false if the type cannot be default-constructed).
    bool create(const ComponentInfo &ci, bool is_trivial) {
        info = &ci;
        trivial = is_trivial;
        if (ci.size == 0) return true;
        if (!ci.construct) return false;
        data = static_cast<std::byte *>(
            ::operator new(ci.size, std::align_val_t(ci.align < 16 ? 16 : ci.align)));
        ci.construct(data);
        return true;
    }
    bool copy_from(const Value &o) {
        if (!create(*o.info, o.trivial)) return false;
        local_refs = o.local_refs;
        if (!data) return true;
        if (trivial)
            std::memcpy(data, o.data, info->size);
        else if (info->copy)
            info->copy(data, o.data);
        else
            return false;
        return true;
    }
};

struct ChildSpec {
    StrId prefab;
    Vec2 offset;
};

struct Prefab {
    StrId name;
    Transform transform;
    std::vector<Value> components; // declaration order (Transform separate)
    std::vector<ChildSpec> children;
};

// Visits every Entity field of a reflected value (nested structs and arrays included).
template <class F>
void for_each_entity(const TypeLayout &layout, std::byte *base, F &&f) {
    for (const FieldInfo &fi : layout.fields) {
        std::byte *p = base + fi.offset;
        const FieldType t = fi.type == FieldType::Array ? fi.element : fi.type;
        const std::size_t n = fi.type == FieldType::Array ? fi.count : 1;
        const std::uint32_t stride =
            t == FieldType::FixedString ? detail::fixed_string_sizeof(fi.size) : fi.size;
        for (std::size_t i = 0; i < n; ++i) {
            if (t == FieldType::Entity) {
                Entity e;
                std::memcpy(&e, p + i * stride, sizeof e);
                if (f(e)) std::memcpy(p + i * stride, &e, sizeof e);
            } else if (t == FieldType::Struct) {
                for_each_entity(*fi.nested, p + i * stride, f);
            }
        }
    }
}

bool has_local_refs(const Value &v) {
    if (!v.data || !v.info->layout) return false;
    bool found = false;
    for_each_entity(*v.info->layout, v.data, [&](Entity &e) {
        if (e.generation == detail::kLocalEntityGeneration) found = true;
        return false;
    });
    return found;
}

void patch_local_refs(const ComponentInfo &info, std::byte *data, std::span<const Entity> locals) {
    if (!data || !info.layout) return;
    for_each_entity(*info.layout, data, [&](Entity &e) {
        if (e.generation != detail::kLocalEntityGeneration) return false;
        LAM_ASSERT(e.index < locals.size(), "prefab: local entity reference out of range");
        e = e.index < locals.size() ? locals[e.index] : Entity{};
        return true;
    });
}

Transform from_2d(const Transform2D &t) {
    return {t.position, t.rotation, t.scale};
}

} // namespace

struct PrefabLibrary::Impl {
    std::vector<std::unique_ptr<Prefab>> prefabs; // load order
    std::vector<StrId> names;
    std::unordered_map<StrId, std::size_t> index;
    mutable std::vector<Entity> locals;   // spawn recursion stack
    mutable std::byte *scratch = nullptr; // patched copies (immediate spawn)
    mutable std::size_t scratch_bytes = 0;

    ~Impl() {
        if (scratch) ::operator delete(scratch, std::align_val_t(16));
    }
    const Prefab *find(StrId name) const {
        const auto it = index.find(name);
        return it == index.end() ? nullptr : prefabs[it->second].get();
    }
    std::byte *scratch_for(const ComponentInfo &info) const {
        LAM_CHECK(info.align <= 16, "prefab: component alignment above 16 bytes");
        if (scratch_bytes < info.size) {
            if (scratch) ::operator delete(scratch, std::align_val_t(16));
            scratch_bytes = info.size < 256 ? 256 : info.size;
            scratch = static_cast<std::byte *>(::operator new(scratch_bytes, std::align_val_t(16)));
        }
        return scratch;
    }

    // Spawn targets: the World now, or Commands for apply().
    struct WorldSink {
        const Impl &impl;
        World &w;
        Entity create() { return w.create(); }
        void add_transform(Entity e, const Transform &t) { w.add<Transform>(e, t); }
        void add_parent(Entity e, const Parent &p) { w.add<Parent>(e, p); }
        void mark_spawned(Entity e) { w.add<JustSpawned>(e); }
        void add(Entity e, const Value &v, std::span<const Entity> refs) {
            if (!v.data) {
                add_to_world(w, e, v.info->id, detail::Pool::tag_dummy(), false);
                return;
            }
            if (!v.local_refs) {
                add_to_world(w, e, v.info->id, v.data, false);
                return;
            }
            std::byte *tmp = impl.scratch_for(*v.info);
            if (v.trivial) {
                std::memcpy(tmp, v.data, v.info->size);
            } else {
                v.info->construct(tmp);
                v.info->copy(tmp, v.data);
            }
            patch_local_refs(*v.info, tmp, refs);
            add_to_world(w, e, v.info->id, tmp, true);
            if (!v.trivial) v.info->destroy(tmp);
        }
    };
    struct CommandsSink {
        Commands &c;
        Entity create() { return c.spawn(); }
        void add_transform(Entity e, const Transform &t) { c.add<Transform>(e, t); }
        void add_parent(Entity e, const Parent &p) { c.add<Parent>(e, p); }
        void mark_spawned(Entity) {} // Commands::apply marks spawns
        void add(Entity e, const Value &v, std::span<const Entity> refs) {
            void *p = add_to_commands(c, e, v.info->id);
            if (!p || !v.data) return;
            if (v.trivial) {
                std::memcpy(p, v.data, v.info->size);
            } else {
                v.info->construct(p);
                v.info->copy(p, v.data);
            }
            if (v.local_refs) patch_local_refs(*v.info, static_cast<std::byte *>(p), refs);
        }
    };

    template <class Sink>
    void instantiate(Sink &sink, const Prefab &p, Entity self, const Entity *parent,
                     const Transform &parent_world, Vec2 offset, bool mark) const {
        const std::size_t base = locals.size();
        locals.push_back(self);
        for (std::size_t k = 0; k < p.children.size(); ++k)
            locals.push_back(sink.create());
        Transform local = p.transform;
        local.position += offset;
        const Transform world =
            parent ? from_2d(combine(to_transform2d(parent_world), to_transform2d(local))) : local;
        sink.add_transform(self, world);
        for (const Value &v : p.components)
            sink.add(self, v, std::span<const Entity>(locals.data() + base, 1 + p.children.size()));
        if (mark) sink.mark_spawned(self);
        if (parent) {
            Parent pc;
            pc.entity = *parent;
            pc.local = local;
            sink.add_parent(self, pc);
        }
        for (std::size_t k = 0; k < p.children.size(); ++k) {
            const Prefab *child = find(p.children[k].prefab);
            LAM_ASSERT(child != nullptr, "prefab: unknown child prefab");
            if (!child) continue;
            const Entity ce = locals[base + 1 + k];
            instantiate(sink, *child, ce, &self, world, p.children[k].offset, true);
        }
        locals.resize(base);
    }
};

void *PrefabLibrary::add_to_world(World &w, Entity e, ComponentId id, void *src, bool move) {
    return w.add_from(e, id, src, move);
}

void *PrefabLibrary::add_to_commands(Commands &c, Entity e, ComponentId id) {
    return c.push_add(e, id);
}

PrefabLibrary::PrefabLibrary() : impl_(std::make_unique<Impl>()) {}
PrefabLibrary::~PrefabLibrary() = default;

bool PrefabLibrary::has(StrId name) const {
    return impl_->find(name) != nullptr;
}
std::span<const StrId> PrefabLibrary::names() const {
    return impl_->names;
}

void PrefabLibrary::clear() {
    impl_->prefabs.clear();
    impl_->names.clear();
    impl_->index.clear();
}

Entity PrefabLibrary::spawn(World &w, StrId name, Vec2 position) const {
    const Prefab *p = impl_->find(name);
    LAM_ASSERT(p != nullptr, "PrefabLibrary::spawn: unknown prefab");
    if (!p) return {};
    const Entity root = w.create();
    Impl::WorldSink sink{*impl_, w};
    impl_->instantiate(sink, *p, root, nullptr, Transform{}, position, true);
    return root;
}

void PrefabLibrary::spawn_into(Commands &c, Entity e, StrId name, Vec2 position) const {
    const Prefab *p = impl_->find(name);
    LAM_ASSERT(p != nullptr, "PrefabLibrary::spawn_into: unknown prefab");
    if (!p) return;
    Impl::CommandsSink sink{c};
    impl_->instantiate(sink, *p, e, nullptr, Transform{}, position, false);
}

Status PrefabLibrary::load(std::string_view path, const SerializeContext &ctx) {
    Result<std::string> text =
        ctx.assets ? ctx.assets->read_text(path) : read_file(std::string(path));
    if (!text) return text.error();
    return load_text(*text, path, ctx);
}

namespace {

struct Pending {
    StrId name;
    DataReader in;
    int state = 0; // 0 new, 1 compiling, 2 done
    std::unique_ptr<Prefab> result;
};

struct Compiler {
    std::vector<Pending> &pending;
    const std::unordered_map<StrId, std::size_t> &existing_index;
    const std::vector<std::unique_ptr<Prefab>> &existing;
    const SerializeContext &ctx;
    ComponentId transform_id;

    Pending *find_pending(StrId name) {
        for (Pending &p : pending)
            if (p.name == name) return &p;
        return nullptr;
    }
    const Prefab *lookup(StrId name) {
        if (Pending *p = find_pending(name)) return p->result.get();
        const auto it = existing_index.find(name);
        return it == existing_index.end() ? nullptr : existing[it->second].get();
    }
    Value *ensure_value(Prefab &pf, const ComponentInfo &info, DataReader &where) {
        for (Value &v : pf.components)
            if (v.info == &info) return &v;
        Value v;
        if (!v.create(info, ComponentRegistry::global().trivially_copyable(info.id))) {
            where.fail("component \"" + std::string(info.name) + "\" is not default-constructible");
            return nullptr;
        }
        pf.components.push_back(std::move(v));
        return &pf.components.back();
    }

    void compile(Pending &p) {
        if (p.state == 2) return;
        DataReader &in = p.in;
        if (p.state == 1) {
            in.fail("\"extends\" forms a cycle");
            return;
        }
        p.state = 1;
        auto pf = std::make_unique<Prefab>();
        pf->name = p.name;
        if (in.has("extends")) {
            const StrId base_name = in.id("extends");
            const Prefab *base = nullptr;
            if (Pending *bp = find_pending(base_name)) {
                compile(*bp);
                base = bp->result.get();
            } else {
                base = lookup(base_name);
            }
            if (!base) {
                in.fail("\"extends\": unknown prefab \"" + id_display(base_name) + "\"");
            } else {
                pf->transform = base->transform;
                pf->children = base->children;
                for (const Value &bv : base->components) {
                    Value v;
                    if (!v.copy_from(bv)) {
                        in.fail("component \"" + std::string(bv.info->name) +
                                "\" cannot be copied");
                        break;
                    }
                    pf->components.push_back(std::move(v));
                }
            }
        }
        if (in.has("children")) {
            DataArray arr = in.array("children");
            for (std::size_t k = 0; k < arr.size(); ++k) {
                DataReader c = arr.object(k);
                if (!c.valid()) break;
                ChildSpec cs;
                cs.prefab = c.id("prefab");
                if (c.has("offset")) cs.offset = c.vec2("offset");
                pf->children.push_back(cs);
            }
        }
        std::vector<Entity> remap(1 + pf->children.size());
        for (std::size_t k = 0; k < remap.size(); ++k)
            remap[k] = Entity{static_cast<std::uint32_t>(k), detail::kLocalEntityGeneration};
        SerializeContext local_ctx = ctx;
        local_ctx.entity_remap = remap;
        const ComponentRegistry &reg = ComponentRegistry::global();
        if (in.has("tags")) {
            DataArray arr = in.array("tags");
            for (std::size_t k = 0; k < arr.size(); ++k) {
                const std::string tag = arr.string(k);
                if (tag.empty()) break;
                const ComponentInfo *info = reg.find(tag);
                if (!info) {
                    arr.fail("unknown tag component \"" + tag + "\"");
                    break;
                }
                if (info->id == transform_id) continue;
                if (!ensure_value(*pf, *info, in)) break;
            }
        }
        if (in.has("components")) {
            DataReader comps = in.object("components");
            for (const std::string_view key : comps.keys()) {
                const ComponentInfo *info = reg.find(key);
                if (!info) {
                    comps.fail("unknown component \"" + std::string(key) + "\"");
                    break;
                }
                if (!(info->flags & kSerialize) || !info->read) {
                    comps.fail("component \"" + std::string(key) +
                               "\" is not serializable (needs kSerialize)");
                    break;
                }
                DataReader cin = comps.object(key);
                if (!cin.valid()) break;
                if (info->id == transform_id) {
                    (void)info->read(&pf->transform, cin, local_ctx);
                    continue;
                }
                Value *v = ensure_value(*pf, *info, comps);
                if (!v) break;
                (void)info->read(v->data ? static_cast<void *>(v->data) : detail::Pool::tag_dummy(),
                                 cin, local_ctx);
            }
        }
        for (Value &v : pf->components)
            v.local_refs = has_local_refs(v);
        p.result = std::move(pf);
        p.state = 2;
    }
};

} // namespace

Status PrefabLibrary::load_text(std::string_view json_text, std::string_view file_name,
                                const SerializeContext &ctx) {
    Result<DataDocument> parsed = DataDocument::parse(json_text, file_name);
    if (!parsed) return parsed.error();
    DataDocument doc = std::move(parsed.value());
    DataReader root = doc.root();
    DataReader list = root.object("prefabs");
    std::vector<Pending> pending;
    if (list.valid()) {
        const std::vector<std::string_view> keys = list.keys();
        pending.resize(keys.size());
        for (std::size_t i = 0; i < keys.size(); ++i) {
            pending[i].name = intern(keys[i]);
            pending[i].in = list.object(keys[i]);
        }
    }
    Compiler compiler{pending, impl_->index, impl_->prefabs, ctx,
                      ComponentRegistry::global().id<Transform>()};
    for (Pending &p : pending) {
        if (doc.failed()) break;
        if (p.in.valid()) compiler.compile(p);
    }
    LAM_TRY(doc.finish());
    // Every child reference must resolve, and the child graph must be acyclic.
    for (const Pending &p : pending) {
        for (const ChildSpec &c : p.result->children) {
            if (!compiler.lookup(c.prefab))
                return make_error(ErrorCode::Invalid,
                                  std::string(file_name) + ": prefab \"" + id_display(p.name) +
                                      "\": unknown child prefab \"" + id_display(c.prefab) + "\"");
        }
    }
    for (const Pending &p : pending) {
        // Depth-first walk with an explicit stack; a path longer than the prefab count is a cycle.
        const std::size_t limit = pending.size() + impl_->prefabs.size() + 1;
        std::vector<std::pair<const Prefab *, std::size_t>> stack{{p.result.get(), 0}};
        while (!stack.empty()) {
            if (stack.size() > limit)
                return make_error(ErrorCode::Invalid, std::string(file_name) + ": prefab \"" +
                                                          id_display(p.name) +
                                                          "\": children form a cycle");
            auto &[pf, next] = stack.back();
            if (next == pf->children.size()) {
                stack.pop_back();
                continue;
            }
            const Prefab *child = compiler.lookup(pf->children[next++].prefab);
            if (child == p.result.get())
                return make_error(ErrorCode::Invalid, std::string(file_name) + ": prefab \"" +
                                                          id_display(p.name) +
                                                          "\": children form a cycle");
            stack.push_back({child, 0});
        }
    }
    // Commit (replacing same-named prefabs in place).
    for (Pending &p : pending) {
        const auto it = impl_->index.find(p.name);
        if (it != impl_->index.end()) {
            impl_->prefabs[it->second] = std::move(p.result);
        } else {
            impl_->index.emplace(p.name, impl_->prefabs.size());
            impl_->prefabs.push_back(std::move(p.result));
            impl_->names.push_back(p.name);
        }
    }
    return {};
}

} // namespace lam
