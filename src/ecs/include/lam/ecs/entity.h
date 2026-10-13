#pragma once
// Generational entity handle. Copying one never keeps the entity alive; once the slot is reused
// the old handle just stops being alive.
#include <cstdint>
namespace lam {
// Generation 0 means null. A slot whose generation reaches UINT32_MAX is retired.
struct Entity {
    std::uint32_t index = 0;                                    // slot in the entity table
    std::uint32_t generation = 0;                               // 0 = null
    static constexpr Entity null() { return {}; }
    // Neither of these says whether the entity is alive.
    constexpr bool is_null() const { return generation == 0; }
    constexpr explicit operator bool() const { return generation != 0; }
    constexpr bool operator==(const Entity &) const = default;
    // Stable 64-bit key for hashing, maps and logs.
    constexpr std::uint64_t bits() const { return index | (std::uint64_t(generation) << 32); }
};
}
