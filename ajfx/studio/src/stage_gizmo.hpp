#pragma once

#include "stage_camera.hpp"

#include <ajfx/math.hpp>

#include <optional>

// The move and turn gizmo over a thing on the stage (ImGuizmo), drawn over
// the viewport and dragged with the pointer. The pose it is given is the
// thing's own as the scene keeps it (a position and a turn as an Object's
// rotation); what comes back is where the artist has dragged it to.
namespace ajfx::studio {

enum class GizmoMode : std::uint8_t { Move, Turn };

struct GizmoPose final {
    Vec3 position{};
    Vec3 turn{};
};

struct GizmoOutcome final {
    // The pointer is over the gizmo, or dragging it: the rest of the
    // viewport is not for the pointer this frame.
    bool took_pointer{false};
    // Where it has been dragged to this frame, while it is.
    std::optional<GizmoPose> moved;
};

// Draws the gizmo for `pose` and takes the pointer's drag on it. `camera`
// must be over `view` in the pixels ImGui draws in. Call between ImGui's
// NewFrame and Render.
GizmoOutcome draw_gizmo(const StageCamera& camera, const gui::Rect& view, GizmoMode mode, const GizmoPose& pose);

} // namespace ajfx::studio
