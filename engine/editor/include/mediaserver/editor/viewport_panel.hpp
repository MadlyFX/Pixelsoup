#pragma once

#include <mediaserver/editor/preview_resolution.hpp>

#include <mediaserver/editor/element.hpp>
#include <mediaserver/editor/masks_panel.hpp>
#include <mediaserver/editor/measurement.hpp>
#include <mediaserver/editor/model_surface_cache.hpp>
#include <mediaserver/editor/stage_placement.hpp>
#include <mediaserver/editor/stage_view.hpp>
#include <mediaserver/editor/transform_tool.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/output/feed_map.hpp>
#include <mediaserver/render/mapped_scene.hpp>
#include <mediaserver/render/model_cache.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/manipulate.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/raycast.hpp>
#include <mediaserver/scene/scene.hpp>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

// The chooser uses the same authored name as Scene and Inspector, falling back
// to a scene kind and node number when no element metadata is available.
[[nodiscard]] std::string viewport_deep_select_label(
    const scene::Scene& scene, doc::NodeId node,
    std::span<const ElementInfo> elements);

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

// Which handle owns the current drag. Screen is the direct-on-object drag;
// X/Y/Z are the gizmo's axis arrows, rings or scale handles for the active
// tool; View is the outer screen-space ring that rotates about the view axis
// or scales uniformly.
enum class GizmoAxis : core::u8 {
    None,
    Screen,
    X,
    Y,
    Z,
    // Surface calibration has a dedicated, screen-space ring for rotation
    // about stage-up.
    Yaw,
    View,
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
    // Stage canvas, or any Alt+press on Stage: preserve selection and reserve
    // the drag for camera navigation.
    Navigate,
};

// What one viewport frame reported to the shell above it.
struct ViewportFrame final {
    // Where the render actually landed, letterboxed inside the panel.
    gui::Rect image{};
    bool hovered{};
    // The viewport owns the pointer, including the release frame. The Stage
    // shell uses this for bare-canvas camera navigation while picked media
    // use the translation path below.
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
    // The selection's move, rotate or scale this frame, measured like
    // `translation` from where the gesture began. Set by gizmo and direct
    // drags (with `dragging`) and by a keyboard transform. Already snapped.
    std::optional<TransformDelta> transform{};

    // A Blender-style keyboard transform (G, R or S over the Stage) began,
    // and the shell should capture the selection as it stands now. It ends
    // with `transform_ended`; `transform_cancelled` then says the operator
    // backed out and `transform` carries the identity delta that restores it.
    bool transform_began{};
    bool transform_ended{};
    bool transform_cancelled{};
    // A keyboard transform owns the pointer and keyboard this frame, the
    // right button included: camera navigation stands down.
    bool keyboard_transform{};

    // The viewport capture ended this frame, whether it held an element drag,
    // a gizmo handle, or a bare-canvas gesture.
    bool released{};

    // A Ctrl+Alt drag on Stage drew a box and let go: every unlocked element
    // it touched. `marquee_adds` when Shift was held, to add rather than
    // replace the selection.
    std::optional<std::vector<doc::NodeId>> marquee;
    bool marquee_adds{};
    // Alt+drag began on a gizmo handle: copy the selection in place first, as
    // Unreal does, and the drag carries the originals away.
    bool duplicate{};
    // A middle drag from the gizmo's centre is moving the pivot: the middle
    // button is not the camera's until it is let go.
    bool pivot_editing{};
    // The Stage view's menu chose a projection this frame.
    std::optional<StageProjection> projection_choice;
    // The Stage view's menu asked for four views (true) or one (false).
    std::optional<bool> layout_choice;
    // The Stage view's shading menu chose how this view shades.
    std::optional<render::ViewShading> shading_choice;
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
    // The renderer's measure of one normalized unit for the scene this view
    // shows (scene::mask_source_extent). Shape rotation, feather and
    // expansion are drawn in it rather than re-measured from the corners,
    // which a crop or warp may have rescaled.
    scene::MaskCoordinateExtent extent{};

    bool operator==(const MaskViewportSourceMapping&) const = default;
};

struct MaskViewportNormalizedPoint final {
    float x{};
    float y{};

    bool operator==(const MaskViewportNormalizedPoint&) const = default;
};

// Exact forward/inverse mapping used by both drawing and interaction. A
// SourceLayer point maps through the projective transform taking the unit
// square onto the quad - how a flat surface lands on screen, and how the
// renderer traces each pixel back onto the layer. A missing or degenerate
// quad, or a point beyond the plane's horizon, returns no value; it never
// falls back to canvas space. AuthoredCanvas ignores source_mapping by
// contract.
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

// Blocks until no surface hierarchy is queued or building.
[[nodiscard]] bool wait_for_viewport_surface_hierarchies_for_test(
    ViewportPanel& panel) noexcept;

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
                       std::span<const ViewportPlate> plates = {},
                       std::span<const ElementInfo> elements = {});

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
    [[nodiscard]] PreviewResolution resolution() const noexcept { return resolution_; }
    void set_resolution(PreviewResolution resolution) noexcept {
        if (valid(resolution)) resolution_ = resolution;
    }
    // The preview-resolution menu: a screen icon and the current choice. Its
    // width follows that choice.
    [[nodiscard]] float resolution_control_width(
        const gui::UiContext& ctx) const noexcept;
    void draw_resolution_control(gui::UiContext& ctx, const gui::Rect& bounds);
    // Surface and vertex snapping for a Move drag: true when it placed the
    // element exactly, so the grid step leaves `delta` alone.
    [[nodiscard]] bool snap_drag(const gui::UiContext& ctx,
                                 const scene::Scene& scene,
                                 std::span<const doc::NodeId> selection,
                                 const ViewportFrame& frame,
                                 TransformDelta& delta);
    [[nodiscard]] bool grid_visible() const noexcept { return show_grid_; }
    [[nodiscard]] bool overlays_visible() const noexcept {
        return show_overlays_;
    }
    [[nodiscard]] bool gizmo_visible() const noexcept { return show_gizmo_; }
    // Every element's bounding box outlined, selected or not.
    [[nodiscard]] bool bounds_visible() const noexcept { return show_bounds_; }
    void set_bounds_visible(bool visible) noexcept { show_bounds_ = visible; }
    // Nodes outlined in the caution colour beside the selection, without
    // selecting them: what a hovered History step touched. Empty for none.
    void set_highlight(std::span<const doc::NodeId> nodes) {
        highlight_.assign(nodes.begin(), nodes.end());
    }
    // What the Stage view's menu, in the top-right corner of its image, shows
    // as this view's projection. The shell owns the view and sets it before
    // `draw`; a choice from the menu comes back in the frame.
    [[nodiscard]] StageProjection stage_projection() const noexcept {
        return stage_projection_;
    }
    void set_stage_projection(StageProjection projection) noexcept {
        stage_projection_ = projection;
    }
    // Whether the Stage is showing four views, so the menu offers the other
    // layout.
    void set_stage_four_views(bool four) noexcept { stage_four_views_ = four; }
    // What the shading menu, left of the view menu, shows for this view.
    [[nodiscard]] render::ViewShading stage_shading() const noexcept {
        return stage_shading_;
    }
    void set_stage_shading(render::ViewShading shading) noexcept {
        stage_shading_ = shading;
    }
    // A Stage panel draws its toolbar across the top of `draw`'s bounds
    // unless this is off: in four views one toolbar serves them all, drawn by
    // `draw_toolbar`.
    void set_toolbar_shown(bool shown) noexcept { toolbar_shown_ = shown; }
    void draw_toolbar(gui::UiContext& ctx, const gui::Rect& toolbar,
                      float frames_per_second);
    // Takes `other`'s tool, orientation, snapping, measuring and display
    // toggles, so views that share one toolbar behave as one.
    void copy_tools_from(const ViewportPanel& other) noexcept;
    // Builds triangle hierarchies on `other`'s worker, so views of one scene
    // build each model once.
    void share_surfaces_with(const ViewportPanel& other) noexcept {
        surface_cache_ = other.surface_cache_;
    }
    // The gizmo's tool, its orientation, and increment snapping. G, R and S
    // choose the tool as they start a keyboard transform; the Stage toolbar
    // sets all three.
    [[nodiscard]] TransformTool tool() const noexcept { return tool_; }
    void set_tool(TransformTool tool) noexcept { tool_ = tool; }
    [[nodiscard]] TransformOrientation orientation() const noexcept {
        return orientation_;
    }
    void set_orientation(TransformOrientation orientation) noexcept {
        orientation_ = orientation;
    }
    [[nodiscard]] bool snapping() const noexcept { return snapping_; }
    void set_snapping(bool snapping) noexcept { snapping_ = snapping; }
    // Surface snapping, as Unreal's: an element dragged by its body rides
    // over whatever is under the pointer, resting on it.
    [[nodiscard]] bool surface_snapping() const noexcept {
        return surface_snapping_;
    }
    void set_surface_snapping(bool snapping) noexcept {
        surface_snapping_ = snapping;
    }
    // The move, rotate and scale increments the toolbar chose.
    [[nodiscard]] const SnapSteps& snap_steps() const noexcept {
        return snap_steps_;
    }
    void set_snap_steps(const SnapSteps& steps) noexcept { snap_steps_ = steps; }
    // A keyboard transform is running: it owns the keyboard and pointer
    // until Enter or a left click confirms it, or Escape or a right click
    // cancels it.
    [[nodiscard]] bool transforming() const noexcept {
        return keyboard_.active || swallow_pointer_;
    }
    // Abandons a keyboard transform without reporting it: for when the shell
    // can no longer author into the Stage at all.
    void cancel_transform() noexcept { keyboard_.active = false; }
    [[nodiscard]] bool deep_select_open() const noexcept {
        return deep_select_open_;
    }

    // The world box around `nodes` as this viewport picks them: drawables by
    // their unit bounds, imported models by their own, and anything else with
    // a world position (a light, a group) as that point. Empty when none of
    // them is in `scene`.
    [[nodiscard]] std::optional<WorldBounds>
    world_bounds(const scene::Scene& scene, std::span<const doc::NodeId> nodes);

    // Where the line of sight through canvas pixel (x, y) first meets a
    // surface of `scene`, locked elements included. Imported models are hit
    // on their triangles once a background worker has built their hierarchy,
    // and on their bounds until then; nothing here waits or builds.
    [[nodiscard]] std::optional<scene::SurfaceHit>
    surface_at(const scene::Scene& scene, float canvas_x, float canvas_y,
               const scene::SurfaceFilter& counts = {});

    // The point the Measure tool would take at canvas pixel (x, y): the
    // nearest vertex of the surface there within `reach` canvas pixels, else
    // the nearest edge midpoint, else the surface point itself; with no
    // surface, the stage floor. Vertices come from the same background
    // hierarchies as `surface_at`; until one is ready the surface point
    // stands in.
    [[nodiscard]] std::optional<MeasurePoint>
    measure_point_at(const scene::Scene& scene, float canvas_x, float canvas_y,
                     float reach, const scene::SurfaceFilter& counts = {});

    // The Measure tool (M over Stage, or the toolbar). Leaving it clears the
    // measurement; nothing it shows is ever authored.
    [[nodiscard]] bool measuring() const noexcept { return measuring_; }
    void set_measuring(bool measuring) noexcept {
        measuring_ = measuring;
        if (!measuring) {
            measure_.clear();
            measure_hover_.reset();
        }
    }
    [[nodiscard]] const MeasureChain& measurement() const noexcept {
        return measure_;
    }

    // The pivot moved by a middle drag from the gizmo's centre, while the
    // selection it was set for is still selected.
    [[nodiscard]] std::optional<scene::Vec3> pivot_override() const noexcept {
        return pivot_override_;
    }

    // Local bounds of the model at `source`, once the model cache has it.
    // Asking starts the decode of a model nothing in the scene uses yet.
    [[nodiscard]] std::optional<scene::ModelLocalBounds>
    model_source_bounds(const std::string& source);

    // Where dropped assets will land, drawn over this viewport's image: each
    // one's bounds at its placement and a mark where it rests on the surface.
    void draw_placement_preview(gui::UiContext& ctx, const gui::Rect& image,
                                const scene::Scene& scene,
                                std::span<const StagePlacement> placements,
                                std::string_view caption);

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

    // Blender's axis constraint while a keyboard transform runs: one axis, or
    // the plane that excludes it (Shift+X/Y/Z).
    enum class Constraint : core::u8 {
        None,
        Axis,
        Plane,
    };

    struct KeyboardTransform final {
        bool active{};
        TransformTool tool{TransformTool::Move};
        Constraint constraint{Constraint::None};
        std::size_t axis{};
        TransformOrientation orientation{TransformOrientation::Global};
        NumericEntry entry;
        scene::Vec3 pivot{};
        std::array<scene::Vec3, 3U> local_axes{};
        // Pointer travel in panel pixels, scaled down while Shift is held.
        float press_x{};
        float press_y{};
        float pointer_x{};
        float pointer_y{};
        float last_x{};
        float last_y{};
        float turns{};
    };

    // Applies the snap toggle (inverted while Ctrl is held) to a delta.
    [[nodiscard]] TransformDelta snapped(const gui::UiContext& ctx,
                                         TransformDelta delta) const noexcept;
    // The running keyboard transform's constraint lines and readout.
    void draw_keyboard_transform(gui::UiContext& ctx, gui::UiDrawList& list,
                                 const gui::Rect& image,
                                 const scene::Scene& scene,
                                 gui::UiPoint centre,
                                 std::optional<float> reading) const;

    gui::UiId id_{};
    ViewportRole role_{ViewportRole::Program};
    TransformTool tool_{TransformTool::Move};
    TransformOrientation orientation_{TransformOrientation::Global};
    bool snapping_{};
    SnapSteps snap_steps_{};
    KeyboardTransform keyboard_;
    // A keyboard transform ended on a button press: the press and its release
    // belong to it, not to picking or the camera.
    bool swallow_pointer_{};
    bool show_grid_{true};
    PreviewResolution resolution_{PreviewResolution::Full};
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
    // Rotate and scale gizmo drags, in panel pixels.
    TransformTool drag_tool_{TransformTool::Move};
    scene::Vec3 drag_pivot_{};
    std::array<scene::Vec3, 3U> drag_basis_{};
    float drag_centre_x_{};
    float drag_centre_y_{};
    float drag_last_x_{};
    float drag_last_y_{};
    float drag_turns_{};
    float drag_press_reach_{};
    output::ProjectionSurface surface_drag_start_{};
    std::optional<output::ProjectionSurface> surface_preview_;
    double surface_preview_deadline_{};

    bool deep_select_armed_{};
    bool deep_select_open_{};
    double deep_press_time_{};
    float deep_press_pointer_x_{};
    float deep_press_pointer_y_{};
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
    // Triangle hierarchies for `surface_at`, built on their own worker -
    // shared by the Stage's views (see `share_surfaces_with`).
    std::shared_ptr<ModelSurfaceCache> surface_cache_{
        std::make_shared<ModelSurfaceCache>()};
    // Generated primitive meshes, so vertex snapping reaches a box's corners.
    std::map<scene::MeshKey, std::shared_ptr<const scene::MeshData>>
        primitive_meshes_;
    [[nodiscard]] std::shared_ptr<const scene::MeshData>
    primitive_mesh(scene::MeshKey key);

    bool measuring_{};
    MeasureChain measure_;
    std::optional<MeasurePoint> measure_hover_;

    // The element a Move drag began on, as it stood at the press, for
    // surface and vertex snapping. `drag_grab_` is where on it the pointer
    // took hold; `drag_source_vertex_` its vertex nearest there, found the
    // first time V is held.
    bool surface_snapping_{};
    bool show_bounds_{};
    StageProjection stage_projection_{StageProjection::Perspective};
    bool stage_four_views_{};
    render::ViewShading stage_shading_{render::ViewShading::Lit};
    bool toolbar_shown_{true};
    bool drawing_masks_{};
    void draw_stage_view_menu(gui::UiContext& ctx, const gui::Rect& menu,
                              ViewportFrame& frame);
    void draw_stage_shading_menu(gui::UiContext& ctx, const gui::Rect& menu,
                                 ViewportFrame& frame);
    doc::NodeId drag_node_{};
    scene::Mat4 drag_start_world_{};
    scene::ModelLocalBounds drag_start_bounds_{};
    std::optional<scene::Vec3> drag_grab_;
    std::optional<scene::Vec3> drag_source_vertex_;
    std::optional<MeasurePoint> drag_snap_target_;

    // The pivot an operator moved for this selection, until it changes.
    std::optional<scene::Vec3> pivot_override_;
    std::vector<doc::NodeId> pivot_selection_;
    std::vector<doc::NodeId> highlight_;
    bool pivot_dragging_{};
    bool pivot_moved_{};
    float pivot_press_x_{};
    float pivot_press_y_{};

    bool marquee_active_{};
    float marquee_x_{};
    float marquee_y_{};
    [[nodiscard]] std::vector<doc::NodeId>
    elements_in_marquee(const scene::Scene& scene, const gui::Rect& image,
                        const gui::Rect& box);
    [[nodiscard]] scene::ModelLocalBounds
    element_local_bounds(const scene::Scene& scene, doc::NodeId node);
    [[nodiscard]] std::optional<scene::Vec3>
    nearest_element_vertex(const scene::Scene& scene, doc::NodeId node,
                           const scene::Mat4& world, scene::Vec3 near);

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
    friend bool detail::wait_for_viewport_surface_hierarchies_for_test(
        ViewportPanel& panel) noexcept;
};

} // namespace mediaserver::editor
