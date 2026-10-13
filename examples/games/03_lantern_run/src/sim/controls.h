#pragma once
// Named input actions (App resource). Ids are resolved once at startup through ActionMap;
// the rules read them from the tick's InputFrame, and the Autopilot bot writes the same ids
// (InputFrame::press / set_axis).
#include <lam/input/actions.h>
#include <lam/input/input_frame.h>

namespace lantern {

struct Controls {
    lam::AxisAction move;                                       // WASD / arrows / left stick
    lam::ButtonAction dash;                                     // Space / Shift / pad south
    lam::ButtonAction confirm;                                  // Enter / Space: start, run again
    lam::ButtonAction pause;                                    // P / Esc (frame-time, FrameInput)
};

// Defines the four actions with their default bindings.
Controls bind_controls(lam::ActionMap &map);

} // namespace lantern
