#pragma once
// The solver as an InputDriver. Reads the focused world's Board and PuzzleState, plans with
// solve() and presses one direction whenever the puzzle is idle. It only presses buttons, so a
// replay of a bot run needs no bot. Owned by the App as a resource.
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include <lam/input/driver.h>
#include "sim/rules.h"
#include "sim/sim_plugin.h"

namespace pathar {

class SolverBot final : public lam::InputDriver {
public:
    explicit SolverBot(Controls controls) : controls_(controls) {}
    void fill(const lam::Ctx &ctx, std::span<lam::InputFrame, lam::kMaxPlayers> frames) override;
    int plans() const { return plans_; }           // solver calls so far
    int presses() const { return presses_; }       // directions pressed so far

private:
    Controls controls_;
    std::vector<Dir> plan_;
    std::size_t next_ = 0;
    std::uint32_t serial_ = 0;                     // Board::serial the plan belongs to
    State expect_;                                 // position the next planned step starts from
    bool failed_ = false;                          // the last search found nothing
    int plans_ = 0;
    int presses_ = 0;
};

}  // namespace pathar
