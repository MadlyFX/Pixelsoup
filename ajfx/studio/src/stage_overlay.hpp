#pragma once

#include "guides.hpp"
#include "package_builder.hpp"
#include "preview.hpp"
#include "stage_camera.hpp"
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

// The preview's camera as the renderer builds it from the orbit, for drawing
// over its picture (stage_camera.hpp).
[[nodiscard]] CameraPose camera_pose(const Orbit& orbit);

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
    // pointer, so a press on a handle is the handle's. True on a frame a
    // handle was held: only then does `document` hold an edit to take back.
    [[nodiscard]] bool draw(gui::UiContext& ctx, const gui::Rect& view, const Orbit& orbit, Document& document,
                            const OverlayInputs& inputs);
    // A handle is being dragged: the edit is one gesture, for undo.
    [[nodiscard]] bool dragging() const noexcept { return handles_.dragging(); }

private:
    void draw_paths(gui::UiContext& ctx, const StageCamera& camera, const OverlayInputs& inputs);
    void draw_guides(gui::UiContext& ctx, const StageCamera& camera, const GuideSet& guides);

    HandleDrags handles_;
};

} // namespace ajfx::studio
