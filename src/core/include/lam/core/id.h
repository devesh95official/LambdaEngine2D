#pragma once
// String ids resolved once at load, and process-local type ids. The name table is always on
// since saves write names; unknown ids print as "#<16 hex digits>".
//   using namespace lam;  StrId jump = "jump"_id;    // or: using namespace lam::literals;
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
namespace lam {
struct StrId {
    std::uint64_t value = 0;                                    // 0 = none
    constexpr bool operator==(const StrId &) const = default;
    constexpr explicit operator bool() const { return value != 0; }
};
// FNV-1a 64 of the bytes; "" -> {0}. Never 0 for a non-empty string.
constexpr StrId str_id(std::string_view s) {
    if (s.empty()) return {};
    std::uint64_t h = 14695981039346656037ULL;
    for (char c : s) {
        h ^= static_cast<std::uint8_t>(c);
        h *= 1099511628211ULL;
    }
    return {h == 0 ? 1 : h};
}
// str_id + register_id_name (load time; allocates).
StrId intern(std::string_view s);
// Remembers the name of an id. Thread-safe; copies the name.
void register_id_name(StrId id, std::string_view name);
// The registered name, "" if unknown. The view stays valid for the process lifetime.
std::string_view id_name(StrId id);
// The name, or "#<16 hex digits>" if unknown (cold path).
std::string id_display(StrId id);
// Inline, so `using namespace lam;` brings "..."_id in too; `using namespace lam::literals;` alone
// still works where the rest of lam is not wanted.
inline namespace literals {
constexpr StrId operator""_id(const char *s, std::size_t n) { return str_id(std::string_view(s, n)); }
}
using TypeId = std::uint32_t;                                   // 0 = none
namespace detail {
TypeId next_type_id();
}
// Process-local id per type, dense from 1.
template <class T> TypeId type_id() {
    static const TypeId id = detail::next_type_id();
    return id;
}
}
template <> struct std::hash<lam::StrId> {
    std::size_t operator()(const lam::StrId &id) const noexcept { return static_cast<std::size_t>(id.value); }
};
