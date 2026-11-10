#pragma once

// One ViewportPanel::draw, and one draw_masks, as an object. Each builds its
// Frame from its arguments and runs its phases in order; state one phase
// hands to another lives here, everything else stays local to its phase.
// Internal to the editor library.

#include "viewport_mask_support.hpp"
#include "viewport_support.hpp"

#include <mediaserver/editor/gizmo.hpp>
#include <mediaserver/editor/viewport_panel.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/scene/manipulate.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace mediaserver::editor {

struct ViewportPanel::Frame final {
    ViewportPanel& panel;
    gui::UiContext& ctx;
    const gui::Rect& bounds;
    const rhi::Texture& target;
    const scene::Scene& scene;
    std::span<const doc::NodeId> selection;
    doc::NodeId primary;
    const scene::Camera* camera_overlay;
    const gui::Rect* interaction_occluder;
    float frames_per_second;
    const output::FeedMap* feed_map;
    doc::NodeId selected_surface_node;
    const ViewportProjection* projection;
    std::span<const ViewportPlate> plates;
    std::span<const ElementInfo> elements;
    ViewportFrame result{};

    // Set by begin_frame(): the canvas picking resolves against, how models
    // are bounded, and the Stage view and shading menus over the image.
    scene::Canvas interaction_canvas{};
    float canvas_width{};
    float canvas_height{};
    scene::ModelBoundsProvider model_bounds;
    gui::Rect view_menu{};
    gui::Rect shading_menu{};

    // Set by prepare_surfaces(): the feed map's surfaces as drawn, the one
    // selected (previewed while dragged), and the stage the camera carries.
    std::vector<viewport::ProjectedSurface> projected_surfaces;
    const output::ProjectionSurface* selected_surface_value{};
    bool camera_stage{};
    std::optional<output::StagePose> stage_pose;

    // Set by prepare_gizmos(): the selection's pivot and axes, both gizmos,
    // and what the pointer is over.
    std::optional<scene::Mat4> primary_world;
    std::optional<scene::Vec3> pivot;
    std::array<scene::Vec3, 3U> primary_axes{};
    scene::Vec3 view_forward{};
    TransformTool gizmo_tool{TransformTool::Move};
    GizmoGeometry scene_gizmo{};
    GizmoGeometry surface_gizmo{};
    bool occluded{};
    GizmoAxis hovered_scene_axis{GizmoAxis::None};
    GizmoAxis hovered_surface_axis{GizmoAxis::None};
    bool hovered_surface_yaw{};

    // Set by begin_frame(): the pane the image is placed in, and whether
    // the Hand or Zoom tool owns the pointer this frame.
    gui::Rect pane{};
    bool navigating{};

    // Set by prepare_transform_box(): the primary element's box in the
    // world, for the press that takes hold of one of its handles.
    std::optional<ViewportPanel::LayerBox> layer_box;

    // Set by run_shape_tool(): what the shape tool has drawn so far, in
    // panel pixels - its outlines, the Pen's points, and whether a click
    // here would close the Pen's curve.
    std::vector<std::vector<gui::UiPoint>> tool_outlines;
    std::vector<gui::UiPoint> tool_points;
    bool tool_closes{};

    // Set by pick_hovered_surface(): the unlocked surface under the pointer.
    const viewport::ProjectedSurface* hit_surface{};

    // Set by resolve_deep_select_menu(): the depth chooser's menu.
    gui::UiId deep_menu{};

    // Set by run_keyboard_transform(): where its readout sits and what it
    // reads.
    gui::UiPoint keyboard_centre{};
    std::optional<float> keyboard_reading;

    // Set by begin_interaction(): the image's pointer interaction.
    gui::Interaction it{};

    // Offers the depth chooser's candidates; a choice once one is made.
    std::optional<std::size_t> offer_deep_menu();
};

struct ViewportPanel::MaskFrame final {
    gui::UiContext& ctx;
    const gui::Rect& bounds;
    const rhi::Texture& render_target;
    const scene::Scene& scene;
    const MaskViewportInput& input;
    const scene::Camera* camera_overlay;
    float frames_per_second;
    const ViewportProjection* projection;
    MaskViewportFrame result{};

    // Set by begin_mask_frame(): the target's masks in order, the selected
    // one, the COVERAGE button, the image's interaction and each mask's path.
    scene::Canvas mask_canvas{};
    std::vector<const scene::MaskInstance*> masks;
    const scene::MaskInstance* selected_mask{};
    gui::Rect coverage_button{};
    gui::Interaction coverage_interaction{};
    gui::Interaction interaction{};
    std::optional<MaskViewportNormalizedPoint> normalized_pointer;
    std::vector<viewport::MaskPath> paths;

    // Set by pick_mask(): what a press would take hold of.
    viewport::MaskPick pick{};

    // Set by drag_mask(): the gesture's translation and whether it moved.
    float gesture_dx{};
    float gesture_dy{};
    bool gesture_changed{};

    // The target's mask `id`, or null.
    [[nodiscard]] const scene::MaskInstance* find_mask(doc::NodeId id) const;
};

} // namespace mediaserver::editor
