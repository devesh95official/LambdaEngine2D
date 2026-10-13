#pragma once
// Audio components and PlaySound. Tick systems emit PlaySound instead of calling Audio;
// audio_world plays them in Update.
#include <cstdint>
#include <lam/core/id.h>
#include <lam/ecs/component.h>
#include <lam/math/vec2.h>
namespace lam {
struct VoiceHandle {
    std::uint32_t index = 0, generation = 0;                    // 0 = none
    constexpr explicit operator bool() const { return index != 0; }
    constexpr bool operator==(const VoiceHandle &) const = default;
};
// Put on the player or camera (needs Transform). Without one, spatial sounds play unpanned.
struct AudioListener { float range = 240; };
struct AudioEmitter { StrId group; bool loop = false; float range = 220; float volume = 1; };
struct AudioVoice { VoiceHandle voice; };                       // not saved; re-acquired after a restore
struct PlaySound { StrId group; Vec2 position; bool spatial = false; float volume = 1; };   // World event
}
LAM_REFLECT_FLAGS(lam::AudioListener, lam::kSerialize | lam::kSnapshot, range)
LAM_REFLECT_FLAGS(lam::AudioEmitter, lam::kSerialize | lam::kSnapshot, group, loop, range, volume)
