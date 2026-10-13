// ComponentRegistry and reflected field IO. Object keys and array elements go through the same
// templated reader, so arrays of any field type behave like single fields.
#include <lam/ecs/component.h>

#include <cfloat>
#include <cstring>
#include <limits>
#include <string>

#include <lam/core/assert.h>
#include <lam/core/text.h>

namespace lam {

// ------------------------------------------------------------------------------------------------
// Registry

ComponentRegistry &ComponentRegistry::global() {
    static ComponentRegistry registry;
    return registry;
}

ComponentId ComponentRegistry::add(const ComponentInfo &info, bool trivial) {
    LAM_CHECK(entries_.size() < 0xFFFFu, "ComponentRegistry: too many component types");
    auto e = std::make_unique<Entry>();
    e->info = info;
    e->info.id = static_cast<ComponentId>(entries_.size());
    e->trivial = trivial;
    entries_.push_back(std::move(e));
    return entries_.back()->info.id;
}

const ComponentInfo *ComponentRegistry::find(std::string_view name) const {
    const ComponentInfo *fallback = nullptr;
    for (const auto &e : entries_) {
        if (e->info.name != name) continue;
        if (e->info.layout || e->info.read) return &e->info;
        if (!fallback) fallback = &e->info;
    }
    return fallback;
}

const ComponentInfo &ComponentRegistry::at(ComponentId id) const {
    LAM_CHECK(id < entries_.size(), "ComponentRegistry::at: unknown component id");
    return entries_[id]->info;
}

std::size_t ComponentRegistry::size() const {
    return entries_.size();
}

void ComponentRegistry::set_hooks(ComponentId id, ComponentHook on_add, ComponentHook on_replace,
                                  ComponentHook on_remove) {
    LAM_CHECK(id < entries_.size(), "ComponentRegistry::set_hooks: unknown component id");
    ComponentInfo &info = entries_[id]->info;
    info.on_add = on_add;
    info.on_replace = on_replace;
    info.on_remove = on_remove;
}

void ComponentRegistry::set_flags(ComponentId id, std::uint32_t flags) {
    LAM_CHECK(id < entries_.size(), "ComponentRegistry::register_type: unknown component id");
    ComponentInfo &info = entries_[id]->info;
    LAM_ASSERT((flags & (kSerialize | kHash)) == 0 || info.read != nullptr,
               "register_type: kSerialize/kHash need LAM_REFLECT or LAM_REFLECT_CUSTOM");
    LAM_ASSERT((flags & kTag) == 0 || info.size == 0, "register_type: kTag needs an empty type");
    info.flags = flags;
}

bool ComponentRegistry::trivially_copyable(ComponentId id) const {
    LAM_CHECK(id < entries_.size(), "ComponentRegistry: unknown component id");
    return entries_[id]->trivial;
}

// ------------------------------------------------------------------------------------------------
// Field IO

namespace {

constexpr float kFloatMax = std::numeric_limits<float>::max();
constexpr double kDoubleMax = std::numeric_limits<double>::max();

std::uint32_t element_stride(FieldType t, const FieldInfo &f) {
    return t == FieldType::FixedString ? detail::fixed_string_sizeof(f.size) : f.size;
}

// FixedString<N> layout: char data[N] at 0, uint16 size at round_up(N, 2) (static_assert in
// component.h keeps this in sync).
std::string_view fixed_string_view(const std::byte *p, std::uint32_t n) {
    std::uint16_t size = 0;
    std::memcpy(&size, p + ((n + 1u) & ~1u), sizeof size);
    if (size >= n) size = static_cast<std::uint16_t>(n - 1);
    return {reinterpret_cast<const char *>(p), size};
}

void fixed_string_assign(std::byte *p, std::uint32_t n, std::string_view s) {
    const std::size_t len = utf8_truncate(s, n - 1);
    std::memcpy(p, s.data(), len);
    std::memset(p + len, 0, n - len);
    const auto size = static_cast<std::uint16_t>(len);
    std::memcpy(p + ((n + 1u) & ~1u), &size, sizeof size);
}

std::uint64_t load_uint(const std::byte *p, std::uint32_t size) {
    switch (size) {
    case 1: {
        std::uint8_t v;
        std::memcpy(&v, p, 1);
        return v;
    }
    case 2: {
        std::uint16_t v;
        std::memcpy(&v, p, 2);
        return v;
    }
    case 4: {
        std::uint32_t v;
        std::memcpy(&v, p, 4);
        return v;
    }
    default: {
        std::uint64_t v;
        std::memcpy(&v, p, 8);
        return v;
    }
    }
}

void store_uint(std::byte *p, std::uint32_t size, std::uint64_t v) {
    switch (size) {
    case 1: {
        const auto x = static_cast<std::uint8_t>(v);
        std::memcpy(p, &x, 1);
        break;
    }
    case 2: {
        const auto x = static_cast<std::uint16_t>(v);
        std::memcpy(p, &x, 2);
        break;
    }
    case 4: {
        const auto x = static_cast<std::uint32_t>(v);
        std::memcpy(p, &x, 4);
        break;
    }
    default:
        std::memcpy(p, &v, 8);
        break;
    }
}

template <class V>
V load(const std::byte *p) {
    V v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

template <class V>
void store(std::byte *p, const V &v) {
    std::memcpy(p, &v, sizeof v);
}

bool parse_hex_color(std::string_view s, Color &out) {
    if ((s.size() != 7 && s.size() != 9) || s[0] != '#') return false;
    std::uint32_t v = 0;
    for (std::size_t i = 1; i < s.size(); ++i) {
        const char c = s[i];
        std::uint32_t d = 0;
        if (c >= '0' && c <= '9')
            d = static_cast<std::uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
            d = static_cast<std::uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            d = static_cast<std::uint32_t>(c - 'A' + 10);
        else
            return false;
        v = (v << 4) | d;
    }
    if (s.size() == 7) v = (v << 8) | 0xFFu;
    out = Color::hex(v);
    return true;
}

// A value slot inside a JSON object (by key).
struct KeySource {
    DataReader &in;
    std::string_view key;
    DataType type() const { return in.type(key); }
    void mark() { in.ignore(key); }
    void fail(std::string_view msg) { in.fail(std::string(key) + ": " + std::string(msg)); }
    bool failed() const { return in.failed(); }
    bool boolean() { return in.boolean(key); }
    std::int64_t integer(std::int64_t lo, std::int64_t hi) { return in.integer64(key, lo, hi); }
    std::uint64_t u64() { return in.u64(key); }
    float number() { return in.number(key, -kFloatMax, kFloatMax); }
    double number64() { return in.number64(key, -kDoubleMax, kDoubleMax); }
    Vec2 vec2() { return in.vec2(key); }
    IVec2 ivec2() { return in.ivec2(key); }
    Rect rect() { return in.rect(key); }
    IRect irect() { return in.irect(key); }
    Color color() { return in.color(key); }
    StrId id() { return in.id(key); }
    std::string text() { return in.text(key); }
    DataReader object() { return in.object(key); }
    DataArray array() { return in.array(key); }
};

// A value slot inside a JSON array (by index).
struct IndexSource {
    DataArray &in;
    std::size_t i;
    DataType type() const { return in.type(i); }
    void mark() {}
    void fail(std::string_view msg) { in.fail(std::to_string(i) + ": " + std::string(msg)); }
    bool failed() const { return false; }
    bool boolean() { return in.boolean(i); }
    std::int64_t integer(std::int64_t lo, std::int64_t hi) { return in.integer64(i, lo, hi); }
    std::uint64_t u64() { return in.u64(i); }
    float number() { return in.number(i, -kFloatMax, kFloatMax); }
    double number64() { return in.number64(i, -kDoubleMax, kDoubleMax); }
    Vec2 vec2() { return in.vec2(i); }
    IVec2 ivec2() {
        DataArray a = in.array(i);
        if (a.size() != 2) {
            fail("expected [x, y]");
            return {};
        }
        constexpr int lo = std::numeric_limits<int>::min(), hi = std::numeric_limits<int>::max();
        return {a.integer(0, lo, hi), a.integer(1, lo, hi)};
    }
    Rect rect() {
        DataArray a = in.array(i);
        if (a.size() != 4) {
            fail("expected [x, y, w, h]");
            return {};
        }
        return {a.number(0, -kFloatMax, kFloatMax), a.number(1, -kFloatMax, kFloatMax),
                a.number(2, -kFloatMax, kFloatMax), a.number(3, -kFloatMax, kFloatMax)};
    }
    IRect irect() {
        DataArray a = in.array(i);
        if (a.size() != 4) {
            fail("expected [x, y, w, h]");
            return {};
        }
        constexpr int lo = std::numeric_limits<int>::min(), hi = std::numeric_limits<int>::max();
        return {a.integer(0, lo, hi), a.integer(1, lo, hi), a.integer(2, lo, hi),
                a.integer(3, lo, hi)};
    }
    Color color() {
        if (in.type(i) == DataType::String) {
            Color c;
            if (!parse_hex_color(in.string(i), c)) fail("expected a colour \"#rrggbb[aa]\"");
            return c;
        }
        DataArray a = in.array(i);
        if (a.size() != 3 && a.size() != 4) {
            fail("expected a colour \"#rrggbb[aa]\" or [r, g, b(, a)]");
            return {};
        }
        Color c;
        c.r = static_cast<std::uint8_t>(a.integer(0, 0, 255));
        c.g = static_cast<std::uint8_t>(a.integer(1, 0, 255));
        c.b = static_cast<std::uint8_t>(a.integer(2, 0, 255));
        c.a = a.size() == 4 ? static_cast<std::uint8_t>(a.integer(3, 0, 255)) : std::uint8_t(255);
        return c;
    }
    StrId id() { return in.id(i); }
    std::string text() { return in.text(i); }   // may be empty (FixedString elements)
    DataReader object() { return in.object(i); }
    DataArray array() { return in.array(i); }
};

template <class Src>
bool read_value(FieldType t, const FieldInfo &f, std::byte *p, Src &src,
                const SerializeContext &ctx);

template <class Src>
bool read_entity(std::byte *p, Src &src, const SerializeContext &ctx) {
    const DataType dt = src.type();
    if (dt == DataType::Null) {
        src.mark();
        store(p, Entity{});
        return true;
    }
    if (dt == DataType::Number && !ctx.entity_remap.empty()) {
        const auto k = static_cast<std::size_t>(
            src.integer(0, static_cast<std::int64_t>(ctx.entity_remap.size()) - 1));
        store(p, ctx.entity_remap[k]);
        return true;
    }
    DataArray a = src.array();
    if (a.size() != 2) {
        src.fail("expected an entity [index, generation] or null");
        return false;
    }
    Entity e;
    e.index = static_cast<std::uint32_t>(a.integer64(0, 0, 0xFFFFFFFFll));
    e.generation = static_cast<std::uint32_t>(a.integer64(1, 0, 0xFFFFFFFFll));
    store(p, e);
    return true;
}

template <class Src>
bool read_asset(const FieldInfo &f, std::byte *p, Src &src, const SerializeContext &ctx) {
    const DataType dt = src.type();
    if (dt == DataType::Null) {
        src.mark();
        store<std::uint64_t>(p, 0);
        return true;
    }
    const std::string path = src.text();
    if (path.empty()) {
        store<std::uint64_t>(p, 0);
        return true;
    }
    if (!ctx.assets) {
        src.fail("asset path \"" + path + "\" needs an AssetResolver (SerializeContext::assets)");
        return false;
    }
    const std::uint64_t bits = ctx.assets->resolve(f.asset_type, path);
    if (bits == 0) {
        src.fail("no loader for asset \"" + path + "\"");
        return false;
    }
    store(p, bits);
    return true;
}

template <class Src>
bool read_value(FieldType t, const FieldInfo &f, std::byte *p, Src &src,
                const SerializeContext &ctx) {
    switch (t) {
    case FieldType::Bool:
        store(p, src.boolean());
        return true;
    case FieldType::I8:
        store(p, static_cast<std::int8_t>(src.integer(-128, 127)));
        return true;
    case FieldType::U8:
        store(p, static_cast<std::uint8_t>(src.integer(0, 255)));
        return true;
    case FieldType::I16:
        store(p, static_cast<std::int16_t>(src.integer(-32768, 32767)));
        return true;
    case FieldType::U16:
        store(p, static_cast<std::uint16_t>(src.integer(0, 65535)));
        return true;
    case FieldType::I32:
        store(p, static_cast<std::int32_t>(src.integer(std::numeric_limits<std::int32_t>::min(),
                                                       std::numeric_limits<std::int32_t>::max())));
        return true;
    case FieldType::U32:
        store(p, static_cast<std::uint32_t>(src.integer(0, 0xFFFFFFFFll)));
        return true;
    case FieldType::I64:
        store(p, src.integer(std::numeric_limits<std::int64_t>::min(),
                             std::numeric_limits<std::int64_t>::max()));
        return true;
    case FieldType::U64:
        store(p, src.u64());
        return true;
    case FieldType::F32:
        store(p, src.number());
        return true;
    case FieldType::F64:
        store(p, src.number64()); // full precision: save/load keeps state_hash
        return true;
    case FieldType::Vec2:
        store(p, src.vec2());
        return true;
    case FieldType::IVec2:
        store(p, src.ivec2());
        return true;
    case FieldType::Rect:
        store(p, src.rect());
        return true;
    case FieldType::IRect:
        store(p, src.irect());
        return true;
    case FieldType::Color:
        store(p, src.color());
        return true;
    case FieldType::Entity:
        return read_entity(p, src, ctx);
    case FieldType::StrId:
        store(p, src.id());
        return true;
    case FieldType::FixedString:
        fixed_string_assign(p, f.size, src.text());
        return true;
    case FieldType::Enum: {
        const std::string name = src.text();
        for (std::size_t i = 0; i < f.enum_names.size(); ++i) {
            if (f.enum_names[i] == name) {
                store_uint(p, f.size, i);
                return true;
            }
        }
        std::string msg = "unknown enum value \"" + name + "\" (expected one of:";
        for (std::string_view n : f.enum_names)
            msg += " " + std::string(n);
        src.fail(msg + ")");
        return false;
    }
    case FieldType::Asset:
        return read_asset(f, p, src, ctx);
    case FieldType::Struct: {
        DataReader obj = src.object();
        if (!obj.valid()) return false;
        return read_fields(*f.nested, p, obj, ctx).ok();
    }
    case FieldType::Array: {
        // Up to `count` elements; missing trailing elements keep their current values, like
        // missing keys (prefabs may list only the used entries of a fixed-size array).
        DataArray arr = src.array();
        if (arr.size() > f.count) {
            src.fail("expected an array of at most " + std::to_string(f.count) + " elements");
            return false;
        }
        const std::uint32_t stride = element_stride(f.element, f);
        for (std::size_t i = 0; i < arr.size(); ++i) {
            IndexSource el{arr, i};
            if (!read_value(f.element, f, p + i * stride, el, ctx)) return false;
        }
        return true;
    }
    }
    LAM_UNREACHABLE();
}

void write_value(FieldType t, const FieldInfo &f, const std::byte *p, DataWriter &out,
                 const SerializeContext &ctx) {
    switch (t) {
    case FieldType::Bool:
        out.value(load<bool>(p));
        return;
    case FieldType::I8:
        out.value(load<std::int8_t>(p));
        return;
    case FieldType::U8:
        out.value(load<std::uint8_t>(p));
        return;
    case FieldType::I16:
        out.value(load<std::int16_t>(p));
        return;
    case FieldType::U16:
        out.value(load<std::uint16_t>(p));
        return;
    case FieldType::I32:
        out.value(load<std::int32_t>(p));
        return;
    case FieldType::U32:
        out.value(load<std::uint32_t>(p));
        return;
    case FieldType::I64:
        out.value(load<std::int64_t>(p));
        return;
    case FieldType::U64:
        out.value(load<std::uint64_t>(p));
        return;
    case FieldType::F32:
        out.value(load<float>(p));
        return;
    case FieldType::F64:
        out.value(load<double>(p));
        return;
    case FieldType::Vec2:
        out.value(load<Vec2>(p));
        return;
    case FieldType::IVec2:
        out.value(load<IVec2>(p));
        return;
    case FieldType::Rect:
        out.value(load<Rect>(p));
        return;
    case FieldType::IRect:
        out.value(load<IRect>(p));
        return;
    case FieldType::Color:
        out.value(load<Color>(p));
        return;
    case FieldType::Entity: {
        const auto e = load<Entity>(p);
        if (e.is_null()) {
            out.null();
            return;
        }
        for (std::size_t k = 0; k < ctx.entity_remap.size(); ++k) {
            if (ctx.entity_remap[k] == e) {
                out.value(k);
                return;
            }
        }
        out.begin_array().value(e.index).value(e.generation).end_array();
        return;
    }
    case FieldType::StrId:
        out.value(load<StrId>(p));
        return;
    case FieldType::FixedString:
        out.value(fixed_string_view(p, f.size));
        return;
    case FieldType::Enum: {
        const std::uint64_t v = load_uint(p, f.size);
        if (v < f.enum_names.size())
            out.value(f.enum_names[v]);
        else
            out.value(v); // out of range: read back fails loudly
        return;
    }
    case FieldType::Asset: {
        const auto bits = load<std::uint64_t>(p);
        const std::string_view path =
            (bits && ctx.assets) ? ctx.assets->path_of(f.asset_type, bits) : "";
        if (path.empty())
            out.null();
        else
            out.value(path);
        return;
    }
    case FieldType::Struct:
        out.begin_object();
        write_fields(*f.nested, p, out, ctx);
        out.end_object();
        return;
    case FieldType::Array: {
        const std::uint32_t stride = element_stride(f.element, f);
        out.begin_array();
        for (std::size_t i = 0; i < f.count; ++i)
            write_value(f.element, f, p + i * stride, out, ctx);
        out.end_array();
        return;
    }
    }
    LAM_UNREACHABLE();
}

void hash_value(FieldType t, const FieldInfo &f, const std::byte *p, Hasher &h) {
    switch (t) {
    case FieldType::Bool:
        h.u64(load<bool>(p) ? 1u : 0u);
        return;
    case FieldType::I8:
    case FieldType::I16:
    case FieldType::I32:
    case FieldType::I64: {
        // Sign-extend so the hash does not depend on the field width's upper bits.
        std::int64_t v = 0;
        if (f.size == 1)
            v = load<std::int8_t>(p);
        else if (f.size == 2)
            v = load<std::int16_t>(p);
        else if (f.size == 4)
            v = load<std::int32_t>(p);
        else
            v = load<std::int64_t>(p);
        h.u64(static_cast<std::uint64_t>(v));
        return;
    }
    case FieldType::U8:
    case FieldType::U16:
    case FieldType::U32:
    case FieldType::U64:
    case FieldType::Enum:
        h.u64(load_uint(p, f.size));
        return;
    case FieldType::F32:
        h.f32(load<float>(p));
        return;
    case FieldType::F64:
        h.f64(load<double>(p));
        return;
    case FieldType::Vec2: {
        const auto v = load<Vec2>(p);
        h.f32(v.x);
        h.f32(v.y);
        return;
    }
    case FieldType::IVec2: {
        const auto v = load<IVec2>(p);
        h.u64(static_cast<std::uint64_t>(static_cast<std::int64_t>(v.x)));
        h.u64(static_cast<std::uint64_t>(static_cast<std::int64_t>(v.y)));
        return;
    }
    case FieldType::Rect: {
        const auto r = load<Rect>(p);
        h.f32(r.x);
        h.f32(r.y);
        h.f32(r.w);
        h.f32(r.h);
        return;
    }
    case FieldType::IRect: {
        const auto r = load<IRect>(p);
        h.u64(static_cast<std::uint64_t>(static_cast<std::int64_t>(r.x)));
        h.u64(static_cast<std::uint64_t>(static_cast<std::int64_t>(r.y)));
        h.u64(static_cast<std::uint64_t>(static_cast<std::int64_t>(r.w)));
        h.u64(static_cast<std::uint64_t>(static_cast<std::int64_t>(r.h)));
        return;
    }
    case FieldType::Color: {
        const auto c = load<Color>(p);
        h.u64((std::uint64_t(c.r) << 24) | (std::uint64_t(c.g) << 16) | (std::uint64_t(c.b) << 8) |
              c.a);
        return;
    }
    case FieldType::Entity:
        h.u64(load<Entity>(p).bits());
        return;
    case FieldType::StrId:
        h.u64(load<StrId>(p).value);
        return;
    case FieldType::FixedString: {
        const std::string_view s = fixed_string_view(p, f.size);
        h.u64(s.size());
        h.bytes(s.data(), s.size());
        return;
    }
    case FieldType::Asset:
        h.u64(load<std::uint64_t>(p));
        return;
    case FieldType::Struct:
        hash_fields(*f.nested, p, h);
        return;
    case FieldType::Array: {
        const std::uint32_t stride = element_stride(f.element, f);
        for (std::size_t i = 0; i < f.count; ++i)
            hash_value(f.element, f, p + i * stride, h);
        return;
    }
    }
    LAM_UNREACHABLE();
}

} // namespace

Status read_fields(const TypeLayout &layout, void *dst, DataReader &in,
                   const SerializeContext &ctx) {
    if (!in.valid())
        return make_error(ErrorCode::Parse,
                          in.path() + ": expected an object for " + std::string(layout.name));
    auto *base = static_cast<std::byte *>(dst);
    for (const FieldInfo &f : layout.fields) {
        if (!in.has(f.name)) continue;
        KeySource src{in, f.name};
        if (!read_value(f.type, f, base + f.offset, src, ctx) || in.failed()) break;
    }
    if (in.failed())
        return make_error(ErrorCode::Parse, in.path() + ": invalid " + std::string(layout.name));
    return {};
}

void write_fields(const TypeLayout &layout, const void *src, DataWriter &out,
                  const SerializeContext &ctx) {
    const auto *base = static_cast<const std::byte *>(src);
    for (const FieldInfo &f : layout.fields) {
        out.key(f.name);
        write_value(f.type, f, base + f.offset, out, ctx);
    }
}

void hash_fields(const TypeLayout &layout, const void *src, Hasher &h) {
    const auto *base = static_cast<const std::byte *>(src);
    for (const FieldInfo &f : layout.fields)
        hash_value(f.type, f, base + f.offset, h);
}

namespace detail {
void write_layout_object(const TypeLayout &layout, const void *src, DataWriter &out,
                         const SerializeContext &ctx) {
    out.begin_object();
    write_fields(layout, src, out, ctx);
    out.end_object();
}
} // namespace detail

} // namespace lam
