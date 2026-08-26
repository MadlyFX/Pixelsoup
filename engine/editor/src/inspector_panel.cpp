#include <mediaserver/editor/inspector_panel.hpp>

#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <utility>

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
// A widget id derived from the panel's own, so two Inspectors in one window - a
// detached copy beside the docked one - never fight over hot/active.
[[nodiscard]] gui::UiId mix(gui::UiId base, std::string_view name,
                            std::size_t index) noexcept {
    core::u64 hash = base ^ 0x9E3779B97F4A7C15ULL;
    for (const char c : name) {
        hash ^= static_cast<core::u8>(c);
        hash *= 1099511628211ULL;
    }
    hash ^= static_cast<core::u64>(index) + 0x9E3779B9ULL;
    hash *= 1099511628211ULL;
    return hash;
}

// Splits `bounds` into `count` equal columns with a gap between them.
void columns(const gui::Rect& bounds, std::size_t count, float gap,
             std::span<gui::Rect> out) noexcept {
    if (count == 0U || out.size() < count) {
        return;
    }
    const auto n = static_cast<float>(count);
    const float width = std::max((bounds.width - gap * (n - 1.0F)) / n, 1.0F);
    for (std::size_t i = 0U; i < count; ++i) {
        out[i] = gui::Rect{bounds.x + static_cast<float>(i) * (width + gap),
                           bounds.y, width, bounds.height};
    }
}

// A twirl arrow: down when open, right when closed. The one place the panel
// needs geometry a rectangle cannot express.
void draw_twirl(gui::UiContext& ctx, float cx, float cy, bool open,
                gui::UiColor color) {
    auto& list = ctx.draw_list();
    if (open) {
        list.add_triangle(cx - 4.0F, cy - 2.5F, cx + 4.0F, cy - 2.5F, cx,
                          cy + 3.0F, color);
    } else {
        list.add_triangle(cx - 2.5F, cy - 4.0F, cx + 3.0F, cy, cx - 2.5F,
                          cy + 4.0F, color);
    }
}

void draw_diamond(gui::UiContext& ctx, float cx, float cy, float radius,
                  gui::UiColor color) {
    const std::array<gui::UiPoint, 4U> points{{
        {cx, cy - radius},
        {cx + radius, cy},
        {cx, cy + radius},
        {cx - radius, cy},
    }};
    ctx.draw_list().add_convex_poly(points, color);
}

[[nodiscard]] bool key_button(gui::UiContext& ctx, gui::UiId id,
                              const gui::Rect& bounds, bool animated,
                              bool keyed_here, gui::UiColor background) {
    const gui::Interaction interaction = ctx.grab(id, bounds);
    gui::UiColor color = keyed_here
        ? ctx.theme().playhead
        : (animated ? ctx.theme().keyframe : ctx.theme().text_muted);
    if (interaction.hovered) {
        color = ctx.theme().accent;
    }
    const float cx = bounds.x + bounds.width * 0.5F;
    const float cy = bounds.y + bounds.height * 0.5F;
    draw_diamond(ctx, cx, cy, interaction.held ? 5.0F : 4.5F, color);
    if (!keyed_here && !interaction.held) {
        draw_diamond(ctx, cx, cy, 2.4F, background);
    }
    return interaction.clicked;
}

[[nodiscard]] const scene::ChannelKeyframes* channel_for(
    std::span<const scene::ChannelKeyframes> keyframes,
    doc::NodeId node,
    scene::TransformChannel component) noexcept {
    for (const scene::ChannelKeyframes& channel : keyframes) {
        if (channel.target == node && channel.component == component) {
            return &channel;
        }
    }
    return nullptr;
}

[[nodiscard]] bool keyed_at(const scene::ChannelKeyframes* channel,
                            core::u64 frame) noexcept {
    constexpr core::u64 maximum =
        static_cast<core::u64>(std::numeric_limits<core::i64>::max());
    if (channel == nullptr || frame > maximum) {
        return false;
    }
    const auto wanted = static_cast<core::i64>(frame);
    return std::find(channel->frames.begin(), channel->frames.end(), wanted) !=
           channel->frames.end();
}

[[nodiscard]] std::optional<double>
sample_at(const scene::ChannelKeyframes* channel, core::u64 frame) noexcept {
    constexpr core::u64 maximum =
        static_cast<core::u64>(std::numeric_limits<core::i64>::max());
    if (channel == nullptr || frame > maximum || channel->frames.empty() ||
        channel->values.size() != channel->frames.size()) {
        return std::nullopt;
    }

    const core::i64 wanted = static_cast<core::i64>(frame);
    if (wanted <= channel->frames.front()) {
        return channel->values.front();
    }
    if (wanted >= channel->frames.back()) {
        return channel->values.back();
    }
    const auto after = std::upper_bound(channel->frames.begin(),
                                        channel->frames.end(), wanted);
    const std::size_t hi = static_cast<std::size_t>(
        std::distance(channel->frames.begin(), after));
    const std::size_t lo = hi - 1U;
    const scene::KeyInterpolation interpolation =
        lo < channel->interpolations.size()
            ? channel->interpolations[lo]
            : scene::KeyInterpolation::Linear;
    if (interpolation == scene::KeyInterpolation::Hold) {
        return channel->values[lo];
    }
    const double span =
        static_cast<double>(channel->frames[hi] - channel->frames[lo]);
    double t = static_cast<double>(wanted - channel->frames[lo]) / span;
    if (interpolation == scene::KeyInterpolation::Cubic) {
        t = t * t * (3.0 - 2.0 * t);
    }
    return channel->values[lo] +
           (channel->values[hi] - channel->values[lo]) * t;
}

[[nodiscard]] ElementInfo evaluated_for_inspector(
    const ElementInfo& authored,
    std::span<const scene::ChannelKeyframes> keyframes,
    core::u64 show_frame) noexcept {
    ElementInfo displayed = authored;
    if (is_light(authored.kind)) {
        return displayed;
    }
    const auto apply = [&](scene::PropertyChannel component,
                           float& destination) {
        const auto sampled =
            sample_at(channel_for(keyframes, authored.node, component),
                      show_frame);
        if (sampled.has_value() && std::isfinite(*sampled)) {
            destination = static_cast<float>(*sampled);
        }
    };
    apply(scene::PropertyChannel::PositionX, displayed.position.x);
    apply(scene::PropertyChannel::PositionY, displayed.position.y);
    apply(scene::PropertyChannel::PositionZ, displayed.position.z);
    apply(scene::PropertyChannel::RotationYaw, displayed.rotation.x);
    apply(scene::PropertyChannel::RotationPitch, displayed.rotation.y);
    apply(scene::PropertyChannel::RotationRoll, displayed.rotation.z);
    apply(scene::PropertyChannel::ScaleX, displayed.scale.x);
    apply(scene::PropertyChannel::ScaleY, displayed.scale.y);
    apply(scene::PropertyChannel::ScaleZ, displayed.scale.z);
    apply(scene::PropertyChannel::Opacity, displayed.style.opacity);
    apply(scene::PropertyChannel::Brightness,
          displayed.style.effects.brightness);
    apply(scene::PropertyChannel::BlurRadius,
          displayed.style.effects.blur_radius);
    displayed.style.opacity = std::clamp(displayed.style.opacity, 0.0F, 1.0F);
    displayed.style.effects.brightness =
        std::clamp(displayed.style.effects.brightness, 0.0F, 16.0F);
    displayed.style.effects.blur_radius =
        std::clamp(displayed.style.effects.blur_radius, 0.0F, 64.0F);
    return displayed;
}

// The axis letters each transform group's three fields carry. Rotation is in
// turns about each axis, which an operator reads as yaw/pitch/roll rather than
// as x/y/z.
constexpr std::array<std::string_view, 3U> xyz{{"X", "Y", "Z"}};
constexpr std::array<std::string_view, 3U> ypr{{"Y", "P", "R"}};
constexpr std::array<std::string_view, 3U> rgb{{"R", "G", "B"}};

[[nodiscard]] constexpr std::string_view
blend_name(scene::BlendMode mode) noexcept {
    switch (mode) {
    case scene::BlendMode::Normal:
        return "Normal";
    case scene::BlendMode::Add:
        return "Add";
    case scene::BlendMode::Multiply:
        return "Multiply";
    case scene::BlendMode::Screen:
        return "Screen";
    }
    return "Normal";
}

} // namespace

gui::Rect InspectorPanel::take_row(Cursor& cursor, float height) noexcept {
    const gui::Rect row{cursor.column.x, cursor.y, cursor.column.width, height};
    cursor.y += height;
    return row;
}

bool InspectorPanel::vector_row(gui::UiContext& ctx, Cursor& cursor,
                                std::string_view label,
                                std::span<const std::string_view> axes,
                                std::span<const float> values, float speed,
                                std::size_t& changed_axis,
                                double& changed_value, bool* key_requested,
                                bool animated, bool keyed_here,
                                std::size_t id_salt) {
    const gui::Rect row = take_row(cursor, row_height);
    cursor.y += row_gap;

    const float label_width =
        std::clamp(row.width * label_fraction, label_min, label_max);
    const float key_width = key_requested != nullptr ? 18.0F : 0.0F;
    ctx.label(gui::Rect{row.x, row.y,
                        std::max(label_width - key_width, 0.0F), row.height}, label,
              ctx.theme().text_muted);
    if (key_requested != nullptr) {
        const gui::Rect key_bounds{row.x + label_width - key_width, row.y,
                                   key_width, row.height};
        *key_requested = key_button(ctx, mix(id_, label, 9'000U + id_salt), key_bounds,
                                    animated, keyed_here, ctx.theme().panel);
    }

    const gui::Rect fields{row.x + label_width, row.y,
                           std::max(row.width - label_width, 1.0F), row.height};

    const std::size_t count = std::min(axes.size(), values.size());
    std::array<gui::Rect, 4U> boxes{};
    columns(fields, std::min(count, boxes.size()), field_gap, boxes);

    bool changed = false;
    for (std::size_t i = 0U; i < count && i < boxes.size(); ++i) {
        double out = 0.0;
        if (fields_.draw(ctx, mix(id_, label, i + id_salt), boxes[i], axes[i],
                         static_cast<double>(values[i]),
                         static_cast<double>(speed), out)) {
            changed_axis = i;
            changed_value = out;
            changed = true;
        }
    }
    return changed;
}

bool InspectorPanel::section(gui::UiContext& ctx, Cursor& cursor,
                             std::string_view title, bool& open) {
    const gui::Rect row = take_row(cursor, section_height);
    cursor.y += row_gap;

    const gui::Interaction it = ctx.grab(mix(id_, title, 1000U), row);
    if (it.clicked) {
        open = !open;
    }
    ctx.panel(row, it.hovered ? ctx.theme().widget : ctx.theme().header);
    draw_twirl(ctx, row.x + 8.0F, row.y + row.height * 0.5F, open,
               ctx.theme().text_muted);
    ctx.label(gui::Rect{row.x + 20.0F, row.y, row.width - 20.0F, row.height},
              title, ctx.theme().text);
    return open;
}

void InspectorPanel::draw_header(gui::UiContext& ctx, Cursor& cursor,
                                 const ElementInfo& element,
                                 std::size_t also_selected) {
    const gui::Rect row = take_row(cursor, 30.0F);
    cursor.y += row_gap;

    // The kind swatch, matching the colour the timeline marks the same layer
    // with, so an operator can tie the two panels together at a glance.
    ctx.panel(gui::Rect{row.x, row.y + 7.0F, 4.0F, row.height - 14.0F},
              kind_color(element.kind));

    std::array<char, 64U> title{};
    const int length =
        std::snprintf(title.data(), title.size(), "%.*s %llu",
                      static_cast<int>(kind_name(element.kind).size()),
                      kind_name(element.kind).data(),
                      static_cast<unsigned long long>(element.node.value()));
    const std::string_view title_view{
        title.data(), static_cast<std::size_t>(std::max(length, 0))};

    const gui::Rect name{row.x + 12.0F, row.y, row.width - 12.0F, row.height};
    ctx.label(name, title_view, ctx.theme().text);

    if (also_selected > 0U) {
        std::array<char, 24U> extra{};
        const int extra_length = std::snprintf(extra.data(), extra.size(),
                                               "+%zu more", also_selected);
        const float offset = ctx.font_width(title_view) + 20.0F;
        ctx.label(gui::Rect{name.x + offset, name.y,
                            std::max(name.width - offset, 0.0F), name.height},
                  std::string_view{
                      extra.data(),
                      static_cast<std::size_t>(std::max(extra_length, 0))},
                  ctx.theme().text_muted);
    }
}

void InspectorPanel::draw_transform(gui::UiContext& ctx, Cursor& cursor,
                                    const ElementInfo& element,
                                    InspectorFrame& frame,
                                    std::span<const scene::ChannelKeyframes>
                                        keyframes,
                                    core::u64 show_frame) {
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
        bool animated = false;
        bool keyed_here = true;
        for (std::size_t component_index = 0U; component_index < 3U;
             ++component_index) {
            const auto component = static_cast<scene::TransformChannel>(
                static_cast<core::u8>(static_cast<std::size_t>(group.first) +
                                      component_index));
            const scene::ChannelKeyframes* const channel =
                channel_for(keyframes, element.node, component);
            animated = animated || (channel != nullptr && !channel->frames.empty());
            keyed_here = keyed_here && keyed_at(channel, show_frame);
        }

        std::size_t axis = 0U;
        double value = 0.0;
        bool key_requested = false;
        if (vector_row(ctx, cursor, group.label, group.axes, group.values,
                       group.speed, axis, value, &key_requested, animated,
                       keyed_here)) {
            const auto component =
                static_cast<scene::TransformChannel>(static_cast<core::u8>(
                    static_cast<std::size_t>(group.first) + axis));
            frame.component_edits.push_back(
                ComponentEdit{element.node, component, value});
        }
        if (key_requested) {
            for (std::size_t component_index = 0U; component_index < 3U;
                 ++component_index) {
                const auto component = static_cast<scene::TransformChannel>(
                    static_cast<core::u8>(
                        static_cast<std::size_t>(group.first) +
                        component_index));
                frame.key_edits.push_back(InspectorKeyEdit{
                    element.node, component, group.values[component_index],
                    keyed_here});
            }
        }
    }

    const gui::Rect note = take_row(cursor, 16.0F);
    cursor.y += row_gap;
    draw_diamond(ctx, note.x + 5.0F, note.y + note.height * 0.5F, 3.5F,
                 ctx.theme().text_muted);
    ctx.label(gui::Rect{note.x + 14.0F, note.y,
                        std::max(note.width - 14.0F, 0.0F), note.height},
              "key at playhead  |  rotation in turns",
              ctx.theme().text_muted);
}

void InspectorPanel::draw_light(gui::UiContext& ctx, Cursor& cursor,
                                const ElementInfo& element,
                                InspectorFrame& frame) {
    if (!section(ctx, cursor, "LIGHT", light_open_)) {
        return;
    }

    // A light's fields are plain document properties rather than transform
    // channels, so they are named rather than indexed.
    const auto emit = [&frame, &element](std::string_view property,
                                         double value) {
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
    ctx.label(gui::Rect{swatch.x, swatch.y, label_width, swatch.height},
              "Color", ctx.theme().text_muted);
    ctx.panel(
        gui::Rect{swatch.x + label_width, swatch.y + 2.0F,
                  std::max(swatch.width - label_width, 1.0F),
                  swatch.height - 4.0F},
        gui::UiColor{element.color.x, element.color.y, element.color.z, 1.0F});

    {
        const std::array<float, 3U> values{element.color.x, element.color.y,
                                           element.color.z};
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
        if (vector_row(ctx, cursor, "Intensity", axes, values, 0.05F, axis,
                       value)) {
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
        if (vector_row(ctx, cursor, "Direction", xyz, values, 0.01F, axis,
                       value)) {
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
        if (vector_row(ctx, cursor, "Position", xyz, values, 0.01F, axis,
                       value)) {
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

void InspectorPanel::draw_content(gui::UiContext& ctx, Cursor& cursor,
                                  const ElementInfo& element) {
    if (element.kind != ElementKind::Media &&
        element.kind != ElementKind::Model) {
        return;
    }
    if (!section(ctx, cursor, "CONTENT", content_open_)) {
        return;
    }

    const auto static_row = [&ctx, &cursor](std::string_view label,
                                            std::string_view value) {
        const gui::Rect row = take_row(cursor, row_height);
        cursor.y += row_gap;
        const float label_width = std::clamp(
            row.width * label_fraction, label_min, label_max);
        ctx.label(gui::Rect{row.x, row.y, label_width, row.height}, label,
                  ctx.theme().text_muted);
        const gui::Rect well{row.x + label_width, row.y + 1.0F,
                             std::max(row.width - label_width, 1.0F),
                             row.height - 2.0F};
        ctx.panel(well, ctx.theme().track);
        ctx.push_clip(well);
        ctx.label(gui::Rect{well.x + 6.0F, well.y,
                            std::max(well.width - 12.0F, 0.0F), well.height},
                  value.empty() ? "missing source" : value,
                  value.empty() ? ctx.theme().warning : ctx.theme().text);
        ctx.pop_clip();
    };

    static_row("Source", element.source);
    if (element.kind == ElementKind::Media) {
        std::array<char, 48U> sequence{};
        const int length = std::snprintf(
            sequence.data(), sequence.size(), "%u + %u frames",
            element.sequence_start, element.sequence_count);
        static_row(
            "Sequence",
            std::string_view{sequence.data(),
                             static_cast<std::size_t>(std::max(length, 0))});
    }
}

void InspectorPanel::draw_material(gui::UiContext& ctx, Cursor& cursor,
                                   const ElementInfo& element,
                                   InspectorFrame& frame) {
    if (element.kind != ElementKind::Object &&
        element.kind != ElementKind::Model) {
        return;
    }
    const bool imported_model = element.kind == ElementKind::Model;
    if (!section(ctx, cursor,
                 imported_model ? "FALLBACK MATERIAL" : "MATERIAL",
                 material_open_)) {
        return;
    }

    if (imported_model) {
        const gui::Rect note = take_row(cursor, row_height);
        cursor.y += row_gap;
        ctx.label(note, "Used for source parts without a material",
                  ctx.theme().text_muted);
    }

    const auto emit = [&frame, &element](std::string_view property,
                                         double value) {
        frame.named_edits.push_back(
            NamedEdit{element.node, std::string{property}, value});
    };

    const gui::Rect swatch = take_row(cursor, 14.0F);
    cursor.y += row_gap;
    const float label_width =
        std::clamp(swatch.width * label_fraction, label_min, label_max);
    ctx.label(gui::Rect{swatch.x, swatch.y, label_width, swatch.height},
              "Base Color", ctx.theme().text_muted);
    ctx.panel(gui::Rect{swatch.x + label_width, swatch.y + 2.0F,
                        std::max(swatch.width - label_width, 1.0F),
                        swatch.height - 4.0F},
              gui::UiColor{element.material.base_color.x,
                           element.material.base_color.y,
                           element.material.base_color.z, 1.0F});

    {
        const std::array<float, 3U> values{
            element.material.base_color.x, element.material.base_color.y,
            element.material.base_color.z};
        constexpr std::array<std::string_view, 3U> properties{{
            scene::schema::color_r, scene::schema::color_g,
            scene::schema::color_b}};
        std::size_t axis = 0U;
        double value = 0.0;
        if (vector_row(ctx, cursor, "RGB", rgb, values, 0.005F, axis,
                       value)) {
            emit(properties[axis], std::clamp(value, 0.0, 1.0));
        }
    }

    constexpr std::array<std::string_view, 1U> scalar_axis{{""}};
    {
        const std::array<float, 1U> value{element.material.roughness};
        std::size_t axis = 0U;
        double changed = 0.0;
        if (vector_row(ctx, cursor, "Roughness", scalar_axis, value, 0.005F,
                       axis, changed)) {
            emit(scene::schema::roughness, std::clamp(changed, 0.0, 1.0));
        }
    }
    {
        const std::array<float, 1U> value{element.material.metallic};
        std::size_t axis = 0U;
        double changed = 0.0;
        if (vector_row(ctx, cursor, "Metallic", scalar_axis, value, 0.005F,
                       axis, changed)) {
            emit(scene::schema::metallic, std::clamp(changed, 0.0, 1.0));
        }
    }
    {
        const std::array<float, 3U> values{
            element.material.emissive.x, element.material.emissive.y,
            element.material.emissive.z};
        constexpr std::array<std::string_view, 3U> properties{{
            scene::schema::emissive_r, scene::schema::emissive_g,
            scene::schema::emissive_b}};
        std::size_t axis = 0U;
        double value = 0.0;
        if (vector_row(ctx, cursor, "Emissive", rgb, values, 0.01F, axis,
                       value)) {
            emit(properties[axis], std::max(value, 0.0));
        }
    }
}

void InspectorPanel::draw_compositing(
    gui::UiContext& ctx, Cursor& cursor, const ElementInfo& element,
    InspectorFrame& frame,
    std::span<const scene::ChannelKeyframes> keyframes,
    core::u64 show_frame) {
    if (!section(ctx, cursor, "COMPOSITING", compositing_open_)) {
        return;
    }

    constexpr std::array<std::string_view, 1U> scalar_axis{{""}};
    const scene::ChannelKeyframes* const opacity_channel =
        channel_for(keyframes, element.node, scene::PropertyChannel::Opacity);
    const bool opacity_animated =
        opacity_channel != nullptr && !opacity_channel->frames.empty();
    const bool opacity_keyed = keyed_at(opacity_channel, show_frame);
    const std::array<float, 1U> opacity{element.style.opacity};
    std::size_t axis = 0U;
    double value = 0.0;
    bool key_requested = false;
    if (vector_row(ctx, cursor, "Opacity", scalar_axis, opacity, 0.005F,
                   axis, value, &key_requested, opacity_animated,
                   opacity_keyed)) {
        frame.component_edits.push_back(ComponentEdit{
            element.node, scene::PropertyChannel::Opacity,
            std::clamp(value, 0.0, 1.0)});
    }
    if (key_requested) {
        frame.key_edits.push_back(InspectorKeyEdit{
            element.node, scene::PropertyChannel::Opacity,
            element.style.opacity, opacity_keyed});
    }

    const gui::Rect row = take_row(cursor, row_height);
    cursor.y += row_gap;
    const float label_width =
        std::clamp(row.width * label_fraction, label_min, label_max);
    ctx.label(gui::Rect{row.x, row.y, label_width, row.height}, "Blend Mode",
              ctx.theme().text_muted);
    const gui::Rect well{row.x + label_width, row.y + 1.0F,
                         std::max(row.width - label_width, 1.0F),
                         row.height - 2.0F};
    const gui::Interaction interaction =
        ctx.grab(mix(id_, "blend mode", 7'100U), well);
    ctx.panel(well, interaction.hovered ? ctx.theme().widget_hover
                                        : ctx.theme().track);
    ctx.label(gui::Rect{well.x + 6.0F, well.y,
                        std::max(well.width - 12.0F, 0.0F), well.height},
              blend_name(element.style.blend), ctx.theme().text);
    if (interaction.clicked) {
        const auto current = static_cast<core::u8>(element.style.blend);
        const auto next = static_cast<core::u8>((current + 1U) % 4U);
        frame.named_edits.push_back(NamedEdit{
            element.node, std::string{scene::schema::blend_mode},
            static_cast<double>(next)});
    }
}

void InspectorPanel::draw_effects(
    gui::UiContext& ctx, Cursor& cursor, const ElementInfo& element,
    InspectorFrame& frame,
    std::span<const scene::ChannelKeyframes> keyframes,
    core::u64 show_frame,
    std::span<const scene::EffectInstance> effects) {
    if (!section(ctx, cursor, "EFFECTS", effects_open_)) {
        return;
    }

    const auto effect_row = [&](const scene::EffectInstance& effect) {
        const bool brightness = effect.kind == scene::EffectKind::Brightness;
        const std::string_view title = brightness ? "BRIGHTNESS" : "BLUR";
        const scene::PropertyChannel component = brightness
            ? scene::PropertyChannel::Brightness
            : scene::PropertyChannel::BlurRadius;
        const float speed = brightness ? 0.01F : 0.1F;
        const std::size_t id_salt =
            static_cast<std::size_t>(effect.source.value());
        const gui::Rect title_row = take_row(cursor, 19.0F);
        cursor.y += 2.0F;
        ctx.panel(title_row, ctx.theme().header);
        ctx.panel(gui::Rect{title_row.x, title_row.y, 2.0F,
                            title_row.height},
                  ctx.theme().accent);
        ctx.label(gui::Rect{title_row.x + 8.0F, title_row.y,
                            std::max(title_row.width - 34.0F, 0.0F),
                            title_row.height},
                  title, ctx.theme().text);
        const gui::Rect remove{title_row.x + title_row.width - 24.0F,
                               title_row.y, 24.0F, title_row.height};
        const gui::Interaction remove_it =
            ctx.grab(mix(id_, title, 8'000U + id_salt), remove);
        ctx.label(remove, "x", remove_it.hovered ? ctx.theme().warning
                                                 : ctx.theme().text_muted);
        if (remove_it.clicked) {
            frame.named_edits.push_back(NamedEdit{
                effect.source, std::string{scene::schema::effect_enabled},
                0.0});
        }

        const scene::ChannelKeyframes* const channel =
            channel_for(keyframes, effect.source, component);
        const bool animated = channel != nullptr && !channel->frames.empty();
        const bool keyed_here = keyed_at(channel, show_frame);
        constexpr std::array<std::string_view, 1U> scalar_axis{{""}};
        const std::array<float, 1U> value{effect.value};
        std::size_t axis = 0U;
        double changed = 0.0;
        bool key_requested = false;
        if (vector_row(ctx, cursor,
                       component == scene::PropertyChannel::Brightness
                           ? "Amount"
                           : "Radius",
                       scalar_axis, value, speed, axis, changed,
                       &key_requested, animated, keyed_here, id_salt)) {
            frame.component_edits.push_back(ComponentEdit{
                effect.source, component,
                component == scene::PropertyChannel::Brightness
                    ? std::clamp(changed, 0.0, 16.0)
                    : std::clamp(changed, 0.0, 64.0)});
        }
        if (key_requested) {
            frame.key_edits.push_back(InspectorKeyEdit{
                effect.source, component, effect.value, keyed_here});
        }
        cursor.y += 2.0F;
    };

    const bool any = std::any_of(
        effects.begin(), effects.end(), [&element](const auto& effect) {
            return effect.target == element.node && effect.enabled;
        });
    if (!any) {
        const gui::Rect empty = take_row(cursor, 34.0F);
        cursor.y += row_gap;
        ctx.panel(empty, ctx.theme().track);
        ctx.label(gui::Rect{empty.x + 7.0F, empty.y,
                            std::max(empty.width - 14.0F, 0.0F), empty.height},
                  "No effects - drag one from Effects",
                  ctx.theme().text_muted);
        return;
    }

    for (const scene::EffectInstance& effect : effects) {
        if (effect.target == element.node && effect.enabled) {
            effect_row(effect);
        }
    }
}

InspectorFrame InspectorPanel::draw(gui::UiContext& ctx,
                                    const gui::Rect& bounds,
                                    const ElementInfo* element,
                                    std::size_t also_selected,
                                    std::span<const scene::ChannelKeyframes>
                                        keyframes,
                                    core::u64 show_frame,
                                    std::span<const scene::EffectInstance>
                                        effects) {
    InspectorFrame frame{};

    ctx.panel(bounds, ctx.theme().panel);
    if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
        return frame;
    }

    // A selection that vanished under a live edit ends it, rather than leaving
    // the keyboard in a field belonging to a node that is no longer there.
    if (element == nullptr) {
        fields_.cancel(ctx);
        ctx.label(gui::Rect{bounds.x + panel_inset, bounds.y + panel_inset,
                            std::max(bounds.width - panel_inset * 2.0F, 1.0F),
                            row_height},
                  "select an element", ctx.theme().text_muted);
        return frame;
    }

    // ElementInfo deliberately carries authored document values. The renderer
    // folds animation channels over those values for the current show frame;
    // mirror that fold into a temporary display value so an animated field
    // follows the playhead instead of appearing frozen at its base property.
    // The source ElementInfo remains untouched, preserving the authored/edit
    // contract used by the shell and undo system.
    const ElementInfo displayed =
        evaluated_for_inspector(*element, keyframes, show_frame);

    // The wheel scrolls the panel when the pointer is over it, clamped against
    // last frame's measured height - this frame's is not known until the end.
    if (bounds.contains(ctx.pointer_x(), ctx.pointer_y()) &&
        ctx.wheel() != 0.0F) {
        scroll_ -= ctx.wheel() * 32.0F;
    }
    scroll_ = std::clamp(scroll_, 0.0F,
                         std::max(content_height_ - bounds.height, 0.0F));

    ctx.push_clip(bounds);
    Cursor cursor{gui::Rect{bounds.x + panel_inset, bounds.y,
                            std::max(bounds.width - panel_inset * 2.0F, 1.0F),
                            0.0F},
                  bounds.y + panel_inset - scroll_};

    draw_header(ctx, cursor, displayed, also_selected);
    if (is_light(displayed.kind)) {
        draw_light(ctx, cursor, displayed, frame);
    } else {
        draw_transform(ctx, cursor, displayed, frame, keyframes, show_frame);
    }

    // Delete, mirroring the Edit menu and the Del key.
    cursor.y += row_gap * 2.0F;
    const gui::Rect actions = take_row(cursor, row_height);
    if (ctx.button(mix(id_, "delete", 0U),
                   gui::Rect{actions.x, actions.y,
                             std::min(actions.width, 96.0F), actions.height},
                   "Delete")) {
        frame.delete_requested = true;
    }

    if (!is_light(displayed.kind)) {
        cursor.y += row_gap;
        draw_content(ctx, cursor, displayed);
        draw_material(ctx, cursor, displayed, frame);
        draw_compositing(ctx, cursor, displayed, frame, keyframes, show_frame);
        draw_effects(ctx, cursor, displayed, frame, keyframes, show_frame,
                     effects);
    }

    cursor.y += panel_inset;

    content_height_ = cursor.y + scroll_ - bounds.y;
    ctx.pop_clip();

    frame.editing = fields_.live();
    return frame;
}

} // namespace mediaserver::editor
