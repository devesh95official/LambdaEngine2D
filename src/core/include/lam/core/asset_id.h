#pragma once
// Asset ids and the resolver interface. Kept in core so ecs and lam_app don't need lam_assets;
// AssetServer implements AssetResolver.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <lam/core/id.h>
#include <lam/core/result.h>
namespace lam {
template <class T> struct AssetId {                             // POD, storable in components
    std::uint32_t index = 0;                                    // 0 = none
    std::uint32_t generation = 0;
    constexpr explicit operator bool() const { return index != 0; }
    constexpr bool operator==(const AssetId &) const = default;
    constexpr std::uint64_t bits() const { return index | (std::uint64_t(generation) << 32); }
    static constexpr AssetId from_bits(std::uint64_t b) {
        return {std::uint32_t(b & 0xFFFFFFFFu), std::uint32_t(b >> 32)};
    }
};
enum class LoadState : std::uint8_t { None, Queued, Loading, Ready, Failed };
class AssetResolver {
public:
    virtual ~AssetResolver() = default;
    // Starts (or finds) a load of `path` as asset type `type`, retained by the current scope.
    // Returns AssetId bits, 0 if no loader handles the type and extension.
    virtual std::uint64_t resolve(TypeId type, std::string_view path) = 0;
    // The asset's path, or the name a runtime asset was given (AssetServer::add); "" for an
    // unnamed runtime asset (saved as null) or an unknown id.
    virtual std::string_view path_of(TypeId type, std::uint64_t bits) const = 0;
    virtual Result<std::string> read_text(std::string_view path) const = 0;       // root-relative, sync
    virtual std::size_t pending() const = 0;                    // queued + loading + finalizing
};
}
