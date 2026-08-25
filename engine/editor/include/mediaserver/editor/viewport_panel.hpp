#pragma once

#include <mediaserver/doc/ids.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>

#include <optional>

// The viewport panel, on the bespoke UI rather than Dear ImGui.
//
// This is the first of the editor's panels to come off the throwaway shell. It
// draws the engine's own render target straight into the widget tree - no copy,
// no readback, which is the whole reason the UI shares the engine's RHI - and
// turns a click and a drag inside it into the same geometry the ImGui viewport
// produced, through the same pure math in scene::manipulate.
//
// It authors nothing and owns no scene state. A frame reports what the operator
// pointed at and how far they dragged; the shell above decides what that means
// and emits the document intents. That split is what lets the panel be tested
// with no window, no GPU, and no document - and what makes this a port of the
// panel rather than a second editor.
namespace mediaserver::editor {

// A pointer position resolved into canvas pixels. `inside` is false when the
// pointer is off the rendered image - in the letterbox bars, or outside the
// panel - in which case the coordinates are still meaningful (they run past the
// canvas) but nothing should be picked.
struct CanvasPoint final {
    bool inside{};
    float x{};
    float y{};
};

// Maps a pointer to canvas pixels through the placement the render landed at.
// Pure, so the mapping every click depends on can be checked to the pixel with
// nothing open.
[[nodiscard]] CanvasPoint to_canvas(
    const gui::Rect& image,
    scene::Canvas canvas,
    float pointer_x,
    float pointer_y) noexcept;

// What one viewport frame reported to the shell above it.
struct ViewportFrame final {
    // Where the render actually landed, letterboxed inside the panel.
    gui::Rect image{};
    bool hovered{};
    CanvasPoint pointer{};

    // A press landed inside the image this frame. `picked` is the clip under it,
    // or empty for bare canvas - which the shell reads as "clear the selection".
    bool pressed{};
    std::optional<doc::NodeId> picked{};

    // A drag is under way and moved this frame. `translation` is the world-space
    // delta from where the press began, not from the last frame, so a shell that
    // adds it to the position captured at press start is exact however the
    // pointer wandered getting there.
    bool dragging{};
    scene::Vec3 translation{};

    // The drag ended this frame.
    bool released{};
};

class ViewportPanel final {
public:
    explicit ViewportPanel(gui::UiId id) noexcept : id_(id) {}

    // Draws `target` into `bounds` and runs the frame's interaction against
    // `scene` - the committed scene at the current show frame, the same value
    // the renderer drew, so what the operator clicks is what they see.
    ViewportFrame draw(
        gui::UiContext& ctx,
        const gui::Rect& bounds,
        const rhi::Texture& target,
        const scene::Scene& scene);

    [[nodiscard]] bool dragging() const noexcept { return dragging_; }

private:
    gui::UiId id_{};
    bool dragging_{};
    // Where the drag began, in canvas pixels, and the world point it acts at.
    // Held across frames because a drag is a delta from its own start.
    float press_x_{};
    float press_y_{};
    scene::Vec3 press_world_{};
};

} // namespace mediaserver::editor
