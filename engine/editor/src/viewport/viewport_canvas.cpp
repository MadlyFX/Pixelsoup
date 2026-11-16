// The Program view as a 2D canvas: its zoom and pan, and the transform box
// round the selected layer.

#include "viewport_frame.hpp"

#include <mediaserver/editor/gizmo.hpp>
#include <mediaserver/editor/transform_box.hpp>
#include <mediaserver/gui/rect_cut.hpp>

#include <mediaserver/scene/manipulate.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>

namespace mediaserver::editor {

using namespace viewport;

namespace {

constexpr float tau = 6.28318530717958647692F;
// One wheel notch, and one click of the Zoom tool.
constexpr float wheel_zoom_step = 1.2F;
constexpr float click_zoom_step = 2.0F;
// The picture never shrinks below a quarter of its fitted size, and a canvas
// pixel never grows past this many panel units.
constexpr float minimum_zoom_of_fit = 0.25F;
constexpr float maximum_units_per_pixel = 64.0F;
// How much of the picture a pan must leave inside the pane.
constexpr float pan_margin = 48.0F;
constexpr float rotate_snap_turns = 1.0F / 24.0F;

// The rectangle UiContext::image_fit places an image of `aspect` in.
[[nodiscard]] gui::Rect fitted(const gui::Rect& pane, float aspect) noexcept {
    if (pane.width <= 0.0F || pane.height <= 0.0F || aspect <= 0.0F) {
        return gui::Rect{pane.x, pane.y, 0.0F, 0.0F};
    }
    float width = pane.width;
    float height = width / aspect;
    if (height > pane.height) {
        height = pane.height;
        width = height * aspect;
    }
    return gui::Rect{pane.x + (pane.width - width) * 0.5F,
                     pane.y + (pane.height - height) * 0.5F, width, height};
}

[[nodiscard]] gui::CanvasView fitted_view(const gui::Rect& pane,
                                          scene::Canvas canvas) noexcept {
    const gui::Rect fit =
        fitted(program_canvas_board(pane), static_cast<float>(canvas.width) /
                          static_cast<float>(canvas.height));
    gui::CanvasView view{};
    view.zoom = fit.width / static_cast<float>(canvas.width);
    view.pan_x = fit.x - pane.x;
    view.pan_y = fit.y - pane.y;
    return view;
}

[[nodiscard]] float clamped_zoom(float zoom, float fit_zoom) noexcept {
    const float lowest = fit_zoom * minimum_zoom_of_fit;
    const float highest = std::max(maximum_units_per_pixel, fit_zoom);
    return std::clamp(zoom, lowest, highest);
}

// Keeps some of the picture inside the pane, so a pan cannot lose it.
void keep_in_pane(gui::CanvasView& view, const gui::Rect& pane,
                  scene::Canvas canvas) noexcept {
    const float width = static_cast<float>(canvas.width) * view.zoom;
    const float height = static_cast<float>(canvas.height) * view.zoom;
    const float margin_x = std::min(pan_margin, std::min(width, pane.width));
    const float margin_y = std::min(pan_margin, std::min(height, pane.height));
    view.pan_x = std::clamp(view.pan_x, margin_x - width,
                            std::max(pane.width - margin_x, margin_x - width));
    view.pan_y =
        std::clamp(view.pan_y, margin_y - height,
                   std::max(pane.height - margin_y, margin_y - height));
}

// The point of the unit box that lands on (x, y), through the projective map
// that takes the box onto `quad` - the same map masks resolve a pointer with.
[[nodiscard]] std::optional<BoxPoint>
box_point_at(const std::array<gui::UiPoint, 4U>& quad, float x,
             float y) noexcept {
    MaskViewportSourceMapping mapping{};
    for (std::size_t index = 0U; index < quad.size(); ++index) {
        mapping.corners[index] =
            MaskViewportCanvasPoint{quad[index].x, quad[index].y};
    }
    // A unit image over a unit canvas leaves the pointer as it is, so the
    // quad may be in any units the pointer is in.
    const auto mapped = mask_viewport_to_normalized(
        gui::Rect{0.0F, 0.0F, 1.0F, 1.0F}, scene::Canvas{1U, 1U},
        scene::MaskCoordinateSpace::SourceLayer, &mapping, x, y);
    if (!mapped.has_value()) {
        return std::nullopt;
    }
    return BoxPoint{mapped->x, mapped->y};
}

[[nodiscard]] scene::CanvasProjection
project(const scene::Scene& scene, const ViewportProjection* projection,
        scene::Vec3 point) {
    return projection != nullptr && projection->project
               ? projection->project(point)
               : scene::project_to_canvas(scene.camera, scene.canvas, point);
}

[[nodiscard]] scene::Vec3
view_forward_of(const scene::Scene& scene,
                const ViewportProjection* projection) noexcept {
    return projection != nullptr && projection->project
               ? scene::normalize(projection->forward)
               : scene::normalize(scene.camera.target - scene.camera.eye);
}

}  // namespace

gui::Rect program_canvas_board(const gui::Rect& pane) noexcept {
    const float margin =
        std::clamp(std::min(pane.width, pane.height) * 0.06F, 8.0F, 40.0F);
    return pane.width > margin * 4.0F && pane.height > margin * 4.0F
               ? gui::inset(pane, margin)
               : pane;
}

gui::Rect ViewportPanel::shown_image(const gui::Rect& image) const noexcept {
    if (role_ != ViewportRole::Program || canvas_pane_.width <= 0.0F ||
        canvas_pane_.height <= 0.0F) {
        return image;
    }
    return gui::intersection(image, canvas_pane_);
}

void ViewportPanel::set_canvas_scale(float units_per_canvas_pixel) noexcept {
    if (canvas_shown_.width == 0U || canvas_shown_.height == 0U ||
        canvas_pane_.width <= 0.0F || canvas_pane_.height <= 0.0F ||
        !(units_per_canvas_pixel > 0.0F)) {
        return;
    }
    const gui::CanvasView fit = fitted_view(canvas_pane_, canvas_shown_);
    if (canvas_fitted_) {
        canvas_view_ = fit;
    }
    canvas_view_.zoom_about(canvas_pane_,
                            canvas_pane_.x + canvas_pane_.width * 0.5F,
                            canvas_pane_.y + canvas_pane_.height * 0.5F,
                            clamped_zoom(units_per_canvas_pixel, fit.zoom));
    keep_in_pane(canvas_view_, canvas_pane_, canvas_shown_);
    canvas_fitted_ = false;
}

// Runs before the image is placed, so a zoom or pan shows the frame it is
// made. The wheel and a middle drag belong to the view with any tool; the
// Hand and Zoom tools give it the left button as well.
void ViewportPanel::navigate_canvas(Frame& f) {
    auto& ctx = f.ctx;
    const gui::Rect& pane = f.pane;
    const scene::Canvas canvas = f.interaction_canvas;
    // A different output, or the Canvas view, is a different picture.
    if (canvas_shown_ != canvas) {
        canvas_shown_ = canvas;
        canvas_fitted_ = true;
        canvas_panning_ = false;
    }
    canvas_pane_ = pane;
    const gui::CanvasView fit = fitted_view(pane, canvas);
    if (!(fit.zoom > 0.0F)) {
        return;
    }
    if (canvas_fitted_) {
        canvas_view_ = fit;
    }
    const float pointer_x = ctx.pointer_x();
    const float pointer_y = ctx.pointer_y();
    const bool over = pane.contains(pointer_x, pointer_y);
    // A gesture on the picture is measured against where the picture was
    // when it began; the view holds still until it ends.
    const bool busy = dragging_ || surface_dragging_ || keyboard_.active ||
                      mask_gesture_active_;
    const auto zoom_to = [&](float zoom) {
        const float next = clamped_zoom(zoom, fit.zoom);
        if (next == canvas_view_.zoom) {
            return;
        }
        canvas_view_.zoom_about(pane, pointer_x, pointer_y, next);
        canvas_fitted_ = false;
        f.result.navigated = true;
    };
    if (over && !busy) {
        const float wheel = ctx.wheel();
        if (wheel != 0.0F) {
            zoom_to(canvas_view_.zoom * std::pow(wheel_zoom_step, wheel));
        }
        if (!canvas_panning_ && ctx.middle_pointer_pressed()) {
            canvas_panning_ = true;
            canvas_pan_middle_ = true;
            canvas_pan_x_ = pointer_x;
            canvas_pan_y_ = pointer_y;
        }
    }
    if (canvas_tool_ == CanvasTool::Hand || canvas_tool_ == CanvasTool::Zoom) {
        f.navigating = true;
        const gui::Interaction it =
            ctx.grab(gui::ui_id(id_, 0xCA7A5U), busy ? gui::Rect{} : pane);
        if (canvas_tool_ == CanvasTool::Hand) {
            ctx.cursor_for(it, gui::Cursor::Move);
            if (it.pressed && !canvas_panning_) {
                canvas_panning_ = true;
                canvas_pan_middle_ = false;
                canvas_pan_x_ = pointer_x;
                canvas_pan_y_ = pointer_y;
            }
        } else {
            ctx.cursor_for(it, gui::Cursor::Hand);
            if (it.clicked) {
                zoom_to(ctx.alt_held() ? canvas_view_.zoom / click_zoom_step
                                       : canvas_view_.zoom * click_zoom_step);
            }
        }
    }
    if (canvas_panning_) {
        const bool down = canvas_pan_middle_ ? ctx.middle_pointer_down()
                                             : ctx.pointer_down();
        if (down) {
            const float dx = pointer_x - canvas_pan_x_;
            const float dy = pointer_y - canvas_pan_y_;
            if (dx != 0.0F || dy != 0.0F) {
                canvas_view_.pan_x += dx;
                canvas_view_.pan_y += dy;
                canvas_fitted_ = false;
            }
            canvas_pan_x_ = pointer_x;
            canvas_pan_y_ = pointer_y;
            f.result.navigated = true;
            ctx.set_cursor(gui::Cursor::Move);
        } else {
            canvas_panning_ = false;
        }
    }
    if (!canvas_fitted_) {
        keep_in_pane(canvas_view_, pane, canvas);
    }
}

std::optional<ViewportPanel::LayerBox>
ViewportPanel::layer_box(const scene::Scene& scene, doc::NodeId node,
                         const ViewportProjection* projection) {
    if (node == doc::NodeId{}) {
        return std::nullopt;
    }
    // A flat layer's box is its own outline: the plane its picture, and its
    // masks, lie on.
    const scene::Mat4* world{};
    scene::Primitive primitive{scene::Primitive::Quad};
    for (const scene::MediaInstance& media : scene.media) {
        if (media.source_node == node) {
            world = &media.world;
            primitive = media.mesh.primitive;
            break;
        }
    }
    if (world == nullptr) {
        for (const scene::MeshInstance& instance : scene.instances) {
            if (instance.source == node) {
                world = &instance.world;
                primitive = instance.mesh.primitive;
                break;
            }
        }
    }
    if (world == nullptr) {
        for (const scene::SceneScreen& screen : scene.screens) {
            if (screen.node == node) {
                world = &screen.world;
                primitive = scene::Primitive::Quad;
                break;
            }
        }
    }
    LayerBox box{};
    const auto plane = world != nullptr ? scene::mask_source_plane(primitive)
                                        : std::nullopt;
    if (plane.has_value()) {
        box.sides = true;
        for (std::size_t index = 0U; index < box.world.size(); ++index) {
            box.world[index] = scene::transform_point(*world, (*plane)[index]);
            box.canvas[index] = project(scene, projection, box.world[index]);
            if (!box.canvas[index].visible ||
                !std::isfinite(box.canvas[index].x) ||
                !std::isfinite(box.canvas[index].y)) {
                return std::nullopt;
            }
            box.centre = box.centre + box.world[index] * 0.25F;
        }
        const scene::Vec3 across = box.world[1U] - box.world[0U];
        const scene::Vec3 down = box.world[3U] - box.world[0U];
        if (scene::length(across) < 1.0e-6F || scene::length(down) < 1.0e-6F) {
            return std::nullopt;
        }
        box.basis[0U] = scene::normalize(across);
        box.basis[1U] = scene::normalize(down);
        box.basis[2U] =
            scene::normalize(scene::cross(box.basis[0U], box.basis[1U]));
        return box;
    }
    // Anything with a volume is boxed by what it covers on the canvas.
    const auto placed = scene::world_for(scene, node);
    if (!placed.has_value()) {
        return std::nullopt;
    }
    const scene::ModelLocalBounds bounds = element_local_bounds(scene, node);
    const scene::Vec3 size = bounds.maximum - bounds.minimum;
    if (!(size.x > 0.0F) && !(size.y > 0.0F) && !(size.z > 0.0F)) {
        return std::nullopt;
    }
    float left = 0.0F;
    float top = 0.0F;
    float right = 0.0F;
    float bottom = 0.0F;
    for (std::size_t index = 0U; index < 8U; ++index) {
        const scene::Vec3 local{
            (index & 1U) != 0U ? bounds.maximum.x : bounds.minimum.x,
            (index & 2U) != 0U ? bounds.maximum.y : bounds.minimum.y,
            (index & 4U) != 0U ? bounds.maximum.z : bounds.minimum.z};
        const scene::CanvasProjection at = project(
            scene, projection, scene::transform_point(*placed, local));
        if (!at.visible || !std::isfinite(at.x) || !std::isfinite(at.y)) {
            return std::nullopt;
        }
        left = index == 0U ? at.x : std::min(left, at.x);
        right = index == 0U ? at.x : std::max(right, at.x);
        top = index == 0U ? at.y : std::min(top, at.y);
        bottom = index == 0U ? at.y : std::max(bottom, at.y);
    }
    box.canvas[0U] = scene::CanvasProjection{left, top, 0.0F, true};
    box.canvas[1U] = scene::CanvasProjection{right, top, 0.0F, true};
    box.canvas[2U] = scene::CanvasProjection{right, bottom, 0.0F, true};
    box.canvas[3U] = scene::CanvasProjection{left, bottom, 0.0F, true};
    box.centre = scene::transform_point(
        *placed, (bounds.minimum + bounds.maximum) * 0.5F);
    box.basis = world_axes_of(*placed);
    return box;
}

void ViewportPanel::prepare_transform_box(Frame& f) {
    auto& frame = f.result;
    auto& ctx = f.ctx;
    if (role_ != ViewportRole::Program) {
        return;
    }
    const bool held = dragging_ && box_handle_ != TransformBoxHandle::None;
    // A shape tool draws onto the selected layer, so it needs the layer's
    // outline too, though it shows no handles on it.
    const bool shapes = draws_shape(canvas_tool_) ||
                        canvas_tool_ == CanvasTool::Gradient;
    if ((canvas_tool_ != CanvasTool::Select && !shapes) || drawing_masks_ ||
        !box_enabled_ || !f.primary_world.has_value()) {
        if (held) {
            dragging_ = false;
        }
        box_handle_ = TransformBoxHandle::None;
        return;
    }
    f.layer_box = layer_box(f.scene, f.primary, f.projection);
    if (!f.layer_box.has_value()) {
        return;
    }
    std::array<gui::UiPoint, 4U> corners{};
    for (std::size_t index = 0U; index < corners.size(); ++index) {
        corners[index] =
            canvas_to_image(frame.image, f.interaction_canvas,
                            f.layer_box->canvas[index].x,
                            f.layer_box->canvas[index].y);
    }
    frame.box = transform_box(corners, f.layer_box->sides);
    if (shapes) {
        box_handle_ = TransformBoxHandle::None;
        return;
    }
    if (held) {
        frame.box_handle = box_handle_;
    } else if (!f.occluded && !dragging_ &&
               f.pane.contains(ctx.pointer_x(), ctx.pointer_y())) {
        frame.box_handle =
            hit_transform_box(frame.box, ctx.pointer_x(), ctx.pointer_y());
    }
    if (frame.box_handle != TransformBoxHandle::None) {
        ctx.set_cursor(handle_cursor(frame.box, frame.box_handle));
    }
}

bool ViewportPanel::press_transform_box(Frame& f) {
    auto& frame = f.result;
    auto& ctx = f.ctx;
    if (frame.box_handle == TransformBoxHandle::None ||
        !f.layer_box.has_value() || !frame.box.visible) {
        return false;
    }
    box_handle_ = frame.box_handle;
    box_press_ = *f.layer_box;
    box_press_quad_ = frame.box.corners;
    box_press_point_ =
        box_point_at(box_press_quad_, ctx.pointer_x(), ctx.pointer_y())
            .value_or(handle_point(box_handle_));
    frame.picked = f.primary;
    drag_axis_ = GizmoAxis::View;
    drag_tool_ = box_handle_ == TransformBoxHandle::Rotate
                     ? TransformTool::Rotate
                     : TransformTool::Scale;
    drag_node_ = f.primary;
    drag_pivot_ = box_press_.centre;
    drag_basis_ = box_press_.basis;
    drag_centre_x_ = frame.box.centre.x;
    drag_centre_y_ = frame.box.centre.y;
    drag_last_x_ = ctx.pointer_x();
    drag_last_y_ = ctx.pointer_y();
    drag_turns_ = 0.0F;
    drag_press_reach_ = std::max(
        std::hypot(ctx.pointer_x() - drag_centre_x_,
                   ctx.pointer_y() - drag_centre_y_),
        1.0F);
    press_x_ = frame.pointer.x;
    press_y_ = frame.pointer.y;
    press_pointer_x_ = ctx.pointer_x();
    drag_grab_.reset();
    drag_source_vertex_.reset();
    dragging_ = true;
    return true;
}

bool ViewportPanel::drag_transform_box(Frame& f) {
    auto& frame = f.result;
    auto& ctx = f.ctx;
    if (!dragging_ || box_handle_ == TransformBoxHandle::None) {
        return false;
    }
    if (!f.it.held) {
        return true;
    }
    if (frame.pointer.x == press_x_ && frame.pointer.y == press_y_ &&
        drag_turns_ == 0.0F) {
        return true;
    }
    frame.dragging = true;
    frame.gizmo_axis = GizmoAxis::View;
    TransformDelta delta{};
    delta.tool = drag_tool_;
    delta.basis = box_press_.basis;
    delta.pivot = box_press_.centre;
    if (box_handle_ == TransformBoxHandle::Rotate) {
        // A flat layer turns in its own plane, so it stays flat to whatever
        // it is facing; anything else turns about the line of sight.
        const scene::Vec3 forward = view_forward_of(f.scene, f.projection);
        const scene::Vec3 axis =
            box_press_.sides ? box_press_.basis[2U] : forward * -1.0F;
        drag_turns_ += sweep_turns(drag_centre_x_, drag_centre_y_,
                                   drag_last_x_, drag_last_y_, ctx.pointer_x(),
                                   ctx.pointer_y(), axis, forward);
        drag_last_x_ = ctx.pointer_x();
        drag_last_y_ = ctx.pointer_y();
        delta.axis = axis;
        delta.turns = ctx.shift_held()
                          ? snap_to_step(drag_turns_, rotate_snap_turns)
                          : drag_turns_;
    } else if (box_press_.sides) {
        const auto now = box_point_at(box_press_quad_, ctx.pointer_x(),
                                      ctx.pointer_y());
        if (!now.has_value()) {
            // Past the layer's horizon: hold the last size it had.
            return true;
        }
        const BoxPoint anchor =
            ctx.alt_held() ? BoxPoint{0.5F, 0.5F}
                           : handle_point(opposite(box_handle_));
        const BoxScale scale = resolve_box_scale(
            box_handle_, anchor,
            BoxPoint{now->u - box_press_point_.u, now->v - box_press_point_.v},
            ctx.shift_held());
        delta.factors = scene::Vec3{scale.across, scale.down, 1.0F};
        // The box is a parallelogram in the world, so a point of it is its
        // first corner and a share of each side.
        delta.pivot =
            box_press_.world[0U] +
            (box_press_.world[1U] - box_press_.world[0U]) * anchor.u +
            (box_press_.world[3U] - box_press_.world[0U]) * anchor.v;
    } else {
        const float reach = std::hypot(ctx.pointer_x() - drag_centre_x_,
                                       ctx.pointer_y() - drag_centre_y_);
        const float factor = std::max(reach / drag_press_reach_,
                                      transform_box_minimum_scale);
        delta.factors = scene::Vec3{factor, factor, factor};
    }
    frame.transform = delta;
    return true;
}

std::optional<CanvasBoxMetrics>
ViewportPanel::canvas_box_metrics(const scene::Scene& scene, doc::NodeId node,
                                  const ViewportProjection* projection) {
    const auto box = layer_box(scene, node, projection);
    if (!box.has_value()) {
        return std::nullopt;
    }
    const auto& c = box->canvas;
    CanvasBoxMetrics metrics{};
    metrics.sides = box->sides;
    metrics.x = (c[0U].x + c[1U].x + c[2U].x + c[3U].x) * 0.25F;
    metrics.y = (c[0U].y + c[1U].y + c[2U].y + c[3U].y) * 0.25F;
    metrics.width = std::hypot(c[1U].x - c[0U].x, c[1U].y - c[0U].y);
    metrics.height = std::hypot(c[3U].x - c[0U].x, c[3U].y - c[0U].y);
    if (box->sides) {
        metrics.turns =
            std::atan2(c[1U].y - c[0U].y, c[1U].x - c[0U].x) / tau;
    }
    return metrics;
}

std::optional<TransformDelta>
ViewportPanel::canvas_box_edit(const scene::Scene& scene, doc::NodeId node,
                               const ViewportProjection* projection,
                               CanvasBoxField field, float value) {
    const auto box = layer_box(scene, node, projection);
    const auto metrics = canvas_box_metrics(scene, node, projection);
    if (!box.has_value() || !metrics.has_value() || !std::isfinite(value)) {
        return std::nullopt;
    }
    TransformDelta delta{};
    delta.pivot = box->centre;
    delta.basis = box->basis;
    switch (field) {
    case CanvasBoxField::X:
    case CanvasBoxField::Y: {
        const float dx = field == CanvasBoxField::X ? value - metrics->x : 0.0F;
        const float dy = field == CanvasBoxField::Y ? value - metrics->y : 0.0F;
        delta.tool = TransformTool::Move;
        delta.translation =
            projection != nullptr && projection->project
                ? scene::screen_drag_to_translation(
                      box->centre, projection->forward, projection->up,
                      projection->project, dx, dy)
                : scene::screen_drag_to_translation(scene.camera, scene.canvas,
                                                    box->centre, dx, dy);
        return delta;
    }
    case CanvasBoxField::Width:
    case CanvasBoxField::Height: {
        const float current = field == CanvasBoxField::Width ? metrics->width
                                                             : metrics->height;
        if (!(current > 1.0e-3F) || !(value > 0.0F)) {
            return std::nullopt;
        }
        const float factor =
            std::max(value / current, transform_box_minimum_scale);
        delta.tool = TransformTool::Scale;
        if (!box->sides) {
            delta.factors = scene::Vec3{factor, factor, factor};
        } else if (field == CanvasBoxField::Width) {
            delta.factors = scene::Vec3{factor, 1.0F, 1.0F};
        } else {
            delta.factors = scene::Vec3{1.0F, factor, 1.0F};
        }
        return delta;
    }
    case CanvasBoxField::Rotation: {
        if (!box->sides) {
            return std::nullopt;
        }
        // A turn about an axis that points at the viewer runs anticlockwise
        // as seen; the reading is clockwise.
        const scene::Vec3 forward = view_forward_of(scene, projection);
        const float facing = scene::dot(box->basis[2U], forward);
        delta.tool = TransformTool::Rotate;
        delta.axis = box->basis[2U];
        delta.turns = (value - metrics->turns) * (facing < 0.0F ? -1.0F : 1.0F);
        return delta;
    }
    }
    return std::nullopt;
}

namespace {

// How close to the Pen's first point a click closes the curve, and how far a
// press must travel before it is pulling a point's handles out.
constexpr float pen_close_pixels = 9.0F;
constexpr float pen_handle_pixels = 3.0F;
// The smallest box, either way, a drag has to cover to count as a shape.
constexpr float shape_minimum_pixels = 4.0F;
constexpr std::size_t ellipse_segments = 48U;
constexpr std::size_t curve_segments = 16U;

// The projective map of the unit box onto `quad`, forwards: where the point
// (u, v) of the box is in the quad's own units.
[[nodiscard]] std::optional<gui::UiPoint>
box_point_to(const std::array<gui::UiPoint, 4U>& quad, float u,
             float v) noexcept {
    MaskViewportSourceMapping mapping{};
    for (std::size_t index = 0U; index < quad.size(); ++index) {
        mapping.corners[index] =
            MaskViewportCanvasPoint{quad[index].x, quad[index].y};
    }
    return mask_normalized_to_viewport(
        gui::Rect{0.0F, 0.0F, 1.0F, 1.0F}, scene::Canvas{1U, 1U},
        scene::MaskCoordinateSpace::SourceLayer, &mapping, u, v);
}

[[nodiscard]] float cubic_at(float a, float b, float c, float d,
                             float t) noexcept {
    const float s = 1.0F - t;
    return s * s * s * a + 3.0F * s * s * t * b + 3.0F * s * t * t * c +
           t * t * t * d;
}

}  // namespace

void ViewportPanel::cancel_shape() noexcept {
    shape_drawing_ = false;
    shape_cancelled_ = false;
    pen_points_.clear();
    pen_dragging_ = false;
}

std::optional<scene::Vec3> ViewportPanel::canvas_point_on_plane(
    const scene::Scene& scene, const ViewportProjection* projection,
    scene::Vec3 origin, scene::Vec3 right, scene::Vec3 up, float x, float y) {
    // A unit square of the plane about `origin`, as the view shows it; the
    // pixel is then a share of the way across and down that square.
    const std::array<scene::Vec3, 4U> square{
        origin - right * 0.5F + up * 0.5F, origin + right * 0.5F + up * 0.5F,
        origin + right * 0.5F - up * 0.5F, origin - right * 0.5F - up * 0.5F};
    std::array<gui::UiPoint, 4U> shown{};
    for (std::size_t index = 0U; index < square.size(); ++index) {
        const scene::CanvasProjection at =
            project(scene, projection, square[index]);
        if (!at.visible || !std::isfinite(at.x) || !std::isfinite(at.y)) {
            return std::nullopt;
        }
        shown[index] = gui::UiPoint{at.x, at.y};
    }
    const auto point = box_point_at(shown, x, y);
    if (!point.has_value()) {
        return std::nullopt;
    }
    return square[0U] + right * point->u - up * point->v;
}

// The Rectangle, Ellipse and Pen tools. Each owns the left button over the
// pane while it is chosen, draws in the space its shape will be authored in -
// across and down the selected flat layer, or canvas pixels over bare
// picture - and reports the finished shape once.
void ViewportPanel::run_shape_tool(Frame& f) {
    auto& frame = f.result;
    auto& ctx = f.ctx;
    if (role_ != ViewportRole::Program || !draws_shape(canvas_tool_) ||
        drawing_masks_ || !box_enabled_) {
        cancel_shape();
        return;
    }
    f.navigating = true;
    const bool layer = f.layer_box.has_value() && f.layer_box->sides &&
                       frame.box.visible;
    // What was begun on one layer, or on none, does not carry to another.
    if (shape_in_progress() &&
        (shape_node_ != f.primary || (shape_on_layer_ && !layer))) {
        cancel_shape();
    }
    if (canvas_tool_ != CanvasTool::Pen) {
        pen_points_.clear();
        pen_dragging_ = false;
    }
    const std::array<gui::UiPoint, 4U> quad = frame.box.corners;
    const scene::Canvas canvas = f.interaction_canvas;
    const auto to_space =
        [&](gui::UiPoint at) -> std::optional<gui::UiPoint> {
        if (shape_on_layer_) {
            const auto point = box_point_at(quad, at.x, at.y);
            if (!point.has_value()) {
                return std::nullopt;
            }
            return gui::UiPoint{point->u, point->v};
        }
        const CanvasPoint point = to_canvas(frame.image, canvas, at.x, at.y);
        return gui::UiPoint{point.x, point.y};
    };
    const auto to_panel =
        [&](float x, float y) -> std::optional<gui::UiPoint> {
        if (shape_on_layer_) {
            return box_point_to(quad, x, y);
        }
        return canvas_to_image(frame.image, canvas, x, y);
    };
    const gui::UiPoint pointer{ctx.pointer_x(), ctx.pointer_y()};
    const bool busy = canvas_panning_;
    const gui::Interaction it = ctx.grab(
        gui::ui_id(id_, 0x5A9E5U), f.occluded || busy ? gui::Rect{} : f.pane);
    // A press starts on the layer if there is one, on bare picture if a card
    // may be made there, and otherwise nowhere.
    const bool may_start = layer || card_drawing_enabled_;
    const auto begin = [&] {
        shape_on_layer_ = layer;
        shape_node_ = f.primary;
    };

    if (canvas_tool_ == CanvasTool::Pen) {
        std::optional<gui::UiPoint> first;
        if (!pen_points_.empty()) {
            first = to_panel(pen_points_.front().x, pen_points_.front().y);
        }
        const bool closable = pen_points_.size() >= 3U && first.has_value();
        f.tool_closes =
            closable && std::hypot(pointer.x - first->x, pointer.y - first->y) <=
                            pen_close_pixels;
        bool close = false;
        if (it.pressed) {
            if (f.tool_closes) {
                close = true;
            } else if (pen_points_.empty() ? may_start : true) {
                if (pen_points_.empty()) {
                    begin();
                }
                if (const auto at = to_space(pointer)) {
                    pen_points_.push_back(scene::MaskPointGeometry{
                        at->x, at->y, at->x, at->y, at->x, at->y});
                    pen_dragging_ = true;
                    shape_press_ = pointer;
                }
            }
        }
        if (pen_dragging_ && it.held && !it.released && !pen_points_.empty()) {
            // Dragging on from a new point pulls its handles out either
            // side of it, the far one mirroring the near.
            if (std::hypot(pointer.x - shape_press_.x,
                           pointer.y - shape_press_.y) > pen_handle_pixels) {
                if (const auto at = to_space(pointer)) {
                    scene::MaskPointGeometry& point = pen_points_.back();
                    point.out_x = at->x;
                    point.out_y = at->y;
                    point.in_x = 2.0F * point.x - at->x;
                    point.in_y = 2.0F * point.y - at->y;
                }
            }
        }
        if (it.released || !it.held) {
            pen_dragging_ = false;
        }
        if (!ctx.typing() && !pen_points_.empty()) {
            if (ctx.key_pressed(gui::Key::Escape)) {
                cancel_shape();
            } else if (ctx.key_pressed(gui::Key::Backspace) &&
                       !pen_dragging_) {
                pen_points_.pop_back();
            } else if (ctx.key_pressed(gui::Key::Enter) && closable) {
                close = true;
            }
        }
        if (close) {
            CanvasShape shape{};
            shape.tool = CanvasTool::Pen;
            shape.on_layer = shape_on_layer_;
            shape.points = pen_points_;
            frame.shape = std::move(shape);
            cancel_shape();
        }
        frame.shape_drawing = !pen_points_.empty();
        // The curve so far, then a straight line on to the pointer.
        if (!pen_points_.empty()) {
            std::vector<gui::UiPoint> curve;
            for (std::size_t index = 0U; index < pen_points_.size(); ++index) {
                const scene::MaskPointGeometry& from = pen_points_[index];
                if (const auto at = to_panel(from.x, from.y)) {
                    f.tool_points.push_back(*at);
                    if (index == 0U) {
                        curve.push_back(*at);
                    }
                }
                if (index + 1U == pen_points_.size()) {
                    break;
                }
                const scene::MaskPointGeometry& to = pen_points_[index + 1U];
                for (std::size_t step = 1U; step <= curve_segments; ++step) {
                    const float t = static_cast<float>(step) /
                                    static_cast<float>(curve_segments);
                    if (const auto at = to_panel(
                            cubic_at(from.x, from.out_x, to.in_x, to.x, t),
                            cubic_at(from.y, from.out_y, to.in_y, to.y, t))) {
                        curve.push_back(*at);
                    }
                }
            }
            if (!pen_dragging_ && f.pane.contains(pointer.x, pointer.y)) {
                curve.push_back(f.tool_closes && first.has_value() ? *first
                                                                  : pointer);
            }
            f.tool_outlines.push_back(std::move(curve));
        }
        return;
    }

    if (it.pressed && may_start) {
        begin();
        shape_drawing_ = true;
        shape_cancelled_ = false;
        shape_press_ = pointer;
    }
    if (shape_drawing_ && !ctx.typing() &&
        ctx.key_pressed(gui::Key::Escape)) {
        shape_cancelled_ = true;
    }
    if (shape_drawing_ && (it.held || it.released)) {
        // Shift keeps the box square on screen; Alt grows it from the
        // press, which is then its middle.
        float dx = pointer.x - shape_press_.x;
        float dy = pointer.y - shape_press_.y;
        if (ctx.shift_held()) {
            const float side = std::max(std::abs(dx), std::abs(dy));
            dx = dx < 0.0F ? -side : side;
            dy = dy < 0.0F ? -side : side;
        }
        const gui::UiPoint from =
            ctx.alt_held()
                ? gui::UiPoint{shape_press_.x - dx, shape_press_.y - dy}
                : shape_press_;
        const gui::UiPoint to{shape_press_.x + dx, shape_press_.y + dy};
        const auto a = to_space(from);
        const auto b = to_space(to);
        const bool large = std::abs(to.x - from.x) >= shape_minimum_pixels &&
                           std::abs(to.y - from.y) >= shape_minimum_pixels;
        if (a.has_value() && b.has_value() && !shape_cancelled_) {
            const float left = std::min(a->x, b->x);
            const float right = std::max(a->x, b->x);
            const float top = std::min(a->y, b->y);
            const float bottom = std::max(a->y, b->y);
            if (it.released) {
                if (large && right > left && bottom > top) {
                    CanvasShape shape{};
                    shape.tool = canvas_tool_;
                    shape.on_layer = shape_on_layer_;
                    shape.left = left;
                    shape.top = top;
                    shape.right = right;
                    shape.bottom = bottom;
                    frame.shape = std::move(shape);
                }
            } else {
                std::vector<gui::UiPoint> outline;
                const auto add = [&](float x, float y) {
                    if (const auto at = to_panel(x, y)) {
                        outline.push_back(*at);
                    }
                };
                if (canvas_tool_ == CanvasTool::Rectangle) {
                    add(left, top);
                    add(right, top);
                    add(right, bottom);
                    add(left, bottom);
                    add(left, top);
                } else {
                    for (std::size_t step = 0U; step <= ellipse_segments;
                         ++step) {
                        const float angle =
                            tau * static_cast<float>(step) /
                            static_cast<float>(ellipse_segments);
                        add((left + right) * 0.5F +
                                std::cos(angle) * (right - left) * 0.5F,
                            (top + bottom) * 0.5F +
                                std::sin(angle) * (bottom - top) * 0.5F);
                    }
                }
                f.tool_outlines.push_back(std::move(outline));
            }
        }
    }
    if (it.released || !it.held) {
        shape_drawing_ = false;
        shape_cancelled_ = false;
    }
    frame.shape_drawing = shape_drawing_;
}

// The Gradient tool: the selected flat layer's fill, dragged about on the
// picture. Its handles are where the renderer puts the ramp, so what moves
// under the pointer is the picture itself.
void ViewportPanel::run_gradient_tool(Frame& f) {
    auto& frame = f.result;
    auto& ctx = f.ctx;
    if (role_ != ViewportRole::Program ||
        canvas_tool_ != CanvasTool::Gradient || drawing_masks_ ||
        !box_enabled_) {
        fill_drag_ = FillDrag::None;
        return;
    }
    f.navigating = true;
    const scene::MeshInstance* filled{};
    for (const scene::MeshInstance& instance : f.scene.instances) {
        if (instance.source == f.primary &&
            instance.material.fill.has_value()) {
            filled = &instance;
            break;
        }
    }
    const bool layer = filled != nullptr && f.layer_box.has_value() &&
                       f.layer_box->sides && frame.box.visible;
    if (!layer || (fill_drag_ != FillDrag::None && fill_node_ != f.primary)) {
        fill_drag_ = FillDrag::None;
    }
    // The tool owns the pane whether or not there is a fill to drag, so a
    // press with it never moves the layer by mistake.
    const gui::Interaction it = ctx.grab(
        gui::ui_id(id_, 0x6AD1EU),
        f.occluded || canvas_panning_ ? gui::Rect{} : f.pane);
    if (!layer) {
        return;
    }
    // The surface's real size, as the renderer measures it for the fill.
    const float width = scene::length(f.layer_box->world[1U] -
                                      f.layer_box->world[0U]);
    const float height = scene::length(f.layer_box->world[3U] -
                                       f.layer_box->world[0U]);
    const std::array<gui::UiPoint, 4U> quad = frame.box.corners;
    const gui::UiPoint pointer{ctx.pointer_x(), ctx.pointer_y()};
    const auto pointer_point = box_point_at(quad, pointer.x, pointer.y);
    scene::Fill shown = *filled->material.fill;
    if (shown.mode == scene::FillMode::Solid) {
        fill_drag_ = FillDrag::None;
        return;
    }
    const auto near_handle = [&](BoxPoint handle) {
        const auto at = box_point_to(quad, handle.u, handle.v);
        return at.has_value() &&
               std::hypot(pointer.x - at->x, pointer.y - at->y) <=
                   transform_box_hit_pixels;
    };
    if (it.pressed && pointer_point.has_value()) {
        const FillHandles handles = fill_handles(shown, width, height);
        fill_node_ = f.primary;
        fill_press_ = shown;
        fill_press_point_ = *pointer_point;
        // The end before the middle: on a short ramp the two are close,
        // and the end is the one that cannot be reached any other way.
        fill_drag_ = near_handle(handles.end)      ? FillDrag::End
                     : near_handle(handles.centre) ? FillDrag::Centre
                                                   : FillDrag::Line;
        shape_press_ = pointer;
    }
    if (fill_drag_ != FillDrag::None && it.held && !it.released &&
        pointer_point.has_value()) {
        const bool moved = std::hypot(pointer.x - shape_press_.x,
                                      pointer.y - shape_press_.y) > 0.0F;
        if (moved) {
            switch (fill_drag_) {
            case FillDrag::Centre:
                shown = fill_with_centre(
                    fill_press_,
                    BoxPoint{fill_press_.center_x + pointer_point->u -
                                 fill_press_point_.u,
                             fill_press_.center_y + pointer_point->v -
                                 fill_press_point_.v});
                break;
            case FillDrag::End:
                shown = fill_with_end(fill_press_, width, height,
                                      *pointer_point);
                break;
            case FillDrag::Line:
                shown = fill_along(fill_press_, width, height,
                                   fill_press_point_, *pointer_point);
                break;
            case FillDrag::None:
                break;
            }
            frame.fill = shown;
        }
    }
    if (it.released || !it.held) {
        fill_drag_ = FillDrag::None;
    }
    // The ramp as it now stands - the dragged one while a drag is under way,
    // so the handles lead the document rather than trail it.
    const FillHandles handles = fill_handles(shown, width, height);
    if (!handles.visible) {
        return;
    }
    std::vector<gui::UiPoint> outline;
    const auto add = [&](float u, float v) {
        if (const auto at = box_point_to(quad, u, v)) {
            outline.push_back(*at);
        }
    };
    if (shown.mode == scene::FillMode::Linear) {
        add(handles.start.u, handles.start.v);
        add(handles.end.u, handles.end.v);
    } else {
        const float radius_u = handles.end.u - handles.centre.u;
        const float radius_v = radius_u * width / height;
        for (std::size_t step = 0U; step <= ellipse_segments; ++step) {
            const float angle = tau * static_cast<float>(step) /
                                static_cast<float>(ellipse_segments);
            add(handles.centre.u + std::cos(angle) * radius_u,
                handles.centre.v + std::sin(angle) * radius_v);
        }
    }
    f.tool_outlines.push_back(std::move(outline));
    for (const BoxPoint handle : {handles.centre, handles.end}) {
        if (const auto at = box_point_to(quad, handle.u, handle.v)) {
            f.tool_points.push_back(*at);
        }
    }
}

void ViewportPanel::draw_shape_tool(Frame& f) {
    auto& ctx = f.ctx;
    gui::UiDrawList& list = ctx.draw_list();
    const gui::Theme& theme = ctx.theme();
    for (const std::vector<gui::UiPoint>& outline : f.tool_outlines) {
        for (std::size_t index = 1U; index < outline.size(); ++index) {
            haloed_line(list, outline[index - 1U], outline[index], 3.0F, 1.4F,
                        theme.accent);
        }
    }
    for (std::size_t index = 0U; index < f.tool_points.size(); ++index) {
        const bool closing = index == 0U && f.tool_closes;
        haloed_square(list, f.tool_points[index], closing ? 10.0F : 7.0F, 1.5F,
                      closing ? theme.accent : theme.text);
    }
}

}  // namespace mediaserver::editor
