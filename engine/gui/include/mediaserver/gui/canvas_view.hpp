#pragma once

#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_renderer.hpp>

// A pannable, zoomable 2D canvas: a node graph's view. Graph space is where
// the document keeps positions; the screen is where they are drawn. The view
// is only the pan - where the graph's origin sits, from the canvas's top left
// - and the zoom, screen units per graph unit. The canvas rectangle comes from
// each frame's layout, so every mapping takes it, and a panel that moves or
// resizes keeps the same part of the graph at the same place in it.
namespace mediaserver::gui {

struct CanvasView final {
    float pan_x{};
    float pan_y{};
    float zoom{1.0F};

    [[nodiscard]] UiPoint to_screen(const Rect& canvas, float x, float y) const noexcept {
        return UiPoint{canvas.x + pan_x + x * zoom, canvas.y + pan_y + y * zoom};
    }

    [[nodiscard]] Rect to_screen(const Rect& canvas, const Rect& graph) const noexcept {
        const UiPoint at = to_screen(canvas, graph.x, graph.y);
        return Rect{at.x, at.y, graph.width * zoom, graph.height * zoom};
    }

    // The graph point under a screen point. A view with no zoom shows no
    // graph, and answers the origin.
    [[nodiscard]] UiPoint to_graph(
        const Rect& canvas, float screen_x, float screen_y) const noexcept {
        if (zoom <= 0.0F) {
            return UiPoint{};
        }
        return UiPoint{(screen_x - canvas.x - pan_x) / zoom,
                       (screen_y - canvas.y - pan_y) / zoom};
    }

    // The part of the graph the canvas shows.
    [[nodiscard]] Rect visible(const Rect& canvas) const noexcept {
        if (zoom <= 0.0F) {
            return Rect{};
        }
        return Rect{-pan_x / zoom, -pan_y / zoom, canvas.width / zoom,
                    canvas.height / zoom};
    }

    // Changes the zoom to `next`, keeping the graph point under the screen
    // point (anchor_x, anchor_y) where it is: a wheel turn zooms about the
    // pointer, a button about the canvas's centre.
    void zoom_about(
        const Rect& canvas, float anchor_x, float anchor_y, float next) noexcept {
        const UiPoint held = to_graph(canvas, anchor_x, anchor_y);
        pan_x = anchor_x - canvas.x - held.x * next;
        pan_y = anchor_y - canvas.y - held.y * next;
        zoom = next;
    }

    // Pans so the graph point (x, y) sits at the canvas's centre.
    void centre_on(const Rect& canvas, float x, float y) noexcept {
        pan_x = canvas.width * 0.5F - x * zoom;
        pan_y = canvas.height * 0.5F - y * zoom;
    }

    bool operator==(const CanvasView&) const = default;
};

} // namespace mediaserver::gui
