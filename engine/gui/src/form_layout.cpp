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
    const Rect strip{left_, cursor_, width_, height};
    last_bottom_ = cursor_ + height;
    cursor_ = last_bottom_ + gap_after;
    return strip;
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

FormSection FormLayout::section() noexcept {
    // The rule sits `section_gap` below the last row's bottom, whatever gap
    // that row left behind it, so sections read the same after a field as
    // after a caption.
    cursor_ = last_bottom_ + metrics_.section_gap;
    const Rect rule = next(metrics_.divider, metrics_.section_gap);
    const Rect heading = next(metrics_.caption, metrics_.row_gap);
    return FormSection{rule, heading};
}

float FormLayout::extent() const noexcept {
    return last_bottom_ - origin_ + metrics_.gap;
}

FormSection draw_form_section(UiContext& ctx, FormLayout& form, std::string_view title) {
    const FormSection section = form.section();
    ctx.panel(section.rule, ctx.theme().divider);
    ctx.label(section.heading, title, ctx.theme().text);
    return section;
}

float ScrollRegion::max_offset() const noexcept {
    return std::max(content_height_ - viewport_height_, 0.0F);
}

float ScrollRegion::begin(UiContext& ctx, const Rect& viewport) {
    viewport_height_ = viewport.height;
    if (viewport.contains(ctx.pointer_x(), ctx.pointer_y())) {
        offset_ -= ctx.wheel() * ctx.theme().metrics.scroll_step;
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
