#pragma once

#include "guides.hpp"
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_renderer.hpp>

#include <ajfx/math.hpp>

#include <array>
#include <optional>

// The preview's camera as the page's toolkit sees it, and the handles dragged
// through it. Kept apart from the preview's renderer (preview.hpp) and its
// overlay (stage_overlay.hpp), so a handle's drag can be driven with
// synthetic input and no window or GPU (`ajfx.studio_ui`).
namespace ajfx::studio {

namespace gui = mediaserver::gui;

// Where a camera stands and looks: right-handed, a vertical field of view in
// turns, as the renderer builds it.
struct CameraPose final {
    Vec3 eye{0.0F, 1.0F, 4.5F};
    Vec3 target{0.0F, 0.9F, 0.0F};
    float field_of_view{0.11F};
};

// The camera over a view, for drawing in the effect's space and for turning
// the pointer into a ray.
class StageCamera final {
public:
    StageCamera(const CameraPose& pose, const gui::Rect& view);

    // Where `world` lands in the view, or nothing behind the camera.
    [[nodiscard]] std::optional<gui::UiPoint> project(Vec3 world) const;
    // The pointer's ray into the scene.
    void ray(float x, float y, Vec3& origin, Vec3& direction) const;
    [[nodiscard]] Vec3 forward() const noexcept { return forward_; }
    // The camera as a gizmo takes it: 4x4, column-major, right-handed, the
    // view from world to the camera and the projection from the camera to
    // clip space (depth -1 to 1), over the same view `project` lands in.
    [[nodiscard]] std::array<float, 16> view_matrix() const noexcept;
    [[nodiscard]] std::array<float, 16> projection_matrix(float near_plane, float far_plane) const noexcept;
    // Pixels per metre at `world`'s depth, for sizing by distance.
    [[nodiscard]] float pixels_per_metre(Vec3 world) const;

private:
    gui::Rect view_;
    Vec3 eye_{};
    Vec3 forward_{};
    Vec3 right_{};
    Vec3 up_{};
    float tan_half_{1.0F};
    float aspect_{1.0F};
    float near_{0.01F};
};

} // namespace ajfx::studio
