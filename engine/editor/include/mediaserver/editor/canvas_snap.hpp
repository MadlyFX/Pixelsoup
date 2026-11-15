#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

// Snapping a moved box to lines, and the arithmetic of lining several boxes
// up, in the pixels of a canvas. Pure, so what a layer lands on - and which
// guide lights to say so - is tested with nothing open.
namespace mediaserver::editor {

// A box on the canvas: a layer's extent there, or the canvas itself.
struct CanvasBox final {
    float left{};
    float top{};
    float right{};
    float bottom{};

    [[nodiscard]] float centre_x() const noexcept {
        return (left + right) * 0.5F;
    }
    [[nodiscard]] float centre_y() const noexcept {
        return (top + bottom) * 0.5F;
    }

    bool operator==(const CanvasBox&) const = default;
};

// One axis of a snap: how far to shift the box beyond where the drag put it,
// and the line it then sits on. No line: nothing was within reach.
struct AxisSnap final {
    float shift{};
    std::optional<float> line;

    bool operator==(const AxisSnap&) const = default;
};

// Snaps the nearest of a box's near edge, middle and far edge (`edges`, as
// the drag has them) to the nearest of `lines`, if one is within `reach`.
// The smallest shift wins; of equal shifts, the earlier line.
[[nodiscard]] AxisSnap snap_axis(const std::array<float, 3U>& edges,
                                 std::span<const float> lines,
                                 float reach) noexcept;

// The lines a box offers others to snap to along one axis: its near edge,
// its middle and its far edge.
void add_snap_lines(std::vector<float>& lines, float low, float high);

enum class CanvasAlign : unsigned char {
    Left,
    CentreAcross,
    Right,
    Top,
    CentreDown,
    Bottom,
};

// How far across and down each of `boxes` moves to line up along `align`
// with `reference`: the box round them all, or the canvas for one alone.
struct CanvasMove final {
    float x{};
    float y{};

    bool operator==(const CanvasMove&) const = default;
};

[[nodiscard]] std::vector<CanvasMove>
align_boxes(std::span<const CanvasBox> boxes, const CanvasBox& reference,
            CanvasAlign align);

// The box round all of `boxes`; empty input gives an empty box.
[[nodiscard]] CanvasBox bounds_of(std::span<const CanvasBox> boxes) noexcept;

// Spaces the boxes' middles evenly between the two outermost, across
// (`across` true) or down. Fewer than three boxes have nothing to space.
[[nodiscard]] std::vector<CanvasMove>
distribute_boxes(std::span<const CanvasBox> boxes, bool across);

// The spacing between a ruler's numbered ticks: the smallest of 1, 2 or 5
// times a power of ten whose ticks are at least `minimum` panel units apart
// when one canvas pixel takes `scale` of them.
[[nodiscard]] float ruler_step(float scale, float minimum) noexcept;

}  // namespace mediaserver::editor
