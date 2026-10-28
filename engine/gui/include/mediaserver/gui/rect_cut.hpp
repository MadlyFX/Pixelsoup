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
