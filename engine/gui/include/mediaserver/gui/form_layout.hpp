#pragma once

#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/metrics.hpp>

#include <string_view>

// The two pieces a settings form is built from, so a dialog stops carrying a
// `y += 22.0F` per row and a guessed content height for its scroll bar.
//
// FormLayout is a cursor down a column: each call hands out the next row's
// rectangles and advances past it by the Metrics rhythm. It is pure geometry -
// no context, no drawing - so the rows a form produces can be asserted
// directly, and what it measured (extent()) is the true content height rather
// than a constant someone has to remember to grow.
//
// ScrollRegion is the persistent half: the offset a form or list is scrolled
// to, moved by the wheel, clipped to a viewport, and clamped against the
// height the content actually measured.
namespace mediaserver::gui {

class UiContext;

// A labelled row: the label column on the left, the field filling the rest.
struct FormRow final {
    Rect label;
    Rect field;
};

// The rule that opens a section and the heading under it.
struct FormSection final {
    Rect rule;
    Rect heading;
};

class FormLayout final {
public:
    // Lays out down `content`, inset by `metrics.inset` at the sides and
    // `metrics.gap` at the top, scrolled up by `scroll`.
    FormLayout(const Rect& content, const Metrics& metrics, float scroll = 0.0F) noexcept;

    // A full-width strip `height` tall, followed by `gap_after`. Every other
    // call is this with the Metrics rhythm filled in.
    [[nodiscard]] Rect next(float height, float gap_after) noexcept;
    // A full-width control row: a checkbox, a row of segmented buttons.
    [[nodiscard]] Rect control() noexcept;
    // A label and a field on one control row.
    [[nodiscard]] FormRow row() noexcept;
    // A line of secondary text: a hint, a note, a status message.
    [[nodiscard]] Rect caption() noexcept;
    // A rule `section_gap` below the last row, and the heading
    // `section_gap` below the rule.
    [[nodiscard]] FormSection section() noexcept;
    void space(float height) noexcept { cursor_ += height; }

    // Where the next row starts, on screen.
    [[nodiscard]] float cursor() const noexcept { return cursor_; }
    // The column rows are laid across, without the vertical extent.
    [[nodiscard]] float left() const noexcept { return left_; }
    [[nodiscard]] float width() const noexcept { return width_; }
    // How tall everything handed out so far is, from the top of `content` to
    // the bottom of the last row plus the top margin mirrored below it - the
    // height a ScrollRegion needs to know.
    [[nodiscard]] float extent() const noexcept;

    [[nodiscard]] const Metrics& metrics() const noexcept { return metrics_; }

private:
    Metrics metrics_;
    float origin_{};
    float left_{};
    float width_{};
    float cursor_{};
    // The bottom of the last row, so a trailing gap is not counted as content.
    float last_bottom_{};
};

// Draws a section's rule and heading in the theme's divider and text colours.
FormSection draw_form_section(UiContext& ctx, FormLayout& form, std::string_view title);

class ScrollRegion final {
public:
    // Scrolls by this frame's wheel when the pointer is over `viewport`,
    // clamps against the content height measured last frame, and clips to
    // `viewport`. Returns the offset to lay the content out at. Must be
    // matched by end().
    float begin(UiContext& ctx, const Rect& viewport);
    // Pops the clip and records `content_height` - FormLayout::extent() - so
    // the offset never runs past the end, this frame's content having
    // shrunk included.
    void end(UiContext& ctx, float content_height);

    [[nodiscard]] float offset() const noexcept { return offset_; }
    // How far the content can scroll: zero when it fits.
    [[nodiscard]] float max_offset() const noexcept;
    // Back to the top, forgetting what was measured: a dialog reopening.
    void reset() noexcept { *this = ScrollRegion{}; }

private:
    float offset_{};
    float viewport_height_{};
    float content_height_{};
    bool measured_{};
};

} // namespace mediaserver::gui
