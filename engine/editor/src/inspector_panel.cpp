#include <mediaserver/editor/inspector_panel.hpp>

#include <mediaserver/gui/color_picker.hpp>

#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <span>
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

// The name a protocol goes by in the Inspector, spelled the way the tracker
// vendors spell it.
[[nodiscard]] std::string_view tracker_protocol_name(
    scene::TrackerProtocolKind protocol) noexcept {
    return protocol == scene::TrackerProtocolKind::OpenTrackIO
        ? std::string_view{"OpenTrackIO"}
        : std::string_view{"FreeD"};
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
                                bool keyed_here, std::size_t id_salt, int precision) {
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
                         static_cast<double>(speed), out, precision, &edit,
                         summary.mixed)) {
            emit_numeric(bindings[i], field, edit, std::move(summary), frame);
        }
    }
}

std::optional<InspectorPanel::EditableValue>
InspectorPanel::editable_value(const ElementInfo& element,
                               const FieldBinding& binding) const {
    switch (binding.domain) {
    case FieldDomain::Animation:
        if (element.kind != ElementKind::Model) return std::nullopt;
        if (binding.property == scene::schema::animation_clip) return EditableValue{element.node,element.node,static_cast<double>(element.animation.clip)};
        if (binding.property == scene::schema::wind_period_frames) return EditableValue{element.node,element.node,static_cast<double>(element.animation.wind_period_frames)};
        if (binding.property == scene::schema::wind_x) return EditableValue{element.node,element.node,element.animation.wind.x};
        if (binding.property == scene::schema::wind_y) return EditableValue{element.node,element.node,element.animation.wind.y};
        if (binding.property == scene::schema::wind_z) return EditableValue{element.node,element.node,element.animation.wind.z};
        return std::nullopt;
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
    case FieldDomain::Camera:
        if (element.kind != ElementKind::Camera) {
            return std::nullopt;
        }
        if (binding.property == scene::schema::focus_distance_m)
            return EditableValue{element.node, element.node, element.focus_distance_m};
        if (binding.property == scene::schema::aperture_f_number)
            return EditableValue{element.node, element.node, element.aperture_f_number};
        if (binding.property == scene::schema::shutter_seconds)
            return EditableValue{element.node, element.node, element.shutter_seconds};
        if (binding.property == scene::schema::eye_x) {
            return EditableValue{element.node, element.node,
                                 element.position.x};
        }
        if (binding.property == scene::schema::eye_y) {
            return EditableValue{element.node, element.node,
                                 element.position.y};
        }
        if (binding.property == scene::schema::eye_z) {
            return EditableValue{element.node, element.node,
                                 element.position.z};
        }
        if (binding.property == scene::schema::target_x) {
            return EditableValue{element.node, element.node, element.target.x};
        }
        if (binding.property == scene::schema::target_y) {
            return EditableValue{element.node, element.node, element.target.y};
        }
        if (binding.property == scene::schema::target_z) {
            return EditableValue{element.node, element.node, element.target.z};
        }
        if (binding.property == scene::schema::fov_turns) {
            return EditableValue{element.node, element.node,
                                 element.field_of_view};
        }
        return std::nullopt;
    case FieldDomain::Tracking:
        // The value shown is the feed's, and so is the node the edit is
        // addressed to: `feed_` is the feed this camera is bound to, set while
        // its rows are being drawn. The element is still the camera, which is
        // what makes a locked camera's feed read-only too.
        if (element.kind != ElementKind::Camera || feed_.value() == 0U) {
            return std::nullopt;
        }
        for (const scene::TrackingSource& source : sources_) {
            if (source.node != feed_) {
                continue;
            }
            if (binding.property == scene::schema::tracking_port) {
                return EditableValue{
                    element.node, feed_,
                    static_cast<double>(source.settings.port)};
            }
            if (binding.property == scene::schema::tracking_camera_id) {
                return EditableValue{
                    element.node, feed_,
                    static_cast<double>(source.settings.camera_id)};
            }
            if (binding.property
                == scene::schema::tracking_sensor_height_mm) {
                return EditableValue{element.node, feed_,
                                     source.settings.sensor_height_mm};
            }
            if (binding.property == scene::schema::tracking_sensor_aspect) {
                return EditableValue{element.node, feed_,
                                     source.settings.sensor_aspect_ratio};
            }
            if (binding.property
                == scene::schema::tracking_fallback_focal_mm) {
                return EditableValue{
                    element.node, feed_,
                    source.settings.fallback_focal_length_mm};
            }
        }
        return std::nullopt;
    case FieldDomain::Plate:
        if (element.kind != ElementKind::Camera) {
            return std::nullopt;
        }
        if (binding.property == scene::schema::camera_inner_frustum_margin_px) {
            return EditableValue{
                element.node, element.node,
                static_cast<double>(element.icvfx.margin_px)};
        }
        if (binding.property
            == scene::schema::camera_inner_frustum_soft_edge_px) {
            return EditableValue{
                element.node, element.node,
                static_cast<double>(element.icvfx.soft_edge_px)};
        }
        if (binding.property
            == scene::schema::camera_inner_frustum_scale_numerator) {
            return EditableValue{
                element.node, element.node,
                static_cast<double>(element.icvfx.resolution_numerator)};
        }
        if (binding.property
            == scene::schema::camera_inner_frustum_scale_denominator) {
            return EditableValue{
                element.node, element.node,
                static_cast<double>(element.icvfx.resolution_denominator)};
        }
        if (binding.property == scene::schema::camera_render_order) {
            return EditableValue{
                element.node, element.node,
                static_cast<double>(element.icvfx.render_order)};
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
        if (binding.property == scene::schema::ies_rotation_turns)
            return element.kind == ElementKind::PointLight
                ? std::optional<EditableValue>{EditableValue{element.node, element.node, element.ies_rotation}} : std::nullopt;
        if (binding.property == scene::schema::angular_radius_turns)
            return EditableValue{element.node, element.node, element.angular_radius};
        if (binding.property == scene::schema::color_temperature_kelvin)
            return EditableValue{element.node, element.node, element.color_temperature_kelvin};
        if (element.kind == ElementKind::PointLight) {
            if (binding.property == scene::schema::light_width) return EditableValue{element.node, element.node, element.light_width};
            if (binding.property == scene::schema::light_height) return EditableValue{element.node, element.node, element.light_height};
            if (binding.property == scene::schema::light_radius) return EditableValue{element.node, element.node, element.light_radius};
            if (binding.property == scene::schema::area_rotation_turns) return EditableValue{element.node, element.node, element.area_rotation};
            if (binding.property == scene::schema::inner_cone_turns)
                return EditableValue{element.node, element.node, element.inner_cone};
            if (binding.property == scene::schema::outer_cone_turns)
                return EditableValue{element.node, element.node, element.outer_cone};
            if (binding.property == scene::schema::direction_x)
                return EditableValue{element.node, element.node, element.direction.x};
            if (binding.property == scene::schema::direction_y)
                return EditableValue{element.node, element.node, element.direction.y};
            if (binding.property == scene::schema::direction_z)
                return EditableValue{element.node, element.node, element.direction.z};
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
        if (binding.property == scene::schema::base_alpha) return EditableValue{element.node, element.node, element.material.base_alpha};
        if (binding.property == scene::schema::clearcoat) return EditableValue{element.node, element.node, element.material.clearcoat};
        if (binding.property == scene::schema::clearcoat_roughness) return EditableValue{element.node, element.node, element.material.clearcoat_roughness};
        if (binding.property == scene::schema::clearcoat_normal_scale) return EditableValue{element.node, element.node, element.material.clearcoat_normal_scale};
        if (binding.property == scene::schema::sheen_roughness) return EditableValue{element.node, element.node, element.material.sheen_roughness};
        if (binding.property == scene::schema::transmission) return EditableValue{element.node, element.node, element.material.transmission};
        if (binding.property == scene::schema::ior) return EditableValue{element.node, element.node, element.material.ior};
        if (binding.property == scene::schema::specular) return EditableValue{element.node, element.node, element.material.specular};
        if (binding.property == scene::schema::anisotropy_strength) return EditableValue{element.node, element.node, element.material.anisotropy_strength};
        if (binding.property == scene::schema::anisotropy_rotation) return EditableValue{element.node, element.node, element.material.anisotropy_rotation};
        if (binding.property == scene::schema::sheen_color_r) return EditableValue{element.node, element.node, element.material.sheen_color.x};
        if (binding.property == scene::schema::sheen_color_g) return EditableValue{element.node, element.node, element.material.sheen_color.y};
        if (binding.property == scene::schema::sheen_color_b) return EditableValue{element.node, element.node, element.material.sheen_color.z};
        if (binding.property == scene::schema::specular_color_r) return EditableValue{element.node, element.node, element.material.specular_color.x};
        if (binding.property == scene::schema::specular_color_g) return EditableValue{element.node, element.node, element.material.specular_color.y};
        if (binding.property == scene::schema::specular_color_b) return EditableValue{element.node, element.node, element.material.specular_color.z};
        return std::nullopt;
    case FieldDomain::Effect: {
        if (is_light(element.kind)) {
            return std::nullopt;
        }
        std::size_t ordinal = 0U;
        for (const scene::EffectInstance &effect : effects_) {
          // Disabled effects are still listed and still editable, so they still
          // count here. Skipping them would shift every later ordinal of the
          // same kind and route a field's edit to the wrong effect.
          if (effect.target != element.node ||
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
            if (binding.property.empty()) {
              authored = static_cast<double>(effect.chroma_key.mix);
            } else if (binding.property ==
                       scene::schema::chroma_key_color_r) {
              authored = static_cast<double>(effect.chroma_key.key_color.x);
            } else if (binding.property ==
                       scene::schema::chroma_key_color_g) {
              authored = static_cast<double>(effect.chroma_key.key_color.y);
            } else if (binding.property ==
                       scene::schema::chroma_key_color_b) {
              authored = static_cast<double>(effect.chroma_key.key_color.z);
            } else if (binding.property ==
                       scene::schema::chroma_key_tolerance) {
              authored = static_cast<double>(effect.chroma_key.tolerance);
            } else if (binding.property ==
                       scene::schema::chroma_key_softness) {
              authored = static_cast<double>(effect.chroma_key.softness);
            } else if (binding.property ==
                       scene::schema::chroma_key_morphology_px) {
              authored = static_cast<double>(effect.chroma_key.morphology_px);
            } else if (binding.property ==
                       scene::schema::chroma_key_spill_suppression) {
              authored =
                  static_cast<double>(effect.chroma_key.spill_suppression);
            } else {
              return std::nullopt;
            }
          } else if (binding.effect_kind == scene::EffectKind::LumaKey) {
            if (binding.property.empty()) {
              authored = static_cast<double>(effect.luma_key.mix);
            } else if (binding.property ==
                       scene::schema::luma_key_low_threshold) {
              authored = static_cast<double>(effect.luma_key.low_threshold);
            } else if (binding.property ==
                       scene::schema::luma_key_high_threshold) {
              authored = static_cast<double>(effect.luma_key.high_threshold);
            } else if (binding.property ==
                       scene::schema::luma_key_low_softness) {
              authored = static_cast<double>(effect.luma_key.low_softness);
            } else if (binding.property ==
                       scene::schema::luma_key_high_softness) {
              authored = static_cast<double>(effect.luma_key.high_softness);
            } else {
              return std::nullopt;
            }
          }
          const double raw = sampled.has_value() && std::isfinite(*sampled)
                                 ? *sampled
                                 : authored;
          double minimum = 0.0;
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
            if (binding.property ==
                scene::schema::chroma_key_morphology_px) {
              minimum = -static_cast<double>(
                  scene::max_key_morphology_radius_px);
              maximum = static_cast<double>(
                  scene::max_key_morphology_radius_px);
            }
          }
          return EditableValue{element.node, effect.source,
                               std::clamp(raw, minimum, maximum)};
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
      case FieldDomain::Animation:
        if (binding.property==scene::schema::animation_clip) return std::clamp(std::round(value),0.0,63.0);
        if (binding.property==scene::schema::wind_period_frames) return std::clamp(std::round(value),2.0,1000000.0);
        return std::clamp(value,-100.0,100.0);
      case FieldDomain::Transform:
        return value;
      case FieldDomain::Compositing:
        return std::clamp(value, 0.0, 1.0);
      case FieldDomain::Light:
        if (binding.property == scene::schema::light_width || binding.property == scene::schema::light_height || binding.property == scene::schema::light_radius) return std::clamp(value, 0.001, 10000.0);
        if (binding.property == scene::schema::area_rotation_turns || binding.property == scene::schema::ies_rotation_turns) return std::clamp(value, -1.0, 1.0);
        if (binding.property == scene::schema::angular_radius_turns)
          return std::clamp(value, 0.0, 0.249);
        if (binding.property == scene::schema::color_temperature_kelvin)
          return std::clamp(value, 1000.0, 40000.0);
        if (binding.property == scene::schema::inner_cone_turns)
          return std::clamp(value, 0.0, 0.249);
        if (binding.property == scene::schema::outer_cone_turns)
          return std::clamp(value, 0.001, 0.249);
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
      case FieldDomain::Camera:
        if (binding.property == scene::schema::focus_distance_m) return std::clamp(value, 0.0, 100000.0);
        if (binding.property == scene::schema::aperture_f_number) return std::clamp(value, 0.1, 128.0);
        if (binding.property == scene::schema::shutter_seconds)
          return std::clamp(value, 0.00001, 1.0);
        if (binding.property == scene::schema::fov_turns) {
          return std::clamp(value, 0.001, 0.499);
        }
        return value;
      case FieldDomain::Tracking:
        // A port and a camera id are whole numbers, and the ranges are the
        // protocol's own: a UDP port, and a FreeD camera byte.
        if (binding.property == scene::schema::tracking_port) {
          return std::clamp(std::round(value), 0.0, 65535.0);
        }
        if (binding.property == scene::schema::tracking_camera_id) {
          return std::clamp(std::round(value), 0.0, 255.0);
        }
        return std::max(value, 0.0);
      case FieldDomain::Plate:
        // Every one of these is whole pixels or a whole ordinal. Rounding here
        // rather than at the document boundary means the field itself never
        // shows a margin of 23.6 pixels.
        if (binding.property == scene::schema::camera_render_order) {
          return std::round(value);
        }
        if (binding.property
                == scene::schema::camera_inner_frustum_scale_numerator ||
            binding.property
                == scene::schema::camera_inner_frustum_scale_denominator) {
          return std::clamp(
              std::round(value), 1.0,
              static_cast<double>(scene::max_icvfx_resolution_scale));
        }
        if (binding.property
            == scene::schema::camera_inner_frustum_soft_edge_px) {
          return std::clamp(
              std::round(value), 0.0,
              static_cast<double>(scene::max_icvfx_soft_edge_px));
        }
        return std::clamp(std::round(value), 0.0,
                          static_cast<double>(scene::max_icvfx_margin_px));
      case FieldDomain::Material:
        if (binding.property == scene::schema::base_alpha) return std::clamp(value, 0.0, 1.0);
        if (binding.property == scene::schema::clearcoat) return std::clamp(value, 0.0, 1.0);
        if (binding.property == scene::schema::clearcoat_roughness) return std::clamp(value, 0.0, 1.0);
        if (binding.property == scene::schema::clearcoat_normal_scale) return std::clamp(value, 0.0, 4.0);
        if (binding.property == scene::schema::sheen_roughness) return std::clamp(value, 0.0, 1.0);
        if (binding.property == scene::schema::transmission) return std::clamp(value, 0.0, 1.0);
        if (binding.property == scene::schema::ior) return std::clamp(value, 1.0, 3.0);
        if (binding.property == scene::schema::specular) return std::clamp(value, 0.0, 1.0);
        if (binding.property == scene::schema::anisotropy_strength) return std::clamp(value, 0.0, 1.0);
        if (binding.property == scene::schema::anisotropy_rotation) return std::clamp(value, -6.283185307, 6.283185307);
        if (binding.property == scene::schema::sheen_color_r) return std::clamp(value, 0.0, 1.0);
        if (binding.property == scene::schema::sheen_color_g) return std::clamp(value, 0.0, 1.0);
        if (binding.property == scene::schema::sheen_color_b) return std::clamp(value, 0.0, 1.0);
        if (binding.property == scene::schema::specular_color_r) return std::clamp(value, 0.0, 1.0);
        if (binding.property == scene::schema::specular_color_g) return std::clamp(value, 0.0, 1.0);
        if (binding.property == scene::schema::specular_color_b) return std::clamp(value, 0.0, 1.0);
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
          if (binding.property ==
              scene::schema::chroma_key_morphology_px) {
            return std::clamp(
                std::round(value),
                -static_cast<double>(scene::max_key_morphology_radius_px),
                static_cast<double>(scene::max_key_morphology_radius_px));
          }
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
      case FieldDomain::Animation:
      case FieldDomain::Screen:
      case FieldDomain::Tracking:
      case FieldDomain::Camera:
      case FieldDomain::Plate:
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
                        static_cast<int>(kind_name(element).size()),
                        kind_name(element).data(),
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

void InspectorPanel::draw_linked(gui::UiContext& ctx, Cursor& cursor,
                                 const ElementInfo& element, InspectorFrame& frame) {
    ctx.label(take_row(cursor, row_height), "BLENDER SCENE LINK", ctx.theme().text_muted);
    if (element.linked_batch_instances) {
        ctx.label(take_row(cursor,row_height),std::to_string(element.linked_batch_instances)+" shared instances",ctx.theme().text);
        ctx.label(take_row(cursor,row_height),"Placement and visibility edit the scatter group",ctx.theme().text_muted);
    }
    if (!element.linked_collection) ctx.label(take_row(cursor, row_height), element.linked_held
        ? "Hold shot placement (world)" : "Follow source with offset (local)", ctx.theme().text);
    const auto action = [&](std::string_view name, std::string_view label) {
        if (ctx.button(mix(id_, name, element.node.value()), take_row(cursor, row_height), label))
            frame.text_edits.push_back({element.node, "link_action", std::string{name}});
        cursor.y += row_gap;
    };
    if (!element.linked_collection) action(element.linked_held ? "follow" : "hold", element.linked_held
        ? "Follow source with offset" : "Hold shot placement");
    const auto vector = [&](std::string_view label, scene::Vec3 value, std::string_view prefix) {
        ctx.label(take_row(cursor, row_height), label, ctx.theme().text_muted);
        std::array<gui::Rect, 3> boxes;
        columns(take_row(cursor, row_height), 3, 4, boxes);
        const std::array<float, 3> values{value.x, value.y, value.z};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const std::string key = std::string{prefix} + std::to_string(axis);
            double changed = values[axis];
            if (fields_.draw(ctx, mix(id_, key, element.node.value()), boxes[axis], xyz[axis],
                    values[axis], 0.01, changed))
                frame.named_edits.push_back({element.node, key, changed});
        }
        cursor.y += row_gap;
    };
    if (!element.linked_collection) {
        vector("Shot position (world meters)", element.linked_world, "link_world_");
        if (element.linked_placement_override) action("reset", "Reset placement to source");
        const bool shot_visible = element.linked_visibility_override ? element.visible : element.effective_visible;
        action(shot_visible ? "hide" : "show", shot_visible ? "Hide this occurrence" : "Show this occurrence");
        if (element.linked_visibility_override) action("visibility", "Follow source visibility");
        if (element.linked_material_overrides) action("material", is_light(element.kind) ? "Reset light properties to source" : "Reset material factors to source");
        std::string overrides = "Overrides:";
        if (element.linked_placement_override) overrides += " placement";
        if (element.linked_visibility_override) overrides += " visibility";
        if (element.linked_material_overrides) overrides += is_light(element.kind) ? " light" : " material";
        if (overrides == "Overrides:") overrides += " none";
        ctx.label(take_row(cursor,row_height),overrides,ctx.theme().text_muted);
    } else {
        vector("Link stage alignment (meters)", element.linked_alignment, "link_stage_");
    }
    ctx.label(take_row(cursor,row_height), element.linked_paused ? "PAUSED - accepted content retained"
        : element.linked_pending ? "UPDATING - accepted content retained"
        : element.linked_failed ? "UPDATE REFUSED - accepted content retained"
        : element.linked_connected ? "SYNCING WITH BLENDER - edits go both ways"
        : "BLENDER CLOSED OR ON ANOTHER SCENE - saved content shown",
        element.linked_failed ? ctx.theme().warning : ctx.theme().text_muted);
    if (!element.linked_status.empty()) {
        // Wrap worker diagnostics within the Inspector instead of clipping the
        // reason off the edge of a narrow dock.
        auto remaining = element.linked_status;
        const auto width = std::max<std::size_t>(20, static_cast<std::size_t>(std::max(cursor.column.width,120.0F) / 7.0F));
        while (!remaining.empty()) {
            auto count = std::min(width,remaining.size());
            if (count < remaining.size()) { const auto space = remaining.rfind(' ',count); if (space != std::string::npos && space > 0) count = space; }
            ctx.label(take_row(cursor,row_height),remaining.substr(0,count),ctx.theme().text_muted);
            remaining.erase(0,count); if (!remaining.empty() && remaining.front() == ' ') remaining.erase(0,1);
        }
    }
    if (!element.linked_revision.empty()) ctx.label(take_row(cursor,row_height),
        "Accepted revision " + element.linked_revision.substr(0,12),ctx.theme().text_muted);
    action(element.linked_paused ? "resume" : "pause", element.linked_paused ? "Resume source updates" : "Pause source updates");
    action("details", "Blender connection...");
    if (element.linked_collection) {
        action("duplicate", "Duplicate with shared source");
        action("copy", "Copy independent snapshot");
        action("unlink", "Unlink collection - keep accepted scene");
        ctx.label(take_row(cursor,row_height),"Unlink keeps assets, placement and appearance.",ctx.theme().text_muted);
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
        {"Rotation (turns)",
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

    cursor.y += 16.0F + row_gap;

}

void InspectorPanel::draw_ies(gui::UiContext& ctx, Cursor& cursor,
                              const ElementInfo& element, InspectorFrame& frame) {
    if (element.local_kind == scene::LocalLightKind::Rectangle || element.local_kind == scene::LocalLightKind::Sphere) return;
    if (ies_node_ != element.node || ies_authored_ != element.ies_source) {
        ies_node_ = element.node;
        ies_authored_ = element.ies_source;
        ies_draft_ = element.ies_source;
    }
    const auto label = take_row(cursor, 20.0F);
    ctx.label(label, "IES profile (.ies)", ctx.theme().text_muted);
    const auto field = take_row(cursor, 24.0F);
    static_cast<void>(ctx.text_field(mix(id_, "ies.path", element.node.value()), field, ies_draft_));
    cursor.y += row_gap;
    const auto actions = take_row(cursor, 24.0F);
    const float half = (actions.width - 6.0F) * 0.5F;
    const bool apply = ctx.button(mix(id_, "ies.apply", element.node.value()), {actions.x, actions.y, half, actions.height}, "APPLY PROFILE");
    const bool clear = ctx.button(mix(id_, "ies.clear", element.node.value()), {actions.x + half + 6.0F, actions.y, half, actions.height}, "CLEAR PROFILE");
    if (clear || (apply && ies_draft_.size() <= 1024U)) {
        for (const auto& selected : selected_) {
            if (selected.locked || selected.kind != ElementKind::PointLight
                || selected.local_kind == scene::LocalLightKind::Rectangle || selected.local_kind == scene::LocalLightKind::Sphere) continue;
            frame.text_edits.push_back({selected.node, std::string{scene::schema::ies_source}, clear ? std::string{} : ies_draft_});
        }
    }
    if (ies_draft_.size() > 1024U)
        ctx.label(take_row(cursor, 24.0F), "IES path exceeds 1024 bytes", ctx.theme().warning);
    if (!element.ies_source.empty()) {
        const std::array<float, 1U> values{element.ies_rotation};
        constexpr std::array<std::string_view, 1U> axes{{"turns"}};
        const std::array<FieldBinding, 1U> bindings{{{FieldDomain::Light, {}, scene::schema::ies_rotation_turns}}};
        vector_row(ctx, cursor, "IES rotation", axes, values, 0.002F, bindings, frame);
        if (element.local_kind == scene::LocalLightKind::Point) {
            const std::array<float, 3U> direction{element.direction.x, element.direction.y, element.direction.z};
            const std::array<FieldBinding, 3U> aim{{
                {FieldDomain::Light, {}, scene::schema::direction_x},
                {FieldDomain::Light, {}, scene::schema::direction_y},
                {FieldDomain::Light, {}, scene::schema::direction_z}}};
            vector_row(ctx, cursor, "IES aim", xyz, direction, 0.01F, aim, frame);
        }
    }
    cursor.y += row_gap;
}

void InspectorPanel::draw_light(gui::UiContext& ctx, Cursor& cursor,
                                const ElementInfo& element,
                                InspectorFrame& frame) {
    if (!section(ctx, cursor, "LIGHT", light_open_)) {
        return;
    }

    const auto choice_row = [&](std::string_view label,
                                std::string_view property,
                                std::span<const std::string_view> options,
                                std::size_t selected, core::u64 salt,
                                std::span<const scene::LightUnit> units = {}) {
        if (element.linked && (property == scene::schema::light_kind || property == scene::schema::light_unit)) {
            ctx.label(take_row(cursor,row_height),std::string{label}+": "+std::string{options[selected]}+" (Blender)",ctx.theme().text_muted);
            return;
        }
        const gui::Rect row = take_row(cursor, row_height);
        cursor.y += row_gap;
        const float label_width =
            std::clamp(row.width * label_fraction, label_min, label_max);
        ctx.label(gui::Rect{row.x, row.y, label_width, row.height}, label,
                  ctx.theme().text_muted);
        const gui::Rect well{row.x + label_width, row.y, std::max(row.width - label_width, 1.0F), row.height};
        const bool boolean = property == scene::schema::casts_shadow
            || property == scene::schema::use_color_temperature || property == scene::schema::two_sided;
        bool changed = false;
        if (boolean) {
            bool value = selected != 0U;
            changed = ctx.checkbox(mix(id_, property, element.node.value() + salt), well, value, "");
            selected = value ? 1U : 0U;
        } else {
            changed = ctx.dropdown(mix(id_, property, element.node.value() + salt), well, options, selected);
        }
        if (changed) frame.named_edits.push_back(NamedEdit{element.node, std::string{property},
            units.empty() ? static_cast<double>(selected) : static_cast<double>(units[selected])});
    };

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
        constexpr std::array<std::string_view, 2U> labels{{"UNITLESS", "LUX"}};
        constexpr std::array<scene::LightUnit, 2U> units{{scene::LightUnit::Unitless, scene::LightUnit::Lux}};
        choice_row("Units", scene::schema::light_unit, labels,
                   element.light_unit == scene::LightUnit::Lux ? 1U : 0U, 41U, units);
    } else if (element.local_kind == scene::LocalLightKind::Rectangle || element.local_kind == scene::LocalLightKind::Sphere) {
        constexpr std::array<std::string_view, 3> labels{{"UNITLESS", "LM", "NITS"}};
        constexpr std::array<scene::LightUnit, 3> units{{scene::LightUnit::Unitless, scene::LightUnit::Lumens, scene::LightUnit::Nits}};
        choice_row("Units", scene::schema::light_unit, labels,
            element.light_unit == scene::LightUnit::Nits ? 2U : element.light_unit == scene::LightUnit::Lumens ? 1U : 0U, 41U, units);
    } else {
        constexpr std::array<std::string_view, 3U> labels{{"UNITLESS", "LM", "CD"}};
        constexpr std::array<scene::LightUnit, 3U> units{{scene::LightUnit::Unitless, scene::LightUnit::Lumens, scene::LightUnit::Candela}};
        choice_row("Units", scene::schema::light_unit, labels,
                   static_cast<std::size_t>(element.light_unit), 41U, units);
    }
    constexpr std::array<std::string_view, 2U> temperature{{"OFF", "ON"}};
    choice_row("Temperature", scene::schema::use_color_temperature, temperature,
               element.use_color_temperature ? 1U : 0U, 51U);
    if (element.use_color_temperature) {
        constexpr std::array<std::string_view, 1U> axes{{""}};
        const std::array<float, 1U> values{{element.color_temperature_kelvin}};
        const std::array<FieldBinding, 1U> bindings{{
            {FieldDomain::Light, {}, scene::schema::color_temperature_kelvin}}};
        vector_row(ctx, cursor, "Kelvin", axes, values, 25.0F, bindings, frame);
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
        constexpr std::array<std::string_view, 1U> radius_axes{{"turns"}};
        const std::array<float, 1U> radius_values{{element.angular_radius}};
        const std::array<FieldBinding, 1U> radius_bindings{{
            {FieldDomain::Light, {}, scene::schema::angular_radius_turns}}};
        vector_row(ctx, cursor, "Sun radius", radius_axes, radius_values,
                   0.0001F, radius_bindings, frame, nullptr, false, false, 0U, 6);
        return;
    }

    if (!element.linked) {
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

    if (element.kind != ElementKind::PointLight) {
        return;
    }

    // A point light and a spot are the same node with a different `kind`, so
    // the cone controls appear beside it rather than in a different element
    // type. Everything below is emitted as a named edit; the server translates
    // the ones that are not really numbers.


    const bool spot = element.local_kind == scene::LocalLightKind::Spot;
    const bool rectangle = element.local_kind == scene::LocalLightKind::Rectangle;
    const bool sphere = element.local_kind == scene::LocalLightKind::Sphere;
    constexpr std::array<std::string_view, 4U> kinds{{"Point", "Spot", "Rectangle", "Sphere"}};
    choice_row("Kind", scene::schema::light_kind, kinds, static_cast<std::size_t>(element.local_kind), 11U);
    const auto area_number = [&](std::string_view label, std::string_view property, float value) {
        const std::array<float, 1> values{value};
        constexpr std::array<std::string_view, 1> axes{{""}};
        const std::array<FieldBinding, 1> bindings{{{FieldDomain::Light, {}, property}}};
        vector_row(ctx, cursor, label, axes, values, 0.01F, bindings, frame);
    };
    if (sphere) {
        area_number("Radius m", scene::schema::light_radius, element.light_radius);
        ctx.label(take_row(cursor, row_height), "Area shadows: deferred", ctx.theme().text_muted);
        return;
    }
    if (rectangle) {
        area_number("Width m", scene::schema::light_width, element.light_width);
        area_number("Height m", scene::schema::light_height, element.light_height);
        area_number("Roll turns", scene::schema::area_rotation_turns, element.area_rotation);
        constexpr std::array<std::string_view, 2> sides{{"ONE", "TWO"}};
        choice_row("Two sided", scene::schema::two_sided, sides, element.two_sided ? 1U : 0U, 61U);
    }
    if (!spot && !rectangle) return;

    {
        const std::array<float, 3U> values{
            element.direction.x, element.direction.y, element.direction.z};
        constexpr std::array<std::string_view, 3U> axes{{"X", "Y", "Z"}};
        constexpr std::array<std::string_view, 3U> properties{{
            scene::schema::direction_x, scene::schema::direction_y,
            scene::schema::direction_z}};
        std::array<FieldBinding, 3U> bindings{};
        for (std::size_t i = 0U; i < bindings.size(); ++i) {
            bindings[i] = FieldBinding{FieldDomain::Light, {}, properties[i]};
        }
        vector_row(ctx, cursor, "Aim", axes, values, 0.01F, bindings, frame);
    }
    if (rectangle) {
        ctx.label(take_row(cursor, row_height), "Area shadows: deferred", ctx.theme().text_muted);
        return;
    }
    {
        // Half-angles in turns, which is what the document stores and what the
        // parser validates. Showing degrees here would mean two units for one
        // number and a conversion to get wrong in both directions.
        const std::array<float, 1U> values{element.inner_cone};
        constexpr std::array<std::string_view, 1U> axes{{""}};
        const std::array<FieldBinding, 1U> bindings{{
            {FieldDomain::Light, {}, scene::schema::inner_cone_turns}}};
        vector_row(ctx, cursor, "Inner cone", axes, values, 0.002F, bindings,
                   frame);
    }
    {
        const std::array<float, 1U> values{element.outer_cone};
        constexpr std::array<std::string_view, 1U> axes{{""}};
        const std::array<FieldBinding, 1U> bindings{{
            {FieldDomain::Light, {}, scene::schema::outer_cone_turns}}};
        vector_row(ctx, cursor, "Outer cone", axes, values, 0.002F, bindings,
                   frame);
    }

    constexpr std::array<std::string_view, 2U> casting{{"OFF", "CAST"}};
    choice_row("Cast shadow", scene::schema::casts_shadow, casting,
               element.casts_shadow ? 1U : 0U, 21U);
    if (element.casts_shadow) {
        constexpr std::array<std::string_view, 3U> tiers{
            {"SMALL", "MEDIUM", "LARGE"}};
        choice_row("Tier", scene::schema::shadow_tier, tiers,
                   static_cast<std::size_t>(element.shadow_tier), 31U);
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
    bool aspect_locked = element.aspect_locked;
    if (ctx.checkbox(mix(id_, "aspect lock", element.node.value()), well, aspect_locked, "Lock ratio") && !element.locked) {
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
                 element.linked ? "LINKED MATERIAL FACTORS" : (imported_model ? "FALLBACK MATERIAL" : "MATERIAL"),
                 material_open_)) {
        return;
    }

    if (imported_model) {
        const gui::Rect note = take_row(cursor, row_height);
        cursor.y += row_gap;
        ctx.label(note, element.linked ? "Each edited factor overrides source materials" : "Used for source parts without a material",
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

void InspectorPanel::draw_material_extensions(gui::UiContext& ctx, Cursor& cursor,
    const ElementInfo& element, InspectorFrame& frame) {
    if (element.kind != ElementKind::Object && element.kind != ElementKind::Model) return;
    if (!section(ctx, cursor, "SURFACE LAYERS", material_extensions_open_)) return;
    constexpr std::array<std::string_view, 1U> scalar_axis{{""}};
    {
        const std::array<float, 1U> value{element.material.base_alpha};
        const std::array<FieldBinding, 1U> bindings{{{FieldDomain::Material, {}, scene::schema::base_alpha}}};
        vector_row(ctx, cursor, "Alpha", scalar_axis, value, 0.005F, bindings, frame);
    }
    {
        const std::array<float, 1U> value{element.material.clearcoat};
        const std::array<FieldBinding, 1U> bindings{{{FieldDomain::Material, {}, scene::schema::clearcoat}}};
        vector_row(ctx, cursor, "Clearcoat", scalar_axis, value, 0.005F, bindings, frame);
    }
    {
        const std::array<float, 1U> value{element.material.clearcoat_roughness};
        const std::array<FieldBinding, 1U> bindings{{{FieldDomain::Material, {}, scene::schema::clearcoat_roughness}}};
        vector_row(ctx, cursor, "Coat roughness", scalar_axis, value, 0.005F, bindings, frame);
    }
    {
        const std::array<float, 1U> value{element.material.clearcoat_normal_scale};
        const std::array<FieldBinding, 1U> bindings{{{FieldDomain::Material, {}, scene::schema::clearcoat_normal_scale}}};
        vector_row(ctx, cursor, "Coat normal", scalar_axis, value, 0.005F, bindings, frame);
    }
    {
        const std::array<float, 1U> value{element.material.sheen_roughness};
        const std::array<FieldBinding, 1U> bindings{{{FieldDomain::Material, {}, scene::schema::sheen_roughness}}};
        vector_row(ctx, cursor, "Sheen roughness", scalar_axis, value, 0.005F, bindings, frame);
    }
    {
        const std::array<float, 1U> value{element.material.transmission};
        const std::array<FieldBinding, 1U> bindings{{{FieldDomain::Material, {}, scene::schema::transmission}}};
        vector_row(ctx, cursor, "Transmission", scalar_axis, value, 0.005F, bindings, frame);
    }
    {
        const std::array<float, 1U> value{element.material.ior};
        const std::array<FieldBinding, 1U> bindings{{{FieldDomain::Material, {}, scene::schema::ior}}};
        vector_row(ctx, cursor, "IOR", scalar_axis, value, 0.005F, bindings, frame);
    }
    {
        const std::array<float, 1U> value{element.material.specular};
        const std::array<FieldBinding, 1U> bindings{{{FieldDomain::Material, {}, scene::schema::specular}}};
        vector_row(ctx, cursor, "Specular", scalar_axis, value, 0.005F, bindings, frame);
    }
    {
        const std::array<float, 1U> value{element.material.anisotropy_strength};
        const std::array<FieldBinding, 1U> bindings{{{FieldDomain::Material, {}, scene::schema::anisotropy_strength}}};
        vector_row(ctx, cursor, "Anisotropy", scalar_axis, value, 0.005F, bindings, frame);
    }
    {
        const std::array<float, 1U> value{element.material.anisotropy_rotation};
        const std::array<FieldBinding, 1U> bindings{{{FieldDomain::Material, {}, scene::schema::anisotropy_rotation}}};
        vector_row(ctx, cursor, "Anisotropy radians", scalar_axis, value, 0.005F, bindings, frame);
    }
    {
        const std::array<float, 1U> value{element.material.sheen_color.x};
        const std::array<FieldBinding, 1U> bindings{{{FieldDomain::Material, {}, scene::schema::sheen_color_r}}};
        vector_row(ctx, cursor, "Sheen R", scalar_axis, value, 0.005F, bindings, frame);
    }
    {
        const std::array<float, 1U> value{element.material.sheen_color.y};
        const std::array<FieldBinding, 1U> bindings{{{FieldDomain::Material, {}, scene::schema::sheen_color_g}}};
        vector_row(ctx, cursor, "Sheen G", scalar_axis, value, 0.005F, bindings, frame);
    }
    {
        const std::array<float, 1U> value{element.material.sheen_color.z};
        const std::array<FieldBinding, 1U> bindings{{{FieldDomain::Material, {}, scene::schema::sheen_color_b}}};
        vector_row(ctx, cursor, "Sheen B", scalar_axis, value, 0.005F, bindings, frame);
    }
    {
        const std::array<float, 1U> value{element.material.specular_color.x};
        const std::array<FieldBinding, 1U> bindings{{{FieldDomain::Material, {}, scene::schema::specular_color_r}}};
        vector_row(ctx, cursor, "Specular color R", scalar_axis, value, 0.005F, bindings, frame);
    }
    {
        const std::array<float, 1U> value{element.material.specular_color.y};
        const std::array<FieldBinding, 1U> bindings{{{FieldDomain::Material, {}, scene::schema::specular_color_g}}};
        vector_row(ctx, cursor, "Specular color G", scalar_axis, value, 0.005F, bindings, frame);
    }
    {
        const std::array<float, 1U> value{element.material.specular_color.z};
        const std::array<FieldBinding, 1U> bindings{{{FieldDomain::Material, {}, scene::schema::specular_color_b}}};
        vector_row(ctx, cursor, "Specular color B", scalar_axis, value, 0.005F, bindings, frame);
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
    constexpr std::array<std::string_view, 4U> blends{{"Normal", "Add", "Multiply", "Screen"}};
    std::size_t next = blend_mixed ? blends.size() : static_cast<std::size_t>(element.style.blend);
    if (ctx.dropdown(mix(id_, "blend mode", 7'100U), well, blends, next)) {
        for (const doc::NodeId node : blend_incompatible) append_unique(frame.skipped_incompatible, node);
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
              effect.enabled ? ctx.theme().accent : ctx.theme().track);
    // The document has carried an enabled flag on every effect from the start
    // and the renderer honours it; without a control the only way to see the
    // picture without an effect was to delete it and lose its keys with it.
    // It is the layer list's eye, because switching a thing off is one idea.
    constexpr float enable_width = 22.0F;
    constexpr float remove_width = 64.0F;
    const gui::Rect enable{title_row.x + 3.0F, title_row.y + 1.0F,
                           enable_width, std::max(title_row.height - 2.0F,
                                                  0.0F)};
    // Named rather than offset from `title`: the additive salts around here
    // alias whenever two effects of one kind sit a salt-step apart in node id,
    // and a control that answers to another control's press is worse than a
    // longer literal.
    const gui::Interaction enable_it =
        ctx.grab(mix(id_, "effect switch", id_salt), enable);
    ctx.panel(enable, enable_it.hovered ? ctx.theme().widget_hover
                                        : ctx.theme().header);
    gui::draw_eye_glyph(ctx.draw_list(), enable.x + enable.width * 0.5F,
                        enable.y + enable.height * 0.5F, effect.enabled,
                        effect.enabled
                            ? (enable_it.hovered ? ctx.theme().text
                                                 : ctx.theme().text_muted)
                            : ctx.theme().warning);
    if (enable_it.clicked) {
      frame.named_edits.push_back(
          NamedEdit{effect.source, std::string{scene::schema::effect_enabled},
                    effect.enabled ? 0.0 : 1.0});
    }
    const float title_x = enable.x + enable.width + 6.0F;
    ctx.label(gui::Rect{title_x, title_row.y,
                        std::max(title_row.x + title_row.width - title_x -
                                     remove_width - 4.0F,
                                 0.0F),
                        title_row.height},
              title,
              effect.enabled ? ctx.theme().text : ctx.theme().text_muted);
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
      constexpr float open_width = 82.0F;
      const gui::Rect open{note.x + std::max(note.width - open_width, 0.0F),
                           note.y, std::min(open_width, note.width),
                           note.height};
      ctx.label(gui::Rect{note.x + 7.0F, note.y,
                          std::max(note.width - open.width - 14.0F, 0.0F),
                          note.height},
                "Structured controls in COLOR", ctx.theme().text_muted);
      if (ctx.button(mix(id_, "open color", id_salt), open, "OPEN COLOR")) {
        frame.panel_request = InspectorPanelRequest::Color;
      }
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

    constexpr std::array<std::string_view, 1U> scalar_axis{{""}};
    const auto named_scalar = [&](std::string_view label, float value,
                                  float row_speed,
                                  std::string_view property) {
      const std::array<float, 1U> values{{value}};
      const std::array<FieldBinding, 1U> bindings{{
          {FieldDomain::Effect, {}, property, effect.kind, effect_ordinal}}};
      vector_row(ctx, cursor, label, scalar_axis, values, row_speed, bindings,
                 frame, nullptr, false, false, id_salt);
    };

    if (effect.kind == scene::EffectKind::ChromaKey) {
      constexpr std::array<std::string_view, 3U> key_color_properties{{
          scene::schema::chroma_key_color_r,
          scene::schema::chroma_key_color_g,
          scene::schema::chroma_key_color_b}};
      std::array<FieldBinding, 3U> key_color_bindings{};
      bool color_mixed = false;
      for (std::size_t index = 0U; index < key_color_bindings.size(); ++index) {
        key_color_bindings[index] = FieldBinding{
            FieldDomain::Effect, {}, key_color_properties[index], effect.kind,
            effect_ordinal};
        color_mixed = color_mixed ||
                      summarize(key_color_bindings[index], frame).mixed;
      }
      const gui::Rect swatch = take_row(cursor, 18.0F);
      cursor.y += row_gap;
      const float swatch_label_width =
          std::clamp(swatch.width * label_fraction, label_min, label_max);
      ctx.label(gui::Rect{swatch.x, swatch.y, swatch_label_width,
                          swatch.height},
                "Key Color", ctx.theme().text_muted);
      const gui::Rect color_well{
          swatch.x + swatch_label_width, swatch.y + 2.0F,
          std::max(swatch.width - swatch_label_width - 28.0F, 1.0F),
          std::max(swatch.height - 4.0F, 1.0F)};
      if (color_mixed) {
        ctx.panel(color_well, ctx.theme().track);
        ctx.label(color_well, "Mixed", ctx.theme().text_muted);
      } else {
        ctx.panel(color_well,
                  gui::UiColor{effect.chroma_key.key_color.x,
                               effect.chroma_key.key_color.y,
                               effect.chroma_key.key_color.z, 1.0F});
      }
      const gui::Rect sample_button{
          color_well.x + color_well.width + 6.0F, swatch.y,
          22.0F,
          swatch.height};
      const gui::Interaction sample_it = ctx.grab(
          mix(id_, "chroma sample from view", id_salt), sample_button);
      ctx.panel(sample_button,
                sample_it.held   ? ctx.theme().widget_active
                : sample_it.hovered ? ctx.theme().widget_hover
                                    : ctx.theme().widget);
      gui::draw_eyedropper_glyph(
          ctx.draw_list(), sample_button.x + sample_button.width * 0.5F,
          sample_button.y + sample_button.height * 0.5F,
          sample_it.hovered ? ctx.theme().text : ctx.theme().text_muted);
      if (sample_it.clicked) {
        frame.color_sample_request = InspectorColorSampleRequest{
            effect.source,
            {std::string{scene::schema::chroma_key_color_r},
             std::string{scene::schema::chroma_key_color_g},
             std::string{scene::schema::chroma_key_color_b}}};
      }
      const std::array<float, 3U> key_color{{
          effect.chroma_key.key_color.x, effect.chroma_key.key_color.y,
          effect.chroma_key.key_color.z}};
      vector_row(ctx, cursor, "Key RGB", rgb, key_color, 0.005F,
                 key_color_bindings, frame, nullptr, false, false, id_salt);
      named_scalar("Tolerance", effect.chroma_key.tolerance, 0.005F,
                   scene::schema::chroma_key_tolerance);
      named_scalar("Softness", effect.chroma_key.softness, 0.005F,
                   scene::schema::chroma_key_softness);
      named_scalar("Edge px",
                   static_cast<float>(effect.chroma_key.morphology_px), 1.0F,
                   scene::schema::chroma_key_morphology_px);
      named_scalar("Spill", effect.chroma_key.spill_suppression, 0.005F,
                   scene::schema::chroma_key_spill_suppression);
    } else if (effect.kind == scene::EffectKind::LumaKey) {
      named_scalar("Low", effect.luma_key.low_threshold, 0.005F,
                   scene::schema::luma_key_low_threshold);
      named_scalar("High", effect.luma_key.high_threshold, 0.005F,
                   scene::schema::luma_key_high_threshold);
      named_scalar("Low Soft", effect.luma_key.low_softness, 0.005F,
                   scene::schema::luma_key_low_softness);
      named_scalar("High Soft", effect.luma_key.high_softness, 0.005F,
                   scene::schema::luma_key_high_softness);
      const gui::Rect invert_row = take_row(cursor, row_height);
      cursor.y += row_gap;
      const float invert_label_width = std::clamp(
          invert_row.width * label_fraction, label_min, label_max);
      ctx.label(gui::Rect{invert_row.x, invert_row.y, invert_label_width,
                          invert_row.height},
                "Matte", ctx.theme().text_muted);
      const gui::Rect invert_button{
          invert_row.x + invert_label_width, invert_row.y + 1.0F,
          std::max(invert_row.width - invert_label_width, 1.0F),
          std::max(invert_row.height - 2.0F, 1.0F)};
      if (ctx.button(mix(id_, "luma invert", id_salt), invert_button,
                     effect.luma_key.invert ? "INVERTED" : "NORMAL")) {
        frame.named_edits.push_back(NamedEdit{
            effect.source, std::string{scene::schema::luma_key_invert},
            effect.luma_key.invert ? 0.0 : 1.0});
      }
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
      const gui::Rect note = take_row(cursor, 26.0F);
      constexpr float open_width = 96.0F;
      const gui::Rect open{note.x + std::max(note.width - open_width, 0.0F),
                           note.y, std::min(open_width, note.width),
                           note.height};
      ctx.label(gui::Rect{note.x, note.y,
                          std::max(note.width - open.width - 6.0F, 0.0F),
                          note.height},
                "More controls in TEXTURE", ctx.theme().text_muted);
      if (ctx.button(mix(id_, "open texture", id_salt), open,
                     "OPEN TEXTURE")) {
        frame.panel_request = InspectorPanelRequest::Texture;
      }
    }
    cursor.y += 2.0F;
  };

  // The stack is whatever effects this element carries. A switched-off one
  // keeps its row, drawn muted, because an effect the panel hides is one the
  // operator cannot switch back on. An element carrying none shows nothing
  // under the header: an empty stack already says it is empty.
  std::array<std::size_t, static_cast<std::size_t>(scene::EffectKind::Count)>
      ordinals{};
  for (const scene::EffectInstance &effect : effects) {
    if (effect.target == element.node) {
      const std::size_t kind = static_cast<std::size_t>(effect.kind);
      if (kind >= ordinals.size()) {
        continue;
      }
      effect_row(effect, ordinals[kind]++);
    }
  }
}

void InspectorPanel::draw_camera(gui::UiContext& ctx, Cursor& cursor,
                                 const ElementInfo& element,
                                 InspectorFrame& frame) {
    if (element.kind != ElementKind::Camera) {
        return;
    }
    if (!section(ctx, cursor, "CAMERA", transform_open_)) {
        return;
    }

    constexpr std::array<std::string_view, 3U> xyz_axes{{"X", "Y", "Z"}};
    {
        constexpr std::array<std::string_view, 3U> properties{
            {scene::schema::eye_x, scene::schema::eye_y,
             scene::schema::eye_z}};
        std::array<FieldBinding, 3U> bindings{};
        for (std::size_t i = 0U; i < bindings.size(); ++i) {
            bindings[i] = FieldBinding{FieldDomain::Camera, {}, properties[i]};
        }
        const std::array<float, 3U> values{
            element.position.x, element.position.y, element.position.z};
        vector_row(ctx, cursor, "Position", xyz_axes, values, 0.01F, bindings,
                   frame);
    }
    {
        constexpr std::array<std::string_view, 3U> properties{
            {scene::schema::target_x, scene::schema::target_y,
             scene::schema::target_z}};
        std::array<FieldBinding, 3U> bindings{};
        for (std::size_t i = 0U; i < bindings.size(); ++i) {
            bindings[i] = FieldBinding{FieldDomain::Camera, {}, properties[i]};
        }
        const std::array<float, 3U> values{
            element.target.x, element.target.y, element.target.z};
        vector_row(ctx, cursor, "Target", xyz_axes, values, 0.01F, bindings,
                   frame);
    }
    {
        constexpr std::array<std::string_view, 1U> axes{{""}};
        const std::array<FieldBinding, 1U> bindings{
            {{FieldDomain::Camera, {}, scene::schema::fov_turns}}};
        const std::array<float, 1U> values{element.field_of_view};
        vector_row(ctx, cursor, "FOV turns", axes, values, 0.001F, bindings,
                   frame);
    }
    {
        constexpr std::array<std::string_view, 1U> axes{{""}};
        const std::array<FieldBinding, 1U> bindings{
            {{FieldDomain::Camera, {}, scene::schema::shutter_seconds}}};
        const std::array<float, 1U> values{element.shutter_seconds};
        vector_row(ctx, cursor, "Shutter s", axes, values, 0.0001F, bindings,
                   frame, nullptr, false, false, 0U, 6);
    }
    for (const bool focus : {true, false}) {
        constexpr std::array<std::string_view, 1U> axes{{""}};
        const std::array<FieldBinding, 1U> bindings{{{FieldDomain::Camera, {},
            focus ? scene::schema::focus_distance_m : scene::schema::aperture_f_number}}};
        const std::array<float, 1U> values{focus ? element.focus_distance_m : element.aperture_f_number};
        vector_row(ctx, cursor, focus ? "Focus m" : "Aperture f/", axes, values, 0.01F, bindings, frame);
    }
}

void InspectorPanel::draw_camera_tracking(
    gui::UiContext& ctx, Cursor& cursor, const ElementInfo& element,
    InspectorFrame& frame, std::span<const scene::TrackingSource> sources) {
    if (element.kind != ElementKind::Camera) {
        return;
    }
    if (!section(ctx, cursor, "TRACKING", tracking_open_)) {
        return;
    }

    const auto well_row = [&](std::string_view label, std::string_view,
                              std::string_view id, bool lit, core::u64 salt) {
        const gui::Rect row = take_row(cursor, row_height);
        cursor.y += row_gap;
        const float lw = std::clamp(row.width * label_fraction, label_min, label_max);
        ctx.label({row.x, row.y, lw, row.height}, label, ctx.theme().text_muted);
        return ctx.checkbox(mix(id_, id, element.node.value() + salt),
            {row.x + lw, row.y, row.width - lw, row.height}, lit, "") && !element.locked;
    };

    const scene::TrackingSource* bound = nullptr;
    std::vector<std::string_view> feed_names{"None"};
    std::size_t selected_feed = 0U;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        feed_names.push_back(sources[i].name);
        if (sources[i].node == element.tracking_source) { selected_feed = i + 1U; bound = &sources[i]; }
    }
    const gui::Rect feed_row_bounds = take_row(cursor, row_height);
    cursor.y += row_gap;
    const float feed_label_width = std::clamp(feed_row_bounds.width * label_fraction, label_min, label_max);
    ctx.label({feed_row_bounds.x, feed_row_bounds.y, feed_label_width, row_height}, "Feed", ctx.theme().text_muted);
    if (ctx.dropdown(mix(id_, "tracking feed", element.node.value()),
            {feed_row_bounds.x + feed_label_width, feed_row_bounds.y, feed_row_bounds.width - feed_label_width, row_height},
            feed_names, selected_feed) && !element.locked) {
        frame.named_edits.push_back({element.node, std::string{scene::schema::camera_tracking_source},
            selected_feed == 0U ? 0.0 : static_cast<double>(sources[selected_feed - 1U].node.value())});
    }
    {
        // A feed is project state, so it is added here rather than from the
        // Library: it has no place on the stage, and the moment an operator
        // wants one is the moment they are pointing a camera at it.
        const gui::Rect row = take_row(cursor, row_height);
        cursor.y += row_gap;
        if (ctx.button(mix(id_, "tracking.add", element.node.value()),
                       gui::Rect{row.x, row.y, std::min(row.width, 128.0F),
                                 row.height},
                       "+ New feed")
            && !element.locked) {
            frame.add_tracking_source = true;
        }
    }

    // The bound feed's own settings, inline. They belong to the feed and the
    // edits are addressed to its node, but this is where an operator is
    // looking when they want them, so this is where they are.
    if (bound != nullptr) {
        const scene::TrackingSourceSettings& settings = bound->settings;
        const auto feed_toggle = [&](std::string_view label, std::string_view text,
                                     std::string_view property, bool lit, core::u64 salt) {
            const gui::Rect row = take_row(cursor, row_height);
            cursor.y += row_gap;
            const float lw = std::clamp(row.width * label_fraction, label_min, label_max);
            ctx.label({row.x, row.y, lw, row.height}, label, ctx.theme().text_muted);
            const gui::Rect well{row.x + lw, row.y, row.width - lw, row.height};
            const auto id = mix(id_, property, bound->node.value() + salt);
            if (property == scene::schema::tracking_enabled)
                return ctx.checkbox(id, well, lit, "Listening") && !element.locked;
            const bool protocol = property == scene::schema::tracking_protocol;
            const std::array<std::string_view, 2> choices = protocol
                ? std::array<std::string_view, 2>{"FreeD", "OpenTrackIO"}
                : std::array<std::string_view, 2>{"Stage (Y up)", "Z up"};
            std::size_t selected = text == choices[1] ? 1U : 0U;
            return ctx.dropdown(id, well, choices, selected) && !element.locked;
        };
        // Two words, both true: a listening feed is bound to its socket, an
        // authored one is only described. Nothing here half-opens a port.
        if (feed_toggle("State",
                        settings.enabled ? "Listening" : "Not listening",
                        scene::schema::tracking_enabled, settings.enabled,
                        0U)) {
            frame.named_edits.push_back(
                NamedEdit{bound->node,
                          std::string{scene::schema::tracking_enabled},
                          settings.enabled ? 0.0 : 1.0});
        }
        if (feed_toggle("Protocol", tracker_protocol_name(settings.protocol),
                        scene::schema::tracking_protocol, true, 1U)) {
            const bool freed =
                settings.protocol == scene::TrackerProtocolKind::FreeD;
            frame.named_edits.push_back(
                NamedEdit{bound->node,
                          std::string{scene::schema::tracking_protocol},
                          freed ? 1.0 : 0.0});
        }
        feed_ = bound->node;
        constexpr std::array<std::string_view, 1U> scalar_axis{{""}};
        const auto feed_row = [&](std::string_view label,
                                  std::string_view property, float value,
                                  float speed) {
            const std::array<float, 1U> values{value};
            const std::array<FieldBinding, 1U> bindings{
                {{FieldDomain::Tracking, {}, property}}};
            vector_row(ctx, cursor, label, scalar_axis, values, speed,
                       bindings, frame);
        };
        feed_row("Port", scene::schema::tracking_port,
                 static_cast<float>(settings.port), 1.0F);
        feed_row("Camera ID", scene::schema::tracking_camera_id,
                 static_cast<float>(settings.camera_id), 1.0F);
        // The lens the feed reports through. A FreeD packet carries a zoom
        // encoder, not a field of view; these are what turn one into the
        // other, so they describe the equipment and live with the feed.
        feed_row("Sensor mm", scene::schema::tracking_sensor_height_mm,
                 settings.sensor_height_mm, 0.05F);
        feed_row("Sensor AR", scene::schema::tracking_sensor_aspect,
                 settings.sensor_aspect_ratio, 0.005F);
        feed_row("Focal mm", scene::schema::tracking_fallback_focal_mm,
                 settings.fallback_focal_length_mm, 0.5F);
        // Where this tracking system's numbers sit relative to the stage.
        //
        // Nothing in a datagram says which way the tracker was surveyed, and
        // an uncalibrated feed puts the camera somewhere unrelated to the wall
        // and aims it at nothing - which reads as a broken plate rather than a
        // missing setting. Heading is the one an operator touches daily; the
        // rest are set once at bring-up.
        if (feed_toggle("Up axis",
                        settings.calibration.axes
                                == scene::TrackerAxes::ZUpRightHanded
                            ? "Z up"
                            : "Stage (Y up)",
                        scene::schema::tracking_axes, true, 2U)) {
            const bool z_up = settings.calibration.axes
                == scene::TrackerAxes::ZUpRightHanded;
            frame.named_edits.push_back(
                NamedEdit{bound->node,
                          std::string{scene::schema::tracking_axes},
                          z_up ? 0.0 : 1.0});
        }
        // "turns" in the label for the same reason the transform rows say it:
        // Live Data reads pose angles out in degrees because that is what an
        // operator thinks in, and a field that took degrees here while the
        // document stored turns would be off by a factor of 360 in the one
        // place someone is trying to line a plate up.
        feed_row("Heading turns", scene::schema::tracking_heading,
                 settings.calibration.heading, 0.001F);
        feed_row("Origin X", scene::schema::tracking_origin_x,
                 settings.calibration.origin.x, 0.01F);
        feed_row("Origin Y", scene::schema::tracking_origin_y,
                 settings.calibration.origin.y, 0.01F);
        feed_row("Origin Z", scene::schema::tracking_origin_z,
                 settings.calibration.origin.z, 0.01F);
        feed_row("Scale", scene::schema::tracking_scale,
                 settings.calibration.scale, 0.005F);
        feed_ = doc::NodeId{};
    } else if (sources.empty()) {
        const gui::Rect note = take_row(cursor, row_height);
        cursor.y += row_gap;
        ctx.label(note, "Add a feed, then bind it to this camera",
                  ctx.theme().text_muted);
    }

    // How this camera draws its plate. Per camera and not per show, because
    // two cameras on a volume are doing different jobs: a wide master needs
    // more overscan than a long lens, and one parked between setups needs no
    // plate at all.
    if (well_row("Inner frustum",
                 element.icvfx.inner_frustum ? "Drawing a plate"
                                             : "No plate",
                 scene::schema::camera_inner_frustum,
                 element.icvfx.inner_frustum, 2U)) {
        frame.named_edits.push_back(
            NamedEdit{element.node,
                      std::string{scene::schema::camera_inner_frustum},
                      element.icvfx.inner_frustum ? 0.0 : 1.0});
    }
    if (element.icvfx.inner_frustum) {
        constexpr std::array<std::string_view, 1U> scalar_axis{{""}};
        const auto plate_row = [&](std::string_view label,
                                   std::string_view property, float value,
                                   float speed) {
            const std::array<float, 1U> values{value};
            const std::array<FieldBinding, 1U> bindings{
                {{FieldDomain::Plate, {}, property}}};
            vector_row(ctx, cursor, label, scalar_axis, values, speed,
                       bindings, frame);
        };
        plate_row("Margin", scene::schema::camera_inner_frustum_margin_px,
                  static_cast<float>(element.icvfx.margin_px), 1.0F);
        plate_row("Soft edge",
                  scene::schema::camera_inner_frustum_soft_edge_px,
                  static_cast<float>(element.icvfx.soft_edge_px), 1.0F);
        plate_row("Plate scale",
                  scene::schema::camera_inner_frustum_scale_numerator,
                  static_cast<float>(element.icvfx.resolution_numerator),
                  1.0F);
        plate_row("Plate divisor",
                  scene::schema::camera_inner_frustum_scale_denominator,
                  static_cast<float>(element.icvfx.resolution_denominator),
                  1.0F);
        // Bigger draws over, which is the convention every stage operator
        // already knows from every switcher they have used.
        plate_row("Render order", scene::schema::camera_render_order,
                  static_cast<float>(element.icvfx.render_order), 1.0F);
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
                                        effects,
                                    std::span<const scene::TrackingSource>
                                        sources) {
    const std::span<const ElementInfo> selected = element == nullptr
        ? std::span<const ElementInfo>{}
        : std::span<const ElementInfo>{element, 1U};
    return draw_selection(ctx, bounds, selected,
                          element == nullptr ? doc::NodeId{} : element->node,
                          element == nullptr ? 0U : also_selected + 1U,
                          keyframes, show_frame, effects, sources);
}

InspectorFrame InspectorPanel::draw(
    gui::UiContext& ctx, const gui::Rect& bounds,
    std::span<const ElementInfo> selected, doc::NodeId primary,
    std::span<const scene::ChannelKeyframes> keyframes, core::u64 show_frame,
    std::span<const scene::EffectInstance> effects,
    std::span<const scene::TrackingSource> sources) {
    return draw_selection(ctx, bounds, selected, primary, selected.size(),
                          keyframes, show_frame, effects, sources);
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
    std::span<const scene::EffectInstance> effects,
    std::span<const scene::TrackingSource> sources) {
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
    sources_ = sources;
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

    if (displayed->linked) {
        draw_linked(ctx, cursor, *displayed, frame);
        if (is_light(displayed->kind)) draw_light(ctx, cursor, *displayed, frame);
    } else if (is_light(displayed->kind)) {
        draw_light(ctx, cursor, *displayed, frame);
        if (displayed->kind == ElementKind::PointLight && light_open_) draw_ies(ctx, cursor, *displayed, frame);
    } else if (displayed->kind == ElementKind::Camera) {
        draw_camera(ctx, cursor, *displayed, frame);
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
    if (!displayed->linked_collection && ctx.button(mix(id_, "delete", 0U),
                   gui::Rect{actions.x, actions.y,
                             std::min(actions.width, 96.0F), actions.height},
                   "Delete")) {
        frame.delete_requested = true;
    }

    if (displayed->kind == ElementKind::Camera) {
        cursor.y += row_gap;
        draw_camera_tracking(ctx, cursor, *displayed, frame, sources);
    } else if (!is_light(displayed->kind) &&
               displayed->kind != ElementKind::Group) {
        cursor.y += row_gap;
        if (!displayed->linked) draw_content(ctx, cursor, *displayed, frame);
        draw_screen(ctx, cursor, *displayed, frame);
        draw_material(ctx, cursor, *displayed, frame);
        draw_compositing(ctx, cursor, *displayed, frame, keyframes, show_frame);
        draw_effects(ctx, cursor, *displayed, frame, keyframes, show_frame,
                     effects);
        if (!displayed->linked) draw_material_extensions(ctx, cursor, *displayed, frame);
    }

    if (displayed->kind == ElementKind::Model && (!displayed->linked || !displayed->source.empty())) {
        cursor.y += row_gap;
        ctx.label(take_row(cursor,20.0F),"MODEL ANIMATION",ctx.theme().text_muted);
        const auto& animation=displayed->animation;
        const auto toggle=[&](std::string_view property,bool value,std::string_view label) {
            if (ctx.checkbox(mix(id_,property,displayed->node.value()),take_row(cursor,row_height),value,label))
                for (const auto& item:selected_) if (!item.locked && item.kind==ElementKind::Model)
                    frame.named_edits.push_back({item.node,std::string{property},value?1.0:0.0});
            cursor.y+=row_gap;
        };
        toggle(scene::schema::animation_enabled,animation.enabled,"Animate model");
        toggle(scene::schema::animation_loop,animation.loop,"Loop playback");
        const std::array<float,1> clip{static_cast<float>(animation.clip)}, period{static_cast<float>(animation.wind_period_frames)};
        const std::array<std::string_view,1> index{"index"}, frames{"frames"};
        const std::array<FieldBinding,1> clip_binding{{{FieldDomain::Animation,{},scene::schema::animation_clip}}};
        const std::array<FieldBinding,1> period_binding{{{FieldDomain::Animation,{},scene::schema::wind_period_frames}}};
        if (displayed->linked_animation) ctx.label(take_row(cursor,20.0F),"Published assembly clip",ctx.theme().text_muted);
        else vector_row(ctx,cursor,"Clip (first = 0)",index,clip,1.0F,clip_binding,frame);
        ctx.label(take_row(cursor,20.0F),"Start frame: "+std::to_string(animation.start_frame),ctx.theme().text_muted);
        if (ctx.button(mix(id_,"animation.start",displayed->node.value()),take_row(cursor,row_height),"START AT CURRENT SHOW FRAME"))
            for (const auto& item:selected_) if (!item.locked && item.kind==ElementKind::Model)
                frame.text_edits.push_back({item.node,std::string{scene::schema::animation_start_frame},std::to_string(show_frame)});
        if (ctx.button(mix(id_,"animation.zero",displayed->node.value()),take_row(cursor,row_height),"START AT FRAME ZERO"))
            for (const auto& item:selected_) if (!item.locked && item.kind==ElementKind::Model)
                frame.text_edits.push_back({item.node,std::string{scene::schema::animation_start_frame},"0"});
        const std::array<float,3> wind{animation.wind.x,animation.wind.y,animation.wind.z};
        const std::array<FieldBinding,3> wind_binding{{{FieldDomain::Animation,{},scene::schema::wind_x},
            {FieldDomain::Animation,{},scene::schema::wind_y},{FieldDomain::Animation,{},scene::schema::wind_z}}};
        vector_row(ctx,cursor,"Wind displacement",xyz,wind,0.01F,wind_binding,frame);
        vector_row(ctx,cursor,"Wind period",frames,period,1.0F,period_binding,frame);
    }
    if(is_light(displayed->kind) || displayed->kind == ElementKind::Object || displayed->kind == ElementKind::Model) {
        cursor.y += row_gap;
        const auto row = take_row(cursor, row_height);
        const bool light = is_light(displayed->kind);
        bool checked = light ? displayed->bake_indirect : displayed->bake_static;
        const auto property = light ? scene::schema::bake_indirect : scene::schema::bake_static;
        const std::string_view label = light ? "Bake indirect light" : "Static geometry";
        if (displayed->linked_animation) ctx.label(row,"Dynamic node / skin geometry",ctx.theme().text_muted);
        else if(ctx.checkbox(mix(id_,"bake contribution",displayed->node.value()),row,checked,label))
            frame.named_edits.push_back(NamedEdit{displayed->node,std::string{property},checked?1.0:0.0});
    }

    draw_skip_summary(ctx, cursor, frame);
    cursor.y += panel_inset;

    content_height_ = cursor.y + scroll_ - bounds.y;
    ctx.pop_clip();

    frame.editing = frame.editing || fields_.live();
    if (!frame.editing) {
        drag_capture_ = {};
    }
    keyframes_ = {};
    effects_ = {};
    return frame;
}

} // namespace mediaserver::editor
