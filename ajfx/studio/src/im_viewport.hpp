#pragma once

#include "guides.hpp"
#include "package_builder.hpp"
#include "stage_camera.hpp"

#include <imgui.h>

#include <optional>

// The viewport in Dear ImGui: the preview's picture, the guides and particle
// paths drawn over it, the handles of the block being worked on, and what the
// pointer does over the picture. The window is transparent and sits behind
// every other, so a panel over the picture has the pointer where it is and
// the picture has it everywhere else; a handle has it before the camera does.
// All in pixels.
namespace ajfx::studio {

struct ViewportInputs final {
    // The preview's picture, if there is one yet.
    ImTextureID picture{};
    bool has_picture{false};
    const GuideSet* guides{nullptr};
    const Analysis* analysis{nullptr};
    bool show_guides{true};
    bool show_handles{true};
    bool show_paths{false};
    // The layer being worked on: only its paths are drawn, none without one.
    Uid layer{};
    double frame{0.0};
};

// What the pointer did over the picture this frame, for the camera and the
// picking to act on. Distances are in pixels.
struct ViewportEvents final {
    // A handle is held: the document holds an edit to take back.
    bool handle_held{false};
    bool hovered{false};
    // Left button held and moving: orbit by these.
    bool orbiting{false};
    bool panning{false};
    bool dollying{false};
    ImVec2 drag{};
    // Wheel notches while the pointer is over the picture.
    float wheel{0.0F};
    // A press and release without moving.
    bool clicked{false};
    bool double_clicked{false};
    bool ctrl{false};
    ImVec2 click_at{};
};

class Viewport final {
public:
    // Draws the viewport over `view` (pixels) as seen from `pose`, and writes
    // handle drags into `document`.
    ViewportEvents draw(const gui::Rect& view, const CameraPose& pose, Document& document,
                        const ViewportInputs& inputs, float scale);
    [[nodiscard]] bool dragging() const noexcept { return drag_.has_value(); }
    void reset() noexcept { drag_.reset(); }

private:
    struct Drag final {
        Handle handle;
        Vec3 grab{};
        bool vertical{false};
    };
    bool handles(const StageCamera& camera, const gui::Rect& view, Document& document, const GuideSet& guides,
                 float scale);
    std::optional<Drag> drag_;
};

} // namespace ajfx::studio
