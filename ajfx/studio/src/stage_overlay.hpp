#pragma once

#include "guides.hpp"
#include "package_builder.hpp"
#include "preview.hpp"
#include "widgets.hpp"

#include <ajfx/math.hpp>

#include <optional>

// What Studio draws over the preview picture: the guides (shapes, pushes,
// fields, colliders), the handles of the block being worked on, and in Paths
// mode the whole flight of some of the effect's particles at once.
//
// It draws with the preview's own camera, so a guide sits exactly on the
// particles it describes. A handle is dragged with the pointer: positions
// slide along the floor (Shift raises and lowers them), radii pull out from
// their centre, heights go up and down, and every drag writes the block's
// control, so the page, the undo history and the preview follow it.
namespace ajfx::studio {

// The preview's camera as the renderer builds it (right-handed, a vertical
// field of view), for drawing over its picture.
class StageCamera final {
public:
    StageCamera(const Orbit& orbit, const gui::Rect& view);

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

struct OverlayInputs final {
    const GuideSet* guides{nullptr};
    const Analysis* analysis{nullptr};
    // Guides (shapes, pushes, fields, colliders), the handles that drag
    // them, and particle paths, each on or off.
    bool show_guides{true};
    bool show_handles{true};
    bool show_paths{false};
    // The layer being worked on: only its paths are drawn, none without one.
    Uid layer{};
    // The show frame the preview is at.
    double frame{0.0};
};

class StageOverlay final {
public:
    // Draws over `view` and handles drags of the guides' handles, writing
    // their controls into `document`. Call before the camera takes the
    // pointer, so a press on a handle is the handle's.
    void draw(gui::UiContext& ctx, const gui::Rect& view, const Orbit& orbit, Document& document,
              const OverlayInputs& inputs);
    // A handle is being dragged: the edit is one gesture, for undo.
    [[nodiscard]] bool dragging() const noexcept { return drag_.has_value(); }

private:
    void draw_paths(gui::UiContext& ctx, const StageCamera& camera, const OverlayInputs& inputs);
    void draw_guides(gui::UiContext& ctx, const StageCamera& camera, const GuideSet& guides);
    void draw_handles(gui::UiContext& ctx, const gui::Rect& view, const StageCamera& camera, Document& document,
                      const GuideSet& guides);

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
