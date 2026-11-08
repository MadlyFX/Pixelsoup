// The editor chrome drawn over the viewport image.

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

void ViewportPanel::draw_overlays(Frame& f) {
    auto& frame = f.result;
    auto& ctx = f.ctx;
    auto& scene = f.scene;
    auto& selection = f.selection;
    auto& primary = f.primary;
    auto& camera_overlay = f.camera_overlay;
    auto& selected_surface_node = f.selected_surface_node;
    auto& projection = f.projection;
    auto& plates = f.plates;
    auto& model_bounds = f.model_bounds;
    auto& projected_surfaces = f.projected_surfaces;
    gui::UiDrawList& list = ctx.draw_list();
    if (role_ == ViewportRole::Stage && show_grid_) {
        const float step = draw_grid(list, frame.image, scene);
        const std::string text = "Grid " + format_metres(step);
        // The grid's spacing in the bottom-right corner.
        gui::Rect foot = gui::inset(frame.image,
                                    gui::Padding{0.0F, 0.0F, 0.0F, 4.0F});
        foot = gui::cut_bottom(foot, 22.0F);
        ctx.label(gui::cut_right(foot, ctx.font_width(text) + 14.0F), text,
                  ctx.theme().text_muted);
    }
    if (role_ == ViewportRole::Stage && show_overlays_) {
        for (const scene::SceneScreen& screen : scene.screens) {
            draw_screen(list, frame.image, scene, screen,
                        selected(selection, screen.node));
        }
        for (const ProjectedSurface& projected : projected_surfaces) {
            const bool is_selected =
                projected.surface->node == selected_surface_node;
            draw_surface(list, projected,
                         is_selected);
            const scene::CanvasProjection centre = scene::project_to_canvas(
                scene.camera, scene.canvas, projected.display.geometry.center);
            if (centre.visible) {
                const gui::UiPoint label_at =
                    to_ui(frame.image, scene.canvas, centre);
                const std::string fallback_name =
                    "SURFACE " +
                    std::to_string(projected.surface->node.value());
                ctx.label(gui::Rect{label_at.x + 8.0F, label_at.y - 18.0F,
                                    190.0F, 18.0F},
                          projected.display.name.empty()
                              ? std::string_view{fallback_name}
                              : std::string_view{projected.display.name},
                          is_selected ? surface_selected : surface_color);
                if (projected.visual.fallback && is_selected) {
                    ctx.label(gui::Rect{label_at.x + 8.0F, label_at.y + 8.0F,
                                        190.0F, 18.0F},
                              "CALIBRATION BOUNDS (MESH UNAVAILABLE)",
                              surface_fallback);
                }
            }
        }
        for (const scene::TrackedCameraView& tracked :
             scene.tracked_cameras) {
            draw_tracked_camera_overlay(list, frame.image, scene, tracked);
        }
        // Every camera of the scene on Stage, so a selected one has its gizmo
        // on something visible. A hand-built scene that lists none still
        // shows the viewpoint it was given.
        for (const scene::SceneCamera& placed : scene.cameras) {
            draw_camera_overlay(
                list, frame.image, scene, placed.camera,
                std::find(selection.begin(), selection.end(), placed.node) !=
                    selection.end());
        }
        if (camera_overlay != nullptr && scene.cameras.empty()) {
            draw_camera_overlay(list, frame.image, scene, *camera_overlay);
        }
        draw_light_overlays(ctx, list, frame.image, scene, selection, primary);
    }
    // The plate is a region of the *image*, not a thing in the scene, so it
    // belongs on Program Output as much as on Stage - more, in fact, since
    // Program is the picture an operator is judging when they decide whether
    // the margin is wide enough.
    if (show_overlays_) {
        for (const ViewportPlate& plate : plates) {
            draw_inner_frustum_overlay(list, frame.image, scene, plate);
        }
    }
    if (role_ == ViewportRole::Stage && show_bounds_) {
        const gui::UiColor bounds_color{0.45F, 0.78F, 0.86F, 0.55F};
        for (const auto& object : scene.instances) {
            wire_box(list, frame.image, scene, object.world, bounds_color,
                     primitive_local_bounds(object.mesh.primitive), projection);
        }
        for (const auto& clip : scene.media) {
            wire_box(list, frame.image, scene, clip.world, bounds_color,
                     primitive_local_bounds(clip.mesh.primitive), projection);
        }
        for (const auto& model : scene.models) {
            wire_box(list, frame.image, scene, model.world, bounds_color,
                     model_bounds_for(model).value_or(scene::ModelLocalBounds{}),
                     projection);
        }
    }
    draw_selection_outlines(list, frame.image, scene, selection, primary,
                            model_bounds, projection);
    if (!highlight_.empty()) {
        draw_selection_outlines(list, frame.image, scene, highlight_, {},
                                model_bounds, projection,
                                ctx.theme().caution);
    }
    // The selection's size along the world axes, in metres.
    if (role_ == ViewportRole::Stage && show_overlays_ && projection == nullptr &&
        !selection.empty()) {
        if (const auto box = world_bounds(scene, selection)) {
            const scene::Vec3 size = box->maximum - box->minimum;
            if (size.x > 0.0F || size.y > 0.0F || size.z > 0.0F) {
                const std::string text =
                    "W " + format_metres(size.x) + "   H " +
                    format_metres(size.y) + "   D " + format_metres(size.z);
                // The size in the bottom-left corner.
                gui::Rect foot = gui::inset(frame.image, 6.0F);
                foot = gui::cut_bottom(foot, 22.0F);
                const gui::Rect badge =
                    gui::cut_left(foot, ctx.font_width(text) + 14.0F);
                ctx.panel(badge, gui::UiColor{0.04F, 0.05F, 0.06F, 0.82F});
                ctx.label(gui::inset(badge, gui::Padding{7.0F, 0.0F, 0.0F, 0.0F}),
                          text, ctx.theme().text);
                ctx.tooltip(badge,
                            "The selection's bounding box along the world X "
                            "(W), Y (H) and Z (D) axes.");
            }
        }
    }
}

void ViewportPanel::draw_gizmos(Frame& f) {
    auto& frame = f.result;
    auto& ctx = f.ctx;
    auto& scene = f.scene;
    auto& projection = f.projection;
    auto& selected_surface_value = f.selected_surface_value;
    auto& gizmo_tool = f.gizmo_tool;
    auto& scene_gizmo = f.scene_gizmo;
    auto& surface_gizmo = f.surface_gizmo;
    auto& hovered_scene_axis = f.hovered_scene_axis;
    auto& hovered_surface_axis = f.hovered_surface_axis;
    auto& hovered_surface_yaw = f.hovered_surface_yaw;
    auto& keyboard_centre = f.keyboard_centre;
    auto& keyboard_reading = f.keyboard_reading;
    gui::UiDrawList& list = ctx.draw_list();
    if (role_ == ViewportRole::Stage && show_gizmo_) {
        if (selected_surface_value != nullptr) {
            const GizmoAxis translation_hover =
                hovered_surface_axis == GizmoAxis::Yaw
                    ? GizmoAxis::None
                    : hovered_surface_axis;
            const GizmoAxis translation_active =
                surface_dragging_ && drag_axis_ != GizmoAxis::Yaw
                    ? drag_axis_
                    : GizmoAxis::None;
            draw_gizmo(list, ctx.theme(), surface_gizmo, translation_hover,
                       translation_active);
            draw_yaw_ring(list, ctx.theme(), surface_gizmo,
                          hovered_surface_yaw,
                          surface_dragging_ && drag_axis_ == GizmoAxis::Yaw);
            if (surface_gizmo.visible) {
                ctx.label(gui::Rect{surface_gizmo.origin.x + 54.0F,
                                    surface_gizmo.origin.y - 10.0F, 34.0F,
                                    18.0F},
                          "YAW", surface_yaw);
            }
        } else {
            const GizmoAxis active =
                dragging_ && drag_axis_ != GizmoAxis::Screen ? drag_axis_
                                                             : GizmoAxis::None;
            const GizmoAxis hovered =
                dragging_ ? GizmoAxis::None : hovered_scene_axis;
            switch (gizmo_tool) {
            case TransformTool::Move:
                draw_gizmo(list, ctx.theme(), scene_gizmo, hovered, active);
                break;
            case TransformTool::Rotate:
                draw_rotate_gizmo(list, ctx.theme(), scene_gizmo, hovered,
                                  active);
                break;
            case TransformTool::Scale:
                draw_scale_gizmo(list, ctx.theme(), scene_gizmo, hovered,
                                 active);
                break;
            }
            if (scene_gizmo.visible && active != GizmoAxis::None &&
                drag_tool_ != TransformTool::Move) {
                line(list, gui::UiPoint{drag_centre_x_, drag_centre_y_},
                     gui::UiPoint{ctx.pointer_x(), ctx.pointer_y()}, 1.0F,
                     ctx.theme().gizmo_view);
            }
        }
    }
    if (keyboard_.active) {
        draw_keyboard_transform(ctx, list, frame.image, scene, keyboard_centre,
                                keyboard_reading);
    }
    if (measuring_ && role_ == ViewportRole::Stage && projection == nullptr) {
        draw_measurements(ctx, list, frame.image, scene, measure_,
                          measure_hover_);
    }
    if (marquee_active_) {
        const float x0 = std::min(marquee_x_, ctx.pointer_x());
        const float y0 = std::min(marquee_y_, ctx.pointer_y());
        const float x1 = std::max(marquee_x_, ctx.pointer_x());
        const float y1 = std::max(marquee_y_, ctx.pointer_y());
        list.add_rect(x0, y0, x1 - x0, y1 - y0,
                      gui::UiColor{0.35F, 0.62F, 1.0F, 0.12F});
        const gui::UiColor edge{0.45F, 0.70F, 1.0F, 0.95F};
        line(list, gui::UiPoint{x0, y0}, gui::UiPoint{x1, y0}, 1.0F, edge);
        line(list, gui::UiPoint{x1, y0}, gui::UiPoint{x1, y1}, 1.0F, edge);
        line(list, gui::UiPoint{x1, y1}, gui::UiPoint{x0, y1}, 1.0F, edge);
        line(list, gui::UiPoint{x0, y1}, gui::UiPoint{x0, y0}, 1.0F, edge);
    }
    if (drag_snap_target_.has_value()) {
        draw_measurements(ctx, list, frame.image, scene, MeasureChain{},
                          drag_snap_target_, false);
    }
}

} // namespace mediaserver::editor
