// Stage shortcuts and Blender-style keyboard transforms.

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

void ViewportPanel::handle_stage_shortcuts(Frame& f) {
    auto& frame = f.result;
    auto& ctx = f.ctx;
    auto& pivot = f.pivot;
    auto& primary_axes = f.primary_axes;
    auto& occluded = f.occluded;
    // --- Keyboard transforms, as in Blender: G, R or S over the Stage moves,
    // rotates or scales the selection with the pointer - no button held - until
    // Enter or a left click confirms it, or Escape or a right click puts it
    // back. X, Y or Z constrains to an axis (twice: the other orientation,
    // three times: free), Shift+X/Y/Z to the plane without it, and a typed
    // number gives the exact distance, angle in degrees, or factor.
    const bool pointer_over_image =
        !occluded && frame.image.contains(ctx.pointer_x(), ctx.pointer_y());
    if (swallow_pointer_ && !ctx.pointer_down() &&
        !ctx.secondary_pointer_down()) {
        swallow_pointer_ = false;
    }
    if (role_ == ViewportRole::Stage && pointer_over_image && !ctx.typing() &&
        ctx.shift_held() && ctx.key_pressed(gui::Key::Tab)) {
        snapping_ = !snapping_;
    }
    if (role_ == ViewportRole::Stage && pointer_over_image && !ctx.typing() &&
        !keyboard_.active && !ctx.ctrl_held() && !ctx.alt_held()) {
        if (ctx.shortcut_text().find_first_of("mM") != std::string_view::npos) {
            set_measuring(!measuring_);
        } else if (measuring_ && ctx.key_pressed(gui::Key::Escape)) {
            if (measure_.points.empty()) {
                set_measuring(false);
            } else {
                measure_.clear();
            }
        }
    }
    if (role_ == ViewportRole::Stage && !keyboard_.active && !measuring_ &&
        !swallow_pointer_ && !dragging_ && !surface_dragging_ &&
        !deep_select_open_ && pivot.has_value() && pointer_over_image &&
        !ctx.pointer_down() && !ctx.secondary_pointer_down() &&
        !ctx.middle_pointer_down() && !ctx.alt_held() && !ctx.ctrl_held()) {
        std::optional<TransformTool> started;
        for (const char typed : ctx.shortcut_text()) {
            if (typed == 'g') {
                started = TransformTool::Move;
            } else if (typed == 'r') {
                started = TransformTool::Rotate;
            } else if (typed == 's') {
                started = TransformTool::Scale;
            }
        }
        if (started.has_value()) {
            keyboard_ = KeyboardTransform{};
            keyboard_.active = true;
            keyboard_.tool = *started;
            keyboard_.orientation = orientation_;
            keyboard_.pivot = *pivot;
            keyboard_.local_axes = primary_axes;
            keyboard_.press_x = keyboard_.pointer_x = keyboard_.last_x =
                ctx.pointer_x();
            keyboard_.press_y = keyboard_.pointer_y = keyboard_.last_y =
                ctx.pointer_y();
            tool_ = *started;
            frame.transform_began = true;
        }
    }
}

void ViewportPanel::run_keyboard_transform(Frame& f) {
    auto& frame = f.result;
    auto& ctx = f.ctx;
    auto& scene = f.scene;
    auto& canvas_width = f.canvas_width;
    auto& canvas_height = f.canvas_height;
    auto& view_forward = f.view_forward;
    gui::UiPoint keyboard_centre{frame.image.x + frame.image.width * 0.5F,
                                 frame.image.y + frame.image.height * 0.5F};
    std::optional<float> keyboard_reading;
    if (keyboard_.active) {
        if (const auto centre = scene::project_to_canvas(
                scene.camera, scene.canvas, keyboard_.pivot);
            centre.visible) {
            keyboard_centre = to_ui(frame.image, scene.canvas, centre);
        }
        if (!frame.transform_began) {
            for (const char typed : ctx.claimed_text()) {
                const char lower = typed >= 'A' && typed <= 'Z'
                                       ? static_cast<char>(typed - 'A' + 'a')
                                       : typed;
                if (lower == 'x' || lower == 'y' || lower == 'z') {
                    const std::size_t axis =
                        static_cast<std::size_t>(lower - 'x');
                    const Constraint wanted =
                        typed == lower ? Constraint::Axis : Constraint::Plane;
                    if (keyboard_.constraint != wanted ||
                        keyboard_.axis != axis) {
                        keyboard_.constraint = wanted;
                        keyboard_.axis = axis;
                        keyboard_.orientation = orientation_;
                    } else if (keyboard_.orientation == orientation_) {
                        keyboard_.orientation =
                            orientation_ == TransformOrientation::Global
                                ? TransformOrientation::Local
                                : TransformOrientation::Global;
                    } else {
                        keyboard_.constraint = Constraint::None;
                        keyboard_.orientation = orientation_;
                    }
                } else {
                    static_cast<void>(
                        keyboard_.entry.type(std::string_view{&typed, 1U}));
                }
            }
            if (ctx.key_pressed(gui::Key::Backspace)) {
                keyboard_.entry.backspace();
            }
        }
        // Shift slows the pointer to a tenth for fine work.
        const float motion_scale = ctx.shift_held() ? 0.1F : 1.0F;
        const float previous_x = keyboard_.pointer_x;
        const float previous_y = keyboard_.pointer_y;
        keyboard_.pointer_x += (ctx.pointer_x() - keyboard_.last_x) * motion_scale;
        keyboard_.pointer_y += (ctx.pointer_y() - keyboard_.last_y) * motion_scale;
        keyboard_.last_x = ctx.pointer_x();
        keyboard_.last_y = ctx.pointer_y();
        // Anticlockwise screen turns, whatever the axis: the axis decides the
        // sign when the delta is built, so changing it mid-way keeps the
        // rotation following the pointer.
        keyboard_.turns += sweep_turns(
            keyboard_centre.x, keyboard_centre.y, previous_x, previous_y,
            keyboard_.pointer_x, keyboard_.pointer_y, -view_forward,
            view_forward);

        const std::array<scene::Vec3, 3U>& axes =
            keyboard_.orientation == TransformOrientation::Global
                ? world_basis
                : keyboard_.local_axes;
        const std::size_t axis = keyboard_.axis;
        const float to_canvas_x = canvas_width / frame.image.width;
        const float to_canvas_y = canvas_height / frame.image.height;
        const float press_canvas_x =
            (keyboard_.press_x - frame.image.x) * to_canvas_x;
        const float press_canvas_y =
            (keyboard_.press_y - frame.image.y) * to_canvas_y;
        const float now_canvas_x =
            (keyboard_.pointer_x - frame.image.x) * to_canvas_x;
        const float now_canvas_y =
            (keyboard_.pointer_y - frame.image.y) * to_canvas_y;
        const float drag_x = now_canvas_x - press_canvas_x;
        const float drag_y = now_canvas_y - press_canvas_y;
        const std::optional<float> typed = keyboard_.entry.value();

        TransformDelta delta{};
        delta.tool = keyboard_.tool;
        delta.pivot = keyboard_.pivot;
        delta.basis = axes;
        switch (keyboard_.tool) {
        case TransformTool::Move:
            if (typed.has_value()) {
                const std::size_t along =
                    keyboard_.constraint == Constraint::Plane
                        ? (axis + 1U) % 3U
                        : (keyboard_.constraint == Constraint::Axis ? axis
                                                                    : 0U);
                delta.translation = axes[along] * *typed;
            } else if (keyboard_.constraint == Constraint::Axis) {
                const auto from = scene::project_to_canvas(
                    scene.camera, scene.canvas, keyboard_.pivot);
                const auto to = scene::project_to_canvas(
                    scene.camera, scene.canvas,
                    keyboard_.pivot + axes[axis]);
                const float unit_x = to.x - from.x;
                const float unit_y = to.y - from.y;
                const float unit_squared = unit_x * unit_x + unit_y * unit_y;
                if (from.visible && to.visible && unit_squared > 1.0e-4F) {
                    delta.translation =
                        axes[axis] *
                        ((drag_x * unit_x + drag_y * unit_y) / unit_squared);
                }
            } else {
                const scene::Vec3 free = scene::screen_drag_to_translation(
                    scene.camera, scene.canvas, keyboard_.pivot, drag_x,
                    drag_y);
                if (keyboard_.constraint == Constraint::Plane) {
                    delta.translation =
                        plane_drag_translation(scene.camera, scene.canvas,
                                               keyboard_.pivot, axes[axis],
                                               press_canvas_x, press_canvas_y,
                                               now_canvas_x, now_canvas_y)
                            .value_or(free -
                                      axes[axis] *
                                          scene::dot(free, axes[axis]));
                } else {
                    delta.translation = free;
                }
            }
            break;
        case TransformTool::Rotate:
            delta.axis = keyboard_.constraint == Constraint::None
                             ? -view_forward
                             : axes[axis];
            delta.turns =
                typed.has_value()
                    ? *typed / 360.0F
                    : keyboard_.turns *
                          (scene::dot(delta.axis, view_forward) < 0.0F ? 1.0F
                                                                       : -1.0F);
            break;
        case TransformTool::Scale: {
            const float press_dx = keyboard_.press_x - keyboard_centre.x;
            const float press_dy = keyboard_.press_y - keyboard_centre.y;
            const float now_dx = keyboard_.pointer_x - keyboard_centre.x;
            const float now_dy = keyboard_.pointer_y - keyboard_centre.y;
            const float reach =
                std::sqrt(press_dx * press_dx + press_dy * press_dy);
            float factor =
                reach > 1.0F
                    ? std::sqrt(now_dx * now_dx + now_dy * now_dy) / reach
                    : 1.0F;
            if (typed.has_value()) {
                factor = *typed;
            }
            switch (keyboard_.constraint) {
            case Constraint::None:
                delta.factors = scene::Vec3{factor, factor, factor};
                break;
            case Constraint::Axis:
                delta.factors =
                    with_component(scene::Vec3{1.0F, 1.0F, 1.0F}, axis, factor);
                break;
            case Constraint::Plane:
                delta.factors = with_component(
                    scene::Vec3{factor, factor, factor}, axis, 1.0F);
                break;
            }
            break;
        }
        }
        if (!typed.has_value()) {
            delta = snapped(ctx, delta);
        }

        const bool confirm =
            ctx.key_pressed(gui::Key::Enter) || ctx.pointer_pressed();
        const bool cancel = ctx.key_pressed(gui::Key::Escape) ||
                            ctx.secondary_pointer_pressed();
        if (cancel) {
            TransformDelta restore{};
            restore.tool = keyboard_.tool;
            restore.pivot = keyboard_.pivot;
            delta = restore;
            frame.transform_cancelled = true;
        }
        if (cancel || confirm) {
            keyboard_.active = false;
            frame.transform_ended = true;
            swallow_pointer_ =
                ctx.pointer_down() || ctx.secondary_pointer_down();
        }
        frame.transform = delta;
        switch (delta.tool) {
        case TransformTool::Move:
            keyboard_reading = scene::length(delta.translation);
            if (keyboard_.constraint == Constraint::Axis) {
                keyboard_reading = scene::dot(delta.translation, axes[axis]);
            }
            break;
        case TransformTool::Rotate:
            keyboard_reading = delta.turns * 360.0F;
            break;
        case TransformTool::Scale:
            keyboard_reading = keyboard_.constraint == Constraint::Plane
                                   ? component_of(delta.factors,
                                                  (axis + 1U) % 3U)
                                   : component_of(delta.factors,
                                                  keyboard_.constraint ==
                                                          Constraint::Axis
                                                      ? axis
                                                      : 0U);
            break;
        }
    }
    frame.keyboard_transform = keyboard_.active || swallow_pointer_;
    f.keyboard_centre = keyboard_centre;
    f.keyboard_reading = keyboard_reading;
}

} // namespace mediaserver::editor
