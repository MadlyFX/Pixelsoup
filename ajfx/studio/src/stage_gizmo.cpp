#include "stage_gizmo.hpp"

#include "stage.hpp"

#include <imgui.h>
// After imgui.h: it declares nothing of ImGui's own.
#include <ImGuizmo.h>

namespace ajfx::studio {

GizmoOutcome draw_gizmo(const StageCamera& camera, const gui::Rect& view, GizmoMode mode, const GizmoPose& pose) {
    GizmoOutcome outcome;
    ImGuizmo::BeginFrame();
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(ImGui::GetBackgroundDrawList());
    ImGuizmo::SetRect(view.x, view.y, view.width, view.height);

    const std::array<float, 16> v = camera.view_matrix();
    const std::array<float, 16> p = camera.projection_matrix(0.05F, 1000.0F);
    PoseMatrix model = pose_matrix(pose.position, pose.turn);
    const bool changed = ImGuizmo::Manipulate(v.data(), p.data(),
                                              mode == GizmoMode::Move ? ImGuizmo::TRANSLATE : ImGuizmo::ROTATE,
                                              // A turn about the thing's own axes is the one an artist sees.
                                              mode == GizmoMode::Move ? ImGuizmo::WORLD : ImGuizmo::LOCAL,
                                              model.data());
    outcome.took_pointer = ImGuizmo::IsOver() || ImGuizmo::IsUsing();
    if (changed && ImGuizmo::IsUsing()) {
        GizmoPose moved;
        pose_from_matrix(model, moved.position, moved.turn);
        outcome.moved = moved;
    }
    return outcome;
}

} // namespace ajfx::studio
