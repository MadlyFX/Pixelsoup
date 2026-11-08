#include "viewport_support.hpp"

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

bool ViewportPanel::snap_drag(const gui::UiContext& ctx,
                              const scene::Scene& scene,
                              std::span<const doc::NodeId> selection,
                              const ViewportFrame& frame,
                              TransformDelta& delta) {
    drag_snap_target_.reset();
    if (role_ != ViewportRole::Stage || frame.image.width <= 0.0F) {
        return false;
    }
    // What moves never counts as somewhere to land.
    const doc::NodeId dragged = drag_node_;
    const scene::SurfaceFilter elsewhere =
        [dragged, selection](doc::NodeId node) {
            return node != dragged && !selected(selection, node);
        };
    if (ctx.move_key_down(gui::MoveKey::V)) {
        if (!drag_source_vertex_.has_value() && drag_grab_.has_value()) {
            drag_source_vertex_ =
                nearest_element_vertex(scene, drag_node_, drag_start_world_,
                                       *drag_grab_)
                    .value_or(*drag_grab_);
        }
        const auto target = measure_point_at(
            scene, frame.pointer.x, frame.pointer.y,
            14.0F * static_cast<float>(scene.canvas.width) / frame.image.width,
            elsewhere);
        if (drag_source_vertex_.has_value() && target.has_value() &&
            (target->snap == MeasureSnap::Vertex ||
             target->snap == MeasureSnap::EdgeMidpoint)) {
            delta.translation = target->position - *drag_source_vertex_;
            drag_snap_target_ = target;
            return true;
        }
        return false;
    }
    if (!surface_snapping_ || drag_axis_ != GizmoAxis::Screen) {
        return false;
    }
    const auto hit =
        surface_at(scene, frame.pointer.x, frame.pointer.y, elsewhere);
    if (!hit.has_value()) {
        return false;
    }
    const scene::Vec3 contact =
        snapping_ != ctx.ctrl_held()
            ? snap_contact(hit->position, hit->normal,
                           ctx.shift_held() ? snap_steps_.fine_move()
                                            : snap_steps_.move)
            : hit->position;
    delta.translation = resting_origin(contact, hit->normal, drag_start_world_,
                                       drag_start_bounds_) -
                        scene::translation_of(drag_start_world_);
    return true;
}

TransformDelta ViewportPanel::snapped(const gui::UiContext& ctx,
                                     TransformDelta delta) const noexcept {
    if (snapping_ == ctx.ctrl_held()) {
        return delta;
    }
    const bool fine = ctx.shift_held();
    switch (delta.tool) {
    case TransformTool::Move: {
        // Per component of the gesture's own frame, so an axis-constrained
        // move snaps its length and a free one lands on the grid's steps.
        const float step = fine ? snap_steps_.fine_move() : snap_steps_.move;
        scene::Vec3 result{};
        for (const scene::Vec3& axis : delta.basis) {
            result = result +
                     axis * snap_to_step(scene::dot(delta.translation, axis),
                                         step);
        }
        delta.translation = result;
        break;
    }
    case TransformTool::Rotate:
        delta.turns = snap_to_step(delta.turns,
                                   fine ? snap_steps_.fine_rotate_turns()
                                        : snap_steps_.rotate_turns);
        break;
    case TransformTool::Scale: {
        const float step =
            fine ? snap_steps_.fine_scale() : snap_steps_.scale;
        delta.factors = scene::Vec3{snap_to_step(delta.factors.x, step),
                                    snap_to_step(delta.factors.y, step),
                                    snap_to_step(delta.factors.z, step)};
        break;
    }
    }
    return delta;
}

void ViewportPanel::draw_keyboard_transform(gui::UiContext& ctx,
                                            gui::UiDrawList& list,
                                            const gui::Rect& image,
                                            const scene::Scene& scene,
                                            gui::UiPoint centre,
                                            std::optional<float> reading) const {
    const std::array<gui::UiColor, 3U> colors{ctx.theme().gizmo_axis_x,
                                              ctx.theme().gizmo_axis_y,
                                              ctx.theme().gizmo_axis_z};
    const std::array<scene::Vec3, 3U>& axes =
        keyboard_.orientation == TransformOrientation::Global
            ? world_basis
            : keyboard_.local_axes;
    // An infinite constraint line, drawn as long as both ends stay in front
    // of the camera.
    const auto constraint_line = [&](std::size_t index) {
        for (const float reach : {200.0F, 40.0F, 8.0F, 1.5F}) {
            const auto from = scene::project_to_canvas(
                scene.camera, scene.canvas,
                keyboard_.pivot - axes[index] * reach);
            const auto to = scene::project_to_canvas(
                scene.camera, scene.canvas,
                keyboard_.pivot + axes[index] * reach);
            if (from.visible && to.visible) {
                line(list, to_ui(image, scene.canvas, from),
                     to_ui(image, scene.canvas, to), 1.6F, colors[index]);
                return;
            }
        }
    };
    if (keyboard_.constraint == Constraint::Axis) {
        constraint_line(keyboard_.axis);
    } else if (keyboard_.constraint == Constraint::Plane) {
        constraint_line((keyboard_.axis + 1U) % 3U);
        constraint_line((keyboard_.axis + 2U) % 3U);
    }
    if (keyboard_.tool != TransformTool::Move) {
        line(list, centre, gui::UiPoint{keyboard_.pointer_x, keyboard_.pointer_y},
             1.0F, ctx.theme().gizmo_view);
        square(list, centre, gizmo_hub_size, ctx.theme().gizmo_view);
    }

    const char* tool = keyboard_.tool == TransformTool::Move
                           ? "MOVE"
                           : (keyboard_.tool == TransformTool::Rotate
                                  ? "ROTATE"
                                  : "SCALE");
    std::string constraint;
    switch (keyboard_.constraint) {
    case Constraint::None:
        constraint = keyboard_.tool == TransformTool::Rotate ? "VIEW AXIS"
                     : keyboard_.tool == TransformTool::Move ? "VIEW PLANE"
                                                             : "UNIFORM";
        break;
    case Constraint::Axis:
        constraint = std::string{axis_name(keyboard_.axis)};
        break;
    case Constraint::Plane:
        constraint = std::string{axis_name((keyboard_.axis + 1U) % 3U)} +
                     axis_name((keyboard_.axis + 2U) % 3U) + " PLANE";
        break;
    }
    if (keyboard_.constraint != Constraint::None) {
        constraint += keyboard_.orientation == TransformOrientation::Global
                          ? " GLOBAL"
                          : " LOCAL";
    }
    std::string value;
    if (keyboard_.entry.active()) {
        value = keyboard_.entry.display() + "|";
    } else if (reading.has_value()) {
        std::array<char, 32U> text{};
        const char* format = keyboard_.tool == TransformTool::Move
                                 ? "%.3f m"
                                 : (keyboard_.tool == TransformTool::Rotate
                                        ? "%.1f deg"
                                        : "%.3fx");
        const int length =
            std::snprintf(text.data(), text.size(), format, *reading);
        value.assign(text.data(),
                     static_cast<std::size_t>(std::max(length, 0)));
    }
    if (snapping_ != ctx.ctrl_held()) {
        value += "  SNAP";
    }
    const std::string status = std::string{tool} + "  " + constraint + "  " +
                               value +
                               "    ENTER / CLICK  CONFIRM    ESC / RIGHT-CLICK"
                               "  CANCEL";
    gui::Rect corner = gui::inset(image, 8.0F);
    corner = gui::cut_top(corner, 22.0F);
    const gui::Rect bar = gui::cut_left(corner, 640.0F);
    ctx.panel(bar, ctx.theme().background);
    ctx.label(gui::inset(bar, 8.0F, 0.0F), status, ctx.theme().text);
}

} // namespace mediaserver::editor
