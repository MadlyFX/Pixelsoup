#include <mediaserver/editor/transform_box.hpp>

#include <mediaserver/editor/gizmo.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace mediaserver::editor {

namespace {

constexpr std::array<TransformBoxHandle, 8U> resize_handles{
    TransformBoxHandle::TopLeft,     TransformBoxHandle::Top,
    TransformBoxHandle::TopRight,    TransformBoxHandle::Right,
    TransformBoxHandle::BottomRight, TransformBoxHandle::Bottom,
    TransformBoxHandle::BottomLeft,  TransformBoxHandle::Left};

[[nodiscard]] float distance(gui::UiPoint a, gui::UiPoint b) noexcept {
    return std::hypot(a.x - b.x, a.y - b.y);
}

[[nodiscard]] gui::UiPoint midpoint(gui::UiPoint a, gui::UiPoint b) noexcept {
    return gui::UiPoint{(a.x + b.x) * 0.5F, (a.y + b.y) * 0.5F};
}

// Whether an edge handle has room between its two corners.
[[nodiscard]] bool edge_shown(const TransformBox& box,
                              TransformBoxHandle handle) noexcept {
    if (!box.sides) {
        return false;
    }
    const bool horizontal = handle == TransformBoxHandle::Top ||
                            handle == TransformBoxHandle::Bottom;
    const float length = horizontal
                             ? std::min(distance(box.corners[0U],
                                                 box.corners[1U]),
                                        distance(box.corners[3U],
                                                 box.corners[2U]))
                             : std::min(distance(box.corners[0U],
                                                 box.corners[3U]),
                                        distance(box.corners[1U],
                                                 box.corners[2U]));
    return length >= transform_box_edge_minimum;
}

}  // namespace

TransformBox transform_box(const std::array<gui::UiPoint, 4U>& corners,
                           bool sides) noexcept {
    TransformBox box{};
    box.sides = sides;
    box.corners = corners;
    float area_twice = 0.0F;
    for (std::size_t index = 0U; index < corners.size(); ++index) {
        const gui::UiPoint& a = corners[index];
        const gui::UiPoint& b = corners[(index + 1U) % corners.size()];
        if (!std::isfinite(a.x) || !std::isfinite(a.y)) {
            return box;
        }
        area_twice += a.x * b.y - b.x * a.y;
        box.centre.x += a.x * 0.25F;
        box.centre.y += a.y * 0.25F;
    }
    if (std::abs(area_twice) < 8.0F) {
        return box;
    }
    // The stalk leaves the top edge away from the box's middle, so it stands
    // outside the layer whichever way that is turned.
    const gui::UiPoint top = midpoint(corners[0U], corners[1U]);
    const float out_x = top.x - box.centre.x;
    const float out_y = top.y - box.centre.y;
    const float reach = std::hypot(out_x, out_y);
    if (reach < 0.5F) {
        return box;
    }
    box.rotate = gui::UiPoint{
        top.x + out_x / reach * transform_box_stalk_pixels,
        top.y + out_y / reach * transform_box_stalk_pixels};
    box.visible = true;
    return box;
}

bool is_corner(TransformBoxHandle handle) noexcept {
    return handle == TransformBoxHandle::TopLeft ||
           handle == TransformBoxHandle::TopRight ||
           handle == TransformBoxHandle::BottomRight ||
           handle == TransformBoxHandle::BottomLeft;
}

bool is_edge(TransformBoxHandle handle) noexcept {
    return handle == TransformBoxHandle::Top ||
           handle == TransformBoxHandle::Right ||
           handle == TransformBoxHandle::Bottom ||
           handle == TransformBoxHandle::Left;
}

BoxPoint handle_point(TransformBoxHandle handle) noexcept {
    switch (handle) {
    case TransformBoxHandle::TopLeft:
        return BoxPoint{0.0F, 0.0F};
    case TransformBoxHandle::Top:
        return BoxPoint{0.5F, 0.0F};
    case TransformBoxHandle::TopRight:
        return BoxPoint{1.0F, 0.0F};
    case TransformBoxHandle::Right:
        return BoxPoint{1.0F, 0.5F};
    case TransformBoxHandle::BottomRight:
        return BoxPoint{1.0F, 1.0F};
    case TransformBoxHandle::Bottom:
        return BoxPoint{0.5F, 1.0F};
    case TransformBoxHandle::BottomLeft:
        return BoxPoint{0.0F, 1.0F};
    case TransformBoxHandle::Left:
        return BoxPoint{0.0F, 0.5F};
    case TransformBoxHandle::None:
    case TransformBoxHandle::Rotate:
        break;
    }
    return BoxPoint{0.5F, 0.5F};
}

TransformBoxHandle opposite(TransformBoxHandle handle) noexcept {
    switch (handle) {
    case TransformBoxHandle::TopLeft:
        return TransformBoxHandle::BottomRight;
    case TransformBoxHandle::Top:
        return TransformBoxHandle::Bottom;
    case TransformBoxHandle::TopRight:
        return TransformBoxHandle::BottomLeft;
    case TransformBoxHandle::Right:
        return TransformBoxHandle::Left;
    case TransformBoxHandle::BottomRight:
        return TransformBoxHandle::TopLeft;
    case TransformBoxHandle::Bottom:
        return TransformBoxHandle::Top;
    case TransformBoxHandle::BottomLeft:
        return TransformBoxHandle::TopRight;
    case TransformBoxHandle::Left:
        return TransformBoxHandle::Right;
    case TransformBoxHandle::None:
    case TransformBoxHandle::Rotate:
        break;
    }
    return TransformBoxHandle::None;
}

gui::UiPoint handle_position(const TransformBox& box,
                             TransformBoxHandle handle) noexcept {
    switch (handle) {
    case TransformBoxHandle::TopLeft:
        return box.corners[0U];
    case TransformBoxHandle::Top:
        return midpoint(box.corners[0U], box.corners[1U]);
    case TransformBoxHandle::TopRight:
        return box.corners[1U];
    case TransformBoxHandle::Right:
        return midpoint(box.corners[1U], box.corners[2U]);
    case TransformBoxHandle::BottomRight:
        return box.corners[2U];
    case TransformBoxHandle::Bottom:
        return midpoint(box.corners[3U], box.corners[2U]);
    case TransformBoxHandle::BottomLeft:
        return box.corners[3U];
    case TransformBoxHandle::Left:
        return midpoint(box.corners[0U], box.corners[3U]);
    case TransformBoxHandle::Rotate:
        return box.rotate;
    case TransformBoxHandle::None:
        break;
    }
    return box.centre;
}

TransformBoxHandle hit_transform_box(const TransformBox& box, float pointer_x,
                                     float pointer_y) noexcept {
    if (!box.visible) {
        return TransformBoxHandle::None;
    }
    const gui::UiPoint pointer{pointer_x, pointer_y};
    if (distance(pointer, box.rotate) <= transform_box_hit_pixels) {
        return TransformBoxHandle::Rotate;
    }
    TransformBoxHandle best = TransformBoxHandle::None;
    float nearest = transform_box_hit_pixels;
    for (const bool corners : {true, false}) {
        for (const TransformBoxHandle handle : resize_handles) {
            if (is_corner(handle) != corners ||
                (!corners && !edge_shown(box, handle))) {
                continue;
            }
            const float away =
                distance(pointer, handle_position(box, handle));
            if (away <= nearest) {
                nearest = away;
                best = handle;
            }
        }
        // A corner within reach wins over any edge handle.
        if (best != TransformBoxHandle::None) {
            return best;
        }
    }
    return best;
}

gui::Cursor handle_cursor(const TransformBox& box,
                          TransformBoxHandle handle) noexcept {
    if (handle == TransformBoxHandle::Rotate) {
        return gui::Cursor::Hand;
    }
    if (!is_corner(handle) && !is_edge(handle)) {
        return gui::Cursor::Arrow;
    }
    const gui::UiPoint at = handle_position(box, handle);
    const float dx = std::abs(at.x - box.centre.x);
    const float dy = std::abs(at.y - box.centre.y);
    // Within a sixteenth of a turn of an axis reads as that axis.
    constexpr float tangent = 0.41421356F;
    if (dy <= dx * tangent) {
        return gui::Cursor::ResizeHorizontal;
    }
    if (dx <= dy * tangent) {
        return gui::Cursor::ResizeVertical;
    }
    return gui::Cursor::ResizeDiagonal;
}

BoxScale resolve_box_scale(TransformBoxHandle handle, BoxPoint anchor,
                           BoxPoint travel, bool uniform) noexcept {
    BoxScale scale{};
    if (!is_corner(handle) && !is_edge(handle)) {
        return scale;
    }
    const BoxPoint held = handle_point(handle);
    const float span_u = held.u - anchor.u;
    const float span_v = held.v - anchor.v;
    const bool across = handle != TransformBoxHandle::Top &&
                        handle != TransformBoxHandle::Bottom &&
                        std::abs(span_u) > 1.0e-6F;
    const bool down = handle != TransformBoxHandle::Left &&
                      handle != TransformBoxHandle::Right &&
                      std::abs(span_v) > 1.0e-6F;
    if (across) {
        scale.across = (span_u + travel.u) / span_u;
    }
    if (down) {
        scale.down = (span_v + travel.v) / span_v;
    }
    if (uniform && across && down) {
        const float both =
            std::abs(scale.across - 1.0F) >= std::abs(scale.down - 1.0F)
                ? scale.across
                : scale.down;
        scale.across = both;
        scale.down = both;
    }
    if (!std::isfinite(scale.across)) {
        scale.across = 1.0F;
    }
    if (!std::isfinite(scale.down)) {
        scale.down = 1.0F;
    }
    scale.across = std::max(scale.across, transform_box_minimum_scale);
    scale.down = std::max(scale.down, transform_box_minimum_scale);
    return scale;
}

void draw_transform_box(gui::UiDrawList& list, const gui::Theme& theme,
                        const TransformBox& box, TransformBoxHandle hovered,
                        TransformBoxHandle active) {
    if (!box.visible) {
        return;
    }
    const gui::UiColor edge = theme.accent;
    for (std::size_t index = 0U; index < box.corners.size(); ++index) {
        haloed_line(list, box.corners[index],
                    box.corners[(index + 1U) % box.corners.size()], 3.0F, 1.2F,
                    edge);
    }
    const auto lit = [&](TransformBoxHandle handle) {
        return handle == active ||
               (active == TransformBoxHandle::None && handle == hovered);
    };
    const gui::UiPoint top =
        handle_position(box, TransformBoxHandle::Top);
    haloed_line(list, top, box.rotate, 3.0F, 1.2F, edge);
    haloed_ring(list, box.rotate, lit(TransformBoxHandle::Rotate) ? 6.0F : 5.0F,
                16U, 4.0F, 2.0F,
                lit(TransformBoxHandle::Rotate) ? theme.text : edge);
    for (const TransformBoxHandle handle : resize_handles) {
        if (is_edge(handle) && !edge_shown(box, handle)) {
            continue;
        }
        const bool bright = lit(handle);
        haloed_square(list, handle_position(box, handle),
                      transform_box_handle_size + (bright ? 2.0F : 0.0F), 1.5F,
                      bright ? edge : theme.text);
    }
}

}  // namespace mediaserver::editor
