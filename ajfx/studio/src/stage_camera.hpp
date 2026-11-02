#pragma once

#include "guides.hpp"
#include "widgets.hpp"

#include <ajfx/math.hpp>

#include <optional>

// The preview's camera as the page's toolkit sees it, and the handles dragged
// through it. Kept apart from the preview's renderer (preview.hpp) and its
// overlay (stage_overlay.hpp), so a handle's drag can be driven with
// synthetic input and no window or GPU (`ajfx.studio_ui`).
namespace ajfx::studio {

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

// The handles of the block being worked on (GuideSet::handles), drawn over
// the view and dragged with the pointer: positions slide along the floor
// (Shift raises and lowers them), radii pull out from their centre, heights
// go up and down. Every drag writes the block's control into the page, so
// the page, the undo history and the preview follow it.
class HandleDrags final {
public:
    // Call before the camera takes the pointer, so a press on a handle is
    // the handle's.
    void draw(gui::UiContext& ctx, const gui::Rect& view, const StageCamera& camera, Document& document,
              const GuideSet& guides);
    // A handle is being dragged: the edit is one gesture, for undo.
    [[nodiscard]] bool dragging() const noexcept { return drag_.has_value(); }
    void reset() noexcept { drag_.reset(); }

private:
    struct Drag final {
        Handle handle;
        // From where the pointer's ray met the drag plane to the handle, so
        // the handle does not jump to the pointer when it is picked up.
        Vec3 grab{};
        // Moving up and down (Shift) rather than across the floor.
        bool vertical{false};
    };
    std::optional<Drag> drag_;
};

} // namespace ajfx::studio
