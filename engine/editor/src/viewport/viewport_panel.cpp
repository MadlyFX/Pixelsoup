// ViewportPanel::draw and the phases that set up its frame.

#include "viewport_frame.hpp"

#include <mediaserver/editor/gizmo.hpp>
#include <mediaserver/editor/light_guides.hpp>
#include <mediaserver/editor/operator_widgets.hpp>
#include <mediaserver/gui/rect_cut.hpp>
#include <mediaserver/gui/shapes.hpp>

#include <mediaserver/scene/manipulate.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <tuple>
#include <utility>

namespace mediaserver::editor {

using namespace viewport;

ViewportFrame ViewportPanel::draw(gui::UiContext& ctx, const gui::Rect& bounds,
                                  const rhi::Texture& target,
                                  const scene::Scene& scene,
                                  std::span<const doc::NodeId> selection,
                                  doc::NodeId primary,
                                  const scene::Camera* camera_overlay,
                                  const gui::Rect* interaction_occluder,
                                  float frames_per_second,
                                  const output::FeedMap* feed_map,
                                  doc::NodeId selected_surface_node,
                                  const ViewportProjection* projection,
                                  std::span<const ViewportPlate> plates,
                                  std::span<const ElementInfo> elements) {
    Frame f{*this,         ctx,
            bounds,        target,
            scene,         selection,
            primary,       camera_overlay,
            interaction_occluder, frames_per_second,
            feed_map,      selected_surface_node,
            projection,    plates,
            elements};
    if (!begin_frame(f)) {
        return f.result;
    }
    prepare_surfaces(f);
    prepare_gizmos(f);
    prepare_transform_box(f);
    run_guides(f);
    run_shape_tool(f);
    run_gradient_tool(f);
    update_pivot_drag(f);
    pick_hovered_surface(f);
    resolve_deep_select_menu(f);
    handle_stage_shortcuts(f);
    run_keyboard_transform(f);
    begin_interaction(f);
    handle_press(f);
    drag_surface(f);
    hold_deep_select(f);
    drag_element(f);
    finish_gesture(f);

    // The editor chrome is a clipped 2D pass over the zero-copy scene image.
    // Program stays clean except for selection outlines; Stage adds the spatial
    // guides an operator needs to author the scene.
    ctx.push_clip(shown_image(f.result.image));
    draw_overlays(f);
    draw_layout_overlays(f);
    draw_gizmos(f);
    ctx.pop_clip();
    draw_rulers(f);
    draw_stage_view_menu(ctx, f.view_menu, f.result);
    draw_stage_shading_menu(ctx, f.shading_menu, f.result);

    f.result.deep_select_open = deep_select_open_;
    f.result.deep_candidate_count = deep_candidates_.size();
    return f.result;
}

bool ViewportPanel::begin_frame(Frame& f) {
    auto& frame = f.result;
    auto& ctx = f.ctx;
    auto& bounds = f.bounds;
    auto& target = f.target;
    auto& scene = f.scene;
    auto& frames_per_second = f.frames_per_second;
    auto& projection = f.projection;
    // One lock-free fence per editor frame prevents a completion from becoming
    // visible halfway through a multi-model pick/outline pass.
    model_publication_fence_ = model_cache_.publication_epoch();

    const scene::Canvas interaction_canvas =
        viewport_canvas(scene, projection);
    const auto canvas_width = static_cast<float>(interaction_canvas.width);
    const auto canvas_height = static_cast<float>(interaction_canvas.height);
    if (canvas_width <= 0.0F || canvas_height <= 0.0F) {
        return false;
    }
    const scene::ModelBoundsProvider model_bounds =
        [this](const scene::ModelInstance& model) {
            return model_bounds_for(model);
        };

    gui::Rect image_bounds = bounds;
    if (role_ == ViewportRole::Stage && toolbar_shown_ &&
        bounds.height > stage_toolbar_height) {
        draw_toolbar(ctx,
                     gui::Rect{bounds.x, bounds.y, bounds.width,
                               stage_toolbar_height},
                     frames_per_second);
        image_bounds.y += stage_toolbar_height;
        image_bounds.height -= stage_toolbar_height;
    }

    // The Program view's rulers take a strip off the top and the left.
    constexpr float ruler_top_height = 18.0F;
    constexpr float ruler_left_width = 36.0F;
    if (role_ == ViewportRole::Program && rulers_shown_ &&
        image_bounds.width > 160.0F && image_bounds.height > 120.0F) {
        f.ruler_top = gui::Rect{image_bounds.x + ruler_left_width,
                                image_bounds.y,
                                image_bounds.width - ruler_left_width,
                                ruler_top_height};
        f.ruler_left = gui::Rect{image_bounds.x,
                                 image_bounds.y + ruler_top_height,
                                 ruler_left_width,
                                 image_bounds.height - ruler_top_height};
        image_bounds.x += ruler_left_width;
        image_bounds.width -= ruler_left_width;
        image_bounds.y += ruler_top_height;
        image_bounds.height -= ruler_top_height;
    }
    f.pane = image_bounds;
    f.interaction_canvas = interaction_canvas;
    if (role_ == ViewportRole::Program) {
        navigate_canvas(f);
    }
    // Letterboxed to the canvas, never stretched: a pixel the operator clicks
    // has to be the pixel the renderer drew, or picking lies. A zoomed
    // Program view keeps that shape and is cropped by its pane.
    if (role_ == ViewportRole::Program && !canvas_fitted_) {
        frame.image = canvas_view_.to_screen(
            image_bounds, gui::Rect{0.0F, 0.0F, canvas_width, canvas_height});
        ctx.push_clip(image_bounds);
        ctx.image(frame.image, target);
        ctx.pop_clip();
    } else {
        frame.image =
            ctx.image_fit(image_bounds, target, canvas_width / canvas_height);
    }
    if (frame.image.width <= 0.0F || frame.image.height <= 0.0F) {
        return false;
    }
    canvas_scale_ = frame.image.width / canvas_width;
    // The view's own menu sits over the image's top-right corner, so the
    // pointer over it picks nothing underneath.
    const gui::Rect view_menu =
        role_ == ViewportRole::Stage && projection == nullptr && !drawing_masks_
            ? stage_view_menu_bounds(
                  frame.image,
                  toolbar_menu_width(ctx,
                                     stage_projection_name(stage_projection_)))
            : gui::Rect{};
    const gui::Rect shading_menu = stage_shading_menu_bounds(
        view_menu,
        toolbar_menu_width(ctx, stage_shading_name(stage_shading_)));
    f.interaction_canvas = interaction_canvas;
    f.canvas_width = canvas_width;
    f.canvas_height = canvas_height;
    f.model_bounds = model_bounds;
    f.view_menu = view_menu;
    f.shading_menu = shading_menu;
    return true;
}

void ViewportPanel::prepare_surfaces(Frame& f) {
    auto& frame = f.result;
    auto& ctx = f.ctx;
    auto& scene = f.scene;
    auto& camera_overlay = f.camera_overlay;
    auto& feed_map = f.feed_map;
    auto& selected_surface_node = f.selected_surface_node;
    if (surface_preview_.has_value()) {
        const output::ProjectionSurface* const committed =
            feed_map != nullptr
                ? feed_map->surface_for(surface_preview_->node)
                : nullptr;
        const bool committed_preview =
            committed != nullptr && *committed == *surface_preview_;
        const bool changed_selection =
            selected_surface_node != surface_preview_->node;
        const bool expired = !surface_dragging_ &&
                             ctx.time_seconds() > surface_preview_deadline_;
        if (committed_preview || changed_selection || expired) {
            surface_preview_.reset();
        }
    }

    std::vector<ProjectedSurface> projected_surfaces;
    const output::ProjectionSurface* selected_surface_value =
        feed_map != nullptr
            ? feed_map->surface_for(selected_surface_node)
            : nullptr;
    if (surface_preview_.has_value() &&
        surface_preview_->node == selected_surface_node) {
        selected_surface_value = &surface_preview_.value();
    }
    if (selected_surface_value != nullptr && selected_surface_value->locked) {
        selected_surface_value = nullptr;
        surface_dragging_ = false;
    }
    // A stage carried by the scene camera is drawn where the authored camera
    // puts it, so the operator sees the room the camera is carrying. Its
    // gizmo would move a surface through world coordinates the document does
    // not hold, so on that stage a surface is picked here and placed in stage
    // coordinates from Feed Mapping.
    const bool camera_stage =
        feed_map != nullptr &&
        feed_map->stage.anchor == output::StageAnchor::Camera;
    std::optional<output::StagePose> stage_pose;
    if (camera_stage) {
        selected_surface_value = nullptr;
        surface_dragging_ = false;
        const auto pose = output::camera_stage_pose(
            camera_overlay != nullptr ? *camera_overlay : scene.camera,
            feed_map->stage.eye);
        if (pose) {
            stage_pose = pose.value();
        }
    }
    if (role_ == ViewportRole::Stage && show_overlays_ && feed_map != nullptr &&
        (!camera_stage || stage_pose.has_value())) {
        projected_surfaces.reserve(feed_map->surfaces.size());
        for (const output::ProjectionSurface& surface : feed_map->surfaces) {
            ProjectedSurface projected{};
            projected.surface = &surface;
            projected.display =
                surface_preview_.has_value() &&
                        surface_preview_->node == surface.node
                    ? surface_preview_.value()
                    : surface;
            const output::ProjectionSurface& display = projected.display;
            const auto mesh =
                display.shape == output::ProjectionSurfaceShape::Planar
                    ? std::shared_ptr<const output::ProjectionMesh>{}
                    : projection_mesh_for(display);
            projected.visual = surface_visual(display, mesh.get(), stage_pose);
            project_surface(projected, frame.image, scene);
            projected_surfaces.push_back(std::move(projected));
        }
    }
    f.projected_surfaces = std::move(projected_surfaces);
    f.selected_surface_value = selected_surface_value;
    f.camera_stage = camera_stage;
    f.stage_pose = stage_pose;
}

void ViewportPanel::prepare_gizmos(Frame& f) {
    auto& frame = f.result;
    auto& ctx = f.ctx;
    auto& scene = f.scene;
    auto& selection = f.selection;
    auto& primary = f.primary;
    auto& interaction_occluder = f.interaction_occluder;
    auto& view_menu = f.view_menu;
    auto& shading_menu = f.shading_menu;
    auto& selected_surface_value = f.selected_surface_value;
    const bool primary_locked = scene::element_locked(scene, primary);
    if (primary_locked) {
        dragging_ = false;
    }
    const auto primary_world = primary_locked
                                   ? std::optional<scene::Mat4>{}
                                   : scene::world_for(scene, primary);
    // Blender's median point: what the selection moves, turns and scales
    // about, and where the gizmo sits.
    std::optional<scene::Vec3> pivot;
    if (primary_world.has_value()) {
        scene::Vec3 sum{};
        float count = 0.0F;
        for (const doc::NodeId node : selection) {
            if (scene::element_locked(scene, node)) {
                continue;
            }
            if (const auto world = scene::world_for(scene, node)) {
                sum = sum + scene::translation_of(*world);
                count += 1.0F;
            }
        }
        pivot = count > 0.0F ? sum * (1.0F / count)
                             : scene::translation_of(*primary_world);
    }
    // A moved pivot belongs to the selection it was set for.
    if (pivot_override_.has_value() &&
        !std::equal(selection.begin(), selection.end(),
                    pivot_selection_.begin(), pivot_selection_.end())) {
        pivot_override_.reset();
    }
    if (pivot.has_value() && pivot_override_.has_value()) {
        pivot = *pivot_override_;
    }
    const std::array<scene::Vec3, 3U> primary_axes =
        primary_world.has_value() ? world_axes_of(*primary_world)
                                  : world_basis;
    const scene::Vec3 view_forward =
        scene::normalize(scene.camera.target - scene.camera.eye);
    if (keyboard_.active && !pivot.has_value()) {
        // The selection went away under a keyboard transform.
        keyboard_.active = false;
        frame.transform_ended = true;
        frame.transform_cancelled = true;
    }
    // A gizmo drag keeps the tool it began with; scale always works on the
    // element's own axes (see TransformOrientation).
    const TransformTool gizmo_tool =
        dragging_ && drag_axis_ != GizmoAxis::Screen ? drag_tool_ : tool_;
    const std::array<scene::Vec3, 3U>& gizmo_axes =
        gizmo_tool == TransformTool::Scale ||
                orientation_ == TransformOrientation::Local
            ? primary_axes
            : world_basis;
    const GizmoGeometry scene_gizmo =
        role_ == ViewportRole::Stage && show_gizmo_ && !keyboard_.active &&
                selected_surface_value == nullptr && pivot.has_value()
            ? gizmo_geometry(frame.image, scene, *pivot, gizmo_axes,
                             gizmo_tool == TransformTool::Rotate)
            : GizmoGeometry{};
    GizmoGeometry surface_gizmo =
        role_ == ViewportRole::Stage && show_gizmo_ &&
                selected_surface_value != nullptr
            ? gizmo_geometry(frame.image, scene,
                             selected_surface_value->geometry.center)
            : GizmoGeometry{};
    const bool occluded =
        (interaction_occluder != nullptr &&
         interaction_occluder->contains(ctx.pointer_x(), ctx.pointer_y())) ||
        view_menu.contains(ctx.pointer_x(), ctx.pointer_y()) ||
        shading_menu.contains(ctx.pointer_x(), ctx.pointer_y());
    GizmoAxis hovered_scene_axis = GizmoAxis::None;
    if (!occluded && !measuring_) {
        switch (gizmo_tool) {
        case TransformTool::Move:
            hovered_scene_axis =
                hit_gizmo(scene_gizmo, ctx.pointer_x(), ctx.pointer_y());
            break;
        case TransformTool::Rotate:
            hovered_scene_axis = hit_rotate_gizmo(scene_gizmo, ctx.pointer_x(),
                                                  ctx.pointer_y());
            break;
        case TransformTool::Scale:
            hovered_scene_axis = hit_scale_gizmo(scene_gizmo, ctx.pointer_x(),
                                                 ctx.pointer_y());
            break;
        }
    }
    GizmoAxis hovered_surface_axis =
        occluded ? GizmoAxis::None
                 : hit_gizmo(surface_gizmo, ctx.pointer_x(), ctx.pointer_y());
    const bool hovered_surface_yaw =
        !occluded && hovered_surface_axis == GizmoAxis::None &&
        hit_yaw_ring(surface_gizmo, ctx.pointer_x(), ctx.pointer_y());
    if (hovered_surface_yaw) {
        hovered_surface_axis = GizmoAxis::Yaw;
    }
    f.primary_world = primary_world;
    f.pivot = pivot;
    f.primary_axes = primary_axes;
    f.view_forward = view_forward;
    f.gizmo_tool = gizmo_tool;
    f.scene_gizmo = scene_gizmo;
    f.surface_gizmo = surface_gizmo;
    f.occluded = occluded;
    f.hovered_scene_axis = hovered_scene_axis;
    f.hovered_surface_axis = hovered_surface_axis;
    f.hovered_surface_yaw = hovered_surface_yaw;
}

void ViewportPanel::update_pivot_drag(Frame& f) {
    auto& frame = f.result;
    auto& ctx = f.ctx;
    auto& scene = f.scene;
    auto& selection = f.selection;
    auto& projection = f.projection;
    auto& interaction_canvas = f.interaction_canvas;
    auto& canvas_width = f.canvas_width;
    auto& scene_gizmo = f.scene_gizmo;
    auto& occluded = f.occluded;
    // A middle drag from the gizmo's centre moves the pivot, snapping like
    // the Measure tool; a middle click there puts it back. Unreal does this
    // with Alt+middle, which pans here.
    if (role_ == ViewportRole::Stage && projection == nullptr && !occluded &&
        scene_gizmo.visible && !keyboard_.active &&
        ctx.middle_pointer_pressed()) {
        const float dx = ctx.pointer_x() - scene_gizmo.origin.x;
        const float dy = ctx.pointer_y() - scene_gizmo.origin.y;
        if (dx * dx + dy * dy <= 12.0F * 12.0F) {
            pivot_dragging_ = true;
            pivot_moved_ = false;
            pivot_press_x_ = ctx.pointer_x();
            pivot_press_y_ = ctx.pointer_y();
        }
    }
    if (pivot_dragging_) {
        frame.pivot_editing = true;
        if (ctx.middle_pointer_down()) {
            const float dx = ctx.pointer_x() - pivot_press_x_;
            const float dy = ctx.pointer_y() - pivot_press_y_;
            pivot_moved_ = pivot_moved_ || dx * dx + dy * dy > 9.0F;
            if (pivot_moved_) {
                const CanvasPoint at = to_canvas(
                    frame.image, interaction_canvas, ctx.pointer_x(),
                    ctx.pointer_y());
                if (const auto point = measure_point_at(
                        scene, at.x, at.y,
                        12.0F * canvas_width / frame.image.width)) {
                    pivot_override_ = point->position;
                    pivot_selection_.assign(selection.begin(), selection.end());
                    drag_snap_target_ = point;
                }
            }
        } else {
            if (!pivot_moved_) {
                pivot_override_.reset();
            }
            pivot_dragging_ = false;
            drag_snap_target_.reset();
        }
    }
}

void ViewportPanel::pick_hovered_surface(Frame& f) {
    auto& ctx = f.ctx;
    auto& projected_surfaces = f.projected_surfaces;
    auto& occluded = f.occluded;
    const ProjectedSurface* hit_surface = nullptr;
    if (!occluded) {
        float closest_surface = surface_hit_pixels;
        for (const ProjectedSurface& projected : projected_surfaces) {
            if (projected.surface->locked) {
                continue;
            }
            const float distance = surface_hit_distance(
                projected, ctx.pointer_x(), ctx.pointer_y());
            if (distance < closest_surface) {
                closest_surface = distance;
                hit_surface = &projected;
            }
        }
    }
    f.hit_surface = hit_surface;
}

void ViewportPanel::resolve_deep_select_menu(Frame& f) {
    auto& frame = f.result;
    auto& ctx = f.ctx;
    // The depth chooser is a context menu: drawn over every panel, floating
    // ones included, and owning the pointer and keyboard while it is open.
    const gui::UiId deep_menu = gui::ui_id(id_, 0xD33DU);
    f.deep_menu = deep_menu;
    const auto offer_deep_menu = [&f] { return f.offer_deep_menu(); };
    if (deep_select_open_) {
        // A choice lands on the frame the menu closes, so it is read before
        // the closed menu is taken as dismissed.
        if (const auto chosen = offer_deep_menu()) {
            if (*chosen < deep_candidates_.size()) {
                frame.pressed = true;
                frame.picked = deep_candidates_[*chosen];
            }
            deep_select_open_ = false;
            deep_candidates_.clear();
        } else if (!ctx.menu_open(deep_menu)) {
            deep_select_open_ = false;
            deep_candidates_.clear();
        }
    }
}

std::optional<std::size_t> ViewportPanel::Frame::offer_deep_menu() {
    std::vector<std::string> labels;
    labels.reserve(panel.deep_candidates_.size());
    for (const doc::NodeId candidate : panel.deep_candidates_) {
        labels.push_back(viewport_deep_select_label(scene, candidate,
                                                    elements));
    }
    const std::vector<std::string_view> options(labels.begin(),
                                                labels.end());
    return ctx.context_menu(deep_menu, options);
}

} // namespace mediaserver::editor
