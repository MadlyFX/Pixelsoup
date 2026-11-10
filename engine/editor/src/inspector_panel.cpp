#include <mediaserver/editor/inspector_panel.hpp>
#include <mediaserver/editor/effects_panel.hpp>
#include <mediaserver/editor/operator_widgets.hpp>

#include <mediaserver/assets/plateset.hpp>

#include <mediaserver/compositor/compositor_schema.hpp>

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
constexpr float scroll_step = 32.0F;

// The Inspector's rows run `panel_inset` in from the panel's sides and top.
[[nodiscard]] gui::Metrics inspector_metrics(gui::Metrics metrics) noexcept {
    metrics.inset = panel_inset;
    metrics.gap = panel_inset;
    return metrics;
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

// The channel keying one particle parameter component, by its key.
[[nodiscard]] const scene::ChannelKeyframes* parameter_channel(
    std::span<const scene::ChannelKeyframes> keyframes, doc::NodeId node,
    std::string_view key) noexcept {
    for (const scene::ChannelKeyframes& channel : keyframes) {
        if (channel.target == node && channel.component == scene::PropertyChannel::ParticleParameter
            && channel.parameter == key) {
            return &channel;
        }
    }
    return nullptr;
}

// What a particle parameter component holds when it is not keyed: the
// node's own value, else the package's default; nothing for a component the
// package does not publish.
[[nodiscard]] std::optional<double> parameter_base(const ElementInfo& element, std::string_view key) {
    const std::optional<scene::ParticleParameterKey> parsed = scene::parse_particle_parameter_key(key);
    if (!parsed) {
        return std::nullopt;
    }
    const auto published = std::find_if(element.particle_parameters.begin(), element.particle_parameters.end(),
        [&](const ParticleParameterInfo& info) { return info.name == parsed->name; });
    if (published == element.particle_parameters.end() || parsed->component >= published->components) {
        return std::nullopt;
    }
    for (const auto& [authored, value] : element.particle_values) {
        if (authored == key) {
            return value;
        }
    }
    return static_cast<double>(published->fallback[parsed->component]);
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
    if (channel == nullptr || frame > maximum) return std::nullopt;
    return scene::sample_keyframes(*channel, static_cast<core::i64>(frame));
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
    if (authored.kind == ElementKind::Camera) {
        apply(scene::PropertyChannel::CameraFieldOfView, displayed.field_of_view);
        apply(scene::PropertyChannel::CameraShutter, displayed.shutter_seconds);
        apply(scene::PropertyChannel::CameraFocusDistance, displayed.focus_distance_m);
        apply(scene::PropertyChannel::CameraAperture, displayed.aperture_f_number);
        displayed.field_of_view = std::clamp(displayed.field_of_view, 0.001F, 0.499F);
        displayed.shutter_seconds = std::clamp(displayed.shutter_seconds, 0.00001F, 1.0F);
        displayed.focus_distance_m = std::clamp(displayed.focus_distance_m, 0.0F, 100000.0F);
        displayed.aperture_f_number = std::clamp(displayed.aperture_f_number, 0.1F, 128.0F);
    }
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
    switch (protocol) {
    case scene::TrackerProtocolKind::OpenTrackIO:
        return "OpenTrackIO";
    case scene::TrackerProtocolKind::NatNet:
        return "NatNet";
    case scene::TrackerProtocolKind::FreeD:
    default:
        return "FreeD";
    }
}

// The protocol choices in the order the Director's authoring numbers them.
constexpr std::array<std::string_view, 3U> tracker_protocol_choices{
    {"FreeD", "OpenTrackIO", "NatNet"}};

// One value of a vehicle's rig, by the compositor's name for it; nothing for
// a name the rig does not have.
[[nodiscard]] std::optional<float> rig_value(
    const compositor::LedCarProcessParameters& rig,
    std::string_view property) noexcept {
    namespace names = compositor::schema;
    if (property == names::position_x) return rig.position.x;
    if (property == names::position_y) return rig.position.y;
    if (property == names::position_z) return rig.position.z;
    if (property == names::rotation_yaw_turns) return rig.yaw;
    if (property == names::rotation_pitch_turns) return rig.pitch;
    if (property == names::rotation_roll_turns) return rig.roll;
    if (property == names::plate_scale) return rig.plate_scale;
    if (property == names::array_scale) return rig.array_scale;
    if (property == names::horizontal_blend) return rig.horizontal_blend;
    if (property == names::reflection_height) return rig.reflection_height;
    if (property == names::reflection_scale) return rig.reflection_scale;
    return std::nullopt;
}

// The element as a new one of its kind starts: what it is - its identity,
// its kind, its place in the show and its links - kept, and every value an
// operator authors put back to ElementInfo's own defaults, which mirror the
// scene's. Reset to default reads a field's default off this.
[[nodiscard]] ElementInfo pristine_like(const ElementInfo& element) {
    const ElementInfo fresh{};
    ElementInfo pristine = element;
    pristine.position = fresh.position;
    pristine.rotation = fresh.rotation;
    pristine.scale = fresh.scale;
    pristine.color = fresh.color;
    pristine.intensity = fresh.intensity;
    pristine.direction = fresh.direction;
    pristine.range = fresh.range;
    pristine.color_temperature_kelvin = fresh.color_temperature_kelvin;
    pristine.angular_radius = fresh.angular_radius;
    pristine.ies_rotation = fresh.ies_rotation;
    pristine.light_width = fresh.light_width;
    pristine.light_height = fresh.light_height;
    pristine.light_radius = fresh.light_radius;
    pristine.area_rotation = fresh.area_rotation;
    pristine.inner_cone = fresh.inner_cone;
    pristine.outer_cone = fresh.outer_cone;
    pristine.animation = fresh.animation;
    pristine.particle_seed = fresh.particle_seed;
    pristine.particle_start_frame = fresh.particle_start_frame;
    pristine.particle_values = fresh.particle_values;
    // A fill keeps its kind - Solid, Linear, Radial is a choice, not a value -
    // and starts its colours and placement over.
    std::optional<scene::Fill> fill;
    if (element.material.fill.has_value()) {
        fill = scene::Fill{};
        fill->mode = element.material.fill->mode;
    }
    pristine.material = fresh.material;
    pristine.material.fill = fill;
    pristine.style.opacity = fresh.style.opacity;
    pristine.style.horizontal_blend = fresh.style.horizontal_blend;
    pristine.raster_width = fresh.raster_width;
    pristine.raster_height = fresh.raster_height;
    pristine.time_offset = fresh.time_offset;
    pristine.rig = fresh.rig;
    pristine.field_of_view = fresh.field_of_view;
    pristine.shutter_seconds = fresh.shutter_seconds;
    pristine.aperture_f_number = fresh.aperture_f_number;
    pristine.focus_distance_m = fresh.focus_distance_m;
    pristine.icvfx = fresh.icvfx;
    pristine.environment = fresh.environment;
    return pristine;
}

} // namespace

ElementInfo evaluated_element(
    const ElementInfo& authored,
    std::span<const scene::ChannelKeyframes> keyframes,
    core::u64 show_frame) noexcept {
    return evaluated_for_inspector(authored, keyframes, show_frame);
}

gui::Rect InspectorPanel::take_row(Cursor& cursor, float height) noexcept {
    return cursor.next(height, 0.0F);
}

std::optional<double> InspectorPanel::default_value(
    const FieldBinding& binding) {
    for (const ElementInfo& element : selected_) {
        if (element.locked || !editable_value(element, binding).has_value()) {
            continue;
        }
        // Read the field off a pristine copy of the element, with its
        // effects as they are first added and nothing animated.
        std::vector<scene::EffectInstance> fresh_effects;
        for (const scene::EffectInstance& effect : effects_) {
            if (effect.target != element.node) {
                continue;
            }
            scene::EffectInstance fresh = scene::initial_effect(effect.kind);
            fresh.source = effect.source;
            fresh.target = effect.target;
            fresh.order = effect.order;
            fresh.enabled = effect.enabled;
            fresh_effects.push_back(fresh);
        }
        const std::span<const scene::EffectInstance> effects = effects_;
        const std::span<const scene::ChannelKeyframes> keyframes = keyframes_;
        effects_ = fresh_effects;
        keyframes_ = {};
        const std::optional<EditableValue> fresh =
            editable_value(pristine_like(element), binding);
        effects_ = effects;
        keyframes_ = keyframes;
        if (fresh.has_value()) {
            return fresh->value;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

void InspectorPanel::vector_row(gui::UiContext& ctx, Cursor& cursor,
                                std::string_view label,
                                std::span<const std::string_view> axes,
                                std::span<const float> values, float speed,
                                std::span<const FieldBinding> bindings,
                                InspectorFrame& frame,
                                bool* key_requested, bool animated,
                                bool keyed_here, std::size_t id_salt, int precision) {
    gui::Rect row = take_row(cursor, row_height);
    cursor.space(row_gap);

    float label_width = std::clamp(row.width * label_fraction, label_min, label_max);
    const float key_width = key_requested != nullptr ? 18.0F : 0.0F;
    const std::size_t count = std::min({axes.size(), values.size(), bindings.size()});
    const float needed = ctx.font_width(label) + key_width + 10.0F;
    const float field_min = ctx.font_width("X -0.000") + 14.0F;
    const bool stacked = needed > label_width ||
        (row.width - label_width - field_gap * static_cast<float>(count > 0U ? count - 1U : 0U))
            < static_cast<float>(count) * field_min;
    if (stacked) {
        // A narrow dock gets a full label above full-width fields, rather
        // than clipping a label and a number in the same row.
        label_width = row.width;
    }
    std::array<std::optional<double>, 4U> defaults{};
    bool differs = false;
    for (std::size_t i = 0U; i < std::min(count, defaults.size()); ++i) {
        defaults[i] = default_value(bindings[i]);
        if (defaults[i].has_value() &&
            std::abs(static_cast<double>(values[i]) - *defaults[i]) >
                1.0e-5 * std::max(1.0, std::abs(*defaults[i]))) {
            differs = true;
        }
    }
    const float reset_width = differs ? reset_button_width : 0.0F;
    ctx.label(gui::Rect{row.x, row.y,
                        std::max(label_width - key_width - reset_width, 0.0F),
                        row.height},
              label, ctx.theme().text_muted);
    if (key_requested != nullptr) {
        const gui::Rect key_bounds{row.x + label_width - key_width, row.y,
                                   key_width, row.height};
        *key_requested = key_button(ctx, gui::ui_id(id_, label, 9'000U + id_salt), key_bounds,
                                    animated, keyed_here, ctx.theme().panel);
    }
    // Any component away from its default carries the reset arrow, at the
    // end of the label and left of the key: a click puts back every
    // component that differs, in one edit.
    {
        if (differs) {
            const gui::Rect reset_bounds{
                row.x + label_width - key_width - reset_button_width, row.y,
                reset_button_width, row.height};
            std::string tip{"Reset to default ("};
            for (std::size_t i = 0U; i < std::min(count, defaults.size()); ++i) {
                if (!defaults[i].has_value()) {
                    continue;
                }
                std::array<char, 32U> text{};
                const int length = std::snprintf(text.data(), text.size(),
                                                 i == 0U ? "%.*f" : ", %.*f",
                                                 std::max(precision, 0),
                                                 *defaults[i]);
                tip.append(text.data(),
                           static_cast<std::size_t>(std::max(length, 0)));
            }
            tip += ")";
            if (reset_button(ctx, gui::ui_id(id_, label, 10'000U + id_salt),
                             reset_bounds, tip)) {
                for (std::size_t i = 0U; i < std::min(count, defaults.size());
                     ++i) {
                    if (!defaults[i].has_value() ||
                        std::abs(static_cast<double>(values[i]) - *defaults[i]) <=
                            1.0e-5 * std::max(1.0, std::abs(*defaults[i]))) {
                        continue;
                    }
                    NumberFieldEdit edit{};
                    edit.kind = NumberFieldEditKind::Absolute;
                    edit.value = *defaults[i];
                    emit_numeric(bindings[i], gui::ui_id(id_, label, i + id_salt), edit,
                                 summarize(bindings[i], frame), frame);
                }
            }
        }
    }

    if (stacked) {
        row = take_row(cursor, 26.0F);
        cursor.space(6.0F);
    }
    const gui::Rect fields{row.x + (stacked ? 0.0F : label_width), row.y,
                           std::max(row.width - (stacked ? 0.0F : label_width), 1.0F), row.height};
    std::array<gui::Rect, 4U> boxes{};
    columns(fields, std::min(count, boxes.size()), field_gap, boxes);

    for (std::size_t i = 0U; i < count && i < boxes.size(); ++i) {
        ValueSummary summary = summarize(bindings[i], frame);
        if (summary.mixed) {
            ++frame.mixed_fields;
        }
        const gui::UiId field = gui::ui_id(id_, label, i + id_salt);
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

void InspectorPanel::colour_well(gui::UiContext& ctx, const gui::Rect& well,
                                 std::string_view name,
                                 std::array<float, 3U> channels,
                                 std::span<const FieldBinding> bindings,
                                 InspectorFrame& frame, bool mixed,
                                 std::size_t salt) {
    const gui::UiId id = gui::ui_id(id_, name, 11'000U + salt);
    gui::UiColor color{channels[0], channels[1], channels[2], 1.0F};
    const bool changed = ctx.color_well(id, well, color);
    if (mixed && !ctx.color_open(id)) {
        ctx.rounded(well, ctx.theme().track, ctx.theme().radius_small);
        ctx.label(gui::Rect{well.x + 8.0F, well.y, std::max(well.width - 8.0F, 0.0F),
                            well.height},
                  "Mixed", ctx.theme().text_muted);
    }
    // The whole visit to the picker is one gesture, so one undo step.
    if (ctx.color_open(id)) {
        colour_editing_ = true;
    }
    if (!changed) {
        return;
    }
    const std::array<float, 3U> next{color.r, color.g, color.b};
    for (std::size_t i = 0U; i < std::min<std::size_t>(3U, bindings.size()); ++i) {
        if (std::abs(next[i] - channels[i]) <= 1.0e-6F) {
            continue;
        }
        NumberFieldEdit edit{};
        edit.kind = NumberFieldEditKind::Absolute;
        edit.value = static_cast<double>(next[i]);
        emit_numeric(bindings[i], id + i + 1U, edit, summarize(bindings[i], frame),
                     frame);
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
        // Only a slot screen plays a plate that can be early or late.
        if (binding.property == scene::schema::lane_time_offset &&
            element.vehicle_track != doc::NodeId{}) {
            return EditableValue{element.node, element.node,
                                 static_cast<double>(element.time_offset)};
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
    case FieldDomain::Particles:
        if (element.kind != ElementKind::ParticleEffect) {
            return std::nullopt;
        }
        if (binding.property.starts_with(scene::schema::particle_parameter_prefix)) {
            const std::string_view key = binding.property.substr(scene::schema::particle_parameter_prefix.size());
            const std::optional<double> base = parameter_base(element, key);
            if (!base) {
                return std::nullopt;
            }
            if (const std::optional<double> keyed =
                    sample_at(parameter_channel(keyframes_, element.node, key), show_frame_)) {
                return EditableValue{element.node, element.node, *keyed};
            }
            return EditableValue{element.node, element.node, *base};
        }
        if (binding.property == scene::schema::particle_seed) {
            return EditableValue{element.node, element.node,
                                 static_cast<double>(element.particle_seed)};
        }
        if (binding.property == scene::schema::particle_start_frame) {
            return EditableValue{
                element.node, element.node,
                static_cast<double>(element.particle_start_frame)};
        }
        return std::nullopt;
    case FieldDomain::Rig:
        // The value is the rig's and so is the node the edit is addressed
        // to; the element is still the vehicle track, which is what makes a
        // locked car's layout read-only.
        if (element.kind != ElementKind::Track || !element.vehicle ||
            element.vehicle_rig == doc::NodeId{}) {
            return std::nullopt;
        }
        if (const std::optional<float> value =
                rig_value(element.rig, binding.property)) {
            return EditableValue{element.node, element.vehicle_rig,
                                 static_cast<double>(*value)};
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
        if (element.material.fill.has_value()) {
            const scene::Fill& fill = *element.material.fill;
            const std::pair<std::string_view, float> fills[]{
                {scene::schema::fill_a_r, fill.color_a.r},
                {scene::schema::fill_a_g, fill.color_a.g},
                {scene::schema::fill_a_b, fill.color_a.b},
                {scene::schema::fill_a_a, fill.color_a.a},
                {scene::schema::fill_b_r, fill.color_b.r},
                {scene::schema::fill_b_g, fill.color_b.g},
                {scene::schema::fill_b_b, fill.color_b.b},
                {scene::schema::fill_b_a, fill.color_b.a},
                {scene::schema::fill_angle_turns, fill.angle},
                {scene::schema::fill_center_x, fill.center_x},
                {scene::schema::fill_center_y, fill.center_y},
                {scene::schema::fill_size, fill.size}};
            for (const auto& [property, value] : fills) {
                if (binding.property == property) {
                    return EditableValue{element.node, element.node, value};
                }
            }
        }
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
        // Whole source frames, either way.
        if (binding.property == scene::schema::lane_time_offset) {
          return std::clamp(
              std::round(value),
              -static_cast<double>(scene::max_lane_time_offset),
              static_cast<double>(scene::max_lane_time_offset));
        }
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
        // protocol's own: a UDP port, and a FreeD camera byte or a NatNet
        // rigid body id. The Director narrows the id to a byte for a FreeD
        // feed, which is the one place that knows the feed's protocol.
        if (binding.property == scene::schema::tracking_port
            || binding.property == scene::schema::tracking_command_port) {
          return std::clamp(std::round(value), 0.0, 65535.0);
        }
        if (binding.property == scene::schema::tracking_camera_id) {
          return std::clamp(std::round(value), 0.0, 2147483647.0);
        }
        return std::max(value, 0.0);
      case FieldDomain::Particles:
        if (binding.property.starts_with(scene::schema::particle_parameter_prefix)) {
          constexpr double largest = static_cast<double>(std::numeric_limits<float>::max());
          return std::clamp(value, -largest, largest);
        }
        return std::clamp(std::round(value), 0.0,
                          binding.property == scene::schema::particle_seed
                              ? 4294967295.0
                              : 9007199254740992.0);
      case FieldDomain::Rig: {
        // The ranges the compositor reads a rig with: a size or a distance
        // is never negative, and a blend takes at most half a wall.
        constexpr double extent =
            static_cast<double>(compositor::max_spatial_extent);
        if (binding.property == compositor::schema::horizontal_blend) {
          return std::clamp(
              value, 0.0,
              static_cast<double>(compositor::max_led_car_horizontal_blend));
        }
        if (binding.property == compositor::schema::plate_scale ||
            binding.property == compositor::schema::array_scale ||
            binding.property == compositor::schema::reflection_scale) {
          return std::clamp(value, 0.0, extent);
        }
        return std::clamp(value, -extent, extent);
      }
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
        // The fill's ranges, as the compositor's Background states them.
        if (binding.property == scene::schema::fill_angle_turns) {
          const double wrapped = value - std::floor(value);
          return wrapped < 0.0 ? wrapped + 1.0 : wrapped;
        }
        if (binding.property == scene::schema::fill_center_x ||
            binding.property == scene::schema::fill_center_y) {
          return std::clamp(value, -static_cast<double>(scene::max_fill_center),
                            static_cast<double>(scene::max_fill_center));
        }
        if (binding.property == scene::schema::fill_size) {
          return std::clamp(value, 0.01,
                            static_cast<double>(scene::max_fill_size));
        }
        if (binding.property.starts_with("fill_")) {
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
      case FieldDomain::Camera:
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
      case FieldDomain::Particles:
        if (binding.property.starts_with(scene::schema::particle_parameter_prefix)) {
          const std::string_view key = binding.property.substr(scene::schema::particle_parameter_prefix.size());
          if (parameter_channel(keyframes_, target.author, key) != nullptr) {
            frame.key_edits.push_back(InspectorKeyEdit{target.author, scene::PropertyChannel::ParticleParameter,
                                                       value, false, std::string{key}});
            break;
          }
        }
        frame.named_edits.push_back(
            NamedEdit{target.author, std::string{binding.property}, value});
        break;
      case FieldDomain::Plate:
      case FieldDomain::Rig:
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
    cursor.space(row_gap);

    const gui::Interaction it = ctx.grab(gui::ui_id(id_, title, 1000U), row);
    if (it.clicked) {
        open = !open;
    }
    // A soft band, eased lighter under the pointer, that twirls its group.
    ctx.cursor_for(it, gui::Cursor::Hand);
    const float hover =
        ctx.ease(gui::ui_id(id_, title, 1001U), it.hovered ? 1.0F : 0.0F);
    ctx.rounded(row, gui::blend(ctx.theme().header, ctx.theme().widget, hover),
                ctx.theme().radius);
    draw_twirl(ctx, row.x + 10.0F, row.y + row.height * 0.5F, open,
               hover > 0.5F ? ctx.theme().accent : ctx.theme().text_muted);
    ctx.label(gui::Rect{row.x + 22.0F, row.y, row.width - 22.0F, row.height},
              gui::sentence_case(title), ctx.theme().text);
    return open;
}

void InspectorPanel::draw_header(gui::UiContext& ctx, Cursor& cursor,
                                 const ElementInfo& element,
                                 std::size_t selected_count) {
    const gui::Rect row = take_row(cursor, 30.0F);
    cursor.space(row_gap);

    draw_element_icon(ctx, element, row.x + 8.0F, row.y + row.height * 0.5F,
                      ctx.theme().text_muted);

    const std::string title = element_display_name(element);

    const gui::Rect name{row.x + 24.0F, row.y, row.width - 24.0F, row.height};
    ctx.label(name, title, ctx.theme().text);

    if (selected_count > 1U) {
        std::array<char, 24U> extra{};
        const int extra_length = std::snprintf(
            extra.data(), extra.size(), "%zu selected", selected_count);
        ctx.label(take_row(cursor, row_height),
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
        if (ctx.button(gui::ui_id(id_, name, element.node.value()), take_row(cursor, row_height), label))
            frame.text_edits.push_back({element.node, "link_action", std::string{name}});
        cursor.space(row_gap);
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
            if (fields_.draw(ctx, gui::ui_id(id_, key, element.node.value()), boxes[axis], xyz[axis],
                    values[axis], 0.01, changed))
                frame.named_edits.push_back({element.node, key, changed});
        }
        cursor.space(row_gap);
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
    if (!section(ctx, cursor, "BLENDER CONNECTION", linked_open_)) return;
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
        const auto width = std::max<std::size_t>(20, static_cast<std::size_t>(std::max(cursor.width(),120.0F) / 7.0F));
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
    // A camera is placed like everything else but has no size.
    const std::size_t group_count =
        element.kind == ElementKind::Camera ? 2U : 3U;

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

    for (const Group& group : std::span{groups}.first(group_count)) {
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

    cursor.space(16.0F + row_gap);

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
    constexpr float browse_width = 66.0F;
    const gui::Rect path{field.x, field.y, std::max(field.width - browse_width - 6.0F, 1.0F), field.height};
    static_cast<void>(ctx.text_field(gui::ui_id(id_, "ies.path", element.node.value()), path, ies_draft_));
    ctx.tooltip(path, ies_draft_.empty() ? "Choose an IES light profile" : ies_draft_);
    if (ctx.button(gui::ui_id(id_, "ies.browse", element.node.value()),
                   {field.right() - browse_width, field.y, browse_width, field.height}, "Browse...")) {
        FilePickerRequest request{FilePickerKind::IesProfile, ies_draft_, {}};
        for (const auto& item : selected_) {
            if (!item.locked && item.kind == ElementKind::PointLight &&
                item.local_kind != scene::LocalLightKind::Rectangle &&
                item.local_kind != scene::LocalLightKind::Sphere)
                request.targets.push_back(item.node);
        }
        frame.file_picker = std::move(request);
    }
    cursor.space(row_gap);
    const auto actions = take_row(cursor, 24.0F);
    const float half = (actions.width - 6.0F) * 0.5F;
    const bool apply = ctx.button(gui::ui_id(id_, "ies.apply", element.node.value()), {actions.x, actions.y, half, actions.height}, "Apply");
    const bool clear = ctx.button(gui::ui_id(id_, "ies.clear", element.node.value()), {actions.x + half + 6.0F, actions.y, half, actions.height}, "Clear");
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
    cursor.space(row_gap);
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
        cursor.space(row_gap);
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
            changed = ctx.checkbox(gui::ui_id(id_, property, element.node.value() + salt), well, value, "");
            selected = value ? 1U : 0U;
        } else {
            changed = ctx.dropdown(gui::ui_id(id_, property, element.node.value() + salt), well, options, selected);
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
    cursor.space(row_gap);
    const float label_width =
        std::clamp(swatch.width * label_fraction, label_min, label_max);
    ctx.label(gui::Rect{swatch.x, swatch.y, label_width, swatch.height},
              "Color", ctx.theme().text_muted);
    const gui::Rect color_well{
        swatch.x + label_width, swatch.y,
        std::max(swatch.width - label_width, 1.0F), swatch.height};
    colour_well(ctx, color_well, "Color",
                {element.color.x, element.color.y, element.color.z},
                color_bindings, frame, color_mixed, 0U);

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
        cursor.space(row_gap);
        const float label_width = std::clamp(
            row.width * label_fraction, label_min, label_max);
        ctx.label(gui::Rect{row.x, row.y, label_width, row.height}, label,
                  ctx.theme().text_muted);
        const gui::Rect well{row.x + label_width, row.y + 1.0F,
                             std::max(row.width - label_width, 1.0F),
                             row.height - 2.0F};
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
    const auto separator = element.source.find_last_of("/\\");
    static_row("Source", separator == std::string::npos ? std::string_view{element.source}
               : std::string_view{element.source}.substr(separator + 1U), source_mixed);
    if (!source_mixed && !element.source.empty())
        ctx.tooltip(cursor.last(), element.source);
    if (element.kind == ElementKind::Media && element.sequence_count > 1U) {
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
    // How large a video clip is held once decoded. A clip with a native rate
    // is a video file; stills and image sequences are always held whole.
    if (element.kind == ElementKind::Media &&
        element.source_frame_rate.has_value()) {
        std::vector<const ElementInfo*> targets;
        bool resolution_mixed = false;
        for (const ElementInfo& selected : selected_) {
            if (selected.locked || selected.kind != ElementKind::Media ||
                !selected.source_frame_rate.has_value()) {
                continue;
            }
            if (!targets.empty() &&
                targets.front()->media_resolution != selected.media_resolution) {
                resolution_mixed = true;
            }
            targets.push_back(&selected);
        }
        if (resolution_mixed) {
            ++frame.mixed_fields;
        }
        const gui::Rect row = take_row(cursor, row_height);
        cursor.space(row_gap);
        const float label_width =
            std::clamp(row.width * label_fraction, label_min, label_max);
        ctx.label(gui::Rect{row.x, row.y, label_width, row.height},
                  "Resolution", ctx.theme().text_muted);
        const gui::Rect well{row.x + label_width, row.y + 1.0F,
                             std::max(row.width - label_width, 1.0F),
                             row.height - 2.0F};
        constexpr std::array<std::string_view, 2U> sizes{{"Full", "Half"}};
        std::size_t next = resolution_mixed
            ? sizes.size()
            : (element.media_resolution == scene::MediaResolution::Half ? 1U
                                                                        : 0U);
        if (ctx.dropdown(gui::ui_id(id_, "media resolution", element.node.value()),
                         well, sizes, next) &&
            next < sizes.size() && !element.locked) {
            const std::string value{
                next == 1U ? scene::schema::media_resolution_half
                           : scene::schema::media_resolution_full};
            for (const ElementInfo* const target : targets) {
                frame.text_edits.push_back(
                    {target->node,
                     std::string{scene::schema::media_resolution}, value});
            }
        }
        ctx.tooltip(well,
                    "Half holds each decoded frame at half width and height: "
                    "a quarter of the video memory, but softer wherever the "
                    "clip is shown larger than half its source size. "
                    "Decoding costs the same.");
    }
}

void InspectorPanel::draw_screen(gui::UiContext& ctx, Cursor& cursor,
                                 const ElementInfo& element,
                                 InspectorFrame& frame) {
    // A vehicle track owns no plane, so it has no format to set: its walls
    // are its slot screens, each with a section of its own.
    if (element.kind != ElementKind::Track || element.vehicle) {
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
    cursor.space(row_gap);
    const float label_width =
        std::clamp(row.width * label_fraction, label_min, label_max);
    ctx.label(gui::Rect{row.x, row.y, label_width, row.height}, "Aspect",
              ctx.theme().text_muted);
    const gui::Rect well{row.x + label_width, row.y + 1.0F,
                         std::max(row.width - label_width, 1.0F),
                         row.height - 2.0F};
    bool aspect_locked = element.aspect_locked;
    if (ctx.checkbox(gui::ui_id(id_, "aspect lock", element.node.value()), well, aspect_locked, "Lock ratio") && !element.locked) {
        frame.named_edits.push_back(
            NamedEdit{element.node,
                      std::string{scene::schema::lane_aspect_locked},
                      element.aspect_locked ? 0.0 : 1.0});
    }

    // A slot screen's plate can play ahead of its take or behind it. The
    // offset is the screen's, so it stays when the take is swapped: it is for
    // timing one wall against the car, not for footage that came in out of
    // sync, which the plateset's own sync offsets put right.
    if (element.vehicle_track != doc::NodeId{}) {
        constexpr std::array<std::string_view, 1U> unit{{"frames"}};
        const std::array<float, 1U> offset{
            static_cast<float>(element.time_offset)};
        const std::array<FieldBinding, 1U> offset_binding{{
            {FieldDomain::Screen, {}, scene::schema::lane_time_offset}}};
        vector_row(ctx, cursor, "Time offset", unit, offset, 1.0F,
                   offset_binding, frame);
        ctx.tooltip(cursor.last(),
                    "Source frames this screen's plate plays ahead of its "
                    "take (negative: behind). It stays with the screen when "
                    "the take is swapped.");
    }
}

// The car from above, front up, as the plateset page draws it: a tile per
// slot, each standing for that wall's screen. On a vehicle track it shows
// which walls the car has; on a take, which of them this take plays on. A
// wall with a screen is a button that selects it, which is where that wall's
// place, grade and timing are set.
void InspectorPanel::draw_vehicle(gui::UiContext& ctx, Cursor& cursor,
                                  const ElementInfo& element,
                                  InspectorFrame& frame) {
    const bool car = element.kind == ElementKind::Track && element.vehicle;
    const bool take = element.kind == ElementKind::Media && element.take;
    if ((!car && !take) ||
        !section(ctx, cursor, "VEHICLE", vehicle_open_)) {
        return;
    }
    const gui::Theme& theme = ctx.theme();
    constexpr float gap = 4.0F;
    const gui::Rect area = take_row(cursor, 132.0F);
    cursor.space(row_gap);
    const float side = std::max(std::min(area.width, area.height), 0.0F);
    const gui::Rect rig{area.x + (area.width - side) * 0.5F, area.y, side,
                        side};
    const float cell = (side - gap * 2.0F) / 3.0F;
    // Column and row of each slot, in slot order: the eight walls clockwise
    // from the front, then the ceiling over the car.
    constexpr std::array<std::array<int, 2U>, scene::vehicle_slot_count>
        cells{{{1, 0}, {2, 0}, {2, 1}, {2, 2}, {1, 2}, {0, 2}, {0, 1}, {0, 0},
               {1, 1}}};
    for (std::size_t slot = 0U; slot < cells.size(); ++slot) {
        const auto role = static_cast<assets::PlatesetSlot>(slot);
        const gui::Rect tile{
            rig.x + static_cast<float>(cells[slot][0]) * (cell + gap),
            rig.y + static_cast<float>(cells[slot][1]) * (cell + gap), cell,
            cell};
        const doc::NodeId screen = element.slot_screens[slot];
        const bool has_screen = screen != doc::NodeId{};
        const bool plays = take && element.plates[slot];
        gui::Interaction it{};
        if (has_screen) {
            it = ctx.grab(
                gui::ui_id(gui::ui_id(id_, "vehicle wall",
                                      element.node.value()),
                           "slot", slot),
                tile);
            ctx.cursor_for(it, gui::Cursor::Hand);
            if (it.clicked) {
                frame.select_node = screen;
            }
        }
        const float hover = ctx.ease(
            gui::ui_id(gui::ui_id(id_, "vehicle wall hover",
                                  element.node.value()),
                       "slot", slot),
            it.hovered ? 1.0F : 0.0F);
        if (has_screen) {
            // A take lights the walls it plays on and leaves the rest dim.
            const gui::UiColor resting =
                take && !plays ? gui::blend(theme.track, theme.widget, 0.4F)
                               : theme.widget;
            ctx.rounded(tile,
                        gui::blend(resting, theme.accent,
                                   hover * 0.25F + (plays ? 0.18F : 0.0F)),
                        theme.radius_small);
        } else {
            // No screen here. A plate with nowhere to play is the one case
            // worth a warning.
            ctx.rounded_outline(
                tile,
                plays ? theme.warning
                      : gui::blend(theme.track, theme.border, 0.6F),
                theme.radius_small);
        }
        const std::string_view tag = assets::plateset_slot_tag(role);
        ctx.label(gui::Rect{tile.x + std::max((tile.width -
                                               ctx.font_width(tag)) *
                                                  0.5F,
                                              0.0F),
                            tile.y, tile.width, tile.height},
                  tag, has_screen ? theme.text : theme.text_muted);
        std::string tip{assets::plateset_slot_label(role)};
        if (has_screen && take) {
            tip += plays ? ": this take's plate plays on this screen. Click "
                           "to select the screen."
                         : ": this take has no plate for this screen, so it "
                           "shows nothing while the take is up. Click to "
                           "select the screen.";
        } else if (has_screen) {
            tip += ": click to select this screen and set where it stands, "
                   "how it is graded and its time offset.";
        } else if (plays) {
            tip += ": this take has a plate here, but the car has no screen "
                   "for it.";
        } else {
            tip += ": the car has no screen here.";
        }
        ctx.tooltip(tile, tip);
    }
    // The car's layout: every wall moves with these, so they are the car's
    // and shown on its track. A wall's own fields stay a correction on top.
    if (car && element.vehicle_rig != doc::NodeId{}) {
        namespace names = compositor::schema;
        const compositor::LedCarProcessParameters& layout = element.rig;
        const std::array<float, 3U> position{layout.position.x, layout.position.y,
                                             layout.position.z};
        const std::array<FieldBinding, 3U> position_bindings{{
            {FieldDomain::Rig, {}, names::position_x},
            {FieldDomain::Rig, {}, names::position_y},
            {FieldDomain::Rig, {}, names::position_z}}};
        vector_row(ctx, cursor, "Position", xyz, position, 0.01F,
                   position_bindings, frame);
        ctx.tooltip(cursor.last(),
                    "Where the car stands. Every wall and the ceiling move "
                    "with it.");
        const std::array<float, 3U> rotation{layout.yaw, layout.pitch, layout.roll};
        const std::array<FieldBinding, 3U> rotation_bindings{{
            {FieldDomain::Rig, {}, names::rotation_yaw_turns},
            {FieldDomain::Rig, {}, names::rotation_pitch_turns},
            {FieldDomain::Rig, {}, names::rotation_roll_turns}}};
        vector_row(ctx, cursor, "Rotation (turns)", ypr, rotation, 0.002F,
                   rotation_bindings, frame, nullptr, false, false, 0U, 3);
        ctx.tooltip(cursor.last(),
                    "Turns the whole car, walls and ceiling together.");
        constexpr std::array<std::string_view, 1U> none{{""}};
        const auto scalar = [&](std::string_view label,
                                std::string_view property, float value,
                                float speed, int precision,
                                std::string_view tip) {
            const std::array<float, 1U> values{value};
            const std::array<FieldBinding, 1U> bindings{{
                {FieldDomain::Rig, {}, property}}};
            vector_row(ctx, cursor, label, none, values, speed, bindings,
                       frame, nullptr, false, false, 0U, precision);
            ctx.tooltip(cursor.last(), tip);
        };
        scalar("Wall distance", names::array_scale, layout.array_scale, 0.05F, 2,
               "How far each wall stands from the middle of the car.");
        scalar("Wall size", names::plate_scale, layout.plate_scale, 0.05F, 2,
               "Sizes all eight walls together. A wall's own scale multiplies "
               "this.");
        scalar("Edge blend", names::horizontal_blend, layout.horizontal_blend,
               0.005F, 3,
               "How much of each wall's width fades out at its left and right "
               "edges, so neighbouring walls overlap softly. The ceiling is "
               "not blended.");
        scalar("Ceiling height", names::reflection_height,
               layout.reflection_height, 0.05F, 2,
               "How high the ceiling plate hangs over the car.");
        scalar("Ceiling size", names::reflection_scale, layout.reflection_scale,
               0.05F, 2, "Sizes the ceiling plate on its own.");
    }
    if (take && element.plateset != doc::NodeId{}) {
        const gui::Rect row = take_row(cursor, row_height);
        cursor.space(row_gap);
        if (ctx.button(gui::ui_id(id_, "open plateset", element.node.value()),
                       row, "Open plateset")) {
            frame.open_asset = element.plateset;
        }
        ctx.tooltip(row, "Show this take's plateset in the Asset Viewer: "
                         "its plates, sync offsets and moments.");
    }
}

void InspectorPanel::draw_fill(gui::UiContext& ctx, Cursor& cursor,
                               const ElementInfo& element,
                               InspectorFrame& frame) {
    if (element.kind != ElementKind::Object ||
        !element.material.fill.has_value() ||
        !section(ctx, cursor, "FILL", fill_open_)) {
        return;
    }
    const scene::Fill& fill = *element.material.fill;

    // The compositor Background's TYPE, in scene::FillMode order.
    {
        constexpr std::array<std::string_view, 3U> modes{
            "Solid", "Linear gradient", "Radial gradient"};
        const gui::Rect row = take_row(cursor, row_height);
        cursor.space(row_gap);
        const float label_width =
            std::clamp(row.width * label_fraction, label_min, label_max);
        ctx.label(gui::Rect{row.x, row.y, label_width, row.height}, "Type",
                  ctx.theme().text_muted);
        const gui::Rect well{row.x + label_width, row.y,
                             std::max(row.width - label_width, 1.0F),
                             row.height};
        std::size_t selected = static_cast<std::size_t>(fill.mode);
        if (ctx.dropdown(gui::ui_id(id_, "fill mode", element.node.value()), well,
                         modes, selected)) {
            for (const ElementInfo& item : selected_) {
                if (!item.locked && item.material.fill.has_value()) {
                    frame.named_edits.push_back(NamedEdit{
                        item.node, std::string{scene::schema::fill_mode},
                        static_cast<double>(selected)});
                }
            }
        }
    }

    // A colour as a swatch above its four channels.
    constexpr std::array<std::string_view, 4U> rgba{{"R", "G", "B", "A"}};
    const auto colour_rows = [&](std::string_view label,
                                 const scene::FillColor& colour,
                                 const std::array<std::string_view, 4U>& keys,
                                 std::size_t salt) {
        const gui::Rect swatch = take_row(cursor, 14.0F);
        cursor.space(row_gap);
        const float label_width =
            std::clamp(swatch.width * label_fraction, label_min, label_max);
        ctx.label(gui::Rect{swatch.x, swatch.y, label_width, swatch.height},
                  label, ctx.theme().text_muted);
        const gui::Rect well{swatch.x + label_width, swatch.y,
                             std::max(swatch.width - label_width, 1.0F),
                             swatch.height};
        std::array<FieldBinding, 4U> bindings{};
        for (std::size_t i = 0U; i < bindings.size(); ++i) {
            bindings[i] = FieldBinding{FieldDomain::Material, {}, keys[i]};
        }
        colour_well(ctx, well, label, {colour.r, colour.g, colour.b},
                    bindings, frame, false, 20U + salt);
        const std::array<float, 4U> values{colour.r, colour.g, colour.b,
                                           colour.a};
        vector_row(ctx, cursor, "RGBA", rgba, values, 0.005F, bindings, frame,
                   nullptr, false, false, salt);
    };
    colour_rows(fill.mode == scene::FillMode::Solid ? "Colour" : "Colour A",
                fill.color_a,
                {scene::schema::fill_a_r, scene::schema::fill_a_g,
                 scene::schema::fill_a_b, scene::schema::fill_a_a},
                0U);
    if (fill.mode == scene::FillMode::Solid) {
        return;
    }
    colour_rows("Colour B", fill.color_b,
                {scene::schema::fill_b_r, scene::schema::fill_b_g,
                 scene::schema::fill_b_b, scene::schema::fill_b_a},
                10U);

    constexpr std::array<std::string_view, 1U> scalar_axis{{""}};
    const auto scalar = [&](std::string_view label, std::string_view property,
                            float value, float speed) {
        const std::array<float, 1U> values{value};
        const std::array<FieldBinding, 1U> bindings{
            {{FieldDomain::Material, {}, property}}};
        vector_row(ctx, cursor, label, scalar_axis, values, speed, bindings,
                   frame);
    };
    if (fill.mode == scene::FillMode::Linear) {
        scalar("Angle Turns", scene::schema::fill_angle_turns, fill.angle,
               0.001F);
    }
    {
        constexpr std::array<std::string_view, 2U> xy{{"X", "Y"}};
        const std::array<float, 2U> values{fill.center_x, fill.center_y};
        const std::array<FieldBinding, 2U> bindings{
            {{FieldDomain::Material, {}, scene::schema::fill_center_x},
             {FieldDomain::Material, {}, scene::schema::fill_center_y}}};
        vector_row(ctx, cursor, "Center", xy, values, 0.005F, bindings, frame);
    }
    scalar("Size", scene::schema::fill_size, fill.size, 0.01F);
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
                 element.linked ? "LINKED MATERIAL FACTORS" : (imported_model ? "MODEL MATERIALS" : "MATERIAL"),
                 material_open_)) {
        return;
    }

    if (imported_model)
        ctx.tooltip(cursor.last(),
                    element.linked ? "Each edited factor overrides source materials" : "Used for source parts without a material");

    if (element.kind == ElementKind::Object && !element.locked &&
        selected_.size() == 1U) {
        constexpr std::array<std::string_view, 5U> labels{
            "Base color", "Metal / rough", "Normal", "Occlusion", "Emission"};
        const std::array<std::string_view, 5U> sources{
            element.material.base_color_texture,
            element.material.metallic_roughness_texture,
            element.material.normal_texture,
            element.material.occlusion_texture,
            element.material.emissive_texture};
        for (std::size_t role = 0U; role < labels.size(); ++role) {
            const auto row = take_row(cursor, row_height);
            cursor.space(row_gap);
            constexpr float controls = 124.0F;
            ctx.label({row.x, row.y, std::max(1.0F, row.width - controls), row.height},
                      labels[role], ctx.theme().text_muted);
            const bool browse = ctx.button(gui::ui_id(id_, "generator.texture.browse", element.node.value() ^ role),
                {row.right() - controls, row.y, 74.0F, row.height}, "Browse...");
            const bool clear = ctx.button(gui::ui_id(id_, "generator.texture.clear", element.node.value() ^ role),
                {row.right() - 46.0F, row.y, 46.0F, row.height}, "Clear");
            if (browse || clear) {
                FilePickerRequest request{FilePickerKind::GeneratorTexture,
                    std::string{sources[role]}, {element.node}};
                request.texture_role = static_cast<core::u32>(role);
                request.clear_texture = clear;
                frame.file_picker = std::move(request);
            }
            if (role == 1U) ctx.tooltip(row, "Packed image: roughness in green, metallic in blue");
            if (role == 2U) ctx.tooltip(row, "Tangent-space normal image");
        }
    }

    if (imported_model && !element.linked && !element.locked && selected_.size() == 1U) {
        if (material_part_node_ != element.node) {
            material_part_node_ = element.node;
            material_part_ = 0U;
            material_part_options_.clear();
            material_part_labels_.clear();
        }
        if (element.model_material_parts == 0U) {
            ctx.label(take_row(cursor, row_height), element.model_material_status.empty()
                ? "Loading material parts..." : element.model_material_status, ctx.theme().text_muted);
        } else {
            // Immutable model packages admit at most 65,536 parts. Labels are
            // rebuilt only when the selected model/count changes, never on
            // every UI frame (large imports can have thousands of runs).
            const auto part_count = std::min(element.model_material_parts, 65536U);
            material_part_ = std::min(material_part_, static_cast<std::size_t>(part_count - 1U));
            if (material_part_labels_.size() != part_count) {
                material_part_options_.clear();
                material_part_labels_.clear();
                material_part_labels_.reserve(part_count);
                material_part_options_.reserve(part_count);
                for (core::u32 i = 0; i < part_count; ++i)
                    material_part_labels_.push_back("Material part " + std::to_string(i + 1U));
                for (const auto& name : material_part_labels_) material_part_options_.push_back(name);
            }
            static_cast<void>(ctx.dropdown(gui::ui_id(id_, "material.part", element.node.value()),
                take_row(cursor, row_height), material_part_options_, material_part_));
            cursor.space(row_gap);
            constexpr std::array<std::string_view, 5U> labels{
                "Base color", "Metal / rough", "Normal", "Occlusion", "Emission"};
            for (std::size_t role = 0; role < labels.size(); ++role) {
                const auto row = take_row(cursor, row_height);
                cursor.space(row_gap);
                const float controls = 124.0F;
                ctx.label({row.x, row.y, std::max(1.0F, row.width - controls), row.height}, labels[role], ctx.theme().text_muted);
                const bool browse = ctx.button(gui::ui_id(id_, "texture.browse", role),
                    {row.right() - controls, row.y, 74.0F, row.height}, "Browse...");
                const bool clear = ctx.button(gui::ui_id(id_, "texture.clear", role),
                    {row.right() - 46.0F, row.y, 46.0F, row.height}, "Clear");
                if (browse || clear) {
                    FilePickerRequest request{FilePickerKind::ModelTexture, {}, {element.node}};
                    request.model_source = element.source;
                    request.material_part = static_cast<core::u32>(material_part_);
                    request.texture_role = static_cast<core::u32>(role);
                    request.clear_texture = clear;
                    frame.file_picker = std::move(request);
                }
                if (role == 1U) ctx.tooltip(row, "Packed image: roughness in green, metallic in blue");
                if (role == 2U) ctx.tooltip(row, "Tangent-space normal image; the model must have UVs");
            }
            ctx.label(take_row(cursor, row_height), "Texture edits can be undone.", ctx.theme().text_muted);
            ctx.label(take_row(cursor, row_height), "Fallback factors", ctx.theme().text_muted);
        }
    } else if (imported_model && element.linked) {
        ctx.label(take_row(cursor, row_height), "Texture images follow Blender materials.", ctx.theme().text_muted);
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
    cursor.space(row_gap);
    const float label_width =
        std::clamp(swatch.width * label_fraction, label_min, label_max);
    ctx.label(gui::Rect{swatch.x, swatch.y, label_width, swatch.height},
              "Base Color", ctx.theme().text_muted);
    const gui::Rect color_well{
        swatch.x + label_width, swatch.y,
        std::max(swatch.width - label_width, 1.0F), swatch.height};
    colour_well(ctx, color_well, "Base Color",
                {element.material.base_color.x, element.material.base_color.y,
                 element.material.base_color.z},
                color_bindings, frame, color_mixed, 40U);

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
    cursor.space(row_gap);
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
    if (ctx.dropdown(gui::ui_id(id_, "blend mode", 7'100U), well, blends, next)) {
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
  const bool has_effect = std::any_of(effects.begin(), effects.end(), [&](const auto& effect) {
    return effect.target == element.node;
  });
  const auto add_effect = [&] {
    const auto add_row = take_row(cursor, row_height);
    cursor.space(row_gap);
    if (const auto kind = draw_effect_picker(
            ctx, gui::ui_id(id_, "effects.add", element.node.value()), add_row)) {
      for (const auto& selected : selected_) {
        if (selected.locked) continue;
        if (is_light(selected.kind) || selected.kind == ElementKind::Camera ||
            selected.kind == ElementKind::Group) {
          frame.skipped_incompatible.push_back(selected.node);
        } else {
          frame.effect_adds.push_back({selected.node, *kind});
        }
      }
    }
  };
  if (!has_effect) {
    add_effect();
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
    cursor.space(2.0F);
    ctx.panel(title_row, ctx.theme().header);
    ctx.panel(gui::Rect{title_row.x, title_row.y, 2.0F, title_row.height},
              effect.enabled ? ctx.theme().accent : ctx.theme().track);
    // The document has carried an enabled flag on every effect from the start
    // and the renderer honours it; without a control the only way to see the
    // picture without an effect was to delete it and lose its keys with it.
    // It is the layer list's eye, because switching a thing off is one idea.
    constexpr float enable_width = 22.0F;
    constexpr float remove_width = 84.0F;
    const gui::Rect enable{title_row.x + 3.0F, title_row.y + 1.0F,
                           enable_width, std::max(title_row.height - 2.0F,
                                                  0.0F)};
    // Named rather than offset from `title`: the additive salts around here
    // alias whenever two effects of one kind sit a salt-step apart in node id,
    // and a control that answers to another control's press is worse than a
    // longer literal.
    const gui::Interaction enable_it =
        ctx.grab(gui::ui_id(id_, "effect switch", id_salt), enable);
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
    const float title_x = enable.right() + 6.0F;
    ctx.label(gui::Rect{title_x, title_row.y,
                        std::max(title_row.right() - title_x -
                                     remove_width - 4.0F,
                                 0.0F),
                        title_row.height},
              title,
              effect.enabled ? ctx.theme().text : ctx.theme().text_muted);
    const gui::Rect remove{title_row.right() - remove_width,
                           title_row.y, remove_width, title_row.height};
    if (pill_button(ctx, gui::ui_id(id_, title, 8'000U + id_salt), remove,
                    gui::Icon::Trash, "Remove", "Remove this effect",
                    ButtonTone::Danger)) {
      frame.effect_deletes.push_back(effect.source);
    }

    if (structured_effect) {
      const gui::Rect note = take_row(cursor, 34.0F);
      cursor.space(row_gap);
      ctx.panel(note, ctx.theme().track);
      constexpr float open_width = 82.0F;
      const gui::Rect open{note.x + std::max(note.width - open_width, 0.0F),
                           note.y, std::min(open_width, note.width),
                           note.height};
      ctx.label(gui::Rect{note.x + 7.0F, note.y,
                          std::max(note.width - open.width - 14.0F, 0.0F),
                          note.height},
                "Structured controls in COLOR", ctx.theme().text_muted);
      if (ctx.button(gui::ui_id(id_, "open color", id_salt), open, "OPEN COLOR")) {
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
      cursor.space(2.0F);
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
      cursor.space(row_gap);
      const float swatch_label_width =
          std::clamp(swatch.width * label_fraction, label_min, label_max);
      ctx.label(gui::Rect{swatch.x, swatch.y, swatch_label_width,
                          swatch.height},
                "Key Color", ctx.theme().text_muted);
      const gui::Rect color_well{
          swatch.x + swatch_label_width, swatch.y,
          std::max(swatch.width - swatch_label_width - 28.0F, 1.0F),
          swatch.height};
      colour_well(ctx, color_well, "Key Color",
                  {effect.chroma_key.key_color.x, effect.chroma_key.key_color.y,
                   effect.chroma_key.key_color.z},
                  key_color_bindings, frame, color_mixed, 60U + id_salt);
      const gui::Rect sample_button{
          color_well.right() + 6.0F, swatch.y,
          22.0F,
          swatch.height};
      const gui::Interaction sample_it = ctx.grab(
          gui::ui_id(id_, "chroma sample from view", id_salt), sample_button);
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
      cursor.space(row_gap);
      const float invert_label_width = std::clamp(
          invert_row.width * label_fraction, label_min, label_max);
      ctx.label(gui::Rect{invert_row.x, invert_row.y, invert_label_width,
                          invert_row.height},
                "Matte", ctx.theme().text_muted);
      const gui::Rect invert_button{
          invert_row.x + invert_label_width, invert_row.y + 1.0F,
          std::max(invert_row.width - invert_label_width, 1.0F),
          std::max(invert_row.height - 2.0F, 1.0F)};
      if (ctx.button(gui::ui_id(id_, "luma invert", id_salt), invert_button,
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
      if (ctx.button(gui::ui_id(id_, "open texture", id_salt), open,
                     "OPEN TEXTURE")) {
        frame.panel_request = InspectorPanelRequest::Texture;
      }
    }
    cursor.space(2.0F);
  };

  // The stack is whatever effects this element carries. A switched-off one
  // keeps its row, drawn muted, because an effect the panel hides is one the
  // operator cannot switch back on. The picker follows the authored stack.
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
  add_effect();
}

void InspectorPanel::draw_camera(
    gui::UiContext& ctx, Cursor& cursor, const ElementInfo& element,
    InspectorFrame& frame, std::span<const scene::ChannelKeyframes> keyframes,
    core::u64 show_frame) {
    if (element.kind != ElementKind::Camera ||
        !section(ctx, cursor, "CAMERA", camera_open_)) {
        return;
    }
    struct LensField final {
        std::string_view label;
        std::string_view property;
        scene::PropertyChannel channel;
        float value;
        float speed;
        int precision;
    };
    const std::array<LensField, 4U> fields{{
        {"FOV turns", scene::schema::fov_turns,
         scene::PropertyChannel::CameraFieldOfView, element.field_of_view, 0.001F, 3},
        {"Shutter s", scene::schema::shutter_seconds,
         scene::PropertyChannel::CameraShutter, element.shutter_seconds, 0.0001F, 6},
        {"Focus m", scene::schema::focus_distance_m,
         scene::PropertyChannel::CameraFocusDistance, element.focus_distance_m, 0.01F, 3},
        {"Aperture f/", scene::schema::aperture_f_number,
         scene::PropertyChannel::CameraAperture, element.aperture_f_number, 0.01F, 3},
    }};
    constexpr std::array<std::string_view, 1U> axes{{""}};
    for (const LensField& field : fields) {
        const auto* const channel = channel_for(keyframes, element.node, field.channel);
        const bool animated = channel != nullptr && !channel->frames.empty();
        const bool keyed_here = keyed_at(channel, show_frame);
        const std::array<FieldBinding, 1U> bindings{{
            {FieldDomain::Camera, field.channel, field.property}}};
        const std::array<float, 1U> values{field.value};
        bool key_requested = false;
        vector_row(ctx, cursor, field.label, axes, values, field.speed, bindings,
                   frame, &key_requested, animated, keyed_here, 0U, field.precision);
        if (key_requested) {
            frame.key_edits.push_back(InspectorKeyEdit{
                element.node, field.channel, field.value, keyed_here});
        }
    }
}

void InspectorPanel::draw_particles(gui::UiContext& ctx, Cursor& cursor,
                                    const ElementInfo& element,
                                    InspectorFrame& frame,
                                    core::u64 show_frame) {
    if (element.kind != ElementKind::ParticleEffect ||
        !section(ctx, cursor, "PARTICLES", particles_open_)) {
        return;
    }
    cursor.space(row_gap);
    ctx.label(take_row(cursor, 20.0F), element.source, ctx.theme().text_muted);
    constexpr std::array<std::string_view, 1U> number{{""}};
    const std::array<float, 1U> seed{static_cast<float>(element.particle_seed)};
    const std::array<FieldBinding, 1U> seed_binding{
        {{FieldDomain::Particles, {}, scene::schema::particle_seed}}};
    vector_row(ctx, cursor, "Seed", number, seed, 1.0F, seed_binding, frame,
               nullptr, false, false, 0U, 0);
    const std::array<float, 1U> start{
        static_cast<float>(element.particle_start_frame)};
    const std::array<FieldBinding, 1U> start_binding{
        {{FieldDomain::Particles, {}, scene::schema::particle_start_frame}}};
    vector_row(ctx, cursor, "Start frame", number, start, 1.0F, start_binding,
               frame, nullptr, false, false, 0U, 0);
    if (ctx.button(gui::ui_id(id_, "particles.start", element.node.value()),
                   take_row(cursor, row_height), "Start at the playhead")) {
        for (const auto& item : selected_) {
            if (!item.locked && item.kind == ElementKind::ParticleEffect) {
                frame.named_edits.push_back(
                    NamedEdit{item.node, std::string{scene::schema::particle_start_frame},
                              static_cast<double>(show_frame)});
            }
        }
    }
    cursor.space(row_gap);
    if (!element.particle_status.empty()) {
        ctx.label(take_row(cursor, 20.0F), element.particle_status, ctx.theme().warning);
        cursor.space(row_gap);
    }
    // What the package publishes, each with its key diamond. Changing one
    // changes the effect's past too, so the effect simulates again from its
    // start.
    constexpr std::array<std::string_view, 1U> one{{""}};
    constexpr std::array<std::string_view, 3U> xyz_axes{{"X", "Y", "Z"}};
    constexpr std::array<std::string_view, 4U> rgba{{"R", "G", "B", "A"}};
    for (const ParticleParameterInfo& info : element.particle_parameters) {
        const std::size_t count = std::min<std::size_t>(info.components, 4U);
        std::array<std::string, 4U> keys;
        std::array<std::string, 4U> properties;
        std::array<FieldBinding, 4U> bindings{};
        std::array<float, 4U> values{};
        bool animated = false;
        bool keyed_here = count > 0U;
        for (std::size_t c = 0U; c < count; ++c) {
            keys[c] = info.name + "." + std::to_string(c);
            properties[c] = std::string{scene::schema::particle_parameter_prefix} + keys[c];
            bindings[c] = FieldBinding{FieldDomain::Particles, {}, properties[c]};
            const scene::ChannelKeyframes* const channel = parameter_channel(keyframes_, element.node, keys[c]);
            animated = animated || (channel != nullptr && !channel->frames.empty());
            keyed_here = keyed_here && keyed_at(channel, show_frame);
            const std::optional<double> keyed = sample_at(channel, show_frame);
            values[c] = static_cast<float>(keyed.value_or(parameter_base(element, keys[c]).value_or(0.0)));
        }
        if (info.color) {
            const gui::Rect swatch = take_row(cursor, 14.0F);
            cursor.space(row_gap);
            const float label_width = std::clamp(swatch.width * label_fraction, label_min, label_max);
            colour_well(ctx, gui::Rect{swatch.x + label_width, swatch.y,
                                       std::max(swatch.width - label_width, 1.0F), swatch.height},
                        info.name, {values[0], values[1], values[2]},
                        std::span<const FieldBinding>{bindings.data(), 3U}, frame, false, 0U);
        }
        const std::span<const std::string_view> axes = count == 1U ? std::span<const std::string_view>{one}
            : count == 3U ? std::span<const std::string_view>{xyz_axes}
                          : std::span<const std::string_view>{rgba.data(), count};
        bool key_requested = false;
        const float speed = std::max((info.max - info.min) / 200.0F, 0.001F);
        vector_row(ctx, cursor, info.name, axes, std::span<const float>{values.data(), count}, speed,
                   std::span<const FieldBinding>{bindings.data(), count}, frame, &key_requested, animated,
                   keyed_here, 0U, 3);
        if (key_requested) {
            for (std::size_t c = 0U; c < count; ++c) {
                frame.key_edits.push_back(InspectorKeyEdit{element.node, scene::PropertyChannel::ParticleParameter,
                                                           values[c], keyed_here, keys[c]});
            }
        }
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
        cursor.space(row_gap);
        const float lw = std::clamp(row.width * label_fraction, label_min, label_max);
        ctx.label({row.x, row.y, lw, row.height}, label, ctx.theme().text_muted);
        return ctx.checkbox(gui::ui_id(id_, id, element.node.value() + salt),
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
    cursor.space(row_gap);
    const float feed_label_width = std::clamp(feed_row_bounds.width * label_fraction, label_min, label_max);
    ctx.label({feed_row_bounds.x, feed_row_bounds.y, feed_label_width, row_height}, "Feed", ctx.theme().text_muted);
    if (ctx.dropdown(gui::ui_id(id_, "tracking feed", element.node.value()),
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
        cursor.space(row_gap);
        if (ctx.button(gui::ui_id(id_, "tracking.add", element.node.value()),
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
        // The option a dropdown landed on, written by `feed_toggle` when it
        // returns true for a dropdown row.
        std::size_t chosen = 0U;
        const auto feed_toggle = [&](std::string_view label, std::string_view text,
                                     std::string_view property, bool lit, core::u64 salt) {
            const gui::Rect row = take_row(cursor, row_height);
            cursor.space(row_gap);
            const float lw = std::clamp(row.width * label_fraction, label_min, label_max);
            ctx.label({row.x, row.y, lw, row.height}, label, ctx.theme().text_muted);
            const gui::Rect well{row.x + lw, row.y, row.width - lw, row.height};
            const auto id = gui::ui_id(id_, property, bound->node.value() + salt);
            if (property == scene::schema::tracking_enabled)
                return ctx.checkbox(id, well, lit, "Listening") && !element.locked;
            constexpr std::array<std::string_view, 2> axes_choices{
                "Stage (Y up)", "Z up"};
            const std::span<const std::string_view> choices =
                property == scene::schema::tracking_protocol
                ? std::span<const std::string_view>{tracker_protocol_choices}
                : std::span<const std::string_view>{axes_choices};
            std::size_t selected = 0U;
            for (std::size_t index = 0U; index < choices.size(); ++index) {
                if (choices[index] == text) {
                    selected = index;
                }
            }
            if (!ctx.dropdown(id, well, choices, selected) || element.locked) {
                return false;
            }
            chosen = selected;
            return true;
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
            // The index into `tracker_protocol_choices`; the Director spells
            // it into the document.
            frame.named_edits.push_back(
                NamedEdit{bound->node,
                          std::string{scene::schema::tracking_protocol},
                          static_cast<double>(chosen)});
        }
        const bool natnet =
            settings.protocol == scene::TrackerProtocolKind::NatNet;
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
        // For Motive the camera is a rigid body, named by the streaming id
        // its Assets pane shows; zero takes the first body in every frame.
        feed_row(natnet ? "Rigid body" : "Camera ID",
                 scene::schema::tracking_camera_id,
                 static_cast<float>(settings.camera_id), 1.0F);
        if (natnet) {
            // Motive streams frames to a multicast group and answers the
            // Director's version query on a command port. Both default to
            // Motive's own defaults, and change only when its streaming pane
            // has been changed to match.
            feed_row("Command port", scene::schema::tracking_command_port,
                     static_cast<float>(settings.command_port), 1.0F);
            const gui::Rect row = take_row(cursor, row_height);
            cursor.space(row_gap);
            const float lw = std::clamp(row.width * label_fraction,
                                        label_min, label_max);
            ctx.label({row.x, row.y, lw, row.height}, "Multicast",
                      ctx.theme().text_muted);
            const gui::Rect well{row.x + lw, row.y, row.width - lw,
                                 row.height};
            const std::string authored_group =
                scene::multicast_group_text(settings.multicast_group);
            if (multicast_node_ != bound->node
                || multicast_authored_ != authored_group) {
                multicast_node_ = bound->node;
                multicast_authored_ = authored_group;
                multicast_draft_ = authored_group;
            }
            if (ctx.text_field(gui::ui_id(id_, "tracking.multicast",
                                   bound->node.value()),
                               well, multicast_draft_)
                && !element.locked) {
                std::array<core::u8, 4U> parsed{};
                if (scene::parse_multicast_group(multicast_draft_, parsed)) {
                    frame.text_edits.push_back(
                        {bound->node,
                         std::string{scene::schema::tracking_multicast_group},
                         multicast_draft_});
                }
            }
            std::array<core::u8, 4U> parsed{};
            ctx.tooltip(well,
                        scene::parse_multicast_group(multicast_draft_, parsed)
                            ? "The group Motive streams to, from its "
                              "Streaming pane. 239.255.42.99 is its default."
                            : "Four numbers 0-255 separated by dots");
        }
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
        cursor.space(row_gap);
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
        cursor.space(2.0F);
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
        scroll_.reset();
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
                  "Select an object on Stage or in Scene", ctx.theme().text_muted);
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
    // what the last frame measured until this one has measured itself.
    Cursor cursor{bounds, inspector_metrics(ctx.theme().metrics),
                  scroll_.begin(ctx, bounds, scroll_step)};

    const ElementInfo& header_element =
        displayed != nullptr ? *displayed : selected_.front();
    draw_header(ctx, cursor, header_element,
                std::max(declared_count, selected_.size()));
    if (displayed == nullptr) {
        const gui::Rect locked = take_row(cursor, 34.0F);
        cursor.space(row_gap);
        ctx.panel(locked, ctx.theme().track);
        ctx.label(locked, "Selection is locked", ctx.theme().warning);
        draw_skip_summary(ctx, cursor, frame);
        scroll_.end(ctx, cursor.extent());
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
        draw_transform(ctx, cursor, *displayed, frame, keyframes, show_frame);
        draw_camera(ctx, cursor, *displayed, frame, keyframes, show_frame);
    } else if (displayed->kind == ElementKind::ParticleEffect) {
        draw_transform(ctx, cursor, *displayed, frame, keyframes, show_frame);
        draw_particles(ctx, cursor, *displayed, frame, show_frame);
    } else if (displayed->on_screen()) {
        // A clip on a track is playing on that track's screen, so the screen
        // is what has a place in the scene. Showing the clip's own transform
        // here would offer nine fields that move nothing. It is still on the
        // clip's node and still what the clip returns to when it is dragged
        // out of the track, which is why nothing is erased to say this.
        const gui::Rect note = take_row(cursor, 34.0F);
        cursor.space(row_gap);
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
    } else if (displayed->kind == ElementKind::Track && displayed->vehicle) {
        // A vehicle track owns no plane: nine fields here would move
        // nothing. Its walls are placed one at a time, from the rig below.
    } else {
        draw_transform(ctx, cursor, *displayed, frame, keyframes, show_frame);
    }

    if (displayed->kind == ElementKind::Camera) {
        cursor.space(row_gap);
        draw_camera_tracking(ctx, cursor, *displayed, frame, sources);
    } else if (!is_light(displayed->kind) &&
               displayed->kind != ElementKind::Group &&
               displayed->kind != ElementKind::ParticleEffect) {
        cursor.space(row_gap);
        if (!displayed->linked) draw_content(ctx, cursor, *displayed, frame);
        draw_vehicle(ctx, cursor, *displayed, frame);
        draw_screen(ctx, cursor, *displayed, frame);
        draw_fill(ctx, cursor, *displayed, frame);
        draw_material(ctx, cursor, *displayed, frame);
        draw_compositing(ctx, cursor, *displayed, frame, keyframes, show_frame);
        draw_effects(ctx, cursor, *displayed, frame, keyframes, show_frame,
                     effects);
        if (!displayed->linked) draw_material_extensions(ctx, cursor, *displayed, frame);
    }

    if (displayed->kind == ElementKind::Model && (!displayed->linked || !displayed->source.empty()) &&
        section(ctx, cursor, "MODEL ANIMATION", animation_open_)) {
        cursor.space(row_gap);
        const auto& animation=displayed->animation;
        const auto toggle=[&](std::string_view property,bool value,std::string_view label) {
            if (ctx.checkbox(gui::ui_id(id_,property,displayed->node.value()),take_row(cursor,row_height),value,label))
                for (const auto& item:selected_) if (!item.locked && item.kind==ElementKind::Model)
                    frame.named_edits.push_back({item.node,std::string{property},value?1.0:0.0});
            cursor.space(row_gap);
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
        if (ctx.button(gui::ui_id(id_,"animation.start",displayed->node.value()),take_row(cursor,row_height),"START AT CURRENT SHOW FRAME"))
            for (const auto& item:selected_) if (!item.locked && item.kind==ElementKind::Model)
                frame.text_edits.push_back({item.node,std::string{scene::schema::animation_start_frame},std::to_string(show_frame)});
        if (ctx.button(gui::ui_id(id_,"animation.zero",displayed->node.value()),take_row(cursor,row_height),"START AT FRAME ZERO"))
            for (const auto& item:selected_) if (!item.locked && item.kind==ElementKind::Model)
                frame.text_edits.push_back({item.node,std::string{scene::schema::animation_start_frame},"0"});
        const std::array<float,3> wind{animation.wind.x,animation.wind.y,animation.wind.z};
        const std::array<FieldBinding,3> wind_binding{{{FieldDomain::Animation,{},scene::schema::wind_x},
            {FieldDomain::Animation,{},scene::schema::wind_y},{FieldDomain::Animation,{},scene::schema::wind_z}}};
        vector_row(ctx,cursor,"Wind displacement",xyz,wind,0.01F,wind_binding,frame);
        vector_row(ctx,cursor,"Wind period",frames,period,1.0F,period_binding,frame);
    }
    if ((is_light(displayed->kind) || displayed->kind == ElementKind::Object || displayed->kind == ElementKind::Model) &&
        section(ctx, cursor, "BAKING", baking_open_)) {
        cursor.space(row_gap);
        const auto row = take_row(cursor, row_height);
        const bool light = is_light(displayed->kind);
        bool checked = light ? displayed->bake_indirect : displayed->bake_static;
        const auto property = light ? scene::schema::bake_indirect : scene::schema::bake_static;
        const std::string_view label = light ? "Bake indirect light" : "Static geometry";
        if (displayed->linked_animation) ctx.label(row,"Dynamic node / skin geometry",ctx.theme().text_muted);
        else if(ctx.checkbox(gui::ui_id(id_,"bake contribution",displayed->node.value()),row,checked,label))
            frame.named_edits.push_back(NamedEdit{displayed->node,std::string{property},checked?1.0:0.0});
    }

    draw_skip_summary(ctx, cursor, frame);
    // Destructive actions follow the editing groups instead of interrupting them.
    cursor.space(row_gap * 2.0F);
    const auto actions = take_row(cursor, row_height);
    if (!displayed->linked_collection &&
        pill_button(ctx, gui::ui_id(id_, "delete", 0U),
                    {actions.x, actions.y, std::min(actions.width, 96.0F), actions.height},
                    gui::Icon::Trash, "Delete", "Delete the selection (Del)",
                    ButtonTone::Danger))
        frame.delete_requested = true;
    scroll_.end(ctx, cursor.extent());
    // The bar runs 2px in from the panel's right edge.
    const gui::Rect thumb = scroll_.thumb(
        gui::Rect{bounds.right() - 5.0F, bounds.y, 3.0F, bounds.height}, 24.0F);
    if (thumb.height > 0.0F) {
        ctx.panel(thumb, ctx.theme().text_muted);
    }

    frame.editing = frame.editing || fields_.live() || colour_editing_;
    colour_editing_ = false;
    if (!frame.editing) {
        drag_capture_ = {};
    }
    keyframes_ = {};
    effects_ = {};
    return frame;
}

InspectorFrame InspectorPanel::draw_surface(gui::UiContext& ctx,
                                            const gui::Rect& bounds,
                                            const output::ProjectionSurface&
                                                surface,
                                            std::span<const scene::
                                                          ChannelKeyframes>
                                                keyframes,
                                            core::u64 frame_number) {
    InspectorFrame frame{};

    ctx.panel(bounds, ctx.theme().panel);
    if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
        return frame;
    }
    // Same rule as an element selection: switching surfaces ends a live edit
    // instead of landing it on the newly selected wall.
    if (selection_signature_.size() != 1U ||
        selection_signature_.front() != surface.node ||
        primary_signature_ != surface.node) {
        fields_.cancel(ctx);
        drag_capture_ = {};
        selection_signature_.assign(1U, surface.node);
        primary_signature_ = surface.node;
        scroll_.reset();
    }
    selected_.clear();

    // The wheel scrolls the panel when the pointer is over it, clamped against
    // what the last frame measured until this one has measured itself.
    Cursor cursor{bounds, inspector_metrics(ctx.theme().metrics),
                  scroll_.begin(ctx, bounds, scroll_step)};

    const gui::Rect header = take_row(cursor, 30.0F);
    cursor.space(row_gap);
    ctx.label(gui::Rect{header.x + 8.0F, header.y, header.width - 8.0F,
                        header.height},
              surface.name.empty() ? std::string_view{"LED surface"}
                                   : std::string_view{surface.name},
              ctx.theme().text);
    ctx.label(take_row(cursor, row_height), "LED surface",
              ctx.theme().text_muted);
    cursor.space(row_gap);

    output::PhysicalSurface geometry = surface.geometry;
    FeedSurfaceTransform changed = FeedSurfaceTransform::None;
    bool edited = false;

    // A labelled row of `count` numeric fields, stacked under its label when
    // the dock is too narrow for both, matching vector_row's layout. A row
    // whose components are keyable carries the same diamond as an element's:
    // hollow keys the shown values at the playhead, filled removes that key.
    const auto fields_row = [&](std::string_view label, std::size_t count,
                                std::span<const scene::TransformChannel>
                                    channels = {},
                                std::span<const double> values = {}) {
        gui::Rect row = take_row(cursor, row_height);
        cursor.space(row_gap);
        float label_width =
            std::clamp(row.width * label_fraction, label_min, label_max);
        const float key_width = channels.empty() ? 0.0F : 18.0F;
        const float field_min = ctx.font_width("X -0.000") + 14.0F;
        const bool stacked =
            ctx.font_width(label) + key_width + 10.0F > label_width ||
            row.width - label_width -
                    field_gap * static_cast<float>(count - 1U) <
                static_cast<float>(count) * field_min;
        if (stacked) {
            label_width = row.width;
        }
        ctx.label(gui::Rect{row.x, row.y,
                            std::max(label_width - key_width, 0.0F),
                            row.height},
                  label, ctx.theme().text_muted);
        if (!channels.empty()) {
            bool animated = false;
            bool keyed_here = true;
            for (const scene::TransformChannel channel : channels) {
                const scene::ChannelKeyframes* const keys =
                    channel_for(keyframes, surface.node, channel);
                animated = animated || (keys != nullptr && !keys->frames.empty());
                keyed_here = keyed_here && keyed_at(keys, frame_number);
            }
            const gui::Rect key_bounds{row.x + label_width - key_width, row.y,
                                       key_width, row.height};
            if (key_button(ctx, gui::ui_id(id_, label, 9'000U + surface.node.value()),
                           key_bounds, animated, keyed_here,
                           ctx.theme().panel)) {
                for (std::size_t index = 0U;
                     index < channels.size() && index < values.size();
                     ++index) {
                    frame.key_edits.push_back(InspectorKeyEdit{
                        surface.node, channels[index], values[index],
                        keyed_here});
                }
            }
        }
        if (stacked) {
            row = take_row(cursor, 26.0F);
            cursor.space(6.0F);
        }
        const gui::Rect fields{row.x + (stacked ? 0.0F : label_width), row.y,
                               std::max(row.width -
                                            (stacked ? 0.0F : label_width),
                                        1.0F),
                               row.height};
        std::array<gui::Rect, 3U> boxes{};
        columns(fields, count, field_gap, boxes);
        return boxes;
    };
    const auto number = [&](std::string_view name, const gui::Rect& box,
                            std::string_view axis, double value, double speed,
                            double& out, int precision = 3) {
        return fields_.draw(ctx, gui::ui_id(id_, name, surface.node.value()), box,
                            axis, value, speed, out, precision);
    };

    if (section(ctx, cursor, "TRANSFORM", transform_open_)) {
        const bool mesh =
            surface.shape == output::ProjectionSurfaceShape::ImportedMesh;
        constexpr std::array<FeedSurfaceTransform, 3U> position_bits{
            FeedSurfaceTransform::PositionX, FeedSurfaceTransform::PositionY,
            FeedSurfaceTransform::PositionZ};
        std::array<float*, 3U> center{&geometry.center.x, &geometry.center.y,
                                      &geometry.center.z};
        constexpr std::array<scene::TransformChannel, 3U> position_channels{
            scene::TransformChannel::PositionX,
            scene::TransformChannel::PositionY,
            scene::TransformChannel::PositionZ};
        const std::array<double, 3U> position_values{
            geometry.center.x, geometry.center.y, geometry.center.z};
        const auto position = fields_row(mesh ? "Origin" : "Position", 3U,
                                         position_channels, position_values);
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
            double out{};
            if (number(std::string{"surface.position."} + xyz[axis][0],
                       position[axis], xyz[axis], *center[axis], 0.01, out)) {
                *center[axis] = static_cast<float>(
                    std::clamp(out, -1'000'000.0, 1'000'000.0));
                changed |= position_bits[axis];
                edited = true;
            }
        }

        // Turns, like every other rotation in the Inspector. Yaw turns the
        // face about stage up and keeps its pitch; pitch tilts it about its
        // own right edge, a quarter turn for a ceiling.
        constexpr double tau = 6.28318530717958647692;
        const double yaw_turns =
            std::atan2(-static_cast<double>(geometry.right.z),
                       static_cast<double>(geometry.right.x)) /
            tau;
        constexpr std::array<scene::TransformChannel, 1U> yaw_channel{
            scene::TransformChannel::RotationYaw};
        const std::array<double, 1U> yaw_value{yaw_turns};
        const auto rotation =
            fields_row("Yaw (turns)", 1U, yaw_channel, yaw_value);
        double yaw{};
        if (number("surface.yaw", rotation[0U], "Y", yaw_turns, 0.002, yaw)) {
            output::set_yaw_keeping_pitch(
                geometry, static_cast<float>(std::clamp(yaw, -10.0, 10.0)));
            changed |= FeedSurfaceTransform::Yaw;
            edited = true;
        }
        const output::SurfacePitchBasis pitch_basis =
            output::surface_pitch_basis(geometry);
        const double pitch_turns =
            std::atan2(static_cast<double>(pitch_basis.sine),
                       static_cast<double>(pitch_basis.cosine)) /
            tau;
        const auto tilt = fields_row("Pitch (turns)", 1U);
        gui::Rect pitch_field = tilt[0U];
        const auto set_pitch = [&](double turns) {
            const double current_yaw =
                std::atan2(-static_cast<double>(geometry.right.z),
                           static_cast<double>(geometry.right.x)) /
                tau;
            output::orient(geometry, static_cast<float>(current_yaw),
                           static_cast<float>(turns));
            changed |= FeedSurfaceTransform::Yaw;
            edited = true;
        };
        if (std::abs(pitch_turns) > 1.0e-6) {
            const gui::Rect reset{pitch_field.right() - reset_button_width,
                                  pitch_field.y, reset_button_width,
                                  pitch_field.height};
            pitch_field.width =
                std::max(pitch_field.width - reset_button_width - 2.0F, 1.0F);
            if (reset_button(ctx, gui::ui_id(id_, "surface.pitch.reset",
                                      surface.node.value()),
                             reset, "Reset to upright (0 turns)")) {
                set_pitch(0.0);
            }
        }
        double pitch{};
        if (number("surface.pitch", pitch_field, "P", pitch_turns, 0.002,
                   pitch)) {
            set_pitch(std::clamp(pitch, -0.5, 0.5));
        }

        // Size is calibration, not a keyable transform channel, so it marks no
        // component. A UV mesh carries its own extents.
        if (!mesh) {
            const auto size = fields_row("Size (m)", 2U);
            double width{};
            if (number("surface.width", size[0U], "W", geometry.width, 0.01,
                       width)) {
                geometry.width =
                    static_cast<float>(std::clamp(width, 0.01, 1'000'000.0));
                edited = true;
            }
            double height{};
            if (number("surface.height", size[1U], "H", geometry.height, 0.01,
                       height)) {
                geometry.height =
                    static_cast<float>(std::clamp(height, 0.01, 1'000'000.0));
                edited = true;
            }
        }
        ctx.label(take_row(cursor, row_height),
                  "Shape, pixels and colour are in Feed Mapping",
                  ctx.theme().text_muted);
    }

    // An imported surface without its UV mesh is not document-valid; Feed
    // Mapping refuses to publish one, and so does this.
    if (edited &&
        !(surface.shape == output::ProjectionSurfaceShape::ImportedMesh &&
          surface.mesh_source.empty())) {
        frame.surface_edit = FeedSurfaceEditRequest{
            surface.node,         surface.name,
            geometry,             0U,
            surface.shape,        surface.horizontal_arc_turns,
            surface.mesh_columns, surface.mesh_source,
            surface.mesh_scale,   changed};
    }

    scroll_.end(ctx, cursor.extent());

    frame.editing = fields_.live();
    if (!frame.editing) {
        drag_capture_ = {};
    }
    return frame;
}

} // namespace mediaserver::editor
