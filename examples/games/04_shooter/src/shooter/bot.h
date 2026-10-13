#pragma once
// The autoplay bot. Stateless, so undo and replays reproduce it. In the arena it kites away from
// nearby enemies, strafes, grabs pickups and aims with lead (right stick, which auto-fires) at the
// nearest enemy it can see. In a menu world it picks item 0 ("Restart"/"Resume").
#include <span>
#include <lam/input/driver.h>
#include <lam/input/input_frame.h>
namespace shooter {
class ShooterBot final : public lam::InputDriver {
public:
    void fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) override;
};
}
