// Pointer presses and drags over the viewport image.

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

void ViewportPanel::begin_interaction(Frame& f) {
    auto& frame = f.result;
    auto& ctx = f.ctx;
    auto& scene = f.scene;
    auto& projection = f.projection;
    auto& interaction_canvas = f.interaction_canvas;
    auto& canvas_width = f.canvas_width;
    auto& occluded = f.occluded;
    const gui::Rect interaction_bounds =
        occluded || frame.keyboard_transform ? gui::Rect{} : frame.image;
    const gui::Interaction it = ctx.grab(id_, interaction_bounds);
    frame.hovered = it.hovered;
    frame.held = it.held;
    frame.pointer =
        to_canvas(frame.image, interaction_canvas, ctx.pointer_x(),
                  ctx.pointer_y());
    measure_hover_.reset();
    if (measuring_ && role_ == ViewportRole::Stage && projection == nullptr &&
        it.hovered) {
        // Twelve interface pixels of reach, as Unreal's vertex snap feels.
        measure_hover_ = measure_point_at(
            scene, frame.pointer.x, frame.pointer.y,
            12.0F * canvas_width / frame.image.width);
    }
    f.it = it;
}

void ViewportPanel::handle_press(Frame& f) {
    auto& frame = f.result;
    auto& ctx = f.ctx;
    auto& scene = f.scene;
    auto& primary = f.primary;
    auto& projection = f.projection;
    auto& canvas_width = f.canvas_width;
    auto& model_bounds = f.model_bounds;
    auto& selected_surface_value = f.selected_surface_value;
    auto& camera_stage = f.camera_stage;
    auto& primary_world = f.primary_world;
    auto& pivot = f.pivot;
    auto& view_forward = f.view_forward;
    auto& gizmo_tool = f.gizmo_tool;
    auto& scene_gizmo = f.scene_gizmo;
    auto& surface_gizmo = f.surface_gizmo;
    auto& hovered_scene_axis = f.hovered_scene_axis;
    auto& hovered_surface_axis = f.hovered_surface_axis;
    auto& hit_surface = f.hit_surface;
    auto& it = f.it;
    const auto scene_hits_under_pointer = [&]() {
        const float light_radius = 12.0F * canvas_width / frame.image.width;
        return projection != nullptr && projection->project
                   ? scene::pick_scene_all(
                         scene, frame.pointer.x, frame.pointer.y,
                         projection->project, model_bounds, light_radius,
                         role_ == ViewportRole::Stage && show_overlays_)
                   : scene::pick_scene_all(
                         scene, scene.canvas, frame.pointer.x,
                         frame.pointer.y, model_bounds, light_radius,
                         role_ == ViewportRole::Stage && show_overlays_);
    };
    const auto prepare_deep_select =
        [&](const std::vector<scene::ScenePick>& hits,
            bool plain_pick_available) {
            deep_candidates_.reserve(hits.size());
            for (const scene::ScenePick& hit : hits) {
                deep_candidates_.push_back(hit.node);
            }
            if (hits.size() > 1U ||
                (!plain_pick_available && !hits.empty())) {
                deep_select_armed_ = true;
                deep_press_time_ = ctx.time_seconds();
                deep_press_pointer_x_ = ctx.pointer_x();
                deep_press_pointer_y_ = ctx.pointer_y();
            }
        };

    if (it.pressed) {
        frame.pressed = true;
        drag_axis_ = GizmoAxis::None;
        dragging_ = false;
        surface_dragging_ = false;
        deep_select_armed_ = false;
        deep_select_open_ = false;
        deep_candidates_.clear();

        // On Stage, Alt+left orbits and left added to a right or middle drag
        // pans: camera chords that never pick, or drag, what is underneath.
        // Two Unreal chords come first: Ctrl+Alt draws a selection box, and
        // Alt on a gizmo handle duplicates what it moves.
        const bool other_buttons =
            ctx.secondary_pointer_down() || ctx.middle_pointer_down();
        const bool marquee_chord = role_ == ViewportRole::Stage &&
                                   ctx.ctrl_held() && ctx.alt_held() &&
                                   !other_buttons && projection == nullptr;
        const bool duplicate_chord =
            role_ == ViewportRole::Stage && ctx.alt_held() &&
            !ctx.ctrl_held() && !other_buttons && !measuring_ &&
            hovered_scene_axis != GizmoAxis::None && pivot.has_value();
        const bool camera_chord =
            role_ == ViewportRole::Stage && !marquee_chord && !duplicate_chord &&
            (ctx.alt_held() || other_buttons);
        if (marquee_chord) {
            marquee_active_ = true;
            marquee_x_ = ctx.pointer_x();
            marquee_y_ = ctx.pointer_y();
        } else if (camera_chord) {
            // Left to the shell's camera navigation.
        } else if (measuring_) {
            if (measure_hover_.has_value()) {
                measure_.click(*measure_hover_, ctx.shift_held());
            }
        } else if (hovered_surface_axis != GizmoAxis::None &&
                   selected_surface_value != nullptr) {
            frame.surface_picked = selected_surface_value->node;
            surface_drag_start_ = *selected_surface_value;
            drag_axis_ = hovered_surface_axis;
            press_world_ = selected_surface_value->geometry.center;
            press_x_ = frame.pointer.x;
            press_y_ = frame.pointer.y;
            press_pointer_x_ = ctx.pointer_x();
            if (hovered_surface_axis != GizmoAxis::Yaw) {
                const std::size_t index = axis_index(hovered_surface_axis);
                axis_canvas_x_ = surface_gizmo.canvas_dx[index];
                axis_canvas_y_ = surface_gizmo.canvas_dy[index];
                axis_world_length_ = surface_gizmo.world_length;
                axis_world_ = surface_gizmo.world_axes[index];
            }
            surface_dragging_ = true;
        } else if (hit_surface != nullptr) {
            frame.surface_picked = hit_surface->surface->node;
            surface_drag_start_ = hit_surface->display;
            drag_axis_ = GizmoAxis::Screen;
            press_world_ = hit_surface->display.geometry.center;
            press_x_ = frame.pointer.x;
            press_y_ = frame.pointer.y;
            press_pointer_x_ = ctx.pointer_x();
            // Picked, not dragged, on a stage the camera carries.
            surface_dragging_ = !camera_stage;
        } else if (hovered_scene_axis != GizmoAxis::None &&
                   pivot.has_value()) {
            frame.picked = primary;
            frame.duplicate = duplicate_chord;
            drag_axis_ = hovered_scene_axis;
            drag_tool_ = gizmo_tool;
            drag_node_ = primary;
            drag_start_world_ = primary_world.value_or(scene::identity());
            drag_start_bounds_ = element_local_bounds(scene, primary);
            drag_grab_ = *pivot;
            drag_source_vertex_.reset();
            press_world_ = *pivot;
            drag_pivot_ = *pivot;
            drag_basis_ = scene_gizmo.world_axes;
            drag_centre_x_ = scene_gizmo.origin.x;
            drag_centre_y_ = scene_gizmo.origin.y;
            drag_last_x_ = ctx.pointer_x();
            drag_last_y_ = ctx.pointer_y();
            drag_turns_ = 0.0F;
            axis_world_ = -view_forward;
            if (is_xyz(hovered_scene_axis)) {
                const std::size_t index = axis_index(hovered_scene_axis);
                axis_canvas_x_ = scene_gizmo.canvas_dx[index];
                axis_canvas_y_ = scene_gizmo.canvas_dy[index];
                axis_world_length_ = scene_gizmo.world_length;
                axis_world_ = scene_gizmo.world_axes[index];
            }
            // A scale handle follows the pointer: the factor is how far along
            // the handle (or out from the centre, for the ring) it now is,
            // over how far it was when pressed.
            const float from_x = ctx.pointer_x() - scene_gizmo.origin.x;
            const float from_y = ctx.pointer_y() - scene_gizmo.origin.y;
            drag_press_reach_ = std::sqrt(from_x * from_x + from_y * from_y);
            if (is_xyz(hovered_scene_axis)) {
                const std::size_t index = axis_index(hovered_scene_axis);
                const float handle_x =
                    scene_gizmo.ends[index].x - scene_gizmo.origin.x;
                const float handle_y =
                    scene_gizmo.ends[index].y - scene_gizmo.origin.y;
                const float handle =
                    std::sqrt(handle_x * handle_x + handle_y * handle_y);
                drag_press_reach_ =
                    handle > 0.0F
                        ? (from_x * handle_x + from_y * handle_y) / handle
                        : 0.0F;
            }
            drag_press_reach_ = std::max(drag_press_reach_, 1.0F);
            press_x_ = frame.pointer.x;
            press_y_ = frame.pointer.y;
            dragging_ = true;
            // The gizmo is drawn over the selected element. It owns a moving
            // press, but a stationary hold must still be able to inspect the
            // stack beneath it; otherwise selecting the front item makes the
            // depth chooser unreachable on the next press.
            prepare_deep_select(scene_hits_under_pointer(), true);
        } else {
            // A plain press takes the nearest element wholly in front of the
            // camera; one the camera is partly inside (a floor, a wall, a
            // room) leaves the press to camera navigation. Holding still still
            // offers every element under the pointer, those included.
            const std::vector<scene::ScenePick> hits =
                scene_hits_under_pointer();
            const auto front = std::find_if(
                hits.begin(), hits.end(), [](const scene::ScenePick& hit) {
                    return !hit.crosses_camera;
                });
            // A crossing-only hit leaves a plain press to camera navigation,
            // but holding that press must still offer the hit in the chooser.
            prepare_deep_select(hits, front != hits.end());
            if (front != hits.end()) {
                frame.picked = front->node;
                const auto world = scene::world_for(scene, front->node);
                if (world.has_value()) {
                    press_world_ = scene::translation_of(*world);
                    press_x_ = frame.pointer.x;
                    press_y_ = frame.pointer.y;
                    drag_axis_ = GizmoAxis::Screen;
                    drag_tool_ = TransformTool::Move;
                    drag_pivot_ = press_world_;
                    drag_basis_ = orientation_ == TransformOrientation::Local
                                      ? world_axes_of(*world)
                                      : world_basis;
                    drag_node_ = front->node;
                    drag_start_world_ = *world;
                    drag_start_bounds_ = element_local_bounds(scene, front->node);
                    const doc::NodeId grabbed = front->node;
                    const auto grab = projection == nullptr
                                          ? surface_at(scene, frame.pointer.x,
                                                       frame.pointer.y,
                                                       [grabbed](doc::NodeId node) {
                                                           return node == grabbed;
                                                       })
                                          : std::nullopt;
                    drag_grab_ = grab.has_value()
                                     ? grab->position
                                     : scene::translation_of(*world);
                    drag_source_vertex_.reset();
                    dragging_ = true;
                }
            }
        }
        if (!marquee_chord && (camera_chord || !measuring_) &&
            !frame.picked.has_value() && !frame.surface_picked.has_value()) {
            frame.empty_canvas =
                role_ == ViewportRole::Stage
                    ? EmptyCanvasInteraction::Navigate
                    : EmptyCanvasInteraction::ClearSelection;
        }
    }
}

void ViewportPanel::drag_surface(Frame& f) {
    auto& frame = f.result;
    auto& ctx = f.ctx;
    auto& scene = f.scene;
    auto& projected_surfaces = f.projected_surfaces;
    auto& stage_pose = f.stage_pose;
    auto& surface_gizmo = f.surface_gizmo;
    auto& it = f.it;
    if (it.held && surface_dragging_) {
        output::ProjectionSurface edited = surface_drag_start_;
        const float dx = frame.pointer.x - press_x_;
        const float dy = frame.pointer.y - press_y_;
        bool changed = false;
        if (drag_axis_ == GizmoAxis::Yaw) {
            const float yaw_delta =
                (ctx.pointer_x() - press_pointer_x_) / 360.0F;
            if (yaw_delta != 0.0F) {
                const scene::Vec3 stage_up{0.0F, 1.0F, 0.0F};
                edited.geometry.right = scene::rotate_around(
                    surface_drag_start_.geometry.right, stage_up, yaw_delta);
                edited.geometry.up = scene::rotate_around(
                    surface_drag_start_.geometry.up, stage_up, yaw_delta);
                changed = true;
            }
        } else if (dx != 0.0F || dy != 0.0F) {
            scene::Vec3 translation{};
            if (drag_axis_ == GizmoAxis::Screen) {
                translation = scene::screen_drag_to_translation(
                    scene.camera, scene.canvas, press_world_, dx, dy);
            } else {
                const float axis_pixels_squared =
                    axis_canvas_x_ * axis_canvas_x_ +
                    axis_canvas_y_ * axis_canvas_y_;
                if (axis_pixels_squared > 0.0F) {
                    const float along =
                        (dx * axis_canvas_x_ + dy * axis_canvas_y_) /
                        axis_pixels_squared;
                    translation =
                        axis_world_ * (along * axis_world_length_);
                }
            }
            edited.geometry.center =
                surface_drag_start_.geometry.center + translation;
            changed = translation != scene::Vec3{};
        }
        if (changed) {
            frame.surface_dragging = true;
            surface_preview_ = edited;
            surface_preview_deadline_ = ctx.time_seconds() + 1.0;
            frame.surface_edit = edited;
            frame.gizmo_axis = drag_axis_;
            surface_gizmo = gizmo_geometry(frame.image, scene,
                                           edited.geometry.center);
            for (ProjectedSurface& projected : projected_surfaces) {
                if (projected.surface->node != edited.node) {
                    continue;
                }
                projected.display = edited;
                const auto mesh =
                    edited.shape == output::ProjectionSurfaceShape::Planar
                        ? std::shared_ptr<const output::ProjectionMesh>{}
                        : projection_mesh_for(edited);
                projected.visual =
                    surface_visual(edited, mesh.get(), stage_pose);
                project_surface(projected, frame.image, scene);
                break;
            }
        }
    }
}

void ViewportPanel::hold_deep_select(Frame& f) {
    auto& ctx = f.ctx;
    auto& deep_menu = f.deep_menu;
    auto& it = f.it;
    const auto offer_deep_menu = [&f] { return f.offer_deep_menu(); };
    if (it.held && deep_select_armed_) {
        const float pointer_dx = ctx.pointer_x() - deep_press_pointer_x_;
        const float pointer_dy = ctx.pointer_y() - deep_press_pointer_y_;
        const float travel_squared =
            pointer_dx * pointer_dx + pointer_dy * pointer_dy;
        const float limit_squared =
            deep_select_motion_pixels * deep_select_motion_pixels;
        if (travel_squared > limit_squared) {
            deep_select_armed_ = false;
        } else if (ctx.time_seconds() - deep_press_time_ >=
                   deep_select_hold_seconds) {
            deep_select_armed_ = false;
            deep_select_open_ = true;
            ctx.open_context_menu(deep_menu, deep_press_pointer_x_ + 12.0F,
                                  deep_press_pointer_y_ + 12.0F);
            static_cast<void>(offer_deep_menu());
            dragging_ = false;
            drag_axis_ = GizmoAxis::None;
        }
    }
}

void ViewportPanel::drag_element(Frame& f) {
    auto& frame = f.result;
    auto& ctx = f.ctx;
    auto& scene = f.scene;
    auto& selection = f.selection;
    auto& projection = f.projection;
    auto& view_forward = f.view_forward;
    auto& it = f.it;
    if (it.held && dragging_) {
        const float dx = frame.pointer.x - press_x_;
        const float dy = frame.pointer.y - press_y_;
        // With several candidates, a tiny stationary movement remains part of
        // the long-press gesture. Once travel crosses the threshold the exact
        // same press becomes a normal drag from its original world point.
        if (!deep_select_armed_ && (dx != 0.0F || dy != 0.0F)) {
            frame.dragging = true;
            frame.gizmo_axis = drag_axis_;
            TransformDelta delta{};
            delta.tool = drag_tool_;
            delta.pivot = drag_pivot_;
            delta.basis = drag_basis_;
            // A surface or vertex snap puts the element exactly; the grid
            // step does not round it afterwards.
            bool exact = false;
            switch (drag_tool_) {
            case TransformTool::Move:
                if (drag_axis_ == GizmoAxis::Screen) {
                    delta.translation =
                        projection != nullptr && projection->project
                            ? scene::screen_drag_to_translation(
                                  press_world_, projection->forward,
                                  projection->up, projection->project, dx, dy)
                            : scene::screen_drag_to_translation(
                                  scene.camera, scene.canvas, press_world_, dx,
                                  dy);
                } else {
                    const float axis_pixels_squared =
                        axis_canvas_x_ * axis_canvas_x_ +
                        axis_canvas_y_ * axis_canvas_y_;
                    if (axis_pixels_squared > 0.0F) {
                        const float along =
                            (dx * axis_canvas_x_ + dy * axis_canvas_y_) /
                            axis_pixels_squared;
                        delta.translation =
                            axis_world_ * (along * axis_world_length_);
                    }
                }
                exact = snap_drag(ctx, scene, selection, frame, delta);
                break;
            case TransformTool::Rotate:
                drag_turns_ += sweep_turns(drag_centre_x_, drag_centre_y_,
                                           drag_last_x_, drag_last_y_,
                                           ctx.pointer_x(), ctx.pointer_y(),
                                           axis_world_, view_forward);
                drag_last_x_ = ctx.pointer_x();
                drag_last_y_ = ctx.pointer_y();
                delta.axis = axis_world_;
                delta.turns = drag_turns_;
                break;
            case TransformTool::Scale: {
                const float now_x = ctx.pointer_x() - drag_centre_x_;
                const float now_y = ctx.pointer_y() - drag_centre_y_;
                float reach = std::sqrt(now_x * now_x + now_y * now_y);
                if (is_xyz(drag_axis_)) {
                    const std::size_t index = axis_index(drag_axis_);
                    const float handle_x = axis_canvas_x_;
                    const float handle_y = axis_canvas_y_;
                    const float handle =
                        std::sqrt(handle_x * handle_x + handle_y * handle_y);
                    reach = handle > 0.0F
                                ? (now_x * handle_x + now_y * handle_y) / handle
                                : drag_press_reach_;
                    delta.factors =
                        with_component(scene::Vec3{1.0F, 1.0F, 1.0F}, index,
                                       reach / drag_press_reach_);
                } else {
                    const float factor = reach / drag_press_reach_;
                    delta.factors = scene::Vec3{factor, factor, factor};
                }
                break;
            }
            }
            if (!exact) {
                delta = snapped(ctx, delta);
            }
            frame.transform = delta;
            frame.translation = delta.translation;
        }
    }
    if ((!it.held || !dragging_) && !pivot_dragging_) {
        drag_snap_target_.reset();
    }
}

void ViewportPanel::finish_gesture(Frame& f) {
    auto& frame = f.result;
    auto& ctx = f.ctx;
    auto& scene = f.scene;
    auto& it = f.it;
    if (marquee_active_ && (it.released || !it.held)) {
        marquee_active_ = false;
        const gui::Rect box{std::min(marquee_x_, ctx.pointer_x()),
                            std::min(marquee_y_, ctx.pointer_y()),
                            std::abs(ctx.pointer_x() - marquee_x_),
                            std::abs(ctx.pointer_y() - marquee_y_)};
        frame.marquee = elements_in_marquee(scene, frame.image, box);
        frame.marquee_adds = ctx.shift_held();
    }

    if (it.released) {
        frame.released = true;
        deep_select_armed_ = false;
        dragging_ = false;
        surface_dragging_ = false;
        drag_axis_ = GizmoAxis::None;
    }
}

} // namespace mediaserver::editor
