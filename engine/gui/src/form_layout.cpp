#include <mediaserver/gui/form_layout.hpp>

#include <mediaserver/gui/ui_context.hpp>

#include <algorithm>

namespace mediaserver::gui {

FormLayout::FormLayout(const Rect& content, const Metrics& metrics, float scroll) noexcept
    : metrics_{metrics},
      origin_{content.y - scroll},
      left_{content.x + metrics.inset},
      width_{std::max(content.width - metrics.inset * 2.0F, 0.0F)},
      cursor_{content.y - scroll + metrics.gap},
      last_bottom_{content.y - scroll + metrics.gap} {}

Rect FormLayout::next(float height, float gap_after) noexcept {
    last_ = Rect{left_, cursor_, width_, height};
    last_bottom_ = cursor_ + height;
    cursor_ = last_bottom_ + gap_after;
    return last_;
}

Rect FormLayout::control() noexcept {
    return next(metrics_.control, metrics_.row_gap);
}

FormRow FormLayout::row() noexcept {
    const Rect strip = control();
    const float label = std::min(metrics_.label_column, strip.width);
    return FormRow{
        Rect{strip.x, strip.y, label, strip.height},
        Rect{strip.x + label, strip.y, strip.width - label, strip.height}};
}

Rect FormLayout::caption() noexcept {
    return next(metrics_.caption, metrics_.caption_gap);
}

Rect FormLayout::note() noexcept {
    const Rect line = caption();
    const float label = std::min(metrics_.label_column, line.width);
    return Rect{line.x + label, line.y, line.width - label, line.height};
}

Rect FormLayout::heading() noexcept {
    return next(metrics_.caption, metrics_.row_gap);
}

FormSection FormLayout::section() noexcept {
    // The rule sits `section_gap` below the last row's bottom, whatever gap
    // that row left behind it, so sections read the same after a field as
    // after a caption.
    cursor_ = last_bottom_ + metrics_.section_gap;
    const Rect rule = next(metrics_.divider, metrics_.section_gap);
    return FormSection{rule, heading()};
}

float FormLayout::extent() const noexcept {
    return last_bottom_ - origin_ + metrics_.gap;
}

FormSection draw_form_section(UiContext& ctx, FormLayout& form, std::string_view title) {
    return draw_form_section(ctx, form, title, ctx.theme().text);
}

FormSection draw_form_section(UiContext& ctx, FormLayout& form, std::string_view title,
                              UiColor heading) {
    const FormSection section = form.section();
    ctx.panel(section.rule, ctx.theme().divider);
    ctx.label(section.heading, title, heading);
    return section;
}

float ScrollRegion::max_offset() const noexcept {
    return std::max(content_height_ - viewport_height_, 0.0F);
}

Rect ScrollRegion::thumb(const Rect& track, float min_height) const noexcept {
    const float range = max_offset();
    if (range <= 0.0F || content_height_ <= 0.0F) {
        return Rect{track.x, track.y, track.width, 0.0F};
    }
    const float share = track.height * std::min(viewport_height_ / content_height_, 1.0F);
    const float height = std::min(std::max(share, min_height), track.height);
    return Rect{track.x, track.y + offset_ / range * (track.height - height), track.width,
                height};
}

float ScrollRegion::begin(UiContext& ctx, const Rect& viewport) {
    return begin(ctx, viewport, ctx.theme().metrics.scroll_step);
}

float ScrollRegion::begin(UiContext& ctx, const Rect& viewport, float step) {
    viewport_height_ = viewport.height;
    if (viewport.contains(ctx.pointer_x(), ctx.pointer_y())) {
        offset_ -= ctx.wheel() * step;
    }
    // Before anything is measured there is nothing to clamp against but the
    // top; end() catches the bottom once the content has said how tall it is.
    offset_ = measured_ ? std::clamp(offset_, 0.0F, max_offset())
                        : std::max(offset_, 0.0F);
    ctx.push_clip(viewport);
    return offset_;
}

void ScrollRegion::end(UiContext& ctx, float content_height) {
    ctx.pop_clip();
    content_height_ = std::max(content_height, 0.0F);
    measured_ = true;
    offset_ = std::clamp(offset_, 0.0F, max_offset());
}

} // namespace mediaserver::gui
