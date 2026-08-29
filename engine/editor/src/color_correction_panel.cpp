#include <mediaserver/editor/color_correction_panel.hpp>

#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <cmath>
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
    const gui::Rect header{bounds.x, bounds.y, bounds.width, header_height};
    ctx.panel(header, ctx.theme().header);
    ctx.label(gui::Rect{header.x + 8.0F, header.y,
                        std::max(header.width - 16.0F, 0.0F), header.height},
              "COLOR CORRECTION", ctx.theme().text);
    ctx.panel(gui::Rect{header.x, header.y + header.height - 1.0F,
                        header.width, 1.0F},
              ctx.theme().divider);

    const scene::EffectInstance* levels{};
    const scene::EffectInstance* lift_gamma_gain{};
    const scene::EffectInstance* contrast_pivot{};
    const scene::EffectInstance* color_matrix{};
    const scene::EffectInstance* white_balance{};
    const scene::EffectInstance* curves{};
    for (const scene::EffectInstance& effect : effects) {
        if (effect.target != selected_layer || !effect.enabled) {
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
    const gui::Rect content{bounds.x, bounds.y + header_height, bounds.width,
                            std::max(bounds.height - header_height, 0.0F)};
    if (levels == nullptr && lift_gamma_gain == nullptr &&
        contrast_pivot == nullptr && color_matrix == nullptr &&
        white_balance == nullptr && curves == nullptr) {
        fields_.cancel(ctx);
        scroll_ = 0.0F;
        content_height_ = 0.0F;
        ctx.label(gui::Rect{content.x + 10.0F, content.y + 10.0F,
                            std::max(content.width - 20.0F, 0.0F), 52.0F},
                  selected_layer == doc::NodeId{}
                      ? "Select a layer to grade"
                      : "Add a colour effect to grade this layer",
                  ctx.theme().text_muted);
        return frame;
    }

    const float maximum_scroll =
        std::max(content_height_ - content.height, 0.0F);
    if (content.contains(ctx.pointer_x(), ctx.pointer_y()) &&
        ctx.wheel() != 0.0F) {
        scroll_ = std::clamp(scroll_ - ctx.wheel() * 30.0F,
                             0.0F, maximum_scroll);
    } else {
        scroll_ = std::clamp(scroll_, 0.0F, maximum_scroll);
    }

    constexpr float inset = 8.0F;
    constexpr float heading_height = 23.0F;
    constexpr float row_height = 25.0F;
    constexpr float row_gap = 2.0F;
    float y = content.y + 7.0F - scroll_;
    ctx.push_clip(content);
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
            const gui::Rect heading{
                content.x + inset, y,
                std::max(content.width - inset * 2.0F, 0.0F), heading_height};
            ctx.panel(heading, ctx.theme().header);
            ctx.panel(gui::Rect{heading.x, heading.y, 3.0F, heading.height},
                      colors[channel_index]);
            ctx.label(gui::Rect{heading.x + 9.0F, heading.y,
                                std::max(heading.width - 70.0F, 0.0F),
                                heading.height},
                      names[channel_index], ctx.theme().text);
            const gui::Rect reset{
                heading.x + std::max(heading.width - 58.0F, 0.0F), heading.y,
                58.0F, heading.height};
            const gui::Interaction reset_it = ctx.grab(
                mix(id_, levels->source.value() ^
                             static_cast<core::u64>(0x100U + channel_index)),
                reset);
            ctx.panel(reset, reset_it.hovered ? ctx.theme().widget_hover
                                              : ctx.theme().widget);
            ctx.label(reset, "RESET",
                      reset_it.hovered ? colors[channel_index]
                                       : ctx.theme().text_muted);
            if (reset_it.clicked) {
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
            y += heading_height + row_gap;

            const auto field = [&](std::string_view label,
                                   std::string_view property, float value,
                                   double speed, float minimum, float maximum,
                                   core::u64 salt) {
                const gui::Rect row{
                    content.x + inset, y,
                    std::max(content.width - inset * 2.0F, 0.0F), row_height};
                y += row_height + row_gap;
                const float label_width =
                    std::clamp(row.width * 0.44F, 82.0F, 132.0F);
                ctx.label(gui::Rect{row.x, row.y, label_width, row.height},
                          label, ctx.theme().text_muted);
                const gui::Rect well{
                    row.x + label_width, row.y,
                    std::max(row.width - label_width, 0.0F), row.height};
                double authored{};
                NumberFieldEdit numeric_edit{};
                const gui::UiId numeric_id =
                    mix(id_, levels->source.value() ^ salt);
                const std::size_t edits_before = frame.edits.size();
                if (fields_.draw(
                        ctx, numeric_id, well, {},
                        static_cast<double>(value), speed, authored, 3,
                        &numeric_edit) &&
                    std::isfinite(authored)) {
                    const double normalized = std::clamp(
                        authored, static_cast<double>(minimum),
                        static_cast<double>(maximum));
                    if (normalized != static_cast<double>(value)) {
                        frame.edits.push_back(ColorCorrectionEdit{
                            levels->source, std::string{property}, normalized});
                    }
                }
                record_nudge(numeric_id, property, numeric_edit, edits_before);
            };

            const core::u64 channel_salt =
                static_cast<core::u64>(channel_index) * 16U;
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
            y += 5.0F;
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
            const gui::Rect heading{
                content.x + inset, y,
                std::max(content.width - inset * 2.0F, 0.0F), heading_height};
            ctx.panel(heading, ctx.theme().header);
            ctx.panel(gui::Rect{heading.x, heading.y, 3.0F, heading.height},
                      gui::UiColor{0.94F, 0.62F, 0.24F, 1.0F});
            const std::string title =
                std::string{"LIFT / GAMMA / GAIN  /  "} +
                std::string{section.name};
            ctx.label(gui::Rect{heading.x + 9.0F, heading.y,
                                std::max(heading.width - 70.0F, 0.0F),
                                heading.height},
                      title, ctx.theme().text);
            const gui::Rect reset{
                heading.x + std::max(heading.width - 58.0F, 0.0F), heading.y,
                58.0F, heading.height};
            const gui::Interaction reset_it = ctx.grab(
                mix(id_, lift_gamma_gain->source.value() ^
                             static_cast<core::u64>(0x400U + section_index)),
                reset);
            ctx.panel(reset, reset_it.hovered ? ctx.theme().widget_hover
                                              : ctx.theme().widget);
            ctx.label(reset, "RESET", reset_it.hovered ? ctx.theme().keyframe
                                                        : ctx.theme().text_muted);
            const std::array<float, 3U> values{{
                section.value.x, section.value.y, section.value.z}};
            const std::array<float, 3U> identities{{
                section.identity.x, section.identity.y, section.identity.z}};
            if (reset_it.clicked) {
                for (std::size_t component = 0U; component < 3U; ++component) {
                    if (values[component] != identities[component]) {
                        frame.edits.push_back(ColorCorrectionEdit{
                            lift_gamma_gain->source,
                            std::string{section.properties[component]},
                            static_cast<double>(identities[component])});
                    }
                }
            }
            y += heading_height + row_gap;
            for (std::size_t component = 0U; component < 3U; ++component) {
                const gui::Rect row{
                    content.x + inset, y,
                    std::max(content.width - inset * 2.0F, 0.0F), row_height};
                y += row_height + row_gap;
                const float label_width =
                    std::clamp(row.width * 0.44F, 82.0F, 132.0F);
                ctx.label(gui::Rect{row.x, row.y, label_width, row.height},
                          component_names[component],
                          component_colors[component]);
                const gui::Rect well{
                    row.x + label_width, row.y,
                    std::max(row.width - label_width, 0.0F), row.height};
                double authored{};
                const core::u64 salt =
                    0x500U + static_cast<core::u64>(section_index) * 8U +
                    static_cast<core::u64>(component);
                NumberFieldEdit numeric_edit{};
                const gui::UiId numeric_id =
                    mix(id_, lift_gamma_gain->source.value() ^ salt);
                const std::size_t edits_before = frame.edits.size();
                if (fields_.draw(
                        ctx, numeric_id, well,
                        {}, static_cast<double>(values[component]), 0.01,
                        authored, 3, &numeric_edit) &&
                    std::isfinite(authored)) {
                    const double normalized = std::clamp(
                        authored, static_cast<double>(section.minimum),
                        static_cast<double>(section.maximum));
                    if (normalized != static_cast<double>(values[component])) {
                        frame.edits.push_back(ColorCorrectionEdit{
                            lift_gamma_gain->source,
                            std::string{section.properties[component]},
                            normalized});
                    }
                }
                record_nudge(numeric_id, section.properties[component],
                             numeric_edit, edits_before);
            }
            y += 5.0F;
        }
    }

    if (contrast_pivot != nullptr) {
        const auto& parameters = contrast_pivot->contrast_pivot;
        const gui::Rect heading{
            content.x + inset, y,
            std::max(content.width - inset * 2.0F, 0.0F), heading_height};
        ctx.panel(heading, ctx.theme().header);
        ctx.panel(gui::Rect{heading.x, heading.y, 3.0F, heading.height},
                  gui::UiColor{0.72F, 0.48F, 0.98F, 1.0F});
        ctx.label(gui::Rect{heading.x + 9.0F, heading.y,
                            std::max(heading.width - 70.0F, 0.0F),
                            heading.height},
                  "CONTRAST / PIVOT", ctx.theme().text);
        const gui::Rect reset{
            heading.x + std::max(heading.width - 58.0F, 0.0F), heading.y,
            58.0F, heading.height};
        const gui::Interaction reset_it = ctx.grab(
            mix(id_, contrast_pivot->source.value() ^ 0x700U), reset);
        ctx.panel(reset, reset_it.hovered ? ctx.theme().widget_hover
                                          : ctx.theme().widget);
        ctx.label(reset, "RESET", reset_it.hovered ? ctx.theme().keyframe
                                                    : ctx.theme().text_muted);
        if (reset_it.clicked) {
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
        y += heading_height + row_gap;
        const auto field = [&](std::string_view label,
                               std::string_view property, float value,
                               float minimum, float maximum, core::u64 salt) {
            const gui::Rect row{
                content.x + inset, y,
                std::max(content.width - inset * 2.0F, 0.0F), row_height};
            y += row_height + row_gap;
            const float label_width =
                std::clamp(row.width * 0.44F, 82.0F, 132.0F);
            ctx.label(gui::Rect{row.x, row.y, label_width, row.height}, label,
                      ctx.theme().text_muted);
            const gui::Rect well{
                row.x + label_width, row.y,
                std::max(row.width - label_width, 0.0F), row.height};
            double authored{};
            NumberFieldEdit numeric_edit{};
            const gui::UiId numeric_id =
                mix(id_, contrast_pivot->source.value() ^ salt);
            const std::size_t edits_before = frame.edits.size();
            if (fields_.draw(
                    ctx, numeric_id, well, {}, static_cast<double>(value),
                    0.01, authored, 3, &numeric_edit) &&
                std::isfinite(authored)) {
                const double normalized = std::clamp(
                    authored, static_cast<double>(minimum),
                    static_cast<double>(maximum));
                if (normalized != static_cast<double>(value)) {
                    frame.edits.push_back(ColorCorrectionEdit{
                        contrast_pivot->source, std::string{property},
                        normalized});
                }
            }
            record_nudge(numeric_id, property, numeric_edit, edits_before);
        };
        field("Contrast", scene::schema::contrast, parameters.contrast,
              scene::min_contrast_pivot_contrast,
              scene::max_contrast_pivot_contrast, 0x701U);
        field("Pivot", scene::schema::pivot, parameters.pivot,
              scene::min_contrast_pivot_pivot,
              scene::max_contrast_pivot_pivot, 0x702U);
        y += 5.0F;
    }

    if (white_balance != nullptr) {
        const auto& parameters = white_balance->white_balance;
        const gui::Rect heading{
            content.x + inset, y,
            std::max(content.width - inset * 2.0F, 0.0F), heading_height};
        ctx.panel(heading, ctx.theme().header);
        ctx.panel(gui::Rect{heading.x, heading.y, 3.0F, heading.height},
                  gui::UiColor{0.98F, 0.70F, 0.42F, 1.0F});
        ctx.label(gui::Rect{heading.x + 9.0F, heading.y,
                            std::max(heading.width - 70.0F, 0.0F),
                            heading.height},
                  "WHITE BALANCE", ctx.theme().text);
        const gui::Rect reset{
            heading.x + std::max(heading.width - 58.0F, 0.0F), heading.y,
            58.0F, heading.height};
        const gui::Interaction reset_it = ctx.grab(
            mix(id_, white_balance->source.value() ^ 0xA00U), reset);
        ctx.panel(reset, reset_it.hovered ? ctx.theme().widget_hover
                                          : ctx.theme().widget);
        ctx.label(reset, "RESET", reset_it.hovered ? ctx.theme().keyframe
                                                    : ctx.theme().text_muted);
        if (reset_it.clicked) {
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
        y += heading_height + row_gap;
        const auto field = [&](std::string_view label,
                               std::string_view property, float value,
                               float minimum, float maximum, double speed,
                               int decimals, core::u64 salt) {
            const gui::Rect row{
                content.x + inset, y,
                std::max(content.width - inset * 2.0F, 0.0F), row_height};
            y += row_height + row_gap;
            const float label_width =
                std::clamp(row.width * 0.44F, 82.0F, 132.0F);
            ctx.label(gui::Rect{row.x, row.y, label_width, row.height}, label,
                      ctx.theme().text_muted);
            const gui::Rect well{
                row.x + label_width, row.y,
                std::max(row.width - label_width, 0.0F), row.height};
            double authored{};
            NumberFieldEdit numeric_edit{};
            const gui::UiId numeric_id =
                mix(id_, white_balance->source.value() ^ salt);
            const std::size_t edits_before = frame.edits.size();
            if (fields_.draw(
                    ctx, numeric_id, well, {}, static_cast<double>(value),
                    speed, authored, decimals, &numeric_edit) &&
                std::isfinite(authored)) {
                const double normalized = std::clamp(
                    authored, static_cast<double>(minimum),
                    static_cast<double>(maximum));
                if (normalized != static_cast<double>(value)) {
                    frame.edits.push_back(ColorCorrectionEdit{
                        white_balance->source, std::string{property},
                        normalized});
                }
            }
            record_nudge(numeric_id, property, numeric_edit, edits_before);
        };
        field("Temperature (K)", scene::schema::temperature_kelvin,
              parameters.temperature_kelvin,
              scene::min_white_balance_temperature_kelvin,
              scene::max_white_balance_temperature_kelvin, 25.0, 0, 0xA01U);
        field("Tint", scene::schema::tint, parameters.tint,
              scene::min_white_balance_tint, scene::max_white_balance_tint,
              0.01, 3, 0xA02U);
        y += 5.0F;
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
            const gui::Rect heading{
                content.x + inset, y,
                std::max(content.width - inset * 2.0F, 0.0F), heading_height};
            ctx.panel(heading, ctx.theme().header);
            ctx.panel(gui::Rect{heading.x, heading.y, 3.0F, heading.height},
                      section.color);
            const std::string title =
                std::string{"CURVES  /  "} + std::string{section.name};
            ctx.label(gui::Rect{heading.x + 9.0F, heading.y,
                                std::max(heading.width - 112.0F, 0.0F),
                                heading.height},
                      title, ctx.theme().text);
            if (section.channel == nullptr) {
                const gui::Rect add{
                    heading.x + std::max(heading.width - 54.0F, 0.0F),
                    heading.y, 54.0F, heading.height};
                const gui::Interaction add_it = ctx.grab(
                    mix(id_, curves->source.value() ^
                                 static_cast<core::u64>(0xC00U +
                                     section_index)),
                    add);
                ctx.panel(add, add_it.hovered ? ctx.theme().widget_hover
                                              : ctx.theme().widget);
                ctx.label(add, "ENABLE",
                          add_it.hovered ? section.color
                                         : ctx.theme().text_muted);
                if (add_it.clicked) {
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
                y += heading_height + 8.0F;
                continue;
            }

            const auto& points = section.channel->points;
            const gui::Rect reset{
                heading.x + std::max(heading.width - 108.0F, 0.0F),
                heading.y, 54.0F, heading.height};
            const gui::Rect add{
                heading.x + std::max(heading.width - 54.0F, 0.0F),
                heading.y, 54.0F, heading.height};
            const gui::Interaction reset_it = ctx.grab(
                mix(id_, curves->source.value() ^
                             static_cast<core::u64>(0xC10U + section_index)),
                reset);
            const gui::Interaction add_it = ctx.grab(
                mix(id_, curves->source.value() ^
                             static_cast<core::u64>(0xC20U + section_index)),
                add);
            ctx.panel(reset, reset_it.hovered ? ctx.theme().widget_hover
                                              : ctx.theme().widget);
            ctx.label(reset, "RESET", reset_it.hovered ? section.color
                                                        : ctx.theme().text_muted);
            const bool can_add = points.size() < scene::max_curve_control_points;
            ctx.panel(add, add_it.hovered && can_add
                               ? ctx.theme().widget_hover
                               : ctx.theme().widget);
            ctx.label(add, "+ POINT",
                      add_it.hovered && can_add ? section.color
                                               : ctx.theme().text_muted);
            if (reset_it.clicked) {
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
            if (add_it.clicked && can_add && points.size() >= 2U) {
                const std::size_t endpoint = points.size() - 1U;
                const scene::CurveControlPoint previous = points[endpoint - 1U];
                const float midpoint_x = (previous.x + 1.0F) * 0.5F;
                const float midpoint_y = (previous.y + 1.0F) * 0.5F;
                frame.edits.push_back(ColorCorrectionEdit{
                    curves->source,
                    curve_property(section.prefix, endpoint,
                                   scene::schema::curve_x_suffix),
                    midpoint_x});
                frame.edits.push_back(ColorCorrectionEdit{
                    curves->source,
                    curve_property(section.prefix, endpoint,
                                   scene::schema::curve_y_suffix),
                    midpoint_y});
                frame.edits.push_back(ColorCorrectionEdit{
                    curves->source,
                    curve_property(section.prefix, endpoint + 1U,
                                   scene::schema::curve_x_suffix),
                    1.0});
                frame.edits.push_back(ColorCorrectionEdit{
                    curves->source,
                    curve_property(section.prefix, endpoint + 1U,
                                   scene::schema::curve_y_suffix),
                    1.0});
            }
            y += heading_height + row_gap;

            const gui::Rect plot{
                content.x + inset, y,
                std::max(content.width - inset * 2.0F, 0.0F), 112.0F};
            ctx.panel(plot, ctx.theme().track);
            for (std::size_t division = 1U; division < 4U; ++division) {
                const float fraction = static_cast<float>(division) / 4.0F;
                ctx.draw_list().add_line(
                    plot.x + plot.width * fraction, plot.y,
                    plot.x + plot.width * fraction, plot.y + plot.height,
                    1.0F, ctx.theme().divider);
                ctx.draw_list().add_line(
                    plot.x, plot.y + plot.height * fraction,
                    plot.x + plot.width, plot.y + plot.height * fraction,
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
                float last_y = plot.y + plot.height -
                    std::clamp(sample_value(parameters.lut.front()), 0.0F, 1.0F) *
                        plot.height;
                for (std::size_t sample = 1U; sample < parameters.lut.size();
                     ++sample) {
                    const float x = plot.x + plot.width *
                        static_cast<float>(sample) /
                        static_cast<float>(parameters.lut.size() - 1U);
                    const float value = std::clamp(
                        sample_value(parameters.lut[sample]), 0.0F, 1.0F);
                    const float sample_y = plot.y + plot.height -
                        value * plot.height;
                    ctx.draw_list().add_line(last_x, last_y, x, sample_y,
                                             1.5F, section.color);
                    last_x = x;
                    last_y = sample_y;
                }
            }
            for (const scene::CurveControlPoint point : points) {
                const float px = plot.x + point.x * plot.width;
                const float py = plot.y + plot.height - point.y * plot.height;
                ctx.draw_list().add_rect(px - 2.5F, py - 2.5F, 5.0F, 5.0F,
                                         section.color);
            }
            y += plot.height + row_gap;

            for (std::size_t point = 1U; point + 1U < points.size(); ++point) {
                const auto point_field = [&](std::string_view axis,
                                             std::string_view suffix,
                                             float value, float minimum,
                                             float maximum, core::u64 salt) {
                    const gui::Rect row{
                        content.x + inset, y,
                        std::max(content.width - inset * 2.0F, 0.0F),
                        row_height};
                    y += row_height + row_gap;
                    const float label_width =
                        std::clamp(row.width * 0.44F, 82.0F, 132.0F);
                    const std::string label = "Point " +
                        std::to_string(point) + " " + std::string{axis};
                    ctx.label(gui::Rect{row.x, row.y, label_width, row.height},
                              label, ctx.theme().text_muted);
                    const gui::Rect well{
                        row.x + label_width, row.y,
                        std::max(row.width - label_width, 0.0F), row.height};
                    const std::string property =
                        curve_property(section.prefix, point, suffix);
                    double authored{};
                    NumberFieldEdit numeric_edit{};
                    const gui::UiId numeric_id = mix(
                        id_, curves->source.value() ^ salt ^
                                 static_cast<core::u64>(point * 2U));
                    const std::size_t edits_before = frame.edits.size();
                    if (fields_.draw(ctx, numeric_id, well, {}, value, 0.005,
                                     authored, 3, &numeric_edit) &&
                        std::isfinite(authored)) {
                        const double normalized = std::clamp(
                            authored, static_cast<double>(minimum),
                            static_cast<double>(maximum));
                        if (normalized != static_cast<double>(value)) {
                            frame.edits.push_back(ColorCorrectionEdit{
                                curves->source, property, normalized});
                        }
                    }
                    record_nudge(numeric_id, property, numeric_edit,
                                 edits_before);
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
            y += 5.0F;
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
            const gui::Rect heading{
                content.x + inset, y,
                std::max(content.width - inset * 2.0F, 0.0F), heading_height};
            ctx.panel(heading, ctx.theme().header);
            ctx.panel(gui::Rect{heading.x, heading.y, 3.0F, heading.height},
                      section.color);
            const std::string title =
                std::string{"CHANNEL MIXER  /  "} + std::string{section.name};
            ctx.label(gui::Rect{heading.x + 9.0F, heading.y,
                                std::max(heading.width - 70.0F, 0.0F),
                                heading.height},
                      title, ctx.theme().text);
            const gui::Rect reset{
                heading.x + std::max(heading.width - 58.0F, 0.0F), heading.y,
                58.0F, heading.height};
            const gui::Interaction reset_it = ctx.grab(
                mix(id_, color_matrix->source.value() ^
                             static_cast<core::u64>(0x800U + section_index)),
                reset);
            ctx.panel(reset, reset_it.hovered ? ctx.theme().widget_hover
                                              : ctx.theme().widget);
            ctx.label(reset, "RESET", reset_it.hovered ? section.color
                                                        : ctx.theme().text_muted);
            if (reset_it.clicked) {
                for (std::size_t index = 0U; index < 4U; ++index) {
                    if (section.values[index] != section.identities[index]) {
                        frame.edits.push_back(ColorCorrectionEdit{
                            color_matrix->source,
                            std::string{section.properties[index]},
                            static_cast<double>(section.identities[index])});
                    }
                }
            }
            y += heading_height + row_gap;
            for (std::size_t index = 0U; index < 4U; ++index) {
                const gui::Rect row{
                    content.x + inset, y,
                    std::max(content.width - inset * 2.0F, 0.0F), row_height};
                y += row_height + row_gap;
                const float label_width =
                    std::clamp(row.width * 0.44F, 82.0F, 132.0F);
                ctx.label(gui::Rect{row.x, row.y, label_width, row.height},
                          field_names[index], ctx.theme().text_muted);
                const gui::Rect well{
                    row.x + label_width, row.y,
                    std::max(row.width - label_width, 0.0F), row.height};
                double authored{};
                const core::u64 salt =
                    0x900U + static_cast<core::u64>(section_index) * 8U +
                    static_cast<core::u64>(index);
                NumberFieldEdit numeric_edit{};
                const gui::UiId numeric_id =
                    mix(id_, color_matrix->source.value() ^ salt);
                const std::size_t edits_before = frame.edits.size();
                if (fields_.draw(
                        ctx, numeric_id, well,
                        {}, static_cast<double>(section.values[index]), 0.01,
                        authored, 3, &numeric_edit) &&
                    std::isfinite(authored)) {
                    const double normalized = std::clamp(
                        authored,
                        static_cast<double>(
                            scene::min_color_matrix_coefficient),
                        static_cast<double>(
                            scene::max_color_matrix_coefficient));
                    if (normalized !=
                        static_cast<double>(section.values[index])) {
                        frame.edits.push_back(ColorCorrectionEdit{
                            color_matrix->source,
                            std::string{section.properties[index]},
                            normalized});
                    }
                }
                record_nudge(numeric_id, section.properties[index],
                             numeric_edit, edits_before);
            }
            y += 5.0F;
        }
    }
    ctx.pop_clip();

    content_height_ = std::max(y + scroll_ - content.y, 0.0F);
    scroll_ = std::clamp(scroll_, 0.0F,
                         std::max(content_height_ - content.height, 0.0F));
    frame.editing = fields_.live();
    return frame;
}

} // namespace mediaserver::editor
