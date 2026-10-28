#pragma once

#include <mediaserver/gui/layout.hpp>

#include <algorithm>
#include <cstddef>

// Rectangle cutting: the layout most panel chrome actually needs, one step
// lighter than a LayoutTree. A toolbar is a strip cut off the top of a panel,
// a button is a slice cut off the right of the toolbar, and what is left is
// the content - each cut returns the piece and shrinks the remainder, so no
// caller re-derives `x + width - n` by hand.
//
// A cut never takes more than is there. Asking for 30 pixels of a 20-pixel
// rectangle takes all 20 and leaves an empty remainder at the far edge, so a
// window squeezed too small degrades into empty boxes rather than negative
// sizes that run backwards across their neighbours.
namespace mediaserver::gui {

[[nodiscard]] inline Rect cut_top(Rect& from, float amount) noexcept {
    const float taken = std::clamp(amount, 0.0F, from.height);
    const Rect piece{from.x, from.y, from.width, taken};
    from.y += taken;
    from.height -= taken;
    return piece;
}

[[nodiscard]] inline Rect cut_bottom(Rect& from, float amount) noexcept {
    const float taken = std::clamp(amount, 0.0F, from.height);
    from.height -= taken;
    return Rect{from.x, from.y + from.height, from.width, taken};
}

[[nodiscard]] inline Rect cut_left(Rect& from, float amount) noexcept {
    const float taken = std::clamp(amount, 0.0F, from.width);
    const Rect piece{from.x, from.y, taken, from.height};
    from.x += taken;
    from.width -= taken;
    return piece;
}

[[nodiscard]] inline Rect cut_right(Rect& from, float amount) noexcept {
    const float taken = std::clamp(amount, 0.0F, from.width);
    from.width -= taken;
    return Rect{from.x + from.width, from.y, taken, from.height};
}

// Shrunk by `padding` on each side. Padding wider than the rectangle collapses
// it to zero size, never to a negative one.
[[nodiscard]] inline Rect inset(const Rect& r, const Padding& padding) noexcept {
    const float width = std::max(r.width - padding.left - padding.right, 0.0F);
    const float height = std::max(r.height - padding.top - padding.bottom, 0.0F);
    return Rect{r.x + std::min(padding.left, r.width), r.y + std::min(padding.top, r.height),
                width, height};
}

[[nodiscard]] inline Rect inset(const Rect& r, float amount) noexcept {
    return inset(r, Padding::all(amount));
}

[[nodiscard]] inline Rect inset(const Rect& r, float horizontal, float vertical) noexcept {
    return inset(r, Padding{horizontal, vertical, horizontal, vertical});
}

// A `width` by `height` box centred in `r`. Larger than `r` overhangs it
// evenly, which is what a glyph or a dialog card wants.
[[nodiscard]] inline Rect centered(const Rect& r, float width, float height) noexcept {
    return Rect{r.x + (r.width - width) * 0.5F, r.y + (r.height - height) * 0.5F,
                width, height};
}

// The `index`th of `count` equal side-by-side cells across `r`, with `gap`
// between neighbours: a row of segmented buttons. The last cell ends exactly on
// `r`'s right edge, so rounding never leaves a sliver after it.
[[nodiscard]] inline Rect column(
    const Rect& r, std::size_t index, std::size_t count, float gap) noexcept {
    if (count == 0U) {
        return Rect{r.x, r.y, 0.0F, r.height};
    }
    const float n = static_cast<float>(count);
    const float cell = std::max((r.width - gap * (n - 1.0F)) / n, 0.0F);
    const float x = r.x + static_cast<float>(index) * (cell + gap);
    const float width =
        index + 1U == count ? std::max(r.right() - x, 0.0F) : cell;
    return Rect{x, r.y, width, r.height};
}

// Where a column crosses a row: `column`'s x and width, `row`'s y and height.
// A table lays its columns out once, across its heading, and every row takes
// its cells from them.
[[nodiscard]] constexpr Rect cell(const Rect& column, const Rect& row) noexcept {
    return Rect{column.x, row.y, column.width, row.height};
}

// A `width` by `height` box centred on a point: a handle, a key, a port's hit
// area - anything placed by where it is rather than by the box it sits in.
[[nodiscard]] constexpr Rect around(
    float centre_x, float centre_y, float width, float height) noexcept {
    return Rect{centre_x - width * 0.5F, centre_y - height * 0.5F, width, height};
}

// `r`'s full height from x `from` to x `to`: the part of a row a span of time
// covers, or of a lane a clip does. A span that ends before it starts is
// empty. `span_y` is the same down a column.
[[nodiscard]] constexpr Rect span_x(const Rect& r, float from, float to) noexcept {
    return Rect{from, r.y, std::max(to - from, 0.0F), r.height};
}

[[nodiscard]] constexpr Rect span_y(const Rect& r, float from, float to) noexcept {
    return Rect{r.x, from, r.width, std::max(to - from, 0.0F)};
}

// A `thickness`-wide rule down `r`'s full height, centred on x: a playhead, a
// cut, a seam. `rule_y` runs across `r`, centred on y.
[[nodiscard]] constexpr Rect rule_x(
    const Rect& r, float x, float thickness = 1.0F) noexcept {
    return Rect{x - thickness * 0.5F, r.y, thickness, r.height};
}

[[nodiscard]] constexpr Rect rule_y(
    const Rect& r, float y, float thickness = 1.0F) noexcept {
    return Rect{r.x, y - thickness * 0.5F, r.width, thickness};
}

// The box two corners span, given in either order: a rubber band, or the
// sweep of a drag.
[[nodiscard]] constexpr Rect spanning(float x0, float y0, float x1, float y1) noexcept {
    return Rect{std::min(x0, x1), std::min(y0, y1),
                std::max(x0, x1) - std::min(x0, x1),
                std::max(y0, y1) - std::min(y0, y1)};
}

// Where two boxes overlap; empty, at the later edges, when they do not.
[[nodiscard]] constexpr Rect intersection(const Rect& a, const Rect& b) noexcept {
    const float left = std::max(a.x, b.x);
    const float top = std::max(a.y, b.y);
    return Rect{left, top, std::max(std::min(a.right(), b.right()) - left, 0.0F),
                std::max(std::min(a.bottom(), b.bottom()) - top, 0.0F)};
}

// The smallest box holding both: a selection's extent, or a view and what it
// views.
[[nodiscard]] constexpr Rect enclosing(const Rect& a, const Rect& b) noexcept {
    const float left = std::min(a.x, b.x);
    const float top = std::min(a.y, b.y);
    return Rect{left, top, std::max(a.right(), b.right()) - left,
                std::max(a.bottom(), b.bottom()) - top};
}

// Row `index` of a list of `height`-tall rows `gap` apart down `list`,
// scrolled up by `scroll`. Unlike a cut it is not clamped: a row past the
// list's bottom is still placed there, for the clip to hide or the caller to
// skip.
[[nodiscard]] inline Rect list_row(const Rect& list, std::size_t index, float height,
                                   float scroll = 0.0F, float gap = 0.0F) noexcept {
    return Rect{list.x, list.y - scroll + static_cast<float>(index) * (height + gap),
                list.width, height};
}

} // namespace mediaserver::gui
