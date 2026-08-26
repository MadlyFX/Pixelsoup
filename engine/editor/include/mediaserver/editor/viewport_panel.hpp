#pragma once

#include <mediaserver/doc/ids.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/output/feed_map.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/manipulate.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>

#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

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

// Program is the authored camera output; Stage is the free editor camera with
// grid, light/camera overlays, and a transform gizmo.
enum class ViewportRole : core::u8 {
    Program,
    Stage,
};

// Which translation handle owns the current drag. Screen is the existing
// direct-on-object drag; X/Y/Z are the constrained gizmo arrows.
enum class GizmoAxis : core::u8 {
    None,
    Screen,
    X,
    Y,
    Z,
    // Surface calibration has a dedicated, screen-space ring for rotation
    // about stage-up. Scene elements do not consume this value yet.
    Yaw,
};

// Optional final-output projection for an interactive Program viewport. The
// texture may be a crop, asymmetric physical view, or UV-warped mesh; this
// projector maps scene geometry into those final connector pixels so picking,
// direct manipulation, and selection outlines stay registered to the image.
struct ViewportProjection final {
    scene::Canvas canvas{};
    scene::Vec3 forward{0.0F, 0.0F, -1.0F};
    scene::Vec3 up{0.0F, 1.0F, 0.0F};
    scene::SceneProjector project;
};

// Maps a pointer to canvas pixels through the placement the render landed at.
// Pure, so the mapping every click depends on can be checked to the pixel with
// nothing open.
[[nodiscard]] CanvasPoint to_canvas(const gui::Rect& image,
                                    scene::Canvas canvas, float pointer_x,
                                    float pointer_y) noexcept;

// What one viewport frame reported to the shell above it.
struct ViewportFrame final {
    // Where the render actually landed, letterboxed inside the panel.
    gui::Rect image{};
    bool hovered{};
    // The viewport owns the pointer, including the release frame. The Stage
    // shell uses this for a bare-canvas orbit while picked media use the
    // translation path below.
    bool held{};
    CanvasPoint pointer{};

    // A press landed inside the image this frame. `picked` is the front-most
    // object, clip, model, or light marker under it, or empty for bare canvas.
    bool pressed{};
    std::optional<doc::NodeId> picked{};

    // Projection surfaces are project calibration, not scene elements. Keep
    // their selection and authoring result in a separate lane so selecting an
    // LED face cannot accidentally make it look like an ordinary scene node.
    std::optional<doc::NodeId> surface_picked{};
    std::optional<output::ProjectionSurface> surface_edit{};
    bool surface_dragging{};

    // A stationary long press over overlapping scene elements opens a chooser
    // instead of guessing which depth the operator meant. These fields expose
    // the state for shell hints and deterministic headless coverage.
    bool deep_select_open{};
    std::size_t deep_candidate_count{};

    // A drag is under way and moved this frame. `translation` is the
    // world-space delta from where the press began, not from the last frame, so
    // a shell that adds it to the position captured at press start is exact
    // however the pointer wandered getting there.
    bool dragging{};
    scene::Vec3 translation{};
    GizmoAxis gizmo_axis{GizmoAxis::None};

    // The viewport capture ended this frame, whether it held an element drag,
    // a gizmo handle, or a bare-canvas gesture.
    bool released{};
};

class ViewportPanel final {
  public:
    explicit ViewportPanel(gui::UiId id,
                           ViewportRole role = ViewportRole::Program) noexcept
        : id_(id), role_(role) {}

    // Draws `target` into `bounds` and runs the frame's interaction against
    // `scene` - the committed scene at the current show frame, the same value
    // the renderer drew, so what the operator clicks is what they see.
    ViewportFrame draw(gui::UiContext& ctx, const gui::Rect& bounds,
                       const rhi::Texture& target, const scene::Scene& scene,
                       std::span<const doc::NodeId> selection = {},
                       doc::NodeId primary = {},
                       const scene::Camera* camera_overlay = nullptr,
                       const gui::Rect* interaction_occluder = nullptr,
                       float frames_per_second = 60.0F,
                       const output::FeedMap* feed_map = nullptr,
                       doc::NodeId selected_surface = {},
                       const ViewportProjection* projection = nullptr);

    [[nodiscard]] bool dragging() const noexcept { return dragging_; }
    [[nodiscard]] bool grid_visible() const noexcept { return show_grid_; }
    [[nodiscard]] bool overlays_visible() const noexcept {
        return show_overlays_;
    }
    [[nodiscard]] bool gizmo_visible() const noexcept { return show_gizmo_; }
    [[nodiscard]] bool deep_select_open() const noexcept {
        return deep_select_open_;
    }

  private:
    struct CachedProjectionMesh final {
        std::string source;
        float scale{1.0F};
        core::i64 source_stamp{};
        std::optional<output::ProjectionMesh> mesh;
    };

    [[nodiscard]] std::optional<scene::ModelLocalBounds>
    model_bounds_for(const scene::ModelInstance& model);
    [[nodiscard]] const output::ProjectionMesh*
    projection_mesh_for(const output::ProjectionSurface& surface);

    gui::UiId id_{};
    ViewportRole role_{ViewportRole::Program};
    bool show_grid_{true};
    bool show_overlays_{true};
    bool show_gizmo_{true};
    bool dragging_{};
    bool surface_dragging_{};
    GizmoAxis drag_axis_{GizmoAxis::None};
    // Where the drag began, in canvas pixels, and the world point it acts at.
    // Held across frames because a drag is a delta from its own start.
    float press_x_{};
    float press_y_{};
    scene::Vec3 press_world_{};
    float axis_canvas_x_{};
    float axis_canvas_y_{};
    float axis_world_length_{};
    float press_pointer_x_{};
    output::ProjectionSurface surface_drag_start_{};
    std::optional<output::ProjectionSurface> surface_preview_;
    double surface_preview_deadline_{};

    bool deep_select_armed_{};
    bool deep_select_open_{};
    double deep_press_time_{};
    float deep_press_pointer_x_{};
    float deep_press_pointer_y_{};
    float deep_popup_anchor_x_{};
    float deep_popup_anchor_y_{};
    std::vector<doc::NodeId> deep_candidates_;
    // Successful imports are immutable for a panel's lifetime, matching the
    // renderer's exact-source residency cache. Failed paths are not retained,
    // so a file that appears later becomes pickable without recreating the UI.
    std::unordered_map<std::string, scene::ModelLocalBounds>
        model_bounds_cache_;
    std::unordered_map<core::u64, CachedProjectionMesh>
        projection_mesh_cache_;
};

} // namespace mediaserver::editor
