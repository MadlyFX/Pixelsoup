#pragma once

#include <mediaserver/editor/masks_panel.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/output/feed_map.hpp>
#include <mediaserver/render/mapped_scene.hpp>
#include <mediaserver/render/model_cache.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/manipulate.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <variant>
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

enum class EmptyCanvasInteraction : core::u8 {
    None,
    // Program/preview canvas: clear editor-local scene selection.
    ClearSelection,
    // Stage canvas: preserve selection and reserve the drag for camera orbit.
    Orbit,
};

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
    EmptyCanvasInteraction empty_canvas{EmptyCanvasInteraction::None};

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

// One canvas-space corner of a drawable's normalized source plane. Source
// masks use the four corners below in UL, UR, BR, BL order; group masks ignore
// them and map directly across the authored canvas. Keeping this projection
// explicit prevents a group mask from being silently reinterpreted per child.
struct MaskViewportCanvasPoint final {
    float x{};
    float y{};

    bool operator==(const MaskViewportCanvasPoint&) const = default;
};

struct MaskViewportSourceMapping final {
    std::array<MaskViewportCanvasPoint, 4U> corners{};

    bool operator==(const MaskViewportSourceMapping&) const = default;
};

struct MaskViewportNormalizedPoint final {
    float x{};
    float y{};

    bool operator==(const MaskViewportNormalizedPoint&) const = default;
};

// Exact forward/inverse mapping used by both drawing and interaction. A
// missing or degenerate SourceLayer quad returns no value; it never falls back
// to canvas space. AuthoredCanvas ignores source_mapping by contract.
[[nodiscard]] std::optional<gui::UiPoint> mask_normalized_to_viewport(
    const gui::Rect& image, scene::Canvas canvas,
    scene::MaskCoordinateSpace coordinate_space,
    const MaskViewportSourceMapping* source_mapping, float x,
    float y) noexcept;

[[nodiscard]] std::optional<MaskViewportNormalizedPoint>
mask_viewport_to_normalized(
    const gui::Rect& image, scene::Canvas canvas,
    scene::MaskCoordinateSpace coordinate_space,
    const MaskViewportSourceMapping* source_mapping, float pointer_x,
    float pointer_y) noexcept;

struct MaskViewportInput final {
    MasksPanelTarget target{};
    std::span<const scene::MaskInstance> masks;
    MasksPanelSelection selection{};
    std::optional<MaskViewportSourceMapping> source_layer_mapping;
    bool pen_active{};
    bool coverage_visible{};
    bool authoring_enabled{true};
};

enum class MaskViewportHit : core::u8 {
    None,
    WholeMask,
    Path,
    Point,
    InHandle,
    OutHandle,
    ResizeTopLeft,
    ResizeTopRight,
    ResizeBottomRight,
    ResizeBottomLeft,
    Rotate,
};

struct MaskViewportPointEditIntent final {
    scene::SceneId scene{};
    doc::NodeId target{};
    doc::NodeId mask{};
    doc::NodeId point{};
    scene::MaskPointPatch patch{};

    bool operator==(const MaskViewportPointEditIntent&) const = default;
};

struct MaskViewportTranslateIntent final {
    scene::SceneId scene{};
    doc::NodeId target{};
    doc::NodeId mask{};
    float delta_x{};
    float delta_y{};

    bool operator==(const MaskViewportTranslateIntent&) const = default;
};

struct MaskViewportShapeEditIntent final {
    scene::SceneId scene{};
    doc::NodeId target{};
    doc::NodeId mask{};
    scene::MaskBounds bounds{};
    float rotation_turns{};

    bool operator==(const MaskViewportShapeEditIntent&) const = default;
};

struct MaskViewportPointInsertIntent final {
    scene::SceneId scene{};
    doc::NodeId target{};
    doc::NodeId mask{};
    std::size_t index{};
    scene::MaskPointGeometry geometry{};

    bool operator==(const MaskViewportPointInsertIntent&) const = default;
};

struct MaskViewportCoverageIntent final {
    scene::SceneId scene{};
    doc::NodeId target{};
    bool visible{};

    bool operator==(const MaskViewportCoverageIntent&) const = default;
};

using MaskViewportIntent = std::variant<
    MaskViewportPointEditIntent, MaskViewportTranslateIntent,
    MaskViewportShapeEditIntent, MaskViewportPointInsertIntent,
    MaskViewportCoverageIntent>;

struct MaskViewportFrame final {
    gui::Rect image{};
    // draw_masks() is a distinct interaction domain: ordinary scene object
    // selection is intentionally unavailable and the viewport says so.
    bool object_selection_locked{true};
    bool mapping_valid{};
    bool hovered{};
    bool held{};
    bool dragging{};
    bool released{};
    bool cancelled{};
    MaskViewportHit hit{MaskViewportHit::None};
    bool selection_changed{};
    MasksPanelSelection selection{};
    std::optional<MaskViewportIntent> intent;
};

// Resolves a pointer through the exact letterboxed image rectangle and reads
// that rendered RGBA8 pixel back from the GPU. Readback is intentionally a
// one-shot operator gesture, never part of the ordinary viewport frame path.
[[nodiscard]] std::optional<gui::UiColor> sample_viewport_color(
    rhi::Device& device, const rhi::Texture& target,
    const gui::Rect& image, float pointer_x, float pointer_y) noexcept;

class ViewportPanel;

namespace detail {

[[nodiscard]] bool wait_for_viewport_projection_surface_for_test(
    ViewportPanel& panel, doc::NodeId surface) noexcept;

} // namespace detail

// One plate the wall is showing: where it is in canvas pixels, and how wide
// the camera authored its feather. Parallel to the scene's tracked cameras, in
// the same composite order, so an operator sees the same stack the wall has.
struct ViewportPlate final {
    scene::CanvasRect rect{};
    core::u32 soft_edge_px{};

    bool operator==(const ViewportPlate&) const = default;
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
                       const ViewportProjection* projection = nullptr,
                       // One entry per plate on the wall, in the scene's
                       // composite order. Passed in rather than derived here
                       // because the rectangles come from the wall's physical
                       // geometry, and because every node and this overlay
                       // must be looking at the same ones.
                       std::span<const ViewportPlate> plates = {});

    // Draws the ordinary image/chrome with scene manipulation blocked, then
    // overlays and interacts with first-class masks. It is intentionally
    // separate from draw(): object and surface selection cannot leak into the
    // mask/point domain. This is authoring chrome only; it does not claim that
    // a group-mask GPU composite exists.
    //
    // `projection` is the same forward projection draw() takes, so the overlay
    // registers with whatever picture the panel is actually showing. On Stage
    // that is the authored camera canvas; on Program Output it is the final
    // connector image, which is the one an operator is looking at when they
    // decide a matte is in the wrong place. Passing it also selects the canvas
    // mask coordinates resolve against, so the two views cannot disagree about
    // where a point is.
    MaskViewportFrame draw_masks(
        gui::UiContext& ctx, const gui::Rect& bounds,
        const rhi::Texture& render_target, const scene::Scene& scene,
        const MaskViewportInput& input,
        const scene::Camera* camera_overlay = nullptr,
        float frames_per_second = 60.0F,
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

    // Project-generation boundary. Background workers are process-owned, so
    // this drops only panel-local observations and never joins asset work.
    void reset_project_state() noexcept;

    // Exact-node operator/control mutation. It records an asynchronous new
    // generation only when this panel has already observed that surface.
    [[nodiscard]] bool
    refresh_projection_surface(doc::NodeId surface) noexcept;

  private:
    struct CachedModelBounds final {
        scene::ModelLocalBounds bounds{};
        core::u64 stamp{};
    };

    struct CachedProjectionState final {
        render::OutputProjectionCache cache;
        core::u64 stamp{};
    };

    [[nodiscard]] std::optional<scene::ModelLocalBounds>
    model_bounds_for(const scene::ModelInstance& model);
    [[nodiscard]] core::u64 next_model_bounds_stamp() noexcept;
    [[nodiscard]] core::u64 next_projection_mesh_stamp() noexcept;
    [[nodiscard]] std::shared_ptr<const output::ProjectionMesh>
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
    scene::Vec3 axis_world_{};
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
    // Draw observes only already-decoded immutable assets. Both residency
    // caches perform file/model work on their bounded background workers;
    // contention, a cold source, or a sticky failure therefore leaves the
    // exact-source stale bounds/mesh in place (or returns no geometry).
    render::ModelCache model_cache_;
    core::u64 model_publication_fence_{};
    core::u64 model_bounds_stamp_{};
    std::unordered_map<std::string, CachedModelBounds> model_bounds_cache_;
    core::u64 projection_mesh_stamp_{};
    std::unordered_map<core::u64, CachedProjectionState>
        projection_mesh_cache_;

    bool mask_gesture_active_{};
    bool mask_gesture_cancelled_{};
    MaskViewportHit mask_gesture_hit_{MaskViewportHit::None};
    scene::SceneId mask_gesture_scene_{};
    doc::NodeId mask_gesture_target_{};
    doc::NodeId mask_gesture_mask_{};
    std::optional<doc::NodeId> mask_gesture_point_;
    MaskViewportNormalizedPoint mask_press_{};
    scene::MaskInstance mask_gesture_start_;
    scene::MaskInstance mask_gesture_preview_;
    std::size_t mask_insert_index_{};
    scene::MaskPointGeometry mask_insert_geometry_{};

    friend bool detail::wait_for_viewport_projection_surface_for_test(
        ViewportPanel& panel, doc::NodeId surface) noexcept;
};

} // namespace mediaserver::editor
