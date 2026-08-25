#include <mediaserver/editor/viewport_panel.hpp>

#include <mediaserver/scene/manipulate.hpp>

namespace mediaserver::editor {

CanvasPoint to_canvas(
    const gui::Rect& image,
    scene::Canvas canvas,
    float pointer_x,
    float pointer_y) noexcept {
    CanvasPoint point{};
    if (image.width <= 0.0F || image.height <= 0.0F) {
        return point;
    }
    point.x = ((pointer_x - image.x) / image.width)
        * static_cast<float>(canvas.width);
    point.y = ((pointer_y - image.y) / image.height)
        * static_cast<float>(canvas.height);
    point.inside = image.contains(pointer_x, pointer_y);
    return point;
}

ViewportFrame ViewportPanel::draw(
    gui::UiContext& ctx,
    const gui::Rect& bounds,
    const rhi::Texture& target,
    const scene::Scene& scene) {
    ViewportFrame frame{};

    const auto canvas_width = static_cast<float>(scene.canvas.width);
    const auto canvas_height = static_cast<float>(scene.canvas.height);
    if (canvas_width <= 0.0F || canvas_height <= 0.0F) {
        return frame;
    }

    // Letterboxed to the canvas, never stretched: a pixel the operator clicks
    // has to be the pixel the renderer drew, or picking lies.
    frame.image = ctx.image_fit(bounds, target, canvas_width / canvas_height);
    if (frame.image.width <= 0.0F || frame.image.height <= 0.0F) {
        return frame;
    }

    const gui::Interaction it = ctx.grab(id_, frame.image);
    frame.hovered = it.hovered;
    frame.pointer =
        to_canvas(frame.image, scene.canvas, ctx.pointer_x(), ctx.pointer_y());

    if (it.pressed) {
        frame.pressed = true;
        const scene::MediaInstance* const hit = scene::pick_media(
            scene, scene.canvas, frame.pointer.x, frame.pointer.y);

        dragging_ = false;
        if (hit != nullptr) {
            frame.picked = hit->source_node;
            // A drag needs a depth to act at, which is the picked element's own
            // world position. Without one - a press on bare canvas - there is
            // nothing to move, and the press is a deselect instead.
            const auto world = scene::world_for(scene, hit->source_node);
            if (world.has_value()) {
                press_world_ = scene::translation_of(world.value());
                press_x_ = frame.pointer.x;
                press_y_ = frame.pointer.y;
                dragging_ = true;
            }
        }
    }

    if (it.held && dragging_) {
        const float dx = frame.pointer.x - press_x_;
        const float dy = frame.pointer.y - press_y_;
        if (dx != 0.0F || dy != 0.0F) {
            frame.dragging = true;
            // From the press, not from the last frame: a delta accumulated
            // frame by frame would drift, and this one is exact wherever the
            // pointer went in between.
            frame.translation = scene::screen_drag_to_translation(
                scene.camera, scene.canvas, press_world_, dx, dy);
        }
    }

    if (it.released) {
        frame.released = dragging_;
        dragging_ = false;
    }
    return frame;
}

} // namespace mediaserver::editor
