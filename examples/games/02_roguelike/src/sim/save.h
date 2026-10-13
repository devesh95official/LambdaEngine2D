#pragma once
// Save and load a whole run with the engine's world serialisation: every kSerialize component
// and snapshot resource (RunState, TurnScheduler, FovMemory) plus the TilemapInstance of the
// carved floor and the exact entity layout, so a loaded game continues bit-identically.
// The envelope (game id, version, summary) is what lam::SaveSlots writes atomically.
#include <cstdint>
#include <lam/core/result.h>
#include <lam/ecs/component.h>
#include <lam/scene/save.h>
#include <string>
#include <string_view>

namespace lam {
class World;
}

namespace rl {
inline constexpr int kSaveVersion = 1;
inline constexpr std::string_view kSaveGame = "roguelike";
// Envelope for SaveSlots::save. Fails only if serialisation fails.
lam::Result<lam::SaveEnvelope> save_envelope(const lam::World &w, const lam::SerializeContext &ctx);
// Restores an envelope from SaveSlots::load. All-or-nothing: on failure `w` is unchanged.
lam::Status load_envelope(lam::World &w, const lam::SaveEnvelope &env,
                          const lam::SerializeContext &ctx);
// The run a save belongs to (its summary records RunState::run_seed). Permadeath erases a save
// only when it belongs to the run that just ended.
lam::Result<std::uint64_t> saved_run_seed(const lam::SaveEnvelope &env);
// The same as one canonical JSON text (write_save/read_save), for tests and tools.
lam::Result<std::string> save_run(const lam::World &w, const lam::SerializeContext &ctx);
lam::Status load_run(lam::World &w, std::string_view text, const lam::SerializeContext &ctx);
} // namespace rl
