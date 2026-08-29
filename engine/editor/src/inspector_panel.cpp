#include <mediaserver/editor/inspector_panel.hpp>

#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
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
    apply(scene::PropertyChannel::Saturation,
          displayed.style.effects.saturation);
    apply(scene::PropertyChannel::BlurRadius,
          displayed.style.effects.blur_radius);
    displayed.style.opacity = std::clamp(displayed.style.opacity, 0.0F, 1.0F);
    displayed.style.effects.brightness =
        std::clamp(displayed.style.effects.brightness, 0.0F, 16.0F);
    displayed.style.effects.saturation =
        std::clamp(displayed.style.effects.saturation, 0.0F,
                   scene::max_saturation);
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

void InspectorPanel::vector_row(gui::UiContext& ctx, Cursor& cursor,
                                std::string_view label,
                                std::span<const std::string_view> axes,
                                std::span<const float> values, float speed,
                                std::span<const FieldBinding> bindings,
                                InspectorFrame& frame,
                                bool* key_requested, bool animated,
                                bool keyed_here, std::size_t id_salt) {
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

    const std::size_t count =
        std::min({axes.size(), values.size(), bindings.size()});
    std::array<gui::Rect, 4U> boxes{};
    columns(fields, std::min(count, boxes.size()), field_gap, boxes);

    for (std::size_t i = 0U; i < count && i < boxes.size(); ++i) {
        ValueSummary summary = summarize(bindings[i], frame);
        if (summary.mixed) {
            ++frame.mixed_fields;
        }
        const gui::UiId field = mix(id_, label, i + id_salt);
        double out = 0.0;
        NumberFieldEdit edit{};
        if (fields_.draw(ctx, field, boxes[i], axes[i],
                         static_cast<double>(values[i]),
                         static_cast<double>(speed), out, 3, &edit,
                         summary.mixed)) {
            emit_numeric(bindings[i], field, edit, std::move(summary), frame);
        }
    }
}

std::optional<InspectorPanel::EditableValue>
InspectorPanel::editable_value(const ElementInfo& element,
                               const FieldBinding& binding) const {
    switch (binding.domain) {
    case FieldDomain::Transform:
        if (!is_light(element.kind)) {
            return EditableValue{element.node, element.node,
                                 component_value(element, binding.component)};
        }
        return std::nullopt;
    case FieldDomain::Compositing:
        if (!is_light(element.kind) && element.kind != ElementKind::Group &&
            binding.component == scene::PropertyChannel::Opacity) {
            return EditableValue{element.node, element.node,
                                 element.style.opacity};
        }
        return std::nullopt;
    case FieldDomain::Screen:
        if (element.kind != ElementKind::Track) {
            return std::nullopt;
        }
        if (binding.property == scene::schema::lane_raster_width) {
            return EditableValue{element.node, element.node,
                                 static_cast<double>(element.raster_width)};
        }
        if (binding.property == scene::schema::lane_raster_height) {
            return EditableValue{element.node, element.node,
                                 static_cast<double>(element.raster_height)};
        }
        return std::nullopt;
    case FieldDomain::Light:
        if (!is_light(element.kind)) {
            return std::nullopt;
        }
        if (binding.property == scene::schema::color_r) {
            return EditableValue{element.node, element.node, element.color.x};
        }
        if (binding.property == scene::schema::color_g) {
            return EditableValue{element.node, element.node, element.color.y};
        }
        if (binding.property == scene::schema::color_b) {
            return EditableValue{element.node, element.node, element.color.z};
        }
        if (binding.property == scene::schema::intensity) {
            return EditableValue{element.node, element.node,
                                 element.intensity};
        }
        if (element.kind == ElementKind::DirectionalLight) {
            if (binding.property == scene::schema::direction_x) {
                return EditableValue{element.node, element.node,
                                     element.direction.x};
            }
            if (binding.property == scene::schema::direction_y) {
                return EditableValue{element.node, element.node,
                                     element.direction.y};
            }
            if (binding.property == scene::schema::direction_z) {
                return EditableValue{element.node, element.node,
                                     element.direction.z};
            }
            return std::nullopt;
        }
        if (binding.property == scene::schema::position_x) {
            return EditableValue{element.node, element.node,
                                 element.position.x};
        }
        if (binding.property == scene::schema::position_y) {
            return EditableValue{element.node, element.node,
                                 element.position.y};
        }
        if (binding.property == scene::schema::position_z) {
            return EditableValue{element.node, element.node,
                                 element.position.z};
        }
        if (binding.property == scene::schema::range) {
            return EditableValue{element.node, element.node, element.range};
        }
        return std::nullopt;
    case FieldDomain::Material:
        if (element.kind != ElementKind::Object &&
            element.kind != ElementKind::Model) {
            return std::nullopt;
        }
        if (binding.property == scene::schema::color_r) {
            return EditableValue{element.node, element.node,
                                 element.material.base_color.x};
        }
        if (binding.property == scene::schema::color_g) {
            return EditableValue{element.node, element.node,
                                 element.material.base_color.y};
        }
        if (binding.property == scene::schema::color_b) {
            return EditableValue{element.node, element.node,
                                 element.material.base_color.z};
        }
        if (binding.property == scene::schema::roughness) {
            return EditableValue{element.node, element.node,
                                 element.material.roughness};
        }
        if (binding.property == scene::schema::metallic) {
            return EditableValue{element.node, element.node,
                                 element.material.metallic};
        }
        if (binding.property == scene::schema::emissive_r) {
            return EditableValue{element.node, element.node,
                                 element.material.emissive.x};
        }
        if (binding.property == scene::schema::emissive_g) {
            return EditableValue{element.node, element.node,
                                 element.material.emissive.y};
        }
        if (binding.property == scene::schema::emissive_b) {
            return EditableValue{element.node, element.node,
                                 element.material.emissive.z};
        }
        return std::nullopt;
    case FieldDomain::Effect: {
        if (is_light(element.kind)) {
            return std::nullopt;
        }
        std::size_t ordinal = 0U;
        for (const scene::EffectInstance &effect : effects_) {
          if (effect.target != element.node || !effect.enabled ||
              effect.kind != binding.effect_kind) {
            continue;
          }
          if (ordinal++ != binding.effect_ordinal) {
            continue;
          }
          const bool keyed_primary_component =
              (binding.effect_kind == scene::EffectKind::BoxBlur &&
               binding.component == scene::PropertyChannel::BoxBlurRadius) ||
              (binding.effect_kind == scene::EffectKind::DirectionalBlur &&
               binding.component ==
                   scene::PropertyChannel::DirectionalBlurLength) ||
              (binding.effect_kind == scene::EffectKind::ChromaKey &&
               binding.component == scene::PropertyChannel::ChromaKeyMix) ||
              (binding.effect_kind == scene::EffectKind::LumaKey &&
               binding.component == scene::PropertyChannel::LumaKeyMix);
          const scene::ChannelKeyframes *const channel =
              binding.property.empty() || keyed_primary_component
                  ? channel_for(keyframes_, effect.source, binding.component)
                  : nullptr;
          const std::optional<double> sampled = sample_at(channel, show_frame_);
          double authored = static_cast<double>(effect.value);
          if (binding.effect_kind == scene::EffectKind::FilmGrain) {
            authored = static_cast<double>(effect.film_grain.amount);
          } else if (binding.effect_kind == scene::EffectKind::BoxBlur) {
            if (binding.property == scene::schema::box_blur_radius) {
              authored = static_cast<double>(effect.box_blur.radius_px);
            } else if (binding.property == scene::schema::box_blur_passes) {
              authored = static_cast<double>(effect.box_blur.passes);
            } else {
              return std::nullopt;
            }
          } else if (binding.effect_kind ==
                     scene::EffectKind::DirectionalBlur) {
            if (binding.property == scene::schema::directional_blur_length) {
              authored = static_cast<double>(effect.directional_blur.length_px);
            } else if (binding.property ==
                       scene::schema::directional_blur_angle_turns) {
              authored =
                  static_cast<double>(effect.directional_blur.angle_turns);
            } else {
              return std::nullopt;
            }
          } else if (binding.effect_kind == scene::EffectKind::ChromaKey) {
            authored = static_cast<double>(effect.chroma_key.mix);
          } else if (binding.effect_kind == scene::EffectKind::LumaKey) {
            authored = static_cast<double>(effect.luma_key.mix);
          }
          const double raw = sampled.has_value() && std::isfinite(*sampled)
                                 ? *sampled
                                 : authored;
          double maximum = 64.0;
          if (binding.effect_kind == scene::EffectKind::Brightness) {
            maximum = 16.0;
          } else if (binding.effect_kind == scene::EffectKind::Saturation) {
            maximum = scene::max_saturation;
          } else if (binding.effect_kind == scene::EffectKind::FilmGrain) {
            maximum = scene::max_film_grain_amount;
          } else if (binding.effect_kind == scene::EffectKind::BoxBlur) {
            maximum = binding.property == scene::schema::box_blur_passes
                          ? static_cast<double>(scene::max_box_blur_passes)
                          : static_cast<double>(scene::max_box_blur_radius_px);
          } else if (binding.effect_kind ==
                     scene::EffectKind::DirectionalBlur) {
            maximum =
                binding.property == scene::schema::directional_blur_angle_turns
                    ? 1.0
                    : static_cast<double>(
                          scene::max_directional_blur_length_px);
          } else if (binding.effect_kind == scene::EffectKind::ChromaKey ||
                     binding.effect_kind == scene::EffectKind::LumaKey) {
            maximum = 1.0;
          }
          return EditableValue{element.node, effect.source,
                               std::clamp(raw, 0.0, maximum)};
        }
        return std::nullopt;
    }
    }
    return std::nullopt;
}

InspectorPanel::ValueSummary
InspectorPanel::summarize(const FieldBinding& binding,
                          InspectorFrame& frame) const {
    const auto append_unique = [](std::vector<doc::NodeId>& nodes,
                                  doc::NodeId node) {
        if (std::find(nodes.begin(), nodes.end(), node) == nodes.end()) {
            nodes.push_back(node);
        }
    };

    ValueSummary summary{};
    for (const ElementInfo& element : selected_) {
        if (element.locked) {
            append_unique(frame.skipped_locked, element.node);
            continue;
        }
        const std::optional<EditableValue> value =
            editable_value(element, binding);
        if (!value.has_value()) {
            append_unique(summary.incompatible, element.node);
            continue;
        }
        if (!summary.targets.empty() &&
            summary.targets.front().value != value->value) {
            summary.mixed = true;
        }
        summary.targets.push_back(*value);
    }
    return summary;
}

void InspectorPanel::emit_numeric(const FieldBinding& binding,
                                  gui::UiId field,
                                  const NumberFieldEdit& edit,
                                  ValueSummary summary,
                                  InspectorFrame& frame) {
    if (summary.targets.empty()) {
        return;
    }

    for (const doc::NodeId node : summary.incompatible) {
        if (std::find(frame.skipped_incompatible.begin(),
                      frame.skipped_incompatible.end(), node) ==
            frame.skipped_incompatible.end()) {
            frame.skipped_incompatible.push_back(node);
        }
    }

    if (edit.kind == NumberFieldEditKind::Delta ||
        edit.kind == NumberFieldEditKind::Nudge) {
        const bool same_targets =
            drag_capture_.starts.size() == summary.targets.size() &&
            std::equal(drag_capture_.starts.begin(), drag_capture_.starts.end(),
                       summary.targets.begin(),
                       [](const EditableValue& left,
                          const EditableValue& right) {
                           return left.element == right.element &&
                                  left.author == right.author;
                       });
        const bool same_stream =
            drag_capture_.kind == edit.kind &&
            (edit.kind != NumberFieldEditKind::Nudge ||
             (drag_capture_.direction == edit.direction &&
              drag_capture_.modifiers == edit.modifiers));
        if (drag_capture_.field != field || !same_targets || !same_stream) {
            const bool carry_local_nudge =
                drag_capture_.field == field && same_targets &&
                drag_capture_.kind == NumberFieldEditKind::Nudge &&
                edit.kind == NumberFieldEditKind::Nudge;
            if (carry_local_nudge) {
                for (EditableValue& value : drag_capture_.starts) {
                    value.value += drag_capture_.delta;
                }
            } else {
                drag_capture_.starts = summary.targets;
            }
            drag_capture_.field = field;
            drag_capture_.kind = edit.kind;
            drag_capture_.direction = edit.direction;
            drag_capture_.modifiers = edit.modifiers;
        }
        drag_capture_.delta = edit.delta;
        summary.targets = drag_capture_.starts;
    } else {
        drag_capture_ = {};
    }

    const auto clamp_value = [&binding](double value) {
      switch (binding.domain) {
      case FieldDomain::Transform:
        return value;
      case FieldDomain::Compositing:
        return std::clamp(value, 0.0, 1.0);
      case FieldDomain::Light:
        if (binding.property == scene::schema::intensity ||
            binding.property == scene::schema::range) {
          return std::max(value, 0.0);
        }
        return value;
      case FieldDomain::Screen:
        // A raster is whole pixels. Rounding here rather than at the document
        // boundary means the field itself never shows 1919.6.
        return std::clamp(std::round(value), 1.0,
                          static_cast<double>(scene::max_raster_extent));
      case FieldDomain::Material:
        if (binding.property == scene::schema::color_r ||
            binding.property == scene::schema::color_g ||
            binding.property == scene::schema::color_b ||
            binding.property == scene::schema::roughness ||
            binding.property == scene::schema::metallic) {
          return std::clamp(value, 0.0, 1.0);
        }
        return std::max(value, 0.0);
      case FieldDomain::Effect:
        if (binding.effect_kind == scene::EffectKind::Brightness) {
          return std::clamp(value, 0.0, 16.0);
        }
        if (binding.effect_kind == scene::EffectKind::Saturation) {
          return std::clamp(value, 0.0,
                            static_cast<double>(scene::max_saturation));
        }
        if (binding.effect_kind == scene::EffectKind::FilmGrain) {
          return std::clamp(value,
                            static_cast<double>(scene::min_film_grain_amount),
                            static_cast<double>(scene::max_film_grain_amount));
        }
        if (binding.effect_kind == scene::EffectKind::BoxBlur) {
          if (binding.property == scene::schema::box_blur_radius) {
            return std::clamp(
                std::round(value), 0.0,
                static_cast<double>(scene::max_box_blur_radius_px));
          }
          if (binding.property == scene::schema::box_blur_passes) {
            return std::clamp(std::round(value), 1.0,
                              static_cast<double>(scene::max_box_blur_passes));
          }
        }
        if (binding.effect_kind == scene::EffectKind::DirectionalBlur) {
          if (binding.property == scene::schema::directional_blur_length) {
            return std::clamp(
                value, 0.0,
                static_cast<double>(scene::max_directional_blur_length_px));
          }
          if (binding.property == scene::schema::directional_blur_angle_turns) {
            const double wrapped = value - std::floor(value);
            return wrapped < 0.0 ? wrapped + 1.0 : wrapped;
          }
        }
        if (binding.effect_kind == scene::EffectKind::ChromaKey ||
            binding.effect_kind == scene::EffectKind::LumaKey) {
          return std::clamp(value, 0.0, 1.0);
        }
        return std::clamp(value, 0.0, 64.0);
      }
      return value;
    };

    const std::size_t emitted_components = frame.component_edits.size();
    const std::size_t emitted_named = frame.named_edits.size();
    for (const EditableValue &target : summary.targets) {
      const double raw = edit.kind == NumberFieldEditKind::Delta ||
                                 edit.kind == NumberFieldEditKind::Nudge
                             ? target.value + edit.delta
                             : edit.value;
      const double value = clamp_value(raw);
      if (edit.kind == NumberFieldEditKind::Nudge && value == target.value) {
        continue;
      }
      switch (binding.domain) {
      case FieldDomain::Transform:
      case FieldDomain::Compositing:
        frame.component_edits.push_back(
            ComponentEdit{target.author, binding.component, value});
        break;
      case FieldDomain::Effect:
        if (binding.property.empty() ||
            binding.component == scene::PropertyChannel::BoxBlurRadius ||
            binding.component ==
                scene::PropertyChannel::DirectionalBlurLength ||
            binding.component == scene::PropertyChannel::ChromaKeyMix ||
            binding.component == scene::PropertyChannel::LumaKeyMix) {
          frame.component_edits.push_back(
              ComponentEdit{target.author, binding.component, value});
        } else {
          frame.named_edits.push_back(
              NamedEdit{target.author, std::string{binding.property}, value});
        }
        break;
      case FieldDomain::Light:
      case FieldDomain::Material:
      case FieldDomain::Screen:
        frame.named_edits.push_back(
            NamedEdit{target.author, std::string{binding.property}, value});
        break;
      }
    }
    if (edit.kind == NumberFieldEditKind::Nudge &&
        (frame.component_edits.size() != emitted_components ||
         frame.named_edits.size() != emitted_named)) {
        core::u64 property =
            static_cast<core::u64>(binding.domain) + 1U;
        property ^= (static_cast<core::u64>(binding.component) + 1U) << 8U;
        property ^= (static_cast<core::u64>(binding.effect_kind) + 1U) << 24U;
        property ^= static_cast<core::u64>(binding.effect_ordinal) << 40U;
        if (!binding.property.empty()) {
            property ^= gui::ui_id(binding.property);
        }
        frame.numeric_nudge = NumericNudge{
            field, property, edit.direction, edit.modifiers};
    }
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
                                 std::size_t selected_count) {
    const gui::Rect row = take_row(cursor, 30.0F);
    cursor.y += row_gap;

    // The kind swatch, matching the colour the timeline marks the same layer
    // with, so an operator can tie the two panels together at a glance.
    ctx.panel(gui::Rect{row.x, row.y + 7.0F, 4.0F, row.height - 14.0F},
              kind_color(element.kind));

    std::array<char, 64U> title{};
    const int length = element.display_name.empty()
        ? std::snprintf(title.data(), title.size(), "%.*s %llu",
                        static_cast<int>(kind_name(element.kind).size()),
                        kind_name(element.kind).data(),
                        static_cast<unsigned long long>(element.node.value()))
        : std::snprintf(title.data(), title.size(), "%s",
                        element.display_name.c_str());
    const std::string_view title_view{
        title.data(), static_cast<std::size_t>(std::clamp(
                          length, 0, static_cast<int>(title.size() - 1U)))};

    const gui::Rect name{row.x + 12.0F, row.y, row.width - 12.0F, row.height};
    ctx.label(name, title_view, ctx.theme().text);

    if (selected_count > 1U) {
        std::array<char, 24U> extra{};
        const int extra_length = std::snprintf(
            extra.data(), extra.size(), "%zu selected", selected_count);
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

        std::array<FieldBinding, 3U> bindings{};
        for (std::size_t component_index = 0U; component_index < 3U;
             ++component_index) {
            bindings[component_index] = FieldBinding{
                FieldDomain::Transform,
                static_cast<scene::TransformChannel>(static_cast<core::u8>(
                    static_cast<std::size_t>(group.first) +
                    component_index))};
        }
        bool key_requested = false;
        vector_row(ctx, cursor, group.label, group.axes, group.values,
                   group.speed, bindings, frame, &key_requested, animated,
                   keyed_here);
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

    constexpr std::array<std::string_view, 3U> color_properties{{
        scene::schema::color_r, scene::schema::color_g,
        scene::schema::color_b}};
    std::array<FieldBinding, 3U> color_bindings{};
    bool color_mixed = false;
    for (std::size_t i = 0U; i < color_bindings.size(); ++i) {
        color_bindings[i] =
            FieldBinding{FieldDomain::Light, {}, color_properties[i]};
        color_mixed = color_mixed || summarize(color_bindings[i], frame).mixed;
    }

    // The colour, shown as the bar it actually produces above its own numbers -
    // three floats do not read as a colour, and a light's tint is the thing an
    // operator judges by eye.
    const gui::Rect swatch = take_row(cursor, 14.0F);
    cursor.y += row_gap;
    const float label_width =
        std::clamp(swatch.width * label_fraction, label_min, label_max);
    ctx.label(gui::Rect{swatch.x, swatch.y, label_width, swatch.height},
              "Color", ctx.theme().text_muted);
    const gui::Rect color_well{
        swatch.x + label_width, swatch.y + 2.0F,
        std::max(swatch.width - label_width, 1.0F), swatch.height - 4.0F};
    if (color_mixed) {
        ctx.panel(color_well, ctx.theme().track);
        ctx.label(color_well, "Mixed", ctx.theme().text_muted);
    } else {
        ctx.panel(color_well,
                  gui::UiColor{element.color.x, element.color.y,
                               element.color.z, 1.0F});
    }

    {
        const std::array<float, 3U> values{element.color.x, element.color.y,
                                           element.color.z};
        vector_row(ctx, cursor, "RGB", rgb, values, 0.005F, color_bindings,
                   frame);
    }

    {
        const std::array<float, 1U> values{element.intensity};
        constexpr std::array<std::string_view, 1U> axes{{""}};
        const std::array<FieldBinding, 1U> bindings{{
            {FieldDomain::Light, {}, scene::schema::intensity}}};
        vector_row(ctx, cursor, "Intensity", axes, values, 0.05F, bindings,
                   frame);
    }

    if (element.kind == ElementKind::DirectionalLight) {
        const std::array<float, 3U> values{
            element.direction.x, element.direction.y, element.direction.z};
        constexpr std::array<std::string_view, 3U> properties{
            {scene::schema::direction_x, scene::schema::direction_y,
             scene::schema::direction_z}};
        std::array<FieldBinding, 3U> bindings{};
        for (std::size_t i = 0U; i < bindings.size(); ++i) {
            bindings[i] = FieldBinding{FieldDomain::Light, {}, properties[i]};
        }
        vector_row(ctx, cursor, "Direction", xyz, values, 0.01F, bindings,
                   frame);
        return;
    }

    {
        const std::array<float, 3U> values{
            element.position.x, element.position.y, element.position.z};
        constexpr std::array<std::string_view, 3U> properties{
            {scene::schema::position_x, scene::schema::position_y,
             scene::schema::position_z}};
        std::array<FieldBinding, 3U> bindings{};
        for (std::size_t i = 0U; i < bindings.size(); ++i) {
            bindings[i] = FieldBinding{FieldDomain::Light, {}, properties[i]};
        }
        vector_row(ctx, cursor, "Position", xyz, values, 0.01F, bindings,
                   frame);
    }
    {
        const std::array<float, 1U> values{element.range};
        constexpr std::array<std::string_view, 1U> axes{{""}};
        const std::array<FieldBinding, 1U> bindings{{
            {FieldDomain::Light, {}, scene::schema::range}}};
        vector_row(ctx, cursor, "Range", axes, values, 0.1F, bindings, frame);
    }
}

void InspectorPanel::draw_content(gui::UiContext& ctx, Cursor& cursor,
                                  const ElementInfo& element,
                                  InspectorFrame& frame) {
    if (element.kind != ElementKind::Media &&
        element.kind != ElementKind::Model) {
        return;
    }
    if (!section(ctx, cursor, "CONTENT", content_open_)) {
        return;
    }

    const auto static_row = [&ctx, &cursor](std::string_view label,
                                            std::string_view value,
                                            bool mixed) {
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
                  mixed ? "Mixed"
                        : (value.empty() ? "missing source" : value),
                  mixed || value.empty() ? ctx.theme().warning
                                         : ctx.theme().text);
        ctx.pop_clip();
    };

    bool source_mixed = false;
    const ElementInfo* source_first = nullptr;
    for (const ElementInfo& selected : selected_) {
        if (selected.locked ||
            (selected.kind != ElementKind::Media &&
             selected.kind != ElementKind::Model)) {
            continue;
        }
        if (source_first != nullptr &&
            source_first->source != selected.source) {
            source_mixed = true;
        }
        if (source_first == nullptr) {
            source_first = &selected;
        }
    }
    if (source_mixed) {
        ++frame.mixed_fields;
    }
    static_row("Source", element.source, source_mixed);
    if (element.kind == ElementKind::Media) {
        bool sequence_mixed = false;
        const ElementInfo* sequence_first = nullptr;
        for (const ElementInfo& selected : selected_) {
            if (selected.locked || selected.kind != ElementKind::Media) {
                continue;
            }
            if (sequence_first != nullptr &&
                (sequence_first->sequence_start != selected.sequence_start ||
                 sequence_first->sequence_count != selected.sequence_count)) {
                sequence_mixed = true;
            }
            if (sequence_first == nullptr) {
                sequence_first = &selected;
            }
        }
        if (sequence_mixed) {
            ++frame.mixed_fields;
        }
        std::array<char, 48U> sequence{};
        const int length = std::snprintf(
            sequence.data(), sequence.size(), "%u + %u frames",
            element.sequence_start, element.sequence_count);
        static_row("Sequence",
                   std::string_view{
                       sequence.data(),
                       static_cast<std::size_t>(std::max(length, 0))},
                   sequence_mixed);
    }
}

void InspectorPanel::draw_screen(gui::UiContext& ctx, Cursor& cursor,
                                 const ElementInfo& element,
                                 InspectorFrame& frame) {
    if (element.kind != ElementKind::Track) {
        return;
    }
    if (!section(ctx, cursor, "SCREEN", screen_open_)) {
        return;
    }

    // The format the screen drives, in pixels. It is not read from whatever
    // clip happens to be up: an operator sets a screen to the format of the
    // wall, and a plate of the wrong size should look wrong rather than
    // quietly reshape the wall around itself.
    constexpr std::array<std::string_view, 2U> wh{{"W", "H"}};
    const std::array<float, 2U> raster{
        static_cast<float>(element.raster_width),
        static_cast<float>(element.raster_height)};
    const std::array<FieldBinding, 2U> raster_bindings{{
        {FieldDomain::Screen, {}, scene::schema::lane_raster_width},
        {FieldDomain::Screen, {}, scene::schema::lane_raster_height}}};
    vector_row(ctx, cursor, "Raster", wh, raster, 1.0F, raster_bindings,
               frame);

    // One control, and it says what is true rather than what is not: locked
    // reads "Height follows width", open reads nothing at all beyond the
    // aspect the plane currently has.
    const gui::Rect row = take_row(cursor, row_height);
    cursor.y += row_gap;
    const float label_width =
        std::clamp(row.width * label_fraction, label_min, label_max);
    ctx.label(gui::Rect{row.x, row.y, label_width, row.height}, "Aspect",
              ctx.theme().text_muted);
    const gui::Rect well{row.x + label_width, row.y + 1.0F,
                         std::max(row.width - label_width, 1.0F),
                         row.height - 2.0F};
    const gui::Interaction interaction =
        ctx.grab(mix(id_, "aspect lock", element.node.value()), well);
    ctx.panel(well, interaction.hovered ? ctx.theme().row_alt
                                        : ctx.theme().track);
    std::array<char, 48U> summary{};
    const int length = element.aspect_locked
        ? std::snprintf(summary.data(), summary.size(),
                        "Height follows width")
        : std::snprintf(summary.data(), summary.size(), "%.3f : 1",
                        element.scale.y > 0.0F
                            ? static_cast<double>(element.scale.x /
                                                  element.scale.y)
                            : 0.0);
    ctx.label(gui::Rect{well.x + 6.0F, well.y,
                        std::max(well.width - 12.0F, 0.0F), well.height},
              std::string_view{summary.data(),
                               static_cast<std::size_t>(std::clamp(
                                   length, 0,
                                   static_cast<int>(summary.size() - 1U)))},
              element.aspect_locked ? ctx.theme().text
                                    : ctx.theme().text_muted);
    if (interaction.clicked && !element.locked) {
        frame.named_edits.push_back(
            NamedEdit{element.node,
                      std::string{scene::schema::lane_aspect_locked},
                      element.aspect_locked ? 0.0 : 1.0});
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

    constexpr std::array<std::string_view, 3U> color_properties{{
        scene::schema::color_r, scene::schema::color_g,
        scene::schema::color_b}};
    std::array<FieldBinding, 3U> color_bindings{};
    bool color_mixed = false;
    for (std::size_t i = 0U; i < color_bindings.size(); ++i) {
        color_bindings[i] =
            FieldBinding{FieldDomain::Material, {}, color_properties[i]};
        color_mixed = color_mixed || summarize(color_bindings[i], frame).mixed;
    }

    const gui::Rect swatch = take_row(cursor, 14.0F);
    cursor.y += row_gap;
    const float label_width =
        std::clamp(swatch.width * label_fraction, label_min, label_max);
    ctx.label(gui::Rect{swatch.x, swatch.y, label_width, swatch.height},
              "Base Color", ctx.theme().text_muted);
    const gui::Rect color_well{
        swatch.x + label_width, swatch.y + 2.0F,
        std::max(swatch.width - label_width, 1.0F), swatch.height - 4.0F};
    if (color_mixed) {
        ctx.panel(color_well, ctx.theme().track);
        ctx.label(color_well, "Mixed", ctx.theme().text_muted);
    } else {
        ctx.panel(color_well,
                  gui::UiColor{element.material.base_color.x,
                               element.material.base_color.y,
                               element.material.base_color.z, 1.0F});
    }

    {
        const std::array<float, 3U> values{
            element.material.base_color.x, element.material.base_color.y,
            element.material.base_color.z};
        vector_row(ctx, cursor, "RGB", rgb, values, 0.005F, color_bindings,
                   frame);
    }

    constexpr std::array<std::string_view, 1U> scalar_axis{{""}};
    {
        const std::array<float, 1U> value{element.material.roughness};
        const std::array<FieldBinding, 1U> bindings{{
            {FieldDomain::Material, {}, scene::schema::roughness}}};
        vector_row(ctx, cursor, "Roughness", scalar_axis, value, 0.005F,
                   bindings, frame);
    }
    {
        const std::array<float, 1U> value{element.material.metallic};
        const std::array<FieldBinding, 1U> bindings{{
            {FieldDomain::Material, {}, scene::schema::metallic}}};
        vector_row(ctx, cursor, "Metallic", scalar_axis, value, 0.005F,
                   bindings, frame);
    }
    {
        const std::array<float, 3U> values{
            element.material.emissive.x, element.material.emissive.y,
            element.material.emissive.z};
        constexpr std::array<std::string_view, 3U> properties{{
            scene::schema::emissive_r, scene::schema::emissive_g,
            scene::schema::emissive_b}};
        std::array<FieldBinding, 3U> bindings{};
        for (std::size_t i = 0U; i < bindings.size(); ++i) {
            bindings[i] =
                FieldBinding{FieldDomain::Material, {}, properties[i]};
        }
        vector_row(ctx, cursor, "Emissive", rgb, values, 0.01F, bindings,
                   frame);
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
    const std::array<FieldBinding, 1U> opacity_binding{{
        {FieldDomain::Compositing, scene::PropertyChannel::Opacity}}};
    bool key_requested = false;
    vector_row(ctx, cursor, "Opacity", scalar_axis, opacity, 0.005F,
               opacity_binding, frame, &key_requested, opacity_animated,
               opacity_keyed);
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
    const auto append_unique = [](std::vector<doc::NodeId>& nodes,
                                  doc::NodeId node) {
        if (std::find(nodes.begin(), nodes.end(), node) == nodes.end()) {
            nodes.push_back(node);
        }
    };
    std::vector<const ElementInfo*> blend_targets;
    std::vector<doc::NodeId> blend_incompatible;
    bool blend_mixed = false;
    for (const ElementInfo& selected : selected_) {
        if (selected.locked) {
            append_unique(frame.skipped_locked, selected.node);
        } else if (is_light(selected.kind) ||
                   selected.kind == ElementKind::Group) {
            append_unique(blend_incompatible, selected.node);
        } else {
            if (!blend_targets.empty() &&
                blend_targets.front()->style.blend != selected.style.blend) {
                blend_mixed = true;
            }
            blend_targets.push_back(&selected);
        }
    }
    if (blend_mixed) {
        ++frame.mixed_fields;
    }
    ctx.label(gui::Rect{well.x + 6.0F, well.y,
                        std::max(well.width - 12.0F, 0.0F), well.height},
              blend_mixed ? "Mixed" : blend_name(element.style.blend),
              blend_mixed ? ctx.theme().text_muted : ctx.theme().text);
    if (interaction.clicked) {
        for (const doc::NodeId node : blend_incompatible) {
            append_unique(frame.skipped_incompatible, node);
        }
        const auto current = static_cast<core::u8>(element.style.blend);
        const auto next = static_cast<core::u8>((current + 1U) % 4U);
        for (const ElementInfo* const target : blend_targets) {
            frame.named_edits.push_back(NamedEdit{
                target->node, std::string{scene::schema::blend_mode},
                static_cast<double>(next)});
        }
    }
}

void InspectorPanel::draw_effects(
    gui::UiContext &ctx, Cursor &cursor, const ElementInfo &element,
    InspectorFrame &frame, std::span<const scene::ChannelKeyframes> keyframes,
    core::u64 show_frame, std::span<const scene::EffectInstance> effects) {
  if (!section(ctx, cursor, "EFFECTS", effects_open_)) {
    return;
  }

  const auto effect_row = [&](const scene::EffectInstance &effect,
                              std::size_t effect_ordinal) {
    std::string_view title;
    std::string_view value_label;
    scene::PropertyChannel component{};
    float speed{};
    double maximum{};
    bool structured_effect = false;
    bool spatial_effect = false;
    switch (effect.kind) {
    case scene::EffectKind::Brightness:
      title = "BRIGHTNESS";
      value_label = "Amount";
      component = scene::PropertyChannel::Brightness;
      speed = 0.01F;
      maximum = 16.0;
      break;
    case scene::EffectKind::Saturation:
      title = "SATURATION";
      value_label = "Amount";
      component = scene::PropertyChannel::Saturation;
      speed = 0.01F;
      maximum = scene::max_saturation;
      break;
    case scene::EffectKind::Blur:
      title = "GAUSSIAN BLUR";
      value_label = "Radius";
      component = scene::PropertyChannel::BlurRadius;
      speed = 0.1F;
      maximum = 64.0;
      break;
    case scene::EffectKind::BoxBlur:
      title = "BOX BLUR";
      spatial_effect = true;
      break;
    case scene::EffectKind::DirectionalBlur:
      title = "DIRECTIONAL BLUR";
      spatial_effect = true;
      break;
    case scene::EffectKind::ChromaKey:
      title = "CHROMA KEY";
      value_label = "Mix";
      component = scene::PropertyChannel::ChromaKeyMix;
      speed = 0.01F;
      maximum = 1.0;
      break;
    case scene::EffectKind::LumaKey:
      title = "LUMA KEY";
      value_label = "Mix";
      component = scene::PropertyChannel::LumaKeyMix;
      speed = 0.01F;
      maximum = 1.0;
      break;
    case scene::EffectKind::Levels:
      title = "LEVELS";
      structured_effect = true;
      break;
    case scene::EffectKind::LiftGammaGain:
      title = "LIFT / GAMMA / GAIN";
      structured_effect = true;
      break;
    case scene::EffectKind::ContrastPivot:
      title = "CONTRAST / PIVOT";
      structured_effect = true;
      break;
    case scene::EffectKind::ColorMatrix:
      title = "COLOUR MATRIX / CHANNEL MIXER";
      structured_effect = true;
      break;
    case scene::EffectKind::WhiteBalance:
      title = "WHITE BALANCE";
      structured_effect = true;
      break;
    case scene::EffectKind::Curves:
      title = "CURVES";
      structured_effect = true;
      break;
    case scene::EffectKind::FilmGrain:
      title = "FILM GRAIN";
      value_label = "Amount";
      component = scene::PropertyChannel::FilmGrainAmount;
      speed = 0.005F;
      maximum = scene::max_film_grain_amount;
      break;
    case scene::EffectKind::Count:
      return;
    }
    const std::size_t id_salt = static_cast<std::size_t>(effect.source.value());
    const gui::Rect title_row = take_row(cursor, 19.0F);
    cursor.y += 2.0F;
    ctx.panel(title_row, ctx.theme().header);
    ctx.panel(gui::Rect{title_row.x, title_row.y, 2.0F, title_row.height},
              ctx.theme().accent);
    constexpr float remove_width = 64.0F;
    ctx.label(gui::Rect{title_row.x + 8.0F, title_row.y,
                        std::max(title_row.width - remove_width - 12.0F, 0.0F),
                        title_row.height},
              title, ctx.theme().text);
    const gui::Rect remove{title_row.x + title_row.width - remove_width,
                           title_row.y, remove_width, title_row.height};
    const gui::Interaction remove_it =
        ctx.grab(mix(id_, title, 8'000U + id_salt), remove);
    ctx.panel(remove, remove_it.hovered ? ctx.theme().widget_hover
                                        : ctx.theme().widget);
    ctx.panel(gui::Rect{remove.x, remove.y, 2.0F, remove.height},
              ctx.theme().warning);
    ctx.label(gui::Rect{remove.x + 8.0F, remove.y,
                        std::max(remove.width - 8.0F, 0.0F), remove.height},
              "REMOVE",
              remove_it.hovered ? ctx.theme().warning : ctx.theme().text);
    if (remove_it.clicked) {
      frame.effect_deletes.push_back(effect.source);
    }

    if (structured_effect) {
      const gui::Rect note = take_row(cursor, 34.0F);
      cursor.y += row_gap;
      ctx.panel(note, ctx.theme().track);
      ctx.label(gui::Rect{note.x + 7.0F, note.y,
                          std::max(note.width - 14.0F, 0.0F), note.height},
                "Edit structured controls in COLOR", ctx.theme().text_muted);
      return;
    }

    if (spatial_effect) {
      constexpr std::array<std::string_view, 1U> scalar_axis{{""}};
      if (effect.kind == scene::EffectKind::BoxBlur) {
        const scene::ChannelKeyframes *const channel = channel_for(
            keyframes, effect.source, scene::PropertyChannel::BoxBlurRadius);
        const bool animated = channel != nullptr && !channel->frames.empty();
        const bool keyed_here = keyed_at(channel, show_frame);
        const std::optional<double> sampled = sample_at(channel, show_frame);
        const float displayed_radius = static_cast<float>(std::clamp(
            sampled.has_value() && std::isfinite(*sampled)
                ? std::round(*sampled)
                : static_cast<double>(effect.box_blur.radius_px),
            0.0, static_cast<double>(scene::max_box_blur_radius_px)));
        const std::array<float, 1U> radius{{displayed_radius}};
        const std::array<FieldBinding, 1U> radius_binding{
            {{FieldDomain::Effect,
              scene::PropertyChannel::BoxBlurRadius,
              scene::schema::box_blur_radius,
              effect.kind,
              effect_ordinal}}};
        bool key_requested = false;
        vector_row(ctx, cursor, "Radius px", scalar_axis, radius, 1.0F,
                   radius_binding, frame, &key_requested, animated,
                   keyed_here, id_salt);
        if (key_requested) {
          frame.key_edits.push_back(InspectorKeyEdit{
              effect.source, scene::PropertyChannel::BoxBlurRadius,
              displayed_radius, keyed_here});
        }
        const std::array<float, 1U> passes{
            {static_cast<float>(effect.box_blur.passes)}};
        const std::array<FieldBinding, 1U> passes_binding{
            {{FieldDomain::Effect,
              {},
              scene::schema::box_blur_passes,
              effect.kind,
              effect_ordinal}}};
        vector_row(ctx, cursor, "Passes", scalar_axis, passes, 1.0F,
                   passes_binding, frame, nullptr, false, false, id_salt);
      } else {
        const scene::ChannelKeyframes *const channel = channel_for(
            keyframes, effect.source,
            scene::PropertyChannel::DirectionalBlurLength);
        const bool animated = channel != nullptr && !channel->frames.empty();
        const bool keyed_here = keyed_at(channel, show_frame);
        const std::optional<double> sampled = sample_at(channel, show_frame);
        const float displayed_length = static_cast<float>(std::clamp(
            sampled.has_value() && std::isfinite(*sampled)
                ? *sampled
                : static_cast<double>(effect.directional_blur.length_px),
            0.0,
            static_cast<double>(scene::max_directional_blur_length_px)));
        const std::array<float, 1U> length{{displayed_length}};
        const std::array<FieldBinding, 1U> length_binding{
            {{FieldDomain::Effect,
              scene::PropertyChannel::DirectionalBlurLength,
              scene::schema::directional_blur_length,
              effect.kind,
              effect_ordinal}}};
        bool key_requested = false;
        vector_row(ctx, cursor, "Length px", scalar_axis, length, 0.1F,
                   length_binding, frame, &key_requested, animated,
                   keyed_here, id_salt);
        if (key_requested) {
          frame.key_edits.push_back(InspectorKeyEdit{
              effect.source, scene::PropertyChannel::DirectionalBlurLength,
              displayed_length, keyed_here});
        }
        const std::array<float, 1U> angle{
            {effect.directional_blur.angle_turns}};
        const std::array<FieldBinding, 1U> angle_binding{
            {{FieldDomain::Effect,
              {},
              scene::schema::directional_blur_angle_turns,
              effect.kind,
              effect_ordinal}}};
        vector_row(ctx, cursor, "Angle turns", scalar_axis, angle, 0.001F,
                   angle_binding, frame, nullptr, false, false, id_salt);
      }
      cursor.y += 2.0F;
      return;
    }

    const scene::ChannelKeyframes *const channel =
        channel_for(keyframes, effect.source, component);
    const bool animated = channel != nullptr && !channel->frames.empty();
    const bool keyed_here = keyed_at(channel, show_frame);
    const std::optional<double> sampled = sample_at(channel, show_frame);
    double authored = static_cast<double>(effect.value);
    if (effect.kind == scene::EffectKind::FilmGrain) {
      authored = static_cast<double>(effect.film_grain.amount);
    } else if (effect.kind == scene::EffectKind::ChromaKey) {
      authored = static_cast<double>(effect.chroma_key.mix);
    } else if (effect.kind == scene::EffectKind::LumaKey) {
      authored = static_cast<double>(effect.luma_key.mix);
    }
    const float displayed_value = static_cast<float>(std::clamp(
        sampled.has_value() && std::isfinite(*sampled) ? *sampled : authored,
        0.0, maximum));
    constexpr std::array<std::string_view, 1U> scalar_axis{{""}};
    const std::array<float, 1U> value{displayed_value};
    const std::array<FieldBinding, 1U> binding{
        {{FieldDomain::Effect, component, {}, effect.kind, effect_ordinal}}};
    bool key_requested = false;
    vector_row(ctx, cursor, value_label, scalar_axis, value, speed, binding,
               frame, &key_requested, animated, keyed_here, id_salt);
    if (key_requested) {
      frame.key_edits.push_back(InspectorKeyEdit{effect.source, component,
                                                 displayed_value, keyed_here});
    }
    if (effect.kind == scene::EffectKind::FilmGrain) {
      const gui::Rect note = take_row(cursor, 22.0F);
      ctx.label(note, "Seed, scale, chroma and mode: TEXTURE",
                ctx.theme().text_muted);
    }
    cursor.y += 2.0F;
  };

  const bool any = std::any_of(
      effects.begin(), effects.end(), [&element](const auto &effect) {
        return effect.target == element.node && effect.enabled;
      });
  if (!any) {
    const gui::Rect empty = take_row(cursor, 34.0F);
    cursor.y += row_gap;
    ctx.panel(empty, ctx.theme().track);
    ctx.label(gui::Rect{empty.x + 7.0F, empty.y,
                        std::max(empty.width - 14.0F, 0.0F), empty.height},
              "No effects - drag one from Effects", ctx.theme().text_muted);
    return;
  }

  std::array<std::size_t, static_cast<std::size_t>(scene::EffectKind::Count)>
      ordinals{};
  for (const scene::EffectInstance &effect : effects) {
    if (effect.target == element.node && effect.enabled) {
      const std::size_t kind = static_cast<std::size_t>(effect.kind);
      if (kind >= ordinals.size()) {
        continue;
      }
      effect_row(effect, ordinals[kind]++);
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
    const std::span<const ElementInfo> selected = element == nullptr
        ? std::span<const ElementInfo>{}
        : std::span<const ElementInfo>{element, 1U};
    return draw_selection(ctx, bounds, selected,
                          element == nullptr ? doc::NodeId{} : element->node,
                          element == nullptr ? 0U : also_selected + 1U,
                          keyframes, show_frame, effects);
}

InspectorFrame InspectorPanel::draw(
    gui::UiContext& ctx, const gui::Rect& bounds,
    std::span<const ElementInfo> selected, doc::NodeId primary,
    std::span<const scene::ChannelKeyframes> keyframes, core::u64 show_frame,
    std::span<const scene::EffectInstance> effects) {
    return draw_selection(ctx, bounds, selected, primary, selected.size(),
                          keyframes, show_frame, effects);
}

void InspectorPanel::draw_skip_summary(gui::UiContext& ctx, Cursor& cursor,
                                       const InspectorFrame& frame) {
    const auto draw_nodes = [&ctx, &cursor](std::string_view prefix,
                                            const auto& nodes) {
        if (nodes.empty()) {
            return;
        }
        std::string text{prefix};
        for (const doc::NodeId node : nodes) {
            text += text.size() == prefix.size() ? " " : ", ";
            text += std::to_string(node.value());
        }
        const gui::Rect row = take_row(cursor, 18.0F);
        cursor.y += 2.0F;
        ctx.label(row, text, ctx.theme().warning);
    };
    draw_nodes("Locked skipped:", frame.skipped_locked);
    draw_nodes("Incompatible skipped:", frame.skipped_incompatible);
}

InspectorFrame InspectorPanel::draw_selection(
    gui::UiContext& ctx, const gui::Rect& bounds,
    std::span<const ElementInfo> selected, doc::NodeId primary,
    std::size_t declared_count,
    std::span<const scene::ChannelKeyframes> keyframes, core::u64 show_frame,
    std::span<const scene::EffectInstance> effects) {
    InspectorFrame frame{};

    ctx.panel(bounds, ctx.theme().panel);
    if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
        return frame;
    }

    std::vector<doc::NodeId> signature;
    signature.reserve(selected.size());
    for (const ElementInfo& element : selected) {
        signature.push_back(element.node);
    }
    // A selection change under a live edit ends it, rather than committing a
    // typed value or continuing a scrub against a different set of nodes that
    // happens to expose the same field id.
    if (signature != selection_signature_ || primary != primary_signature_) {
        fields_.cancel(ctx);
        drag_capture_ = {};
        selection_signature_ = signature;
        primary_signature_ = primary;
    }

    if (selected.empty()) {
        fields_.cancel(ctx);
        drag_capture_ = {};
        selected_.clear();
        keyframes_ = {};
        effects_ = {};
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
    selected_.clear();
    selected_.reserve(selected.size());
    for (const ElementInfo& element : selected) {
        selected_.push_back(
            evaluated_for_inspector(element, keyframes, show_frame));
        if (element.locked) {
            frame.skipped_locked.push_back(element.node);
        }
    }
    keyframes_ = keyframes;
    effects_ = effects;
    show_frame_ = show_frame;

    const auto editable_primary = std::find_if(
        selected_.begin(), selected_.end(), [&](const ElementInfo& element) {
            return element.node == primary && !element.locked;
        });
    const auto fallback = std::find_if(
        selected_.begin(), selected_.end(),
        [](const ElementInfo& element) { return !element.locked; });
    const ElementInfo* const displayed = editable_primary != selected_.end()
        ? &*editable_primary
        : (fallback != selected_.end() ? &*fallback : nullptr);

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

    const ElementInfo& header_element =
        displayed != nullptr ? *displayed : selected_.front();
    draw_header(ctx, cursor, header_element,
                std::max(declared_count, selected_.size()));
    if (displayed == nullptr) {
        const gui::Rect locked = take_row(cursor, 34.0F);
        cursor.y += row_gap;
        ctx.panel(locked, ctx.theme().track);
        ctx.label(locked, "Selection is locked", ctx.theme().warning);
        draw_skip_summary(ctx, cursor, frame);
        cursor.y += panel_inset;
        content_height_ = cursor.y + scroll_ - bounds.y;
        ctx.pop_clip();
        fields_.cancel(ctx);
        drag_capture_ = {};
        keyframes_ = {};
        effects_ = {};
        return frame;
    }

    if (is_light(displayed->kind)) {
        draw_light(ctx, cursor, *displayed, frame);
    } else if (displayed->on_screen()) {
        // A clip on a track is playing on that track's screen, so the screen
        // is what has a place in the scene. Showing the clip's own transform
        // here would offer nine fields that move nothing. It is still on the
        // clip's node and still what the clip returns to when it is dragged
        // out of the track, which is why nothing is erased to say this.
        const gui::Rect note = take_row(cursor, 34.0F);
        cursor.y += row_gap;
        ctx.panel(note, ctx.theme().track);
        std::array<char, 96U> line{};
        const int length = displayed->screen_name.empty()
            ? std::snprintf(line.data(), line.size(), "Playing on a screen")
            : std::snprintf(line.data(), line.size(), "Playing on %s",
                            displayed->screen_name.c_str());
        ctx.label(note,
                  std::string_view{line.data(),
                                   static_cast<std::size_t>(std::clamp(
                                       length, 0,
                                       static_cast<int>(line.size() - 1U)))},
                  ctx.theme().text_muted);
    } else {
        draw_transform(ctx, cursor, *displayed, frame, keyframes, show_frame);
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

    if (!is_light(displayed->kind) &&
        displayed->kind != ElementKind::Group) {
        cursor.y += row_gap;
        draw_content(ctx, cursor, *displayed, frame);
        draw_screen(ctx, cursor, *displayed, frame);
        draw_material(ctx, cursor, *displayed, frame);
        draw_compositing(ctx, cursor, *displayed, frame, keyframes, show_frame);
        draw_effects(ctx, cursor, *displayed, frame, keyframes, show_frame,
                     effects);
    }

    draw_skip_summary(ctx, cursor, frame);
    cursor.y += panel_inset;

    content_height_ = cursor.y + scroll_ - bounds.y;
    ctx.pop_clip();

    frame.editing = fields_.live();
    if (!frame.editing) {
        drag_capture_ = {};
    }
    keyframes_ = {};
    effects_ = {};
    return frame;
}

} // namespace mediaserver::editor
