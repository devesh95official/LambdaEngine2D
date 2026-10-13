#pragma once
// Component reflection: one description per type drives prefabs, saves, snapshots, hashing and
// the inspector. Put LAM_REFLECT* at global scope right after the type, with its qualified name.
// Reflected types register during static init. Ids are per process, so saves use names.
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <lam/core/asset_id.h>
#include <lam/core/data.h>
#include <lam/core/fixed_string.h>
#include <lam/core/hash.h>
#include <lam/core/id.h>
#include <lam/core/result.h>
#include <lam/ecs/entity.h>
#include <lam/math/color.h>
#include <lam/math/ease.h>
#include <lam/math/rect.h>
#include <lam/math/vec2.h>
namespace lam {
class World;
// Reflected field kinds. Integers are stored with their own width; Enum values are 0..n-1.
enum class FieldType : std::uint8_t { Bool, I8, U8, I16, U16, I32, U32, I64, U64, F32, F64,
    Vec2, IVec2, Rect, IRect, Color, Entity, StrId, FixedString, Enum, Asset, Struct, Array };
// In JSON: Vec2 [x, y], Rect [x, y, w, h], Color "#rrggbb[aa]" or [r, g, b(, a)], StrId and Enum
// by name, Asset by path, Entity as [index, generation] in saves and child index k in prefabs.
// An Array may list fewer than N elements; the rest keep their values.
struct TypeLayout;
// One reflected field. Built once per type (first use, cold path); lives for the process.
struct FieldInfo {
    std::string_view name;                                      // JSON key (the member name)
    FieldType type = FieldType::Bool;
    FieldType element = FieldType::Bool;                        // element type when type == Array
    std::uint32_t offset = 0;                                   // bytes from the start of the owner
    std::uint32_t size = 0;                                     // bytes of one element; FixedString<N>: N
    std::uint16_t count = 1;                                    // Array length (std::array<E, N>)
    const TypeLayout *nested = nullptr;                         // Struct, or Array of Struct
    std::span<const std::string_view> enum_names = {};          // Enum (values 0..n-1)
    TypeId asset_type = 0;                                      // Asset: T of AssetId<T>
};
// The reflected shape of a type: its name, sizeof and fields in declaration order.
struct TypeLayout {
    std::string_view name;
    std::uint32_t size = 0;                                     // sizeof(T); 0 for tags
    std::span<const FieldInfo> fields;
};
// Per-type behaviour flags.
enum ComponentFlags : std::uint32_t {
    kSerialize = 1,                                             // saved by serialize_world, read by prefabs
    kSnapshot = 2,                                              // copied by snapshots (undo)
    kHash = 4,                                                  // in state_hash; requires kSerialize
    kTag = 8,                                                   // no data, no dense storage
    kDefaultFlags = kSerialize | kSnapshot | kHash,
};
// For reads/writes that touch assets or entities. Prefabs fill entity_remap so an Entity field
// written as k means entity_remap[k]; saves leave it empty.
struct SerializeContext {
    AssetResolver *assets = nullptr;                            // AssetId fields as paths; null: none
    std::span<const Entity> entity_remap = {};
};
// Index of a registered type; 0-based, dense, process-local (not a handle).
using ComponentId = std::uint16_t;
// Hook signature. The pointer is the component in its pool; valid only during the call.
using ComponentHook = void (*)(World &, Entity, void *);
// Everything the engine knows about a registered type. The address never changes.
struct ComponentInfo {
    std::string_view name;                                      // stable; used in JSON and saves
    ComponentId id = 0;
    TypeId type = 0;
    std::uint32_t size = 0, align = 0, flags = 0;               // unreflected types: flags 0; empty types: size 0
    const TypeLayout *layout = nullptr;                         // null: custom ops or unreflected
    void (*construct)(void *dst) = nullptr;                     // value-initialise T in raw storage
    void (*copy)(void *dst, const void *src) = nullptr;         // copy-assign into a constructed T
    void (*move)(void *dst, void *src) = nullptr;               // move-construct into raw storage; src stays constructed
    void (*destroy)(void *p) = nullptr;                         // run ~T
    // Reads the fields of one JSON object into a constructed T (missing keys keep their values).
    Status (*read)(void *dst, DataReader &in, const SerializeContext &ctx) = nullptr;
    // Writes one complete JSON value (an object for reflected types).
    void (*write)(const void *src, DataWriter &out, const SerializeContext &ctx) = nullptr;
    void (*hash)(const void *src, Hasher &h) = nullptr;
    ComponentHook on_add = nullptr;                             // first insert only
    ComponentHook on_replace = nullptr;                         // add<T> on an entity that has T
    ComponentHook on_remove = nullptr;
};
namespace detail {
template <class T> ComponentInfo make_component_info();
}
// Process-global table of registered types. Main thread only.
class ComponentRegistry {
public:
    static ComponentRegistry &global();
    // id() and info() register T on first use.
    template <class T> ComponentId id();
    template <class T> const ComponentInfo &info();
    // Reflected types win over unreflected ones of the same name. Linear: load time only.
    const ComponentInfo *find(std::string_view name) const;
    // LAM_CHECK(id < size()).
    const ComponentInfo &at(ComponentId id) const;
    std::size_t size() const;
    // Installs hooks for T (null clears one). Call during plugin build(), before entities exist.
    template <class T> void set_hooks(ComponentHook on_add, ComponentHook on_replace, ComponentHook on_remove);
    // Type-erased set_hooks.
    void set_hooks(ComponentId id, ComponentHook on_add, ComponentHook on_replace, ComponentHook on_remove);
    // Sets T's flags; unreflected types can only use kSnapshot and kTag. Call before any World
    // stores T.
    template <class T> ComponentId register_type(std::uint32_t flags);
    // True when T is trivially copyable (pools and snapshots memcpy it).
    bool trivially_copyable(ComponentId id) const;
private:
    struct Entry {
        ComponentInfo info;
        bool trivial = false;
    };
    ComponentRegistry() = default;
    ComponentId add(const ComponentInfo &info, bool trivial);
    void set_flags(ComponentId id, std::uint32_t flags);
    std::vector<std::unique_ptr<Entry>> entries_;
};
template <class T> struct Reflect;                              // primary undefined
template <class E> struct ReflectEnum;                          // primary undefined
// Missing keys keep their current value; unknown keys stay unread so DataDocument::finish()
// reports them. Load time.
Status read_fields(const TypeLayout &layout, void *dst, DataReader &in, const SerializeContext &ctx);
// Writes every field as `"name": value` pairs into the object the caller has opened.
void write_fields(const TypeLayout &layout, const void *src, DataWriter &out, const SerializeContext &ctx);
// Hashes every field value (floats canonicalised, padding never read). No allocation.
void hash_fields(const TypeLayout &layout, const void *src, Hasher &h);

// ------------------------------------------------------------------------------------------------
// Implementation details used by the macros below. Not part of the API.
namespace detail {
// "lam::Transform" -> "Transform" (the part after the last "::" before any template arguments).
constexpr std::string_view short_name(std::string_view qualified) {
    const std::size_t lt = qualified.find('<');
    const std::string_view head =
        lt == std::string_view::npos ? qualified : qualified.substr(0, lt);
    const std::size_t sep = head.rfind("::");
    return sep == std::string_view::npos ? qualified : qualified.substr(sep + 2);
}
// The qualified name of T from the compiler (unreflected types).
template <class T>
constexpr std::string_view qualified_type_name() {
    const std::string_view p = __PRETTY_FUNCTION__;
    const std::size_t start = p.find("T = ");
    if (start == std::string_view::npos) return "unknown";
    const std::size_t end = p.find_first_of(";]", start + 4);
    return p.substr(start + 4, end - (start + 4));
}
template <class T>
concept Reflected = requires { Reflect<T>::kFlags; };
template <class T>
concept HasLayout = Reflected<T> && requires { Reflect<T>::layout(); };
template <class T>
concept HasEnumNames = requires { ReflectEnum<T>::names; };
template <class T>
struct IsStdArray : std::false_type {};
template <class E, std::size_t N>
struct IsStdArray<std::array<E, N>> : std::true_type {};
template <class T>
struct IsFixedString : std::false_type {};
template <std::size_t N>
struct IsFixedString<FixedString<N>> : std::true_type {
    static constexpr std::size_t capacity = N;
};
template <class T>
struct IsAssetId : std::false_type {};
template <class A>
struct IsAssetId<AssetId<A>> : std::true_type {
    using asset = A;
};
template <class>
inline constexpr bool kAlwaysFalse = false;
// Byte stride of one element of a field (FixedString stores N, not sizeof).
constexpr std::uint32_t fixed_string_sizeof(std::uint32_t n) {
    return ((n + 1u) & ~1u) + 2u;
}
// Generation used for prefab-local entity references while compiled (never a live generation).
inline constexpr std::uint32_t kLocalEntityGeneration = 0xFFFFFFFFu;

// Fills type/size/nested/enum/asset info for one value of type F.
template <class F>
FieldType describe_value(FieldInfo &f) {
    if constexpr (std::is_same_v<F, bool>) {
        f.size = 1;
        return FieldType::Bool;
    } else if constexpr (std::is_enum_v<F>) {
        static_assert(
            HasEnumNames<F>,
            "enum field: declare LAM_REFLECT_ENUM(Enum, ...) before the owner's LAM_REFLECT");
        f.size = sizeof(F);
        f.enum_names = std::span<const std::string_view>(ReflectEnum<F>::names);
        return FieldType::Enum;
    } else if constexpr (std::is_integral_v<F>) {
        f.size = sizeof(F);
        if constexpr (sizeof(F) == 1)
            return std::is_signed_v<F> ? FieldType::I8 : FieldType::U8;
        else if constexpr (sizeof(F) == 2)
            return std::is_signed_v<F> ? FieldType::I16 : FieldType::U16;
        else if constexpr (sizeof(F) == 4)
            return std::is_signed_v<F> ? FieldType::I32 : FieldType::U32;
        else {
            static_assert(sizeof(F) == 8, "unsupported integer width");
            return std::is_signed_v<F> ? FieldType::I64 : FieldType::U64;
        }
    } else if constexpr (std::is_same_v<F, float>) {
        f.size = 4;
        return FieldType::F32;
    } else if constexpr (std::is_same_v<F, double>) {
        f.size = 8;
        return FieldType::F64;
    } else if constexpr (std::is_same_v<F, Vec2>) {
        f.size = sizeof(Vec2);
        return FieldType::Vec2;
    } else if constexpr (std::is_same_v<F, IVec2>) {
        f.size = sizeof(IVec2);
        return FieldType::IVec2;
    } else if constexpr (std::is_same_v<F, Rect>) {
        f.size = sizeof(Rect);
        return FieldType::Rect;
    } else if constexpr (std::is_same_v<F, IRect>) {
        f.size = sizeof(IRect);
        return FieldType::IRect;
    } else if constexpr (std::is_same_v<F, Color>) {
        f.size = sizeof(Color);
        return FieldType::Color;
    } else if constexpr (std::is_same_v<F, Entity>) {
        f.size = sizeof(Entity);
        return FieldType::Entity;
    } else if constexpr (std::is_same_v<F, StrId>) {
        f.size = sizeof(StrId);
        return FieldType::StrId;
    } else if constexpr (IsFixedString<F>::value) {
        static_assert(sizeof(F) == fixed_string_sizeof(IsFixedString<F>::capacity),
                      "FixedString layout changed; update component.cpp");
        f.size = static_cast<std::uint32_t>(IsFixedString<F>::capacity);
        return FieldType::FixedString;
    } else if constexpr (IsAssetId<F>::value) {
        f.size = sizeof(F);
        f.asset_type = type_id<typename IsAssetId<F>::asset>();
        return FieldType::Asset;
    } else if constexpr (HasLayout<F>) {
        f.size = sizeof(F);
        f.nested = &Reflect<F>::layout();
        return FieldType::Struct;
    } else {
        static_assert(kAlwaysFalse<F>,
                      "unsupported field type: use scalars, Vec2, IVec2, Rect, IRect, Color, "
                      "Entity, StrId, FixedString<N>, reflected enums, AssetId<T>, "
                      "LAM_REFLECT_STRUCT types or std::array of these");
        return FieldType::Bool;
    }
}
// The FieldInfo of member `name` (type F) at byte `offset`.
template <class F>
FieldInfo field_info(std::string_view name, std::size_t offset) {
    FieldInfo f;
    f.name = name;
    f.offset = static_cast<std::uint32_t>(offset);
    if constexpr (IsStdArray<F>::value) {
        using E = typename F::value_type;
        static_assert(!IsStdArray<E>::value, "nested std::array fields are not supported");
        static_assert(std::tuple_size_v<F> <= 65535, "array field too long");
        f.element = describe_value<E>(f);
        f.type = FieldType::Array;
        f.count = static_cast<std::uint16_t>(std::tuple_size_v<F>);
    } else {
        f.type = describe_value<F>(f);
    }
    return f;
}
template <class T>
void op_construct(void *dst) {
    if constexpr (!std::is_empty_v<T>) ::new (dst) T();
}
template <class T>
void op_copy(void *dst, const void *src) {
    if constexpr (!std::is_empty_v<T>) *static_cast<T *>(dst) = *static_cast<const T *>(src);
}
template <class T>
void op_move(void *dst, void *src) {
    if constexpr (!std::is_empty_v<T>) ::new (dst) T(std::move(*static_cast<T *>(src)));
}
template <class T>
void op_destroy(void *p) {
    if constexpr (!std::is_empty_v<T>) static_cast<T *>(p)->~T();
}
template <class T>
Status op_read_layout(void *dst, DataReader &in, const SerializeContext &ctx) {
    return read_fields(Reflect<T>::layout(), dst, in, ctx);
}
void write_layout_object(const TypeLayout &layout, const void *src, DataWriter &out,
                         const SerializeContext &ctx);
template <class T>
void op_write_layout(const void *src, DataWriter &out, const SerializeContext &ctx) {
    write_layout_object(Reflect<T>::layout(), src, out, ctx);
}
template <class T>
void op_hash_layout(const void *src, Hasher &h) {
    hash_fields(Reflect<T>::layout(), src, h);
}
template <class T>
Status op_read_custom(void *dst, DataReader &in, const SerializeContext &ctx) {
    return lam_read(*static_cast<T *>(dst), in, ctx);
}
template <class T>
void op_write_custom(const void *src, DataWriter &out, const SerializeContext &ctx) {
    lam_write(*static_cast<const T *>(src), out, ctx);
}
template <class T>
void op_hash_custom(const void *src, Hasher &h) {
    lam_hash(*static_cast<const T *>(src), h);
}

template <class T>
ComponentInfo make_component_info() {
    static_assert(!std::is_reference_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T>,
                  "components are plain value types");
    ComponentInfo info;
    info.type = type_id<T>();
    if constexpr (std::is_empty_v<T>) {
        info.size = 0;
        info.align = 1;
    } else {
        info.size = static_cast<std::uint32_t>(sizeof(T));
        info.align = static_cast<std::uint32_t>(alignof(T));
    }
    if constexpr (std::is_default_constructible_v<T>) info.construct = &op_construct<T>;
    if constexpr (std::is_copy_assignable_v<T>) info.copy = &op_copy<T>;
    if constexpr (std::is_move_constructible_v<T>) info.move = &op_move<T>;
    info.destroy = &op_destroy<T>;
    if constexpr (Reflected<T>) {
        info.name = Reflect<T>::kName;
        info.flags = Reflect<T>::kFlags;
        if constexpr (Reflect<T>::kCustom) {
            info.read = &op_read_custom<T>;
            info.write = &op_write_custom<T>;
            info.hash = &op_hash_custom<T>;
        } else {
            info.layout = &Reflect<T>::layout();
            info.read = &op_read_layout<T>;
            info.write = &op_write_layout<T>;
            info.hash = &op_hash_layout<T>;
        }
    } else {
        info.name = short_name(qualified_type_name<T>());
        info.flags = 0;
    }
    return info;
}
// Called by the LAM_REFLECT* macros during static initialisation.
template <class T>
ComponentId auto_register() {
    return ComponentRegistry::global().id<T>();
}
} // namespace detail

template <class T>
ComponentId ComponentRegistry::id() {
    static const ComponentId cid =
        add(detail::make_component_info<T>(), std::is_trivially_copyable_v<T>);
    return cid;
}
template <class T>
const ComponentInfo &ComponentRegistry::info() {
    return at(id<T>());
}
template <class T>
void ComponentRegistry::set_hooks(ComponentHook on_add, ComponentHook on_replace,
                                  ComponentHook on_remove) {
    set_hooks(id<T>(), on_add, on_replace, on_remove);
}
template <class T>
ComponentId ComponentRegistry::register_type(std::uint32_t flags) {
    const ComponentId cid = id<T>();
    set_flags(cid, flags);
    return cid;
}
} // namespace lam

// ------------------------------------------------------------------------------------------------
// Macro machinery (C++20 __VA_OPT__ for-each, up to 256 fields).
#define LAM_DETAIL_PARENS ()
#define LAM_DETAIL_EXPAND(...)                                                                     \
    LAM_DETAIL_EXPAND4(LAM_DETAIL_EXPAND4(LAM_DETAIL_EXPAND4(LAM_DETAIL_EXPAND4(__VA_ARGS__))))
#define LAM_DETAIL_EXPAND4(...)                                                                    \
    LAM_DETAIL_EXPAND3(LAM_DETAIL_EXPAND3(LAM_DETAIL_EXPAND3(LAM_DETAIL_EXPAND3(__VA_ARGS__))))
#define LAM_DETAIL_EXPAND3(...)                                                                    \
    LAM_DETAIL_EXPAND2(LAM_DETAIL_EXPAND2(LAM_DETAIL_EXPAND2(LAM_DETAIL_EXPAND2(__VA_ARGS__))))
#define LAM_DETAIL_EXPAND2(...)                                                                    \
    LAM_DETAIL_EXPAND1(LAM_DETAIL_EXPAND1(LAM_DETAIL_EXPAND1(LAM_DETAIL_EXPAND1(__VA_ARGS__))))
#define LAM_DETAIL_EXPAND1(...) __VA_ARGS__
#define LAM_DETAIL_FOR_EACH(lam_m_, lam_t_, ...)                                                   \
    __VA_OPT__(LAM_DETAIL_EXPAND(LAM_DETAIL_FOR_EACH_H(lam_m_, lam_t_, __VA_ARGS__)))
#define LAM_DETAIL_FOR_EACH_H(lam_m_, lam_t_, lam_a_, ...)                                         \
    lam_m_(lam_t_, lam_a_)                                                                         \
        __VA_OPT__(LAM_DETAIL_FOR_EACH_A LAM_DETAIL_PARENS(lam_m_, lam_t_, __VA_ARGS__))
#define LAM_DETAIL_FOR_EACH_A() LAM_DETAIL_FOR_EACH_H
#define LAM_DETAIL_FIELD(lam_t_, lam_f_)                                                           \
    ::lam::detail::field_info<decltype(lam_t_::lam_f_)>(#lam_f_, offsetof(lam_t_, lam_f_)),
// Shared body of LAM_REFLECT_FLAGS and LAM_REFLECT_STRUCT. The trailing FieldInfo{} keeps the
// array non-empty; the span excludes it.
#define LAM_DETAIL_REFLECT_BODY(lam_t_, lam_flags_, lam_component_, ...)                           \
    static constexpr std::uint32_t kFlags = static_cast<std::uint32_t>(lam_flags_);                \
    static constexpr bool kComponent = lam_component_;                                             \
    static constexpr bool kCustom = false;                                                         \
    static constexpr std::string_view kName = ::lam::detail::short_name(#lam_t_);                  \
    static_assert((kFlags & ::lam::kHash) == 0 || (kFlags & ::lam::kSerialize) != 0,               \
                  "kHash requires kSerialize");                                                    \
    static const ::lam::TypeLayout &layout() {                                                     \
        static_assert(std::is_standard_layout_v<lam_t_>, "reflected types: standard layout");    \
        static const ::lam::FieldInfo lam_fields_[] = {                                            \
            LAM_DETAIL_FOR_EACH(LAM_DETAIL_FIELD, lam_t_, __VA_ARGS__)::lam::FieldInfo{}};         \
        static const ::lam::TypeLayout lam_layout_{                                                \
            kName, static_cast<std::uint32_t>(sizeof(lam_t_)),                                     \
            std::span<const ::lam::FieldInfo>(lam_fields_, std::size(lam_fields_) - 1)};           \
        return lam_layout_;                                                                        \
    }

// Standard-layout component with kDefaultFlags. Fields: scalars, math types, Entity, StrId,
// FixedString, reflected enums and structs, AssetId, Fsm, and std::array of any of these.
#define LAM_REFLECT(Type, ...) LAM_REFLECT_FLAGS(Type, ::lam::kDefaultFlags, __VA_ARGS__)
// LAM_REFLECT for World resources passed to World::snapshot_resource<T>().
#define LAM_REFLECT_RESOURCE(Type, ...) LAM_REFLECT(Type, __VA_ARGS__)
// LAM_REFLECT with explicit flags (presentation components: kSerialize | kSnapshot).
#define LAM_REFLECT_FLAGS(Type, flags, ...)                                                        \
    template <> struct lam::Reflect<Type> {                                                        \
        LAM_DETAIL_REFLECT_BODY(Type, flags, true, __VA_ARGS__)                                    \
        static inline const ::lam::ComponentId kAutoId = ::lam::detail::auto_register<Type>();    \
    };
// Empty tag component: kTag | kSerialize | kSnapshot | kHash.
#define LAM_REFLECT_TAG(Type)                                                                      \
    template <> struct lam::Reflect<Type> {                                                        \
        static_assert(std::is_empty_v<Type>, "LAM_REFLECT_TAG types must be empty");               \
        static constexpr std::uint32_t kFlags =                                                    \
            ::lam::kTag | ::lam::kSerialize | ::lam::kSnapshot | ::lam::kHash;                     \
        static constexpr bool kComponent = true;                                                   \
        static constexpr bool kCustom = false;                                                     \
        static constexpr std::string_view kName = ::lam::detail::short_name(#Type);                \
        static const ::lam::TypeLayout &layout() {                                                 \
            static const ::lam::TypeLayout lam_layout_{kName, 0, {}};                              \
            return lam_layout_;                                                                    \
        }                                                                                          \
        static inline const ::lam::ComponentId kAutoId = ::lam::detail::auto_register<Type>();    \
    };
// Nested value type usable as a field (TextureRegion, TweenSpec); not a component by itself.
#define LAM_REFLECT_STRUCT(Type, ...)                                                              \
    template <> struct lam::Reflect<Type> {                                                        \
        LAM_DETAIL_REFLECT_BODY(Type, 0, false, __VA_ARGS__)                                       \
    };
// Enum names in enumerator order (values 0..n-1).
#define LAM_REFLECT_ENUM(Enum, ...)                                                                \
    template <> struct lam::ReflectEnum<Enum> {                                                    \
        static constexpr std::string_view names[] = {__VA_ARGS__};                                 \
        static constexpr std::size_t count = std::size(names);                                     \
    };
// Component or resource with non-field state (Grid, vector): uses ADL free functions
//   Status lam_read(Type &, DataReader &, const SerializeContext &);
//   void lam_write(const Type &, DataWriter &, const SerializeContext &);   // one JSON value
//   void lam_hash(const Type &, Hasher &);
#define LAM_REFLECT_CUSTOM(Type, flags)                                                            \
    template <> struct lam::Reflect<Type> {                                                        \
        static constexpr std::uint32_t kFlags = static_cast<std::uint32_t>(flags);                 \
        static constexpr bool kComponent = true;                                                   \
        static constexpr bool kCustom = true;                                                      \
        static constexpr std::string_view kName = ::lam::detail::short_name(#Type);                \
        static inline const ::lam::ComponentId kAutoId = ::lam::detail::auto_register<Type>();    \
    };
// lam_math cannot include ecs, so the math enum used in components is reflected here.
LAM_REFLECT_ENUM(lam::Ease, "linear", "in_quad", "out_quad", "in_out_quad", "in_cubic", "out_cubic",
                 "in_out_cubic", "in_sine", "out_sine", "in_out_sine", "in_expo", "out_expo", "in_out_expo",
                 "in_back", "out_back", "in_out_back", "out_bounce", "out_elastic", "step")
