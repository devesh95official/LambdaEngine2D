#pragma once
// Lets Fsm<E> be a reflected field when E has LAM_REFLECT_ENUM. Lives here because lam_core
// can't include ecs. A prefab may give just {"state": "chase"}.
//   enum class Wolf : std::uint8_t { Idle, Chase };
//   struct WolfAi { lam::Fsm<Wolf> fsm; };
//   LAM_REFLECT_ENUM(Wolf, "idle", "chase")
//   LAM_REFLECT(WolfAi, fsm)            // after #include <lam/ecs/reflect_fsm.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>
#include <lam/core/fsm.h>
#include <lam/ecs/component.h>
namespace lam {
template <class E>
    requires detail::HasEnumNames<E>
struct Reflect<Fsm<E>> {
    static constexpr std::uint32_t kFlags = 0;
    static constexpr bool kComponent = false;
    static constexpr bool kCustom = false;
    static constexpr std::string_view kName = "Fsm";
    static const TypeLayout &layout() {
        using F = Fsm<E>;
        static_assert(std::is_standard_layout_v<F>, "reflected types: standard layout");
        static const FieldInfo fields[] = {
            detail::field_info<E>("state", offsetof(F, state)),
            detail::field_info<E>("previous", offsetof(F, previous)),
            detail::field_info<std::uint32_t>("ticks_in_state", offsetof(F, ticks_in_state)),
            detail::field_info<bool>("entered", offsetof(F, entered))};
        static const TypeLayout layout_{kName, static_cast<std::uint32_t>(sizeof(F)),
                                        std::span<const FieldInfo>(fields)};
        return layout_;
    }
};
}
