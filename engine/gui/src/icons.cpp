#include <mediaserver/gui/ui_context.hpp>

#include <mediaserver/gui/shapes.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <span>

// The drawn marks: the eye, padlock and pipette, and every Icon. Each glyph is
// written in its own units about its centre - (0, 0) is the middle of the 14px
// square an icon fits, +x right and +y down - so its numbers are its shape, as
// on a designer's grid, rather than offsets re-added to a centre on every line.
namespace mediaserver::gui {
namespace {

constexpr float pi = 3.14159265F;
constexpr float tau = 6.28318530718F;

// A pen over the draw list with its origin at a glyph's centre and one colour.
// Every coordinate it takes is relative to that origin.
class Pen final {
public:
    Pen(UiDrawList& list, float x, float y, UiColor color) noexcept
        : list_(list), x_(x), y_(y), color_(color) {}

    // The same pen with its origin moved by (dx, dy): a part of a glyph drawn
    // about its own centre - an arrowhead at an arc's end, a chain link.
    [[nodiscard]] Pen shifted(float dx, float dy) const noexcept {
        return Pen{list_, x_ + dx, y_ + dy, color_};
    }
    [[nodiscard]] Pen shifted(UiPoint by) const noexcept { return shifted(by.x, by.y); }

    void rect(float left, float top, float width, float height) const {
        list_.add_rect(x_ + left, y_ + top, width, height, color_);
    }

    // A `size` square on the origin: a hub, a pupil.
    void dot(float size) const { rect(-size * 0.5F, -size * 0.5F, size, size); }

    void outline(float left, float top, float width, float height, float thickness) const {
        add_outline(list_, Rect{x_ + left, y_ + top, width, height}, thickness, color_);
    }
    // Soft boxes: a rounded fill, and a rounded stroke inside the box.
    void rounded(float left, float top, float width, float height, float radius) const {
        list_.add_rounded_rect(x_ + left, y_ + top, width, height, radius, color_);
    }
    void rounded_outline(float left, float top, float width, float height,
                         float radius, float thickness) const {
        list_.add_rounded_outline(x_ + left, y_ + top, width, height, radius,
                                  thickness, color_);
    }

    void line(float x0, float y0, float x1, float y1, float thickness) const {
        list_.add_line(x_ + x0, y_ + y0, x_ + x1, y_ + y1, thickness, color_);
    }

    void triangle(float x0, float y0, float x1, float y1, float x2, float y2) const {
        list_.add_triangle(x_ + x0, y_ + y0, x_ + x1, y_ + y1, x_ + x2, y_ + y2, color_);
    }

    // The transport's solid triangles, with the tip at (tip, 0) and the
    // base `half` above and below it, 1.6 halves behind the tip.
    void points_right(float tip, float half) const {
        triangle(tip - half * 1.6F, -half, tip, 0.0F, tip - half * 1.6F, half);
    }
    void points_left(float tip, float half) const {
        triangle(tip + half * 1.6F, -half, tip + half * 1.6F, half, tip, 0.0F);
    }

    void arrowhead(UiPoint tip, UiPoint direction, float length, float half_width) const {
        add_arrowhead(list_, at(tip), direction, length, half_width, color_);
    }

    // Part of a circle about the origin, anti-aliased.
    void arc(float radius, float start, float sweep, float thickness,
             std::size_t segments = 24U) const {
        add_arc(list_, at({}), radius, start, sweep, thickness, color_, segments);
    }

    // The same in `steps` hard-edged strokes, which keep a small ring crisp
    // where an anti-aliased one's fringe would thicken it.
    void stroked_arc(float radius, float start, float sweep, float thickness,
                     std::size_t steps) const {
        for (std::size_t index = 0U; index < steps; ++index) {
            const float a0 = start + sweep * static_cast<float>(index)
                / static_cast<float>(steps);
            const float a1 = start + sweep * static_cast<float>(index + 1U)
                / static_cast<float>(steps);
            line(std::cos(a0) * radius, std::sin(a0) * radius,
                 std::cos(a1) * radius, std::sin(a1) * radius, thickness);
        }
    }
    void ring(float radius, float thickness) const {
        add_ring(list_, at({}), radius, thickness, color_);
    }

    void polyline(std::initializer_list<UiPoint> points, float thickness) const {
        list_.add_polyline(placed(points), thickness, color_);
    }
    // A soft stroke: an anti-aliased polyline with round ends, the hand the
    // newer glyphs are drawn in.
    void stroke(std::initializer_list<UiPoint> points, float thickness = 1.6F) const {
        stroke(std::span<const UiPoint>{points.begin(), points.size()}, thickness);
    }
    void stroke(std::span<const UiPoint> points, float thickness = 1.6F) const {
        if (points.empty()) {
            return;
        }
        list_.add_polyline(placed(points), thickness, color_);
        const float cap = thickness * 0.5F;
        add_disc(list_, at(points.front()), cap, color_, 8U);
        add_disc(list_, at(points.back()), cap, color_, 8U);
    }
    void polyline(std::span<const UiPoint> points, float thickness) const {
        list_.add_polyline(placed(points), thickness, color_);
    }
    void fill(std::initializer_list<UiPoint> points) const {
        list_.add_convex_poly(placed(points), color_);
    }
    void fill(std::span<const UiPoint> points) const {
        list_.add_convex_poly(placed(points), color_);
    }

    // The four corners of `box` as brackets, each arm `across` long on the
    // horizontal and `down` long on the vertical, `thickness` deep, inside it.
    void corner_brackets(const Rect& box, float across, float down, float thickness) const {
        for (const bool right : {false, true}) {
            for (const bool bottom : {false, true}) {
                const float x = right ? box.right() : box.x;
                const float y = bottom ? box.bottom() : box.y;
                rect(right ? x - across : x, bottom ? y - thickness : y, across, thickness);
                rect(right ? x - thickness : x, bottom ? y - down : y, thickness, down);
            }
        }
    }

private:
    // Enough for any glyph; a longer outline is cut short.
    static constexpr std::size_t most_points = 32U;

    [[nodiscard]] UiPoint at(UiPoint offset) const noexcept {
        return UiPoint{x_ + offset.x, y_ + offset.y};
    }

    struct Placed final {
        std::array<UiPoint, most_points> points{};
        std::size_t count{};
        operator std::span<const UiPoint>() const noexcept {
            return std::span<const UiPoint>{points.data(), count};
        }
    };

    [[nodiscard]] Placed placed(std::span<const UiPoint> offsets) const noexcept {
        Placed out{};
        out.count = std::min(offsets.size(), most_points);
        for (std::size_t index = 0U; index < out.count; ++index) {
            out.points[index] = at(offsets[index]);
        }
        return out;
    }
    [[nodiscard]] Placed placed(std::initializer_list<UiPoint> offsets) const noexcept {
        return placed(std::span<const UiPoint>{offsets.begin(), offsets.size()});
    }

    UiDrawList& list_;
    float x_{};
    float y_{};
    UiColor color_{};
};

// `count` points round a circle about the origin, from `start` across
// `sweep`, both ends included.
template <std::size_t count>
[[nodiscard]] std::array<UiPoint, count> arc_points(
    float radius_x, float radius_y, float start, float sweep) noexcept {
    std::array<UiPoint, count> points{};
    for (std::size_t index = 0U; index < count; ++index) {
        const float angle =
            start + sweep * static_cast<float>(index) / static_cast<float>(count - 1U);
        points[index] = UiPoint{std::cos(angle) * radius_x, std::sin(angle) * radius_y};
    }
    return points;
}

} // namespace

void draw_eye_glyph(
    UiDrawList& list, float center_x, float center_y, bool open, UiColor color) {
    const Pen pen{list, center_x, center_y, color};
    constexpr float half = 6.0F;
    constexpr float lid = 3.5F;
    constexpr std::size_t steps = 6U;
    // Each lid is a sine bow across the eye's width, in straight pieces.
    const auto bow = [&](float side) {
        for (std::size_t index = 0U; index < steps; ++index) {
            const float t0 = static_cast<float>(index) / static_cast<float>(steps);
            const float t1 = static_cast<float>(index + 1U) / static_cast<float>(steps);
            pen.line(-half + t0 * half * 2.0F, side * lid * std::sin(t0 * pi),
                     -half + t1 * half * 2.0F, side * lid * std::sin(t1 * pi), 1.0F);
        }
    };
    bow(1.0F);
    if (!open) {
        // A closed lid is the lower arc alone, with two lashes. An eye that is
        // not looking is not an eye with a line through it.
        pen.line(-half, 1.0F, -half - 2.0F, 3.0F, 1.0F);
        pen.line(half, 1.0F, half + 2.0F, 3.0F, 1.0F);
        return;
    }
    bow(-1.0F);
    pen.dot(3.0F);
}

void draw_padlock_glyph(
    UiDrawList& list,
    float center_x,
    float center_y,
    bool closed,
    UiColor color) {
    // The body's top edge is the glyph's origin line.
    const Pen pen{list, center_x, center_y - 1.0F, color};
    // An open lock lifts its shackle clear of the body and shifts it aside, so
    // the two states differ in silhouette and not only in fill.
    const Pen shackle = pen.shifted(closed ? 0.0F : 2.0F, 0.0F);
    shackle.rect(-3.5F, -5.0F, 1.0F, 5.0F);
    shackle.rect(2.5F, -5.0F, 1.0F, closed ? 5.0F : 3.0F);
    shackle.rect(-3.5F, -6.0F, 7.0F, 1.0F);
    if (closed) {
        pen.rect(-5.0F, 0.0F, 10.0F, 8.0F);
        return;
    }
    pen.outline(-5.0F, 0.0F, 10.0F, 8.0F, 1.0F);
}

void draw_eyedropper_glyph(
    UiDrawList& list, float center_x, float center_y, UiColor color) {
    const Pen pen{list, center_x, center_y, color};
    // Two parallel strokes make the glass tube; the short diagonal at the
    // upper-right is its squeeze bulb and the converging lower-left strokes
    // form the sampling tip. This stays legible at the Inspector's 18px row.
    pen.line(2.0F, -5.0F, -4.0F, 1.0F, 2.0F);
    pen.line(5.0F, -2.0F, -1.0F, 4.0F, 2.0F);
    pen.line(1.0F, -6.0F, 6.0F, -1.0F, 2.0F);
    pen.line(-4.0F, 1.0F, -6.0F, 6.0F, 1.5F);
    pen.line(-1.0F, 4.0F, -6.0F, 6.0F, 1.5F);
    pen.rect(-7.0F, 6.0F, 2.0F, 2.0F);
}

void draw_icon(
    UiDrawList& list,
    Icon icon,
    float center_x,
    float center_y,
    UiColor color,
    float scale) {
    if (std::isfinite(scale) && scale > 0.0F && scale != 1.0F) {
        const float previous_scale = list.scale();
        list.set_scale(previous_scale * scale);
        draw_icon(list, icon, center_x / scale, center_y / scale, color);
        list.set_scale(previous_scale);
        return;
    }
    const Pen pen{list, center_x, center_y, color};
    switch (icon) {
    case Icon::Play:
        pen.points_right(5.0F, 5.5F);
        return;
    case Icon::Pause:
        pen.rect(-4.0F, -5.0F, 3.0F, 10.0F);
        pen.rect(1.0F, -5.0F, 3.0F, 10.0F);
        return;
    case Icon::StepBack:
        pen.points_left(-4.0F, 4.5F);
        pen.rect(3.0F, -4.5F, 2.0F, 9.0F);
        return;
    case Icon::StepForward:
        pen.rect(-5.0F, -4.5F, 2.0F, 9.0F);
        pen.points_right(4.0F, 4.5F);
        return;
    case Icon::ToStart:
        pen.rect(-6.0F, -4.5F, 2.0F, 9.0F);
        pen.points_left(-4.0F, 4.5F);
        pen.points_left(1.0F, 4.5F);
        return;
    case Icon::ToEnd:
        pen.points_right(-1.0F, 4.5F);
        pen.points_right(4.0F, 4.5F);
        pen.rect(4.0F, -4.5F, 2.0F, 9.0F);
        return;
    case Icon::Loop: {
        // Most of a ring running into its arrowhead: the round trip.
        constexpr float radius = 5.0F;
        constexpr float start = 0.6F;
        constexpr float sweep = 4.6F;
        pen.arc(radius, start, sweep, 1.6F, 16U);
        const float end = start + sweep;
        const UiPoint at = on_circle({}, radius, end);
        const UiPoint along{-std::sin(end), std::cos(end)};
        pen.arrowhead(UiPoint{at.x + along.x * 2.5F, at.y + along.y * 2.5F},
                      along, 4.0F, 3.2F);
        return;
    }
    case Icon::Snap:
        // A horseshoe magnet, legs up, in one soft stroke.
        pen.stroke({{-4.5F, -5.5F}, {-4.5F, 0.5F}}, 2.4F);
        pen.stroke({{4.5F, -5.5F}, {4.5F, 0.5F}}, 2.4F);
        pen.shifted(0.0F, 0.5F).arc(4.5F, 0.0F, pi, 2.4F, 12U);
        return;
    case Icon::Marker:
        // A pennant on its staff.
        pen.stroke({{-4.0F, -6.0F}, {-4.0F, 6.5F}});
        pen.fill({{-4.0F, -6.0F}, {5.0F, -3.5F}, {-4.0F, -1.0F}});
        return;
    case Icon::Hold:
        // An octagon with flat sides top and bottom, as a stop sign stands.
        pen.fill(arc_points<8U>(6.0F, 6.0F, pi / 8.0F, pi * 7.0F / 4.0F));
        return;
    case Icon::Select:
        // A pointer: the arrow filled, its edge one soft stroke round it.
        // Sharp tips mitre long, so the arrow keeps clear of the square's
        // edge.
        pen.fill({{-4.0F, -5.5F}, {4.5F, 1.0F}, {0.6F, 1.8F}});
        pen.fill({{-4.0F, -5.5F}, {0.6F, 1.8F}, {-1.2F, 5.5F}});
        pen.stroke({{-4.0F, -5.5F}, {4.5F, 1.0F}, {0.6F, 1.8F}, {-1.2F, 5.5F},
                    {-4.0F, -5.5F}},
                   1.3F);
        return;
    case Icon::Ripple:
        // A clip's closing edge pushing everything after it along.
        pen.stroke({{-5.5F, -6.0F}, {-2.0F, -6.0F}, {-2.0F, 6.0F}, {-5.5F, 6.0F}});
        pen.stroke({{0.5F, 0.0F}, {4.5F, 0.0F}});
        pen.arrowhead(UiPoint{6.5F, 0.0F}, UiPoint{1.0F, 0.0F}, 3.5F, 3.0F);
        pen.stroke({{6.5F, -6.0F}, {6.5F, -3.5F}}, 1.4F);
        pen.stroke({{6.5F, 3.5F}, {6.5F, 6.0F}}, 1.4F);
        return;
    case Icon::Roll:
        // The edit point two clips share, moved either way.
        pen.stroke({{-3.5F, -6.0F}, {-1.0F, -6.0F}, {-1.0F, 6.0F}, {-3.5F, 6.0F}});
        pen.stroke({{3.5F, -6.0F}, {1.0F, -6.0F}, {1.0F, 6.0F}, {3.5F, 6.0F}});
        pen.arrowhead(UiPoint{-7.0F, 0.0F}, UiPoint{-1.0F, 0.0F}, 3.0F, 2.6F);
        pen.arrowhead(UiPoint{7.0F, 0.0F}, UiPoint{1.0F, 0.0F}, 3.0F, 2.6F);
        return;
    case Icon::Slip:
        // The source moves inside a clip that stays put.
        pen.rounded_outline(-7.0F, -5.0F, 14.0F, 10.0F, 2.5F, 1.4F);
        pen.stroke({{-2.0F, 0.0F}, {2.0F, 0.0F}}, 1.4F);
        pen.arrowhead(UiPoint{-4.5F, 0.0F}, UiPoint{-1.0F, 0.0F}, 2.6F, 2.3F);
        pen.arrowhead(UiPoint{4.5F, 0.0F}, UiPoint{1.0F, 0.0F}, 2.6F, 2.3F);
        return;
    case Icon::Slide:
        // The clip moves between neighbours that stay put.
        pen.rounded(-2.75F, -4.5F, 5.5F, 9.0F, 1.8F);
        pen.arrowhead(UiPoint{-7.0F, 0.0F}, UiPoint{-1.0F, 0.0F}, 3.0F, 2.6F);
        pen.arrowhead(UiPoint{7.0F, 0.0F}, UiPoint{1.0F, 0.0F}, 3.0F, 2.6F);
        return;
    case Icon::Razor:
        // Scissors: two finger rings and their crossing blades.
        pen.shifted(-4.5F, -3.5F).ring(1.9F, 1.4F);
        pen.shifted(-4.5F, 3.5F).ring(1.9F, 1.4F);
        pen.stroke({{-3.0F, -2.2F}, {6.5F, 4.5F}}, 1.6F);
        pen.stroke({{-3.0F, 2.2F}, {6.5F, -4.5F}}, 1.6F);
        return;
    case Icon::Fit:
        // Four soft corners closing in on what fits between them.
        pen.stroke({{-6.0F, -2.0F}, {-6.0F, -5.0F}, {-3.0F, -5.0F}});
        pen.stroke({{3.0F, -5.0F}, {6.0F, -5.0F}, {6.0F, -2.0F}});
        pen.stroke({{6.0F, 2.0F}, {6.0F, 5.0F}, {3.0F, 5.0F}});
        pen.stroke({{-3.0F, 5.0F}, {-6.0F, 5.0F}, {-6.0F, 2.0F}});
        return;
    case Icon::Plus:
        pen.rect(-5.0F, -1.0F, 10.0F, 2.0F);
        pen.rect(-1.0F, -5.0F, 2.0F, 10.0F);
        return;
    case Icon::Stop:
        pen.dot(9.0F);
        return;
    case Icon::Restart: {
        // Most of a ring running into its arrowhead: go round again.
        constexpr float radius = 5.0F;
        constexpr float start = -1.0F;
        constexpr float sweep = 4.6F;
        pen.arc(radius, start, sweep, 1.6F, 16U);
        const float end = start + sweep;
        const UiPoint at = on_circle({}, radius, end);
        const UiPoint along{-std::sin(end), std::cos(end)};
        pen.arrowhead(UiPoint{at.x + along.x * 2.5F, at.y + along.y * 2.5F},
                      along, 4.0F, 3.2F);
        return;
    }
    case Icon::Details:
        pen.stroke({{-5.0F, -4.0F}, {5.0F, -4.0F}});
        pen.stroke({{-5.0F, 0.0F}, {5.0F, 0.0F}});
        pen.stroke({{-5.0F, 4.0F}, {5.0F, 4.0F}});
        return;
    case Icon::Duplicate:
        // The back frame as a soft outline, the front one solid over it.
        pen.rounded_outline(-6.5F, -6.5F, 9.0F, 9.0F, 2.5F, 1.4F);
        pen.rounded(-2.5F, -2.5F, 9.0F, 9.0F, 2.5F);
        return;
    case Icon::Cache:
        pen.rect(-7.0F, 2.0F, 8.0F, 3.0F);
        pen.rect(1.0F, 3.0F, 6.0F, 1.0F);
        pen.triangle(-1.0F, -6.0F, 5.0F, -6.0F, 2.0F, -2.5F);
        pen.rect(1.5F, -3.0F, 1.0F, 8.0F);
        return;
    case Icon::MoveTool:
        pen.rect(-4.5F, -0.75F, 9.0F, 1.5F);
        pen.rect(-0.75F, -4.5F, 1.5F, 9.0F);
        // An arrowhead at each end of the cross.
        for (const UiPoint out : {UiPoint{1.0F, 0.0F}, UiPoint{-1.0F, 0.0F},
                                  UiPoint{0.0F, -1.0F}, UiPoint{0.0F, 1.0F}}) {
            pen.arrowhead(UiPoint{out.x * 7.0F, out.y * 7.0F}, out, 3.5F, 3.5F);
        }
        return;
    case Icon::RotateTool: {
        // Most of a ring, turning clockwise into its arrowhead, round a hub.
        // The gap before the head is what makes it read as turning.
        constexpr float start = -1.2F;
        constexpr float sweep = 4.3F;
        constexpr float radius = 5.0F;
        pen.arc(radius, start, sweep, 1.6F);
        const float end = start + sweep;
        const UiPoint at = on_circle({}, radius, end);
        // Along the turn.
        const UiPoint along{-std::sin(end), std::cos(end)};
        pen.arrowhead(UiPoint{at.x + along.x * 4.5F, at.y + along.y * 4.5F}, along,
                      5.0F, 3.8F);
        pen.dot(2.5F);
        return;
    }
    case Icon::ScaleTool:
        pen.rect(-6.5F, 1.5F, 5.0F, 5.0F);
        pen.rect(-6.5F, -6.5F, 1.2F, 6.0F);
        pen.rect(-6.5F, -6.5F, 6.0F, 1.2F);
        pen.rect(5.3F, 0.5F, 1.2F, 6.0F);
        pen.rect(0.5F, 5.3F, 6.0F, 1.2F);
        pen.line(-1.0F, 1.0F, 3.0F, -3.0F, 1.6F);
        pen.triangle(6.5F, -6.5F, 6.5F, -1.5F, 1.5F, -6.5F);
        return;
    case Icon::Measure:
        // A ruler's edge with its graduations, longer at the middle.
        pen.outline(-7.0F, -3.5F, 14.0F, 7.0F, 1.2F);
        for (int tick = 0; tick < 5; ++tick) {
            const float x = -4.5F + 2.25F * static_cast<float>(tick);
            pen.rect(x - 0.5F, -3.5F, 1.0F, tick == 2 ? 3.5F : 2.0F);
        }
        return;
    case Icon::World:
        pen.arc(6.0F, 0.0F, tau, 1.2F);
        pen.polyline(arc_points<17U>(2.6F, 6.0F, 0.0F, tau), 1.1F);
        pen.rect(-6.0F, -0.5F, 12.0F, 1.0F);
        return;
    case Icon::Local:
        // An isometric cube: its outline, and the three edges meeting at the
        // near corner.
        pen.polyline({{0.0F, -6.5F}, {5.6F, -3.25F}, {5.6F, 3.25F}, {0.0F, 6.5F},
                      {-5.6F, 3.25F}, {-5.6F, -3.25F}, {0.0F, -6.5F}},
                     1.2F);
        pen.polyline({{-5.6F, -3.25F}, {0.0F, 0.0F}, {5.6F, -3.25F}}, 1.2F);
        pen.rect(-0.6F, 0.0F, 1.2F, 6.5F);
        return;
    case Icon::Grid:
        // A soft square cut into nine.
        pen.rounded_outline(-6.5F, -6.5F, 13.0F, 13.0F, 3.0F, 1.4F);
        for (int line = 1; line < 3; ++line) {
            const float at = -6.5F + 13.0F * static_cast<float>(line) / 3.0F;
            pen.rect(at - 0.6F, -6.0F, 1.2F, 12.0F);
            pen.rect(-6.0F, at - 0.6F, 12.0F, 1.2F);
        }
        return;
    case Icon::Angle: {
        // From the vertex at the lower left: the floor ray, the rising ray,
        // and the swept angle between them.
        const Pen vertex = pen.shifted(-6.0F, 5.0F);
        vertex.rect(0.0F, -0.6F, 13.0F, 1.2F);
        vertex.line(0.0F, 0.0F, 9.5F, -11.5F, 1.3F);
        vertex.polyline(arc_points<7U>(7.0F, 7.0F, 0.0F, -0.88F), 1.2F);
        return;
    }
    case Icon::Surface:
        pen.rect(-7.0F, 5.0F, 14.0F, 1.5F);
        pen.rect(-3.0F, 0.5F, 6.0F, 4.5F);
        pen.rect(-0.75F, -7.0F, 1.5F, 3.0F);
        pen.triangle(-3.0F, -4.5F, 3.0F, -4.5F, 0.0F, -1.5F);
        return;
    case Icon::Guides:
        // The corners of a safe frame, and its centre.
        pen.corner_brackets(Rect{-7.0F, -5.0F, 14.0F, 10.0F}, 4.0F, 3.0F, 1.2F);
        pen.rect(-2.5F, -0.5F, 5.0F, 1.0F);
        pen.rect(-0.5F, -2.5F, 1.0F, 5.0F);
        return;
    case Icon::Gizmo:
        pen.rect(-3.0F, -3.0F, 1.5F, 6.0F);
        pen.triangle(-2.25F, -7.0F, 0.25F, -3.0F, -4.75F, -3.0F);
        pen.rect(-3.0F, 1.5F, 6.5F, 1.5F);
        pen.triangle(7.0F, 2.25F, 3.0F, -0.25F, 3.0F, 4.75F);
        pen.line(-2.5F, 2.5F, -6.5F, 6.5F, 1.5F);
        return;
    case Icon::Bounds:
        pen.corner_brackets(Rect{-6.5F, -6.5F, 13.0F, 13.0F}, 4.0F, 4.0F, 1.2F);
        pen.dot(4.0F);
        return;
    case Icon::Screen:
        pen.rounded_outline(-7.0F, -6.0F, 14.0F, 9.5F, 2.5F, 1.4F);
        pen.stroke({{0.0F, 3.5F}, {0.0F, 5.5F}}, 1.4F);
        pen.stroke({{-3.0F, 6.0F}, {3.0F, 6.0F}}, 1.4F);
        return;
    case Icon::Projection:
        // A frustum opening away from its eye, with the eye as a dot.
        pen.polyline({{-4.0F, -2.0F}, {6.5F, -6.0F}, {6.5F, 6.0F}, {-4.0F, 2.0F},
                      {-4.0F, -2.0F}},
                     1.2F);
        pen.rect(-7.5F, -1.25F, 2.5F, 2.5F);
        return;
    case Icon::Shading:
        // The outline, and the right half filled: lit from the right.
        pen.arc(6.0F, 0.0F, tau, 1.2F);
        pen.fill(arc_points<10U>(6.0F, 6.0F, -pi * 0.5F, pi));
        return;
    case Icon::Target:
        pen.arc(4.5F, 0.0F, tau, 1.3F);
        pen.rect(-7.0F, -0.6F, 3.0F, 1.2F);
        pen.rect(4.0F, -0.6F, 3.0F, 1.2F);
        pen.rect(-0.6F, -7.0F, 1.2F, 3.0F);
        pen.rect(-0.6F, 4.0F, 1.2F, 3.0F);
        pen.dot(2.5F);
        return;
    case Icon::SyncLock: {
        // Two capsule outlines on the diagonal, each through the other. Each
        // runs up and to the right, its ends half circles facing away.
        constexpr float half = 2.6F;
        constexpr float radius = 2.4F;
        constexpr float axis = -pi / 4.0F;
        const UiPoint reach = on_circle({}, half, axis);
        std::array<UiPoint, 19U> capsule{};
        const auto near_end = arc_points<9U>(radius, radius, axis + pi * 0.5F, pi);
        const auto far_end = arc_points<9U>(radius, radius, axis - pi * 0.5F, pi);
        for (std::size_t index = 0U; index < 9U; ++index) {
            capsule[index] = UiPoint{near_end[index].x - reach.x, near_end[index].y - reach.y};
            capsule[index + 9U] = UiPoint{far_end[index].x + reach.x, far_end[index].y + reach.y};
        }
        capsule.back() = capsule.front();
        pen.shifted(-2.0F, 2.0F).polyline(capsule, 1.3F);
        pen.shifted(2.0F, -2.0F).polyline(capsule, 1.3F);
        return;
    }
    case Icon::Solo:
        pen.arc(5.0F, pi, pi, 1.4F);
        pen.rect(-6.2F, 0.0F, 3.0F, 5.5F);
        pen.rect(3.2F, 0.0F, 3.0F, 5.5F);
        return;
    case Icon::FullResolution:
        pen.outline(-6.0F, -6.0F, 12.0F, 12.0F, 1.2F);
        pen.rect(-4.0F, -4.0F, 8.0F, 8.0F);
        return;
    case Icon::HalfResolution:
        pen.outline(-6.0F, -6.0F, 12.0F, 12.0F, 1.2F);
        pen.rect(-4.0F, -4.0F, 4.0F, 4.0F);
        return;
    case Icon::Moments:
        pen.stroke({{0.0F, -1.0F}, {0.0F, 6.5F}});
        pen.fill({{0.0F, -7.0F}, {3.5F, -3.5F}, {0.0F, 0.0F}, {-3.5F, -3.5F}});
        return;
    case Icon::Search:
        // A lens up and to the left, its handle running down to the right.
        pen.shifted(-1.5F, -1.5F).stroked_arc(2.35F, 0.0F, tau, 1.3F, 16U);
        pen.line(0.5F, 0.5F, 3.5F, 3.5F, 1.0F);
        return;
    case Icon::Take:
        // Preview goes up to the wall as Program comes down.
        pen.stroke({{-6.0F, -3.0F}, {5.5F, -3.0F}});
        pen.stroke({{2.5F, -6.0F}, {5.5F, -3.0F}, {2.5F, 0.0F}});
        pen.stroke({{6.0F, 3.5F}, {-5.5F, 3.5F}});
        pen.stroke({{-2.5F, 0.5F}, {-5.5F, 3.5F}, {-2.5F, 6.5F}});
        return;
    case Icon::Night: {
        // The lit edge of the moon (most of a circle) and its shadowed edge
        // (an arc of a second circle up and to the right), meeting at the
        // horns.
        const auto lit = arc_points<13U>(6.0F, 6.0F, 0.2817F, 4.330F);
        const auto shade = arc_points<9U>(5.0F, 5.0F, 3.909F, -2.924F);
        std::array<UiPoint, 13U + 9U> outline{};
        for (std::size_t index = 0U; index < lit.size(); ++index) {
            outline[index] = lit[index];
        }
        for (std::size_t index = 0U; index < shade.size(); ++index) {
            outline[lit.size() + index] =
                UiPoint{shade[index].x + 3.0F, shade[index].y - 2.5F};
        }
        pen.shifted(-0.5F, 0.5F).stroke(outline, 1.5F);
        return;
    }
    case Icon::Import:
        pen.stroke({{-6.0F, 2.0F}, {-6.0F, 6.0F}, {6.0F, 6.0F}, {6.0F, 2.0F}});
        pen.stroke({{0.0F, -6.5F}, {0.0F, 2.5F}});
        pen.stroke({{-3.25F, -0.75F}, {0.0F, 2.5F}, {3.25F, -0.75F}});
        return;
    case Icon::Publish:
        pen.stroke({{-6.0F, 2.0F}, {-6.0F, 6.0F}, {6.0F, 6.0F}, {6.0F, 2.0F}});
        pen.stroke({{0.0F, 3.0F}, {0.0F, -6.0F}});
        pen.stroke({{-3.25F, -2.75F}, {0.0F, -6.0F}, {3.25F, -2.75F}});
        return;
    case Icon::Rescan: {
        // Two arcs chasing each other round.
        constexpr float radius = 5.5F;
        for (const float start : {-2.75F, 0.39F}) {
            constexpr float sweep = 2.2F;
            pen.arc(radius, start, sweep, 1.6F, 12U);
            const float end = start + sweep;
            const UiPoint at = on_circle({}, radius, end);
            const UiPoint along{-std::sin(end), std::cos(end)};
            pen.arrowhead(UiPoint{at.x + along.x * 2.5F, at.y + along.y * 2.5F},
                          along, 4.0F, 3.0F);
        }
        return;
    }
    case Icon::Trash:
        pen.stroke({{-6.0F, -3.75F}, {6.0F, -3.75F}});
        pen.stroke({{-2.0F, -3.75F}, {-2.0F, -6.0F}, {2.0F, -6.0F}, {2.0F, -3.75F}}, 1.4F);
        pen.stroke({{-4.5F, -3.75F}, {-3.5F, 6.25F}, {3.5F, 6.25F}, {4.5F, -3.75F}});
        pen.stroke({{-1.2F, -0.75F}, {-1.0F, 3.25F}}, 1.3F);
        pen.stroke({{1.2F, -0.75F}, {1.0F, 3.25F}}, 1.3F);
        return;
    case Icon::Power:
        pen.arc(5.5F, -pi * 0.5F + 0.62F, tau - 1.24F, 1.7F, 20U);
        pen.stroke({{0.0F, -6.75F}, {0.0F, -0.5F}}, 1.7F);
        return;
    case Icon::MarkIn:
        pen.stroke({{-1.5F, -6.0F}, {-4.5F, -6.0F}, {-4.5F, 6.0F}, {-1.5F, 6.0F}});
        pen.stroke({{0.5F, 0.0F}, {6.0F, 0.0F}});
        pen.stroke({{3.5F, -2.5F}, {6.0F, 0.0F}, {3.5F, 2.5F}});
        return;
    case Icon::MarkOut:
        pen.stroke({{1.5F, -6.0F}, {4.5F, -6.0F}, {4.5F, 6.0F}, {1.5F, 6.0F}});
        pen.stroke({{-0.5F, 0.0F}, {-6.0F, 0.0F}});
        pen.stroke({{-3.5F, -2.5F}, {-6.0F, 0.0F}, {-3.5F, 2.5F}});
        return;
    case Icon::Insert:
    case Icon::Overwrite:
    case Icon::Append: {
        // A track along the bottom, and the edit coming down into it.
        const float drop_x = icon == Icon::Append ? 4.25F : 0.0F;
        const float track_right = icon == Icon::Append ? 1.5F : 6.5F;
        pen.stroke({{-6.5F, 1.0F}, {track_right, 1.0F}, {track_right, 6.5F},
                    {-6.5F, 6.5F}, {-6.5F, 1.0F}},
                   1.4F);
        if (icon == Icon::Insert) {
            pen.stroke({{0.0F, 1.0F}, {0.0F, 6.5F}}, 1.4F);
        } else if (icon == Icon::Overwrite) {
            pen.rect(-2.5F, 1.0F, 5.0F, 5.5F);
        } else {
            pen.outline(3.0F, 1.0F, 3.5F, 5.5F, 1.0F);
        }
        pen.stroke({{drop_x, -6.5F}, {drop_x, -1.5F}});
        pen.stroke({{drop_x - 2.5F, -3.75F}, {drop_x, -1.25F}, {drop_x + 2.5F, -3.75F}});
        return;
    }
    case Icon::Close:
        pen.stroke({{-4.5F, -4.5F}, {4.5F, 4.5F}});
        pen.stroke({{4.5F, -4.5F}, {-4.5F, 4.5F}});
        return;
    case Icon::Droplet: {
        // A round foot tapering to a point: two tangents from the tip and
        // the arc between them.
        const auto foot = arc_points<13U>(4.2F, 4.2F, -0.531F, 4.203F);
        std::array<UiPoint, 13U + 2U> outline{};
        outline.front() = UiPoint{0.0F, -6.5F};
        for (std::size_t index = 0U; index < foot.size(); ++index) {
            outline[index + 1U] = UiPoint{foot[index].x, foot[index].y + 1.8F};
        }
        outline.back() = outline.front();
        pen.stroke(outline, 1.5F);
        return;
    }
    case Icon::Check:
        pen.stroke({{-5.0F, 0.5F}, {-1.5F, 4.0F}, {5.5F, -3.5F}}, 1.8F);
        return;
    case Icon::Link:
        draw_icon(list, Icon::SyncLock, center_x, center_y, color);
        return;
    case Icon::Minus:
        pen.stroke({{-5.0F, 0.0F}, {5.0F, 0.0F}}, 1.8F);
        return;
    case Icon::PlayRange:
        // Play between two marks: a triangle inside a pair of brackets.
        pen.stroke({{-3.5F, -5.5F}, {-6.0F, -5.5F}, {-6.0F, 5.5F}, {-3.5F, 5.5F}}, 1.5F);
        pen.stroke({{3.5F, -5.5F}, {6.0F, -5.5F}, {6.0F, 5.5F}, {3.5F, 5.5F}}, 1.5F);
        pen.fill({{-2.0F, -3.5F}, {3.5F, 0.0F}, {-2.0F, 3.5F}});
        return;
    case Icon::Reset: {
        // Most of a ring turning back anticlockwise into its arrowhead, the
        // way Unreal marks a value that differs from its default.
        constexpr float radius = 4.75F;
        constexpr float start = -0.6F;
        constexpr float sweep = -4.3F;
        pen.arc(radius, start, sweep, 1.6F, 14U);
        const float end = start + sweep;
        const UiPoint at = on_circle({}, radius, end);
        const UiPoint along{std::sin(end), -std::cos(end)};
        pen.arrowhead(UiPoint{at.x + along.x * 3.0F, at.y + along.y * 3.0F},
                      along, 4.0F, 3.2F);
        return;
    }
    case Icon::Map:
        pen.stroke({{-6.5F, -5.0F}, {6.5F, -5.0F}, {6.5F, 5.0F}, {-6.5F, 5.0F},
                    {-6.5F, -5.0F}},
                   1.4F);
        pen.rect(0.5F, 0.0F, 4.5F, 3.5F);
        return;
    }
}

} // namespace mediaserver::gui
