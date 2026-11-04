#pragma once

#include "preview.hpp"
#include "stage_camera.hpp"

namespace ajfx::studio {

// The preview's camera as the renderer builds it from the orbit, for drawing
// over its picture (stage_camera.hpp).
[[nodiscard]] CameraPose camera_pose(const Orbit& orbit);

} // namespace ajfx::studio
