#pragma once

#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_renderer.hpp>

#include <cstddef>

// The marks a canvas, a gizmo or a glyph draws that are not boxes: discs,
// rings and arcs, arrowheads. Each is placed by a point and sized in the draw
// list's logical units, so a node graph's port, a viewport's handle and an
// icon's hub come out of the same few shapes instead of each panel keeping
// its own circle.
//
// Angles are radians measured from +x and turning clockwise on screen, since
// y grows downwards.
namespace mediaserver::gui {

// The point `radius` from `centre` at `angle`.
[[nodiscard]] UiPoint on_circle(UiPoint centre, float radius, float angle) noexcept;

// A filled circle of `sides` sides. Twelve reads as round at every radius
// the UI draws one: a port, a reroute handle, a status dot.
void add_disc(UiDrawList& list, UiPoint centre, float radius, UiColor color,
              std::size_t sides = 12U);

// `sweep` of a circle's edge from `start`, stroked `thickness` wide with an
// anti-aliased edge, in `segments` straight pieces. A full turn closes.
void add_arc(
    UiDrawList& list,
    UiPoint centre,
    float radius,
    float start,
    float sweep,
    float thickness,
    UiColor color,
    std::size_t segments = 24U);

// The whole of a circle's edge.
void add_ring(
    UiDrawList& list,
    UiPoint centre,
    float radius,
    float thickness,
    UiColor color,
    std::size_t segments = 24U);

// A solid arrowhead with its tip at `tip`, pointing along `direction` (any
// length but zero, which draws nothing), `length` from base to tip and
// `half_width` either side of its axis.
void add_arrowhead(
    UiDrawList& list,
    UiPoint tip,
    UiPoint direction,
    float length,
    float half_width,
    UiColor color);

// A filled diamond, `radius` from its centre to each point: a key, an
// effect port, a rotate handle.
void add_diamond(UiDrawList& list, UiPoint centre, float radius, UiColor color);

// The four edges of `bounds`, each `thickness` deep and inside it. An empty
// box, or no thickness, draws nothing.
void add_outline(UiDrawList& list, const Rect& bounds, float thickness, UiColor color);

} // namespace mediaserver::gui
