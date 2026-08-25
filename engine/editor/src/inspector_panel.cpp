#include <mediaserver/editor/inspector_panel.hpp>

#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace mediaserver::editor {
namespace {

namespace gui = mediaserver::gui;
namespace scene = mediaserver::scene;

// Row metrics. Tight enough that a whole transform fits a side dock without
// scrolling, loose enough that a 15px number is not touching its own well.
constexpr float row_height = 22.0F;
constexpr float row_gap = 4.0F;
constexpr float section_height = 24.0F;
constexpr float field_gap = 4.0F;
constexpr float panel_inset = 10.0F;
// The label column: a share of the panel rather than a constant, so a narrow
// dock does not leave the fields two characters wide - with a floor and a
// ceiling, because neither extreme is readable.
constexpr float label_fraction = 0.32F;
constexpr float label_min = 50.0F;
constexpr float label_max = 96.0F;
// How far the pointer must travel before a press counts as a scrub rather than
// a click that opens the field for typing.
constexpr float scrub_threshold = 2.0F;

// A widget id derived from the panel's own, so two Inspectors in one window - a
// detached copy beside the docked one - never fight over hot/active.
[[nodiscard]] gui::UiId mix(
    gui::UiId base, std::string_view name, std::size_t index) noexcept {
    core::u64 hash = base ^ 0x9E3779B97F4A7C15ULL;
    for (const char c : name) {
        hash ^= static_cast<core::u8>(c);
        hash *= 1099511628211ULL;
    }
    hash ^= static_cast<core::u64>(index) + 0x9E3779B9ULL;
    hash *= 1099511628211ULL;
    return hash;
}

// A number as the panel shows it. Three decimals: rotation is in turns, where
// two would quantise a nudge away to nothing.
struct Formatted final {
    std::array<char, 32U> buffer{};
    int length{};

    [[nodiscard]] std::string_view view() const noexcept {
        return std::string_view{buffer.data(), static_cast<std::size_t>(length)};
    }
};

[[nodiscard]] Formatted format_number(double value) noexcept {
    Formatted out{};
    out.length =
        std::snprintf(out.buffer.data(), out.buffer.size(), "%.3f", value);
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
    if (end == begin) {
        return std::nullopt;
    }
    return value;
}

// Splits `bounds` into `count` equal columns with a gap between them.
void columns(
    const gui::Rect& bounds,
    std::size_t count,
    float gap,
    std::span<gui::Rect> out) noexcept {
    if (count == 0U || out.size() < count) {
        return;
    }
    const auto n = static_cast<float>(count);
    const float width = std::max((bounds.width - gap * (n - 1.0F)) / n, 1.0F);
    for (std::size_t i = 0U; i < count; ++i) {
        out[i] = gui::Rect{
            bounds.x + static_cast<float>(i) * (width + gap),
            bounds.y,
            width,
            bounds.height};
    }
}

// A twirl arrow: down when open, right when closed. The one place the panel
// needs geometry a rectangle cannot express.
void draw_twirl(
    gui::UiContext& ctx, float cx, float cy, bool open, gui::UiColor color) {
    auto& list = ctx.draw_list();
    if (open) {
        list.add_triangle(
            cx - 4.0F, cy - 2.5F, cx + 4.0F, cy - 2.5F, cx, cy + 3.0F, color);
    } else {
        list.add_triangle(
            cx - 2.5F, cy - 4.0F, cx + 3.0F, cy, cx - 2.5F, cy + 4.0F, color);
    }
}

// The axis letters each transform group's three fields carry. Rotation is in
// turns about each axis, which an operator reads as yaw/pitch/roll rather than
// as x/y/z.
constexpr std::array<std::string_view, 3U> xyz{{"X", "Y", "Z"}};
constexpr std::array<std::string_view, 3U> ypr{{"Y", "P", "R"}};
constexpr std::array<std::string_view, 3U> rgb{{"R", "G", "B"}};

} // namespace

gui::Rect InspectorPanel::take_row(Cursor& cursor, float height) noexcept {
    const gui::Rect row{
        cursor.column.x, cursor.y, cursor.column.width, height};
    cursor.y += height;
    return row;
}

bool InspectorPanel::number(
    gui::UiContext& ctx,
    gui::UiId id,
    const gui::Rect& bounds,
    std::string_view axis,
    float value,
    float speed,
    double& out) {
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
            const auto parsed = parse_number(typed_);
            if (parsed.has_value()
                && parsed.value() != static_cast<double>(value)) {
                out = parsed.value();
                return true;
            }
        }
        return false;
    }

    const gui::Interaction it = ctx.grab(id, bounds);
    bool changed = false;

    if (it.pressed) {
        scrub_ = id;
        scrub_base_ = static_cast<double>(value);
        scrub_travel_ = 0.0F;
        scrub_moved_ = false;
    }
    if (it.held && scrub_ == id) {
        scrub_travel_ += ctx.pointer_dx();
        if (!scrub_moved_ && std::abs(scrub_travel_) >= scrub_threshold) {
            scrub_moved_ = true;
        }
        if (scrub_moved_ && ctx.pointer_dx() != 0.0F) {
            // From the value the drag began at plus its total travel, never from
            // last frame's: the document is several frames behind a live drag,
            // and re-basing on the lagging value each frame would crawl.
            out = scrub_base_ + static_cast<double>(scrub_travel_ * speed);
            changed = true;
        }
    }
    if (it.released && scrub_ == id) {
        scrub_ = gui::UiId{};
        if (!scrub_moved_ && it.clicked) {
            // A press that never travelled is a click: open the field for
            // typing. The press was spent on the scrub, so the field has to be
            // handed the keyboard rather than waiting to be clicked again.
            typing_ = id;
            // Empty, not pre-filled: clicking a number in a compositor means
            // "replace this", and the widget layer has no selection model to
            // express select-all with. Escape restores the empty string, which
            // parses to nothing and so commits nothing - a clean cancel.
            typed_.clear();
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
    const Formatted text = format_number(static_cast<double>(value));
    ctx.label(
        gui::Rect{
            bounds.x + theme.padding + axis_width,
            bounds.y,
            std::max(bounds.width - theme.padding * 2.0F - axis_width, 0.0F),
            bounds.height},
        text.view(),
        theme.text);
    ctx.pop_clip();

    return changed;
}

bool InspectorPanel::vector_row(
    gui::UiContext& ctx,
    Cursor& cursor,
    std::string_view label,
    std::span<const std::string_view> axes,
    std::span<const float> values,
    float speed,
    std::size_t& changed_axis,
    double& changed_value) {
    const gui::Rect row = take_row(cursor, row_height);
    cursor.y += row_gap;

    const float label_width = std::clamp(
        row.width * label_fraction, label_min, label_max);
    ctx.label(
        gui::Rect{row.x, row.y, label_width, row.height},
        label,
        ctx.theme().text_muted);

    const gui::Rect fields{
        row.x + label_width,
        row.y,
        std::max(row.width - label_width, 1.0F),
        row.height};

    const std::size_t count = std::min(axes.size(), values.size());
    std::array<gui::Rect, 4U> boxes{};
    columns(fields, std::min(count, boxes.size()), field_gap, boxes);

    bool changed = false;
    for (std::size_t i = 0U; i < count && i < boxes.size(); ++i) {
        double out = 0.0;
        if (number(
                ctx,
                mix(id_, label, i),
                boxes[i],
                axes[i],
                values[i],
                speed,
                out)) {
            changed_axis = i;
            changed_value = out;
            changed = true;
        }
    }
    return changed;
}

bool InspectorPanel::section(
    gui::UiContext& ctx, Cursor& cursor, std::string_view title, bool& open) {
    const gui::Rect row = take_row(cursor, section_height);
    cursor.y += row_gap;

    const gui::Interaction it = ctx.grab(mix(id_, title, 1000U), row);
    if (it.clicked) {
        open = !open;
    }
    ctx.panel(row, it.hovered ? ctx.theme().widget : ctx.theme().header);
    draw_twirl(
        ctx,
        row.x + 8.0F,
        row.y + row.height * 0.5F,
        open,
        ctx.theme().text_muted);
    ctx.label(
        gui::Rect{row.x + 20.0F, row.y, row.width - 20.0F, row.height},
        title,
        ctx.theme().text);
    return open;
}

void InspectorPanel::draw_header(
    gui::UiContext& ctx,
    Cursor& cursor,
    const ElementInfo& element,
    std::size_t also_selected) {
    const gui::Rect row = take_row(cursor, 30.0F);
    cursor.y += row_gap;

    // The kind swatch, matching the colour the timeline marks the same layer
    // with, so an operator can tie the two panels together at a glance.
    ctx.panel(
        gui::Rect{row.x, row.y + 7.0F, 4.0F, row.height - 14.0F},
        kind_color(element.kind));

    std::array<char, 64U> title{};
    const int length = std::snprintf(
        title.data(),
        title.size(),
        "%.*s %llu",
        static_cast<int>(kind_name(element.kind).size()),
        kind_name(element.kind).data(),
        static_cast<unsigned long long>(element.node.value()));
    const std::string_view title_view{
        title.data(), static_cast<std::size_t>(std::max(length, 0))};

    const gui::Rect name{
        row.x + 12.0F, row.y, row.width - 12.0F, row.height};
    ctx.label(name, title_view, ctx.theme().text);

    if (also_selected > 0U) {
        std::array<char, 24U> extra{};
        const int extra_length = std::snprintf(
            extra.data(),
            extra.size(),
            "+%zu more",
            also_selected);
        const float offset = ctx.font_width(title_view) + 20.0F;
        ctx.label(
            gui::Rect{
                name.x + offset, name.y, std::max(name.width - offset, 0.0F),
                name.height},
            std::string_view{
                extra.data(), static_cast<std::size_t>(std::max(extra_length, 0))},
            ctx.theme().text_muted);
    }
}

void InspectorPanel::draw_transform(
    gui::UiContext& ctx,
    Cursor& cursor,
    const ElementInfo& element,
    InspectorFrame& frame) {
    if (!section(ctx, cursor, "TRANSFORM", transform_open_)) {
        return;
    }

    // Each group's three fields map onto consecutive TransformChannel values,
    // which is the order the scene model applies them in.
    struct Group final {
        std::string_view label;
        std::span<const std::string_view> axes;
        std::array<float, 3U> values;
        float speed;
        scene::TransformChannel first;
    };
    const std::array<Group, 3U> groups{{
        {"Position",
         xyz,
         {element.position.x, element.position.y, element.position.z},
         0.01F,
         scene::TransformChannel::PositionX},
        {"Rotation",
         ypr,
         {element.rotation.x, element.rotation.y, element.rotation.z},
         0.002F,
         scene::TransformChannel::RotationYaw},
        {"Scale",
         xyz,
         {element.scale.x, element.scale.y, element.scale.z},
         0.01F,
         scene::TransformChannel::ScaleX},
    }};

    for (const Group& group : groups) {
        std::size_t axis = 0U;
        double value = 0.0;
        if (vector_row(
                ctx,
                cursor,
                group.label,
                group.axes,
                group.values,
                group.speed,
                axis,
                value)) {
            const auto component = static_cast<scene::TransformChannel>(
                static_cast<core::u8>(
                    static_cast<std::size_t>(group.first) + axis));
            frame.component_edits.push_back(
                ComponentEdit{element.node, component, value});
        }
    }

    const gui::Rect note = take_row(cursor, 16.0F);
    cursor.y += row_gap;
    ctx.label(note, "rotation in turns", ctx.theme().text_muted);
}

void InspectorPanel::draw_light(
    gui::UiContext& ctx,
    Cursor& cursor,
    const ElementInfo& element,
    InspectorFrame& frame) {
    if (!section(ctx, cursor, "LIGHT", light_open_)) {
        return;
    }

    // A light's fields are plain document properties rather than transform
    // channels, so they are named rather than indexed.
    const auto emit = [&frame, &element](std::string_view property, double value) {
        frame.named_edits.push_back(
            NamedEdit{element.node, std::string{property}, value});
    };

    // The colour, shown as the bar it actually produces above its own numbers -
    // three floats do not read as a colour, and a light's tint is the thing an
    // operator judges by eye.
    const gui::Rect swatch = take_row(cursor, 14.0F);
    cursor.y += row_gap;
    const float label_width =
        std::clamp(swatch.width * label_fraction, label_min, label_max);
    ctx.label(
        gui::Rect{swatch.x, swatch.y, label_width, swatch.height},
        "Color",
        ctx.theme().text_muted);
    ctx.panel(
        gui::Rect{
            swatch.x + label_width,
            swatch.y + 2.0F,
            std::max(swatch.width - label_width, 1.0F),
            swatch.height - 4.0F},
        gui::UiColor{element.color.x, element.color.y, element.color.z, 1.0F});

    {
        const std::array<float, 3U> values{
            element.color.x, element.color.y, element.color.z};
        constexpr std::array<std::string_view, 3U> properties{
            {scene::schema::color_r, scene::schema::color_g,
             scene::schema::color_b}};
        std::size_t axis = 0U;
        double value = 0.0;
        if (vector_row(ctx, cursor, "RGB", rgb, values, 0.005F, axis, value)) {
            emit(properties[axis], value);
        }
    }

    {
        const std::array<float, 1U> values{element.intensity};
        constexpr std::array<std::string_view, 1U> axes{{""}};
        std::size_t axis = 0U;
        double value = 0.0;
        if (vector_row(ctx, cursor, "Intensity", axes, values, 0.05F, axis, value)) {
            emit(scene::schema::intensity, std::max(value, 0.0));
        }
    }

    if (element.kind == ElementKind::DirectionalLight) {
        const std::array<float, 3U> values{
            element.direction.x, element.direction.y, element.direction.z};
        constexpr std::array<std::string_view, 3U> properties{
            {scene::schema::direction_x, scene::schema::direction_y,
             scene::schema::direction_z}};
        std::size_t axis = 0U;
        double value = 0.0;
        if (vector_row(ctx, cursor, "Direction", xyz, values, 0.01F, axis, value)) {
            emit(properties[axis], value);
        }
        return;
    }

    {
        const std::array<float, 3U> values{
            element.position.x, element.position.y, element.position.z};
        constexpr std::array<std::string_view, 3U> properties{
            {scene::schema::position_x, scene::schema::position_y,
             scene::schema::position_z}};
        std::size_t axis = 0U;
        double value = 0.0;
        if (vector_row(ctx, cursor, "Position", xyz, values, 0.01F, axis, value)) {
            emit(properties[axis], value);
        }
    }
    {
        const std::array<float, 1U> values{element.range};
        constexpr std::array<std::string_view, 1U> axes{{""}};
        std::size_t axis = 0U;
        double value = 0.0;
        if (vector_row(ctx, cursor, "Range", axes, values, 0.1F, axis, value)) {
            emit(scene::schema::range, std::max(value, 0.0));
        }
    }
}

InspectorFrame InspectorPanel::draw(
    gui::UiContext& ctx,
    const gui::Rect& bounds,
    const ElementInfo* element,
    std::size_t also_selected) {
    InspectorFrame frame{};

    ctx.panel(bounds, ctx.theme().panel);
    if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
        return frame;
    }

    // A selection that vanished under a live edit ends it, rather than leaving
    // the keyboard in a field belonging to a node that is no longer there.
    if (element == nullptr) {
        if (typing_ != gui::UiId{}) {
            ctx.clear_focus();
            typing_ = gui::UiId{};
        }
        scrub_ = gui::UiId{};
        ctx.label(
            gui::Rect{
                bounds.x + panel_inset,
                bounds.y + panel_inset,
                std::max(bounds.width - panel_inset * 2.0F, 1.0F),
                row_height},
            "select an element",
            ctx.theme().text_muted);
        return frame;
    }

    // The wheel scrolls the panel when the pointer is over it, clamped against
    // last frame's measured height - this frame's is not known until the end.
    if (bounds.contains(ctx.pointer_x(), ctx.pointer_y()) && ctx.wheel() != 0.0F) {
        scroll_ -= ctx.wheel() * 32.0F;
    }
    scroll_ = std::clamp(
        scroll_, 0.0F, std::max(content_height_ - bounds.height, 0.0F));

    ctx.push_clip(bounds);
    Cursor cursor{
        gui::Rect{
            bounds.x + panel_inset,
            bounds.y,
            std::max(bounds.width - panel_inset * 2.0F, 1.0F),
            0.0F},
        bounds.y + panel_inset - scroll_};

    draw_header(ctx, cursor, *element, also_selected);
    if (is_light(element->kind)) {
        draw_light(ctx, cursor, *element, frame);
    } else {
        draw_transform(ctx, cursor, *element, frame);
    }

    // Delete, mirroring the Edit menu and the Del key.
    cursor.y += row_gap * 2.0F;
    const gui::Rect actions = take_row(cursor, row_height);
    if (ctx.button(
            mix(id_, "delete", 0U),
            gui::Rect{actions.x, actions.y, std::min(actions.width, 96.0F),
                      actions.height},
            "Delete")) {
        frame.delete_requested = true;
    }
    cursor.y += panel_inset;

    content_height_ = cursor.y + scroll_ - bounds.y;
    ctx.pop_clip();

    frame.editing = scrub_ != gui::UiId{} || typing_ != gui::UiId{};
    return frame;
}

} // namespace mediaserver::editor
