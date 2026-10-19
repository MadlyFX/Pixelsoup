#include <mediaserver/editor/number_field.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace mediaserver::editor {
namespace {

namespace gui = mediaserver::gui;

// How far the pointer must travel before a press counts as a scrub rather than
// a click that opens the field for typing.
constexpr float scrub_threshold = 2.0F;

struct Formatted final {
    std::array<char, 32U> buffer{};
    int length{};

    [[nodiscard]] std::string_view view() const noexcept {
        return std::string_view{buffer.data(), static_cast<std::size_t>(length)};
    }
};

[[nodiscard]] Formatted format_number(double value, int decimals) noexcept {
    Formatted out{};
    out.length = std::snprintf(
        out.buffer.data(),
        out.buffer.size(),
        "%.*f",
        std::clamp(decimals, 0, 9),
        value);
    if (out.length < 0) {
        out.length = 0;
    }
    return out;
}

// What the operator typed, or nullopt when it is not a number at all - in which
// case the field keeps what it had rather than jumping to zero.
[[nodiscard]] std::optional<double> parse_number(const std::string& text) {
    const char* const begin = text.c_str();
    char* end = nullptr;
    const double value = std::strtod(begin, &end);
    if (end == begin || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

} // namespace

void NumberField::cancel(gui::UiContext& ctx) {
    if (typing_ != gui::UiId{}) {
        if (ctx.focus() == typing_) {
            ctx.clear_focus();
        }
        typing_ = gui::UiId{};
    }
    scrub_ = gui::UiId{};
    nudge_ = gui::UiId{};
    nudge_delta_ = 0.0;
}

bool NumberField::draw(
    gui::UiContext& ctx,
    gui::UiId id,
    const gui::Rect& bounds,
    std::string_view axis,
    double value,
    double speed,
    double& out,
    int decimals,
    NumberFieldEdit* edit,
    bool mixed) {
    const gui::Theme& theme = ctx.theme();
    const float axis_width =
        axis.empty() ? 0.0F : ctx.font_width(axis) + theme.padding;

    // Typing: the widget layer owns the field entirely while it has the
    // keyboard. The edit commits when focus leaves - Enter, Escape (which puts
    // back what it held), or a press anywhere else.
    if (typing_ == id) {
        static_cast<void>(ctx.text_field(id, bounds, typed_));
        if (ctx.focus() != id) {
            typing_ = gui::UiId{};
            nudge_ = gui::UiId{};
            nudge_delta_ = 0.0;
            const auto parsed = parse_number(typed_);
            // In a mixed field, typing the primary target's existing value is
            // still a real commit: it resolves every other compatible target
            // to that absolute value.
            if (parsed.has_value() && (mixed || parsed.value() != value)) {
                out = parsed.value();
                if (edit != nullptr) {
                    *edit = NumberFieldEdit{NumberFieldEditKind::Absolute, out,
                                            0.0};
                }
                return true;
            }
        } else if (typed_.empty() && edit != nullptr) {
            const bool increase = ctx.key_pressed(gui::Key::Up);
            const bool decrease = ctx.key_pressed(gui::Key::Down);
            // Opposing keys in the same input batch cancel rather than
            // inventing an order that the panel cannot represent in one edit.
            if (increase != decrease) {
                const auto direction = increase
                    ? NumberFieldNudgeDirection::Increase
                    : NumberFieldNudgeDirection::Decrease;
                gui::KeyMods modifiers = gui::KeyMods::None;
                if (ctx.shift_held()) {
                    modifiers = modifiers | gui::KeyMods::Shift;
                }
                if (ctx.ctrl_held()) {
                    modifiers = modifiers | gui::KeyMods::Ctrl;
                }
                if (ctx.alt_held()) {
                    modifiers = modifiers | gui::KeyMods::Alt;
                }
                if (nudge_ != id || nudge_direction_ != direction ||
                    nudge_modifiers_ != modifiers) {
                    const double local_value = nudge_ == id
                        ? nudge_value_
                        : value;
                    nudge_ = id;
                    nudge_base_ = local_value;
                    nudge_delta_ = 0.0;
                    nudge_direction_ = direction;
                    nudge_modifiers_ = modifiers;
                }
                double step = std::abs(speed);
                if (gui::has_mod(modifiers, gui::KeyMods::Shift)) {
                    step *= 10.0;
                }
                if (gui::has_mod(modifiers, gui::KeyMods::Ctrl)) {
                    step *= 0.1;
                }
                nudge_delta_ += increase ? step : -step;
                out = nudge_base_ + nudge_delta_;
                nudge_value_ = out;
                if (edit != nullptr) {
                    *edit = NumberFieldEdit{NumberFieldEditKind::Nudge, out,
                                            nudge_delta_, direction,
                                            modifiers};
                }
                return true;
            }
        }
        return false;
    }

    const gui::Interaction it = ctx.grab(id, bounds);
    bool changed = false;

    if (it.pressed) {
        scrub_ = id;
        nudge_ = gui::UiId{};
        nudge_delta_ = 0.0;
        scrub_base_ = value;
        scrub_travel_ = 0.0F;
        scrub_moved_ = false;
    }
    // A batch can contain movement both before and after capture. Only the
    // portion since the actual down event belongs to this scrub. Release
    // batches likewise retain their final captured movement.
    const float captured_x = it.released
        ? ctx.pointer_release_x() : ctx.pointer_x();
    const float captured_dx = it.pressed
        ? captured_x - ctx.pointer_press_x()
        : captured_x - (ctx.pointer_x() - ctx.pointer_dx());
    if ((it.held || it.released) && scrub_ == id) {
        scrub_travel_ += captured_dx;
        if (!scrub_moved_ && std::abs(scrub_travel_) >= scrub_threshold) {
            scrub_moved_ = true;
        }
        if (scrub_moved_ && captured_dx != 0.0F) {
            out = scrub_base_ + static_cast<double>(scrub_travel_) * speed;
            if (edit != nullptr) {
                *edit = NumberFieldEdit{
                    NumberFieldEditKind::Delta, out,
                    static_cast<double>(scrub_travel_) * speed};
            }
            changed = true;
        }
    }
    if (it.released && scrub_ == id) {
        scrub_ = gui::UiId{};
        if (!scrub_moved_ && it.clicked) {
            // A press that never travelled is a click: open the field for
            // typing. The press was spent on the scrub, so the field has to be
            // handed the keyboard rather than waiting to be clicked again.
            //
            // It opens empty, not pre-filled: clicking a number in a compositor
            // means "replace this", and the widget layer has no selection model
            // to express select-all with. Escape restores the empty string,
            // which parses to nothing and so commits nothing - a clean cancel.
            typing_ = id;
            typed_.clear();
            nudge_ = id;
            nudge_base_ = value;
            nudge_delta_ = 0.0;
            nudge_value_ = value;
            ctx.focus_text_field(id, typed_);
        }
    }

    const bool active = scrub_ == id && scrub_moved_;
    const gui::UiColor well = active
        ? theme.widget_active
        : (it.hovered ? theme.widget_hover : theme.widget);
    ctx.panel(bounds, well);
    // The well's own floor line, in the accent when the field is live: an
    // operator scrubbing three fields in a row needs to see which one moved.
    ctx.panel(
        gui::Rect{bounds.x, bounds.y + bounds.height - 1.0F, bounds.width, 1.0F},
        active ? theme.accent : theme.divider);

    ctx.push_clip(bounds);
    if (!axis.empty()) {
        ctx.label(
            gui::Rect{
                bounds.x + theme.padding, bounds.y, axis_width, bounds.height},
            axis,
            theme.text_muted);
    }
    const gui::Rect value_bounds{
        bounds.x + theme.padding + axis_width, bounds.y,
        std::max(bounds.width - theme.padding * 2.0F - axis_width, 0.0F),
        bounds.height};
    if (mixed && !active) {
        ctx.label(value_bounds, "Mixed", theme.text_muted);
    } else {
        const Formatted text = format_number(value, decimals);
        ctx.label(value_bounds, text.view(), theme.text);
    }
    ctx.pop_clip();

    return changed;
}

} // namespace mediaserver::editor
