#include <mediaserver/editor/color_correction_panel.hpp>

#include <mediaserver/editor/operator_widgets.hpp>
#include <mediaserver/gui/rect_cut.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace mediaserver::editor {
namespace {

[[nodiscard]] gui::UiId mix(gui::UiId base, core::u64 value) noexcept {
    base ^= value + 0x9E3779B97F4A7C15ULL + (base << 6U) + (base >> 2U);
    return base;
}

struct ChannelProperties final {
    std::string_view input_black;
    std::string_view input_white;
    std::string_view gamma;
    std::string_view output_black;
    std::string_view output_white;
};

constexpr std::array<ChannelProperties, 4U> channel_properties{{
    {scene::schema::levels_master_input_black,
     scene::schema::levels_master_input_white,
     scene::schema::levels_master_gamma,
     scene::schema::levels_master_output_black,
     scene::schema::levels_master_output_white},
    {scene::schema::levels_red_input_black,
     scene::schema::levels_red_input_white,
     scene::schema::levels_red_gamma,
     scene::schema::levels_red_output_black,
     scene::schema::levels_red_output_white},
    {scene::schema::levels_green_input_black,
     scene::schema::levels_green_input_white,
     scene::schema::levels_green_gamma,
     scene::schema::levels_green_output_black,
     scene::schema::levels_green_output_white},
    {scene::schema::levels_blue_input_black,
     scene::schema::levels_blue_input_white,
     scene::schema::levels_blue_gamma,
     scene::schema::levels_blue_output_black,
     scene::schema::levels_blue_output_white},
}};

[[nodiscard]] std::string curve_property(std::string_view prefix,
                                         std::size_t index,
                                         std::string_view suffix) {
    return std::string{prefix} + std::to_string(index) + std::string{suffix};
}

[[nodiscard]] gui::UiColor hue_color(float hue, float alpha = 1.0F) noexcept {
    hue -= std::floor(hue);
    const float scaled = hue * 6.0F;
    const int sector = static_cast<int>(std::floor(scaled)) % 6;
    const float fraction = scaled - std::floor(scaled);
    switch (sector) {
    case 0: return {1.0F, fraction, 0.0F, alpha};
    case 1: return {1.0F - fraction, 1.0F, 0.0F, alpha};
    case 2: return {0.0F, 1.0F, fraction, alpha};
    case 3: return {0.0F, 1.0F - fraction, 1.0F, alpha};
    case 4: return {fraction, 0.0F, 1.0F, alpha};
    default: return {1.0F, 0.0F, 1.0F - fraction, alpha};
    }
}

} // namespace

ColorCorrectionFrame ColorCorrectionPanel::draw(
    gui::UiContext& ctx, const gui::Rect& bounds,
    doc::NodeId selected_layer,
    std::span<const scene::EffectInstance> effects) {
    ColorCorrectionFrame frame{};
    if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
        return frame;
    }
    const auto record_nudge = [&](gui::UiId focus,
                                  std::string_view property,
                                  const NumberFieldEdit& edit,
                                  std::size_t edits_before) {
        if (edit.kind == NumberFieldEditKind::Nudge &&
            frame.edits.size() != edits_before) {
            frame.numeric_nudge = NumericNudge{
                focus, gui::ui_id(property), edit.direction, edit.modifiers};
        }
    };

    ctx.panel(bounds, ctx.theme().panel);
    constexpr float header_height = 26.0F;
    gui::Metrics m = ctx.theme().metrics;
    gui::Rect content = bounds;
    gui::Rect header = gui::cut_top(content, header_height);
    ctx.panel(header, ctx.theme().header);
    ctx.panel(gui::cut_bottom(header, m.divider), ctx.theme().divider);

    const scene::EffectInstance* levels{};
    const scene::EffectInstance* lift_gamma_gain{};
    const scene::EffectInstance* contrast_pivot{};
    const scene::EffectInstance* color_matrix{};
    const scene::EffectInstance* white_balance{};
    const scene::EffectInstance* curves{};
    bool colour_effect_switched_off = false;
    for (const scene::EffectInstance& effect : effects) {
        if (effect.target != selected_layer) {
            continue;
        }
        if (!effect.enabled) {
            // This panel grades what is reaching the picture, so a switched-off
            // effect gets no controls here. It is still worth naming: telling
            // an operator to add the effect they just muted in the Inspector is
            // a panel arguing with the one next to it.
            switch (effect.kind) {
            case scene::EffectKind::Levels:
            case scene::EffectKind::LiftGammaGain:
            case scene::EffectKind::ContrastPivot:
            case scene::EffectKind::ColorMatrix:
            case scene::EffectKind::WhiteBalance:
            case scene::EffectKind::Curves:
                colour_effect_switched_off = true;
                break;
            default:
                break;
            }
            continue;
        }
        if (effect.kind == scene::EffectKind::Levels && levels == nullptr) {
            levels = &effect;
        } else if (effect.kind == scene::EffectKind::LiftGammaGain &&
                   lift_gamma_gain == nullptr) {
            lift_gamma_gain = &effect;
        } else if (effect.kind == scene::EffectKind::ContrastPivot &&
                   contrast_pivot == nullptr) {
            contrast_pivot = &effect;
        } else if (effect.kind == scene::EffectKind::ColorMatrix &&
                   color_matrix == nullptr) {
            color_matrix = &effect;
        } else if (effect.kind == scene::EffectKind::WhiteBalance &&
                   white_balance == nullptr) {
            white_balance = &effect;
        } else if (effect.kind == scene::EffectKind::Curves &&
                   curves == nullptr) {
            curves = &effect;
        }
    }
    if (levels == nullptr && lift_gamma_gain == nullptr &&
        contrast_pivot == nullptr && color_matrix == nullptr &&
        white_balance == nullptr && curves == nullptr) {
        fields_.cancel(ctx);
        scroll_.reset();
        constexpr float message_height = 52.0F;
        gui::Rect note = gui::inset(content, m.panel_inset);
        ctx.label(gui::cut_top(note, message_height),
                  selected_layer == doc::NodeId{}
                      ? "Select a layer to grade"
                      : (colour_effect_switched_off
                             ? "This layer's colour effects are switched off"
                             : "Add a colour effect to grade this layer"),
                  colour_effect_switched_off ? ctx.theme().warning
                                             : ctx.theme().text_muted);
        return frame;
    }

    // Sections down a column 8px in from the sides: a 23px heading carrying
    // the section's colour and its RESET, then 25px rows 2px apart, and 5px
    // between sections.
    constexpr float heading_height = 23.0F;
    constexpr float section_space = 5.0F;
    m.inset = 8.0F;
    m.gap = 7.0F;
    m.control = 25.0F;
    m.row_gap = 2.0F;
    m.label_column = std::clamp((content.width - m.inset * 2.0F) * 0.44F,
                                82.0F, 132.0F);
    constexpr float scroll_step = 30.0F;
    gui::FormLayout form{content, m, scroll_.begin(ctx, content, scroll_step)};

    // A section's heading strip: its colour down the left edge and its title,
    // with `title_room` kept clear at the right for the heading's button.
    const auto section_heading = [&](std::string_view title,
                                     gui::UiColor stripe,
                                     float title_room = 61.0F) {
        const gui::Rect heading = form.next(heading_height, m.row_gap);
        ctx.panel(heading, ctx.theme().header);
        gui::Rect rest = heading;
        ctx.panel(gui::cut_left(rest, 3.0F), stripe);
        ctx.label(gui::inset(rest, gui::Padding{6.0F, 0.0F, title_room, 0.0F}),
                  title, ctx.theme().text);
        return heading;
    };
    // A button at the heading's right end: RESET, or ENABLE for an unused
    // curve. Returns true on the frame it is clicked.
    const auto heading_button = [&](const gui::Rect& heading, gui::UiId id,
                                    std::string_view label,
                                    gui::UiColor hover, float width = 58.0F) {
        static_cast<void>(hover);
        gui::Rect strip = heading;
        const gui::Rect button =
            gui::inset(gui::cut_right(strip, width + 14.0F),
                       gui::Padding{0.0F, 2.0F, 0.0F, 2.0F});
        const bool resets = label == "RESET";
        return pill_button(ctx, id, button,
                           resets ? std::optional{gui::Icon::Reset}
                                  : std::nullopt,
                           resets ? std::string_view{"Reset"}
                                  : std::string_view{"Enable"},
                           resets ? "Reset this section to its defaults"
                                  : std::string_view{});
    };
    // A row's value is a member of one of the effects above; its default is
    // the same member of that effect as it is first added, found by where
    // the value sits in it.
    const auto default_of = [&](const float& value) -> std::optional<float> {
        const auto at = reinterpret_cast<std::uintptr_t>(&value);
        for (const scene::EffectInstance* effect :
             {levels, lift_gamma_gain, contrast_pivot, color_matrix,
              white_balance}) {
            if (effect == nullptr) {
                continue;
            }
            const auto begin = reinterpret_cast<std::uintptr_t>(effect);
            if (at < begin || at + sizeof(float) > begin + sizeof(*effect)) {
                continue;
            }
            const scene::EffectInstance fresh =
                scene::initial_effect(effect->kind);
            float out{};
            std::memcpy(&out,
                        reinterpret_cast<const unsigned char*>(&fresh) +
                            (at - begin),
                        sizeof(float));
            return out;
        }
        return std::nullopt;
    };
    // A label and a number field on one row. An entry, clamped into
    // [minimum, maximum], that changes the value is an edit of `property` on
    // `source`.
    const auto number_row = [&](std::string_view label,
                                gui::UiColor label_color, gui::UiId numeric_id,
                                doc::NodeId source, std::string_view property,
                                const float& value, double speed,
                                int decimals, float minimum, float maximum) {
        const gui::FormRow row = form.row();
        const std::optional<float> fallback = default_of(value);
        const bool differs =
            fallback.has_value() &&
            std::abs(value - *fallback) >
                1.0e-5F * std::max(1.0F, std::abs(*fallback));
        gui::Rect label_cell = row.label;
        if (differs) {
            const gui::Rect reset_bounds =
                gui::cut_right(label_cell, reset_button_width);
            std::array<char, 48U> tip{};
            std::snprintf(tip.data(), tip.size(), "Reset to default (%.*f)",
                          decimals, static_cast<double>(*fallback));
            if (reset_button(ctx, numeric_id ^ gui::ui_id("reset"),
                             reset_bounds, tip.data())) {
                frame.edits.push_back(ColorCorrectionEdit{
                    source, std::string{property},
                    static_cast<double>(*fallback)});
            }
        }
        ctx.label(label_cell, label, label_color);
        double authored{};
        NumberFieldEdit numeric_edit{};
        const std::size_t edits_before = frame.edits.size();
        if (fields_.draw(ctx, numeric_id, row.field, {},
                         static_cast<double>(value), speed, authored,
                         decimals, &numeric_edit) &&
            std::isfinite(authored)) {
            const double normalized = std::clamp(
                authored, static_cast<double>(minimum),
                static_cast<double>(maximum));
            if (normalized != static_cast<double>(value)) {
                frame.edits.push_back(ColorCorrectionEdit{
                    source, std::string{property}, normalized});
            }
        }
        record_nudge(numeric_id, property, numeric_edit, edits_before);
    };
    const gui::UiColor muted = ctx.theme().text_muted;

    if (levels != nullptr) {
        const std::array<const scene::LevelsChannelParameters*, 4U> channels{{
            &levels->levels.master, &levels->levels.red, &levels->levels.green,
            &levels->levels.blue,
        }};
        constexpr std::array<std::string_view, 4U> names{{
            "LEVELS / MASTER", "LEVELS / RED", "LEVELS / GREEN",
            "LEVELS / BLUE"}};
        const std::array<gui::UiColor, 4U> colors{{
            ctx.theme().accent,
            gui::UiColor{0.95F, 0.28F, 0.28F, 1.0F},
            gui::UiColor{0.24F, 0.82F, 0.42F, 1.0F},
            gui::UiColor{0.30F, 0.52F, 1.0F, 1.0F},
        }};
        for (std::size_t channel_index = 0U;
             channel_index < channels.size(); ++channel_index) {
            const auto& channel = *channels[channel_index];
            const auto& properties = channel_properties[channel_index];
            const gui::Rect heading =
                section_heading(names[channel_index], colors[channel_index]);
            if (heading_button(
                    heading,
                    mix(id_, levels->source.value() ^
                                 static_cast<core::u64>(0x100U + channel_index)),
                    "RESET", colors[channel_index])) {
                const auto emit_reset = [&](std::string_view property,
                                            double value, double current) {
                    if (current != value) {
                        frame.edits.push_back(ColorCorrectionEdit{
                            levels->source, std::string{property}, value});
                    }
                };
                emit_reset(properties.input_black, 0.0, channel.input_black);
                emit_reset(properties.input_white, 1.0, channel.input_white);
                emit_reset(properties.gamma, 1.0, channel.gamma);
                emit_reset(properties.output_black, 0.0,
                           channel.output_black);
                emit_reset(properties.output_white, 1.0,
                           channel.output_white);
            }

            // Two familiar Levels rails make the tonal mapping readable at a
            // glance and editable in place. The fields below remain for exact
            // entry; they are no longer the only way to shape the image.
            const gui::Rect rails = form.next(52.0F, 0.0F);
            const gui::Rect input_rail =
                gui::inset(rails, gui::Padding{8.0F, 4.0F, 8.0F, 36.0F});
            const gui::Rect output_rail =
                gui::inset(rails, gui::Padding{8.0F, 30.0F, 8.0F, 14.0F});
            constexpr std::array<gui::UiColor, 5U> ramp{{
                {0.04F, 0.04F, 0.04F, 1.0F},
                {0.22F, 0.22F, 0.22F, 1.0F},
                {0.48F, 0.48F, 0.48F, 1.0F},
                {0.74F, 0.74F, 0.74F, 1.0F},
                {0.96F, 0.96F, 0.96F, 1.0F}}};
            for (std::size_t segment = 0U; segment + 1U < ramp.size();
                 ++segment) {
                const float x = input_rail.x + input_rail.width *
                    static_cast<float>(segment) /
                    static_cast<float>(ramp.size() - 1U);
                const float width = input_rail.width /
                    static_cast<float>(ramp.size() - 1U) + 0.5F;
                ctx.panel({x, input_rail.y, width, input_rail.height},
                          ramp[segment]);
                ctx.panel({x, output_rail.y, width, output_rail.height},
                          ramp[segment]);
            }
            const auto direct_handle = [&](core::u64 salt,
                                           std::string_view property,
                                           float current, float position,
                                           const gui::Rect& rail,
                                           float minimum, float maximum,
                                           bool gamma_handle = false) {
                const float px = rail.x + position * rail.width;
                const gui::Rect hit{px - 7.0F, rail.y - 6.0F, 14.0F,
                                    rail.height + 12.0F};
                const gui::Interaction interaction = ctx.grab(
                    mix(id_, levels->source.value() ^ salt), hit);
                const gui::UiColor handle_color = interaction.held
                    ? colors[channel_index]
                    : (interaction.hovered ? ctx.theme().text
                                           : ctx.theme().text_muted);
                ctx.draw_list().add_triangle(
                    px - 5.0F, rail.y - 4.0F, px + 5.0F, rail.y - 4.0F,
                    px, rail.y + 3.0F, handle_color);
                ctx.draw_list().add_line(
                    px, rail.y + 2.0F, px, rail.bottom() + 4.0F,
                    interaction.held ? 2.0F : 1.0F, handle_color);
                if (!interaction.held) {
                    return;
                }
                float authored = std::clamp(
                    (ctx.pointer_x() - rail.x) /
                        std::max(rail.width, 1.0F),
                    0.0F, 1.0F);
                if (gamma_handle) {
                    authored = std::clamp(
                        (authored - channel.input_black) /
                            std::max(channel.input_white -
                                         channel.input_black,
                                     0.0001F),
                        0.0001F, 0.9999F);
                    authored = std::log(authored) / std::log(0.5F);
                }
                authored = std::clamp(authored, minimum, maximum);
                if (authored != current) {
                    frame.edits.push_back(ColorCorrectionEdit{
                        levels->source, std::string{property}, authored});
                }
                frame.editing = true;
            };
            const float gamma_position = channel.input_black +
                (channel.input_white - channel.input_black) *
                    std::pow(0.5F, channel.gamma);
            direct_handle(channel_index * 32U + 0x1000U,
                          properties.input_black, channel.input_black,
                          channel.input_black, input_rail, 0.0F,
                          std::max(channel.input_white - 0.0001F, 0.0F));
            direct_handle(channel_index * 32U + 0x1001U,
                          properties.gamma, channel.gamma, gamma_position,
                          input_rail, scene::min_levels_gamma,
                          scene::max_levels_gamma, true);
            direct_handle(channel_index * 32U + 0x1002U,
                          properties.input_white, channel.input_white,
                          channel.input_white, input_rail,
                          std::min(channel.input_black + 0.0001F, 1.0F),
                          1.0F);
            direct_handle(channel_index * 32U + 0x1003U,
                          properties.output_black, channel.output_black,
                          channel.output_black, output_rail, 0.0F, 1.0F);
            direct_handle(channel_index * 32U + 0x1004U,
                          properties.output_white, channel.output_white,
                          channel.output_white, output_rail, 0.0F, 1.0F);
            // Each rail's name just under it.
            ctx.label({input_rail.x, input_rail.bottom(), 52.0F, 12.0F},
                      "INPUT", muted);
            ctx.label({output_rail.x, output_rail.bottom(), 58.0F, 12.0F},
                      "OUTPUT", muted);

            const core::u64 channel_salt =
                static_cast<core::u64>(channel_index) * 16U;
            const auto field = [&](std::string_view label,
                                   std::string_view property, float value,
                                   double speed, float minimum, float maximum,
                                   core::u64 salt) {
                number_row(label, muted,
                           mix(id_, levels->source.value() ^ salt),
                           levels->source, property, value, speed, 3,
                           minimum, maximum);
            };
            field("Input Black", properties.input_black, channel.input_black,
                  0.005, 0.0F,
                  std::max(channel.input_white - 0.0001F, 0.0F),
                  channel_salt + 1U);
            field("Input White", properties.input_white, channel.input_white,
                  0.005,
                  std::min(channel.input_black + 0.0001F, 1.0F), 1.0F,
                  channel_salt + 2U);
            field("Gamma", properties.gamma, channel.gamma, 0.01,
                  scene::min_levels_gamma, scene::max_levels_gamma,
                  channel_salt + 3U);
            field("Output Black", properties.output_black,
                  channel.output_black, 0.005, 0.0F, 1.0F,
                  channel_salt + 4U);
            field("Output White", properties.output_white,
                  channel.output_white, 0.005, 0.0F, 1.0F,
                  channel_salt + 5U);
            form.space(section_space);
        }
    }

    if (lift_gamma_gain != nullptr) {
        struct VectorSection final {
            std::string_view name;
            scene::Vec3 value;
            scene::Vec3 identity;
            std::array<std::string_view, 3U> properties;
            float minimum;
            float maximum;
        };
        const auto& parameters = lift_gamma_gain->lift_gamma_gain;
        const std::array<VectorSection, 3U> sections{{
            {"LIFT", parameters.lift, scene::Vec3{},
             {scene::schema::lift_r, scene::schema::lift_g,
              scene::schema::lift_b},
             scene::min_lift_gamma_gain_lift,
             scene::max_lift_gamma_gain_lift},
            {"GAMMA", parameters.gamma, scene::Vec3{1.0F, 1.0F, 1.0F},
             {scene::schema::gamma_r, scene::schema::gamma_g,
              scene::schema::gamma_b},
             scene::min_lift_gamma_gain_gamma,
             scene::max_lift_gamma_gain_gamma},
            {"GAIN", parameters.gain, scene::Vec3{1.0F, 1.0F, 1.0F},
             {scene::schema::gain_r, scene::schema::gain_g,
              scene::schema::gain_b},
             scene::min_lift_gamma_gain_gain,
             scene::max_lift_gamma_gain_gain},
        }};
        constexpr std::array<std::string_view, 3U> component_names{{
            "Red", "Green", "Blue"}};
        const std::array<gui::UiColor, 3U> component_colors{{
            gui::UiColor{0.95F, 0.28F, 0.28F, 1.0F},
            gui::UiColor{0.24F, 0.82F, 0.42F, 1.0F},
            gui::UiColor{0.30F, 0.52F, 1.0F, 1.0F},
        }};
        for (std::size_t section_index = 0U;
             section_index < sections.size(); ++section_index) {
            const VectorSection& section = sections[section_index];
            const gui::Rect heading = section_heading(
                std::string{"LIFT / GAMMA / GAIN  /  "} +
                    std::string{section.name},
                gui::UiColor{0.94F, 0.62F, 0.24F, 1.0F});
            const std::array<float, 3U> values{{
                section.value.x, section.value.y, section.value.z}};
            const std::array<float, 3U> identities{{
                section.identity.x, section.identity.y, section.identity.z}};
            if (heading_button(
                    heading,
                    mix(id_, lift_gamma_gain->source.value() ^
                                 static_cast<core::u64>(0x400U + section_index)),
                    "RESET", ctx.theme().keyframe)) {
                for (std::size_t component = 0U; component < 3U; ++component) {
                    if (values[component] != identities[component]) {
                        frame.edits.push_back(ColorCorrectionEdit{
                            lift_gamma_gain->source,
                            std::string{section.properties[component]},
                            static_cast<double>(identities[component])});
                    }
                }
            }
            // The colour wheel, and the luma slider beside it with its "Y"
            // underneath.
            const float wheel_size =
                std::clamp(form.width() - 38.0F, 78.0F, 118.0F);
            gui::Rect wheel_row = form.next(wheel_size, 18.0F);
            const gui::Rect wheel = gui::cut_left(wheel_row, wheel_size);
            static_cast<void>(gui::cut_left(wheel_row, 12.0F));
            const gui::Rect luma = gui::cut_left(wheel_row, 18.0F);
            const float centre_x = wheel.center_x();
            const float centre_y = wheel.center_y();
            const float radius = std::max(wheel.width * 0.5F - 4.0F, 1.0F);
            constexpr std::size_t wheel_segments = 48U;
            for (std::size_t segment = 0U; segment < wheel_segments;
                 ++segment) {
                constexpr float tau = 6.28318530717958647692F;
                const float a0 = tau * static_cast<float>(segment) /
                                 static_cast<float>(wheel_segments);
                const float a1 = tau * static_cast<float>(segment + 1U) /
                                 static_cast<float>(wheel_segments);
                const std::array<gui::UiPoint, 3U> wedge{{
                    {centre_x, centre_y},
                    {centre_x + std::cos(a0) * radius,
                     centre_y + std::sin(a0) * radius},
                    {centre_x + std::cos(a1) * radius,
                     centre_y + std::sin(a1) * radius}}};
                ctx.draw_list().add_convex_poly(
                    wedge, hue_color(static_cast<float>(segment) /
                                         static_cast<float>(wheel_segments),
                                     0.58F));
            }
            ctx.draw_list().add_line(centre_x - radius, centre_y,
                                     centre_x + radius, centre_y, 1.0F,
                                     ctx.theme().divider);
            ctx.draw_list().add_line(centre_x, centre_y - radius,
                                     centre_x, centre_y + radius, 1.0F,
                                     ctx.theme().divider);
            const float mean =
                (values[0U] + values[1U] + values[2U]) / 3.0F;
            const float chroma_x =
                (2.0F * values[0U] - values[1U] - values[2U]) / 3.0F;
            const float chroma_y =
                (values[1U] - values[2U]) / 1.7320508075688772F;
            const float chroma_span =
                std::max((section.maximum - section.minimum) * 0.35F,
                         0.001F);
            float knob_x = chroma_x / chroma_span;
            float knob_y = chroma_y / chroma_span;
            const float knob_length =
                std::sqrt(knob_x * knob_x + knob_y * knob_y);
            if (knob_length > 1.0F) {
                knob_x /= knob_length;
                knob_y /= knob_length;
            }
            const gui::Interaction wheel_it = ctx.grab(
                mix(id_, lift_gamma_gain->source.value() ^
                             (0x1400U + section_index)),
                wheel);
            if (wheel_it.held) {
                float x = (ctx.pointer_x() - centre_x) / radius;
                float held_y = (ctx.pointer_y() - centre_y) / radius;
                const float length = std::sqrt(x * x + held_y * held_y);
                if (length > 1.0F) {
                    x /= length;
                    held_y /= length;
                }
                const float new_x = x * chroma_span;
                const float new_y = held_y * chroma_span;
                const std::array<float, 3U> changed{{
                    mean + new_x,
                    mean - new_x * 0.5F +
                        0.8660254037844386F * new_y,
                    mean - new_x * 0.5F -
                        0.8660254037844386F * new_y}};
                for (std::size_t component = 0U; component < 3U;
                     ++component) {
                    const float authored = std::clamp(
                        changed[component], section.minimum,
                        section.maximum);
                    if (authored != values[component]) {
                        frame.edits.push_back(ColorCorrectionEdit{
                            lift_gamma_gain->source,
                            std::string{section.properties[component]},
                            authored});
                    }
                }
                knob_x = x;
                knob_y = held_y;
                frame.editing = true;
            }
            const float marker_x = centre_x + knob_x * radius;
            const float marker_y = centre_y + knob_y * radius;
            ctx.draw_list().add_rect(marker_x - 4.0F, marker_y - 4.0F,
                                     8.0F, 8.0F, ctx.theme().track);
            ctx.draw_list().add_rect(marker_x - 2.0F, marker_y - 2.0F,
                                     4.0F, 4.0F, ctx.theme().text);

            ctx.panel(gui::centered(luma, 4.0F, luma.height),
                      ctx.theme().track);
            const float luma_t = std::clamp(
                (mean - section.minimum) /
                    std::max(section.maximum - section.minimum, 0.001F),
                0.0F, 1.0F);
            float luma_y = luma.y + (1.0F - luma_t) * luma.height;
            const gui::Interaction luma_it = ctx.grab(
                mix(id_, lift_gamma_gain->source.value() ^
                             (0x1500U + section_index)),
                luma);
            if (luma_it.held) {
                const float t = std::clamp(
                    1.0F - (ctx.pointer_y() - luma.y) /
                                   std::max(luma.height, 1.0F),
                    0.0F, 1.0F);
                const float new_mean = section.minimum +
                    (section.maximum - section.minimum) * t;
                const float delta = new_mean - mean;
                for (std::size_t component = 0U; component < 3U;
                     ++component) {
                    const float authored = std::clamp(
                        values[component] + delta, section.minimum,
                        section.maximum);
                    if (authored != values[component]) {
                        frame.edits.push_back(ColorCorrectionEdit{
                            lift_gamma_gain->source,
                            std::string{section.properties[component]},
                            authored});
                    }
                }
                luma_y = ctx.pointer_y();
                frame.editing = true;
            }
            ctx.draw_list().add_line(luma.x + 2.0F, luma_y,
                                     luma.right() - 2.0F, luma_y,
                                     3.0F, ctx.theme().text);
            ctx.label({luma.x - 5.0F, luma.bottom() + 1.0F,
                       luma.width + 10.0F, 14.0F},
                      "Y", muted);
            for (std::size_t component = 0U; component < 3U; ++component) {
                const core::u64 salt =
                    0x500U + static_cast<core::u64>(section_index) * 8U +
                    static_cast<core::u64>(component);
                number_row(component_names[component],
                           component_colors[component],
                           mix(id_, lift_gamma_gain->source.value() ^ salt),
                           lift_gamma_gain->source,
                           section.properties[component], values[component],
                           0.01, 3, section.minimum, section.maximum);
            }
            form.space(section_space);
        }
    }

    if (contrast_pivot != nullptr) {
        const auto& parameters = contrast_pivot->contrast_pivot;
        const gui::Rect heading = section_heading(
            "CONTRAST / PIVOT", gui::UiColor{0.72F, 0.48F, 0.98F, 1.0F});
        if (heading_button(heading,
                           mix(id_, contrast_pivot->source.value() ^ 0x700U),
                           "RESET", ctx.theme().keyframe)) {
            if (parameters.contrast != 1.0F) {
                frame.edits.push_back(ColorCorrectionEdit{
                    contrast_pivot->source,
                    std::string{scene::schema::contrast}, 1.0});
            }
            if (parameters.pivot != 0.5F) {
                frame.edits.push_back(ColorCorrectionEdit{
                    contrast_pivot->source,
                    std::string{scene::schema::pivot}, 0.5});
            }
        }
        const auto field = [&](std::string_view label,
                               std::string_view property, float value,
                               float minimum, float maximum, core::u64 salt) {
            number_row(label, muted,
                       mix(id_, contrast_pivot->source.value() ^ salt),
                       contrast_pivot->source, property, value, 0.01, 3,
                       minimum, maximum);
        };
        field("Contrast", scene::schema::contrast, parameters.contrast,
              scene::min_contrast_pivot_contrast,
              scene::max_contrast_pivot_contrast, 0x701U);
        field("Pivot", scene::schema::pivot, parameters.pivot,
              scene::min_contrast_pivot_pivot,
              scene::max_contrast_pivot_pivot, 0x702U);
        form.space(section_space);
    }

    if (white_balance != nullptr) {
        const auto& parameters = white_balance->white_balance;
        const gui::Rect heading = section_heading(
            "WHITE BALANCE", gui::UiColor{0.98F, 0.70F, 0.42F, 1.0F});
        if (heading_button(heading,
                           mix(id_, white_balance->source.value() ^ 0xA00U),
                           "RESET", ctx.theme().keyframe)) {
            if (parameters.temperature_kelvin !=
                scene::d65_white_balance_temperature_kelvin) {
                frame.edits.push_back(ColorCorrectionEdit{
                    white_balance->source,
                    std::string{scene::schema::temperature_kelvin},
                    scene::d65_white_balance_temperature_kelvin});
            }
            if (parameters.tint != 0.0F) {
                frame.edits.push_back(ColorCorrectionEdit{
                    white_balance->source, std::string{scene::schema::tint},
                    0.0});
            }
        }
        const auto field = [&](std::string_view label,
                               std::string_view property, float value,
                               float minimum, float maximum, double speed,
                               int decimals, core::u64 salt) {
            number_row(label, muted,
                       mix(id_, white_balance->source.value() ^ salt),
                       white_balance->source, property, value, speed,
                       decimals, minimum, maximum);
        };
        field("Temperature (K)", scene::schema::temperature_kelvin,
              parameters.temperature_kelvin,
              scene::min_white_balance_temperature_kelvin,
              scene::max_white_balance_temperature_kelvin, 25.0, 0, 0xA01U);
        field("Tint", scene::schema::tint, parameters.tint,
              scene::min_white_balance_tint, scene::max_white_balance_tint,
              0.01, 3, 0xA02U);
        form.space(section_space);
    }

    if (curves != nullptr) {
        struct CurveSection final {
            std::string_view name;
            std::string_view prefix;
            const scene::CurveChannelParameters* channel{};
            std::size_t lut_component{};
            gui::UiColor color;
        };
        const auto& parameters = curves->curves;
        const std::array<CurveSection, 4U> sections{{
            {"MASTER", scene::schema::curve_master_prefix,
             &parameters.master, 0U, ctx.theme().accent},
            {"RED", scene::schema::curve_red_prefix,
             parameters.red ? &*parameters.red : nullptr, 1U,
             gui::UiColor{0.95F, 0.28F, 0.28F, 1.0F}},
            {"GREEN", scene::schema::curve_green_prefix,
             parameters.green ? &*parameters.green : nullptr, 2U,
             gui::UiColor{0.24F, 0.82F, 0.42F, 1.0F}},
            {"BLUE", scene::schema::curve_blue_prefix,
             parameters.blue ? &*parameters.blue : nullptr, 3U,
             gui::UiColor{0.30F, 0.52F, 1.0F, 1.0F}},
        }};
        for (std::size_t section_index = 0U;
             section_index < sections.size(); ++section_index) {
            const CurveSection& section = sections[section_index];
            if (section.channel == nullptr) {
                // An unused curve is a heading with ENABLE, set apart by
                // its own space rather than a section's.
                const gui::Rect heading = form.next(heading_height, 8.0F);
                ctx.panel(heading, ctx.theme().header);
                gui::Rect rest = heading;
                ctx.panel(gui::cut_left(rest, 3.0F), section.color);
                ctx.label(gui::inset(rest, gui::Padding{6.0F, 0.0F, 103.0F, 0.0F}),
                          std::string{"CURVES  /  "} + std::string{section.name},
                          ctx.theme().text);
                if (heading_button(
                        heading,
                        mix(id_, curves->source.value() ^
                                     static_cast<core::u64>(0xC00U +
                                         section_index)),
                        "ENABLE", section.color, 54.0F)) {
                    for (std::size_t point = 0U; point < 2U; ++point) {
                        const double value = static_cast<double>(point);
                        frame.edits.push_back(ColorCorrectionEdit{
                            curves->source,
                            curve_property(section.prefix, point,
                                           scene::schema::curve_x_suffix),
                            value});
                        frame.edits.push_back(ColorCorrectionEdit{
                            curves->source,
                            curve_property(section.prefix, point,
                                           scene::schema::curve_y_suffix),
                            value});
                    }
                }
                continue;
            }

            const auto& points = section.channel->points;
            const gui::Rect heading = section_heading(
                std::string{"CURVES  /  "} + std::string{section.name},
                section.color, 103.0F);
            const bool can_add = points.size() < scene::max_curve_control_points;
            if (heading_button(
                    heading,
                    mix(id_, curves->source.value() ^
                                 static_cast<core::u64>(0xC10U + section_index)),
                    "RESET", section.color)) {
                for (std::size_t point = 0U; point < points.size(); ++point) {
                    if (points[point].y != points[point].x) {
                        frame.edits.push_back(ColorCorrectionEdit{
                            curves->source,
                            curve_property(section.prefix, point,
                                           scene::schema::curve_y_suffix),
                            static_cast<double>(points[point].x)});
                    }
                }
            }

            constexpr float plot_height = 112.0F;
            const gui::Rect plot = form.next(plot_height, m.row_gap);
            ctx.panel(plot, ctx.theme().track);
            const gui::Interaction plot_it = ctx.grab(
                mix(id_, curves->source.value() ^
                             static_cast<core::u64>(0xC20U + section_index)),
                plot);
            for (std::size_t division = 1U; division < 4U; ++division) {
                const float fraction = static_cast<float>(division) / 4.0F;
                ctx.draw_list().add_line(
                    plot.x + plot.width * fraction, plot.y,
                    plot.x + plot.width * fraction, plot.bottom(),
                    1.0F, ctx.theme().divider);
                ctx.draw_list().add_line(
                    plot.x, plot.y + plot.height * fraction,
                    plot.right(), plot.y + plot.height * fraction,
                    1.0F, ctx.theme().divider);
            }
            const auto sample_value = [&](const scene::Vec4& value) noexcept {
                switch (section.lut_component) {
                case 0U: return value.x;
                case 1U: return value.y;
                case 2U: return value.z;
                default: return value.w;
                }
            };
            if (parameters.lut.size() == scene::curve_lut_entries) {
                float last_x = plot.x;
                float last_y = plot.bottom() -
                    std::clamp(sample_value(parameters.lut.front()), 0.0F, 1.0F) *
                        plot.height;
                for (std::size_t sample = 1U; sample < parameters.lut.size();
                     ++sample) {
                    const float x = plot.x + plot.width *
                        static_cast<float>(sample) /
                        static_cast<float>(parameters.lut.size() - 1U);
                    const float value = std::clamp(
                        sample_value(parameters.lut[sample]), 0.0F, 1.0F);
                    const float sample_y = plot.bottom() -
                        value * plot.height;
                    ctx.draw_list().add_line(last_x, last_y, x, sample_y,
                                             1.5F, section.color);
                    last_x = x;
                    last_y = sample_y;
                }
            }
            if (plot_it.pressed) {
                float nearest = 10.0F;
                std::size_t nearest_point = points.size();
                for (std::size_t point = 0U; point < points.size(); ++point) {
                    const float px = plot.x + points[point].x * plot.width;
                    const float py = plot.bottom() -
                        points[point].y * plot.height;
                    const float dx = ctx.pointer_x() - px;
                    const float dy = ctx.pointer_y() - py;
                    const float distance = std::sqrt(dx * dx + dy * dy);
                    if (distance < nearest) {
                        nearest = distance;
                        nearest_point = point;
                    }
                }
                if (nearest_point < points.size()) {
                    curve_drag_effect_ = curves->source;
                    curve_drag_section_ = section_index;
                    curve_drag_point_ = nearest_point;
                } else if (can_add && points.size() >= 2U) {
                    const float added_x = std::clamp(
                        (ctx.pointer_x() - plot.x) /
                            std::max(plot.width, 1.0F),
                        0.0001F, 0.9999F);
                    const float added_y = std::clamp(
                        1.0F - (ctx.pointer_y() - plot.y) /
                                   std::max(plot.height, 1.0F),
                        0.0F, 1.0F);
                    std::size_t insertion = 1U;
                    while (insertion + 1U < points.size() &&
                           points[insertion].x < added_x) {
                        ++insertion;
                    }
                    for (std::size_t destination = points.size();
                         destination > insertion; --destination) {
                        const auto held = points[destination - 1U];
                        frame.edits.push_back(ColorCorrectionEdit{
                            curves->source,
                            curve_property(section.prefix, destination,
                                           scene::schema::curve_x_suffix),
                            held.x});
                        frame.edits.push_back(ColorCorrectionEdit{
                            curves->source,
                            curve_property(section.prefix, destination,
                                           scene::schema::curve_y_suffix),
                            held.y});
                    }
                    frame.edits.push_back(ColorCorrectionEdit{
                        curves->source,
                        curve_property(section.prefix, insertion,
                                       scene::schema::curve_x_suffix),
                        added_x});
                    frame.edits.push_back(ColorCorrectionEdit{
                        curves->source,
                        curve_property(section.prefix, insertion,
                                       scene::schema::curve_y_suffix),
                        added_y});
                    frame.editing = true;
                }
            }
            if (plot_it.held && curve_drag_effect_ == curves->source &&
                curve_drag_section_ == section_index &&
                curve_drag_point_ < points.size()) {
                const std::size_t point = curve_drag_point_;
                const float minimum_x = point == 0U
                    ? 0.0F
                    : points[point - 1U].x + 0.0001F;
                const float maximum_x = point + 1U == points.size()
                    ? 1.0F
                    : points[point + 1U].x - 0.0001F;
                const float authored_x = point == 0U
                    ? 0.0F
                    : (point + 1U == points.size()
                           ? 1.0F
                           : std::clamp((ctx.pointer_x() - plot.x) /
                                            std::max(plot.width, 1.0F),
                                        minimum_x, maximum_x));
                const float authored_y = std::clamp(
                    1.0F - (ctx.pointer_y() - plot.y) /
                               std::max(plot.height, 1.0F),
                    0.0F, 1.0F);
                if (authored_x != points[point].x) {
                    frame.edits.push_back(ColorCorrectionEdit{
                        curves->source,
                        curve_property(section.prefix, point,
                                       scene::schema::curve_x_suffix),
                        authored_x});
                }
                if (authored_y != points[point].y) {
                    frame.edits.push_back(ColorCorrectionEdit{
                        curves->source,
                        curve_property(section.prefix, point,
                                       scene::schema::curve_y_suffix),
                        authored_y});
                }
                frame.editing = true;
            }
            if (plot_it.released && curve_drag_effect_ == curves->source &&
                curve_drag_section_ == section_index) {
                curve_drag_effect_ = {};
            }
            for (std::size_t point_index = 0U;
                 point_index < points.size(); ++point_index) {
                const scene::CurveControlPoint point = points[point_index];
                const float px = plot.x + point.x * plot.width;
                const float py = plot.bottom() - point.y * plot.height;
                const bool active = curve_drag_effect_ == curves->source &&
                    curve_drag_section_ == section_index &&
                    curve_drag_point_ == point_index;
                const float size = active ? 8.0F : 6.0F;
                ctx.draw_list().add_rect(px - size * 0.5F,
                                         py - size * 0.5F, size, size,
                                         active ? ctx.theme().text
                                                : section.color);
            }
            {
                gui::Rect hint = gui::inset(plot, 7.0F, 4.0F);
                ctx.label(gui::cut_top(hint, 14.0F),
                          can_add ? "CLICK TO ADD  /  DRAG POINTS"
                                  : "DRAG POINTS",
                          muted);
            }

            for (std::size_t point = 1U; point + 1U < points.size(); ++point) {
                const auto point_field = [&](std::string_view axis,
                                             std::string_view suffix,
                                             float value, float minimum,
                                             float maximum, core::u64 salt) {
                    const std::string label = "Point " +
                        std::to_string(point) + " " + std::string{axis};
                    const std::string property =
                        curve_property(section.prefix, point, suffix);
                    number_row(label, muted,
                               mix(id_, curves->source.value() ^ salt ^
                                            static_cast<core::u64>(point * 2U)),
                               curves->source, property, value, 0.005, 3,
                               minimum, maximum);
                };
                const float minimum_x = points[point - 1U].x + 0.0001F;
                const float maximum_x = points[point + 1U].x - 0.0001F;
                point_field("X", scene::schema::curve_x_suffix,
                            points[point].x, minimum_x, maximum_x,
                            0xD00U + static_cast<core::u64>(section_index) *
                                0x80U);
                point_field("Y", scene::schema::curve_y_suffix,
                            points[point].y, 0.0F, 1.0F,
                            0xD40U + static_cast<core::u64>(section_index) *
                                0x80U);
            }
            form.space(section_space);
        }
    }

    if (color_matrix != nullptr) {
        struct MatrixSection final {
            std::string_view name;
            std::array<float, 4U> values;
            std::array<float, 4U> identities;
            std::array<std::string_view, 4U> properties;
            gui::UiColor color;
        };
        const auto& parameters = color_matrix->color_matrix;
        const std::array<MatrixSection, 3U> sections{{
            {"RED OUTPUT",
             {parameters.matrix[0U], parameters.matrix[1U],
              parameters.matrix[2U], parameters.offset.x},
             {1.0F, 0.0F, 0.0F, 0.0F},
             {scene::schema::m00, scene::schema::m01, scene::schema::m02,
              scene::schema::offset_r},
             gui::UiColor{0.95F, 0.28F, 0.28F, 1.0F}},
            {"GREEN OUTPUT",
             {parameters.matrix[3U], parameters.matrix[4U],
              parameters.matrix[5U], parameters.offset.y},
             {0.0F, 1.0F, 0.0F, 0.0F},
             {scene::schema::m10, scene::schema::m11, scene::schema::m12,
              scene::schema::offset_g},
             gui::UiColor{0.24F, 0.82F, 0.42F, 1.0F}},
            {"BLUE OUTPUT",
             {parameters.matrix[6U], parameters.matrix[7U],
              parameters.matrix[8U], parameters.offset.z},
             {0.0F, 0.0F, 1.0F, 0.0F},
             {scene::schema::m20, scene::schema::m21, scene::schema::m22,
              scene::schema::offset_b},
             gui::UiColor{0.30F, 0.52F, 1.0F, 1.0F}},
        }};
        constexpr std::array<std::string_view, 4U> field_names{{
            "From Red", "From Green", "From Blue", "Offset"}};
        for (std::size_t section_index = 0U;
             section_index < sections.size(); ++section_index) {
            const MatrixSection& section = sections[section_index];
            const gui::Rect heading = section_heading(
                std::string{"CHANNEL MIXER  /  "} + std::string{section.name},
                section.color);
            if (heading_button(
                    heading,
                    mix(id_, color_matrix->source.value() ^
                                 static_cast<core::u64>(0x800U + section_index)),
                    "RESET", section.color)) {
                for (std::size_t index = 0U; index < 4U; ++index) {
                    if (section.values[index] != section.identities[index]) {
                        frame.edits.push_back(ColorCorrectionEdit{
                            color_matrix->source,
                            std::string{section.properties[index]},
                            static_cast<double>(section.identities[index])});
                    }
                }
            }
            for (std::size_t index = 0U; index < 4U; ++index) {
                const core::u64 salt =
                    0x900U + static_cast<core::u64>(section_index) * 8U +
                    static_cast<core::u64>(index);
                number_row(field_names[index], muted,
                           mix(id_, color_matrix->source.value() ^ salt),
                           color_matrix->source, section.properties[index],
                           section.values[index], 0.01, 3,
                           scene::min_color_matrix_coefficient,
                           scene::max_color_matrix_coefficient);
            }
            form.space(section_space);
        }
    }
    scroll_.end(ctx, form.extent());

    frame.editing = frame.editing || fields_.live() ||
        curve_drag_effect_ != doc::NodeId{};
    return frame;
}

} // namespace mediaserver::editor
