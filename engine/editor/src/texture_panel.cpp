#include <mediaserver/editor/texture_panel.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

namespace mediaserver::editor {
namespace {

[[nodiscard]] gui::UiId mix(gui::UiId base, core::u64 value) noexcept {
    base ^= value + 0x9E3779B97F4A7C15ULL + (base << 6U) + (base >> 2U);
    return base;
}

} // namespace

TexturePanelFrame TexturePanel::draw(
    gui::UiContext& ctx, const gui::Rect& bounds,
    doc::NodeId selected_layer,
    std::span<const scene::EffectInstance> effects) {
    TexturePanelFrame frame{};
    if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
        return frame;
    }

    ctx.panel(bounds, ctx.theme().panel);
    constexpr float header_height = 26.0F;
    const gui::Rect header{bounds.x, bounds.y, bounds.width, header_height};
    ctx.panel(header, ctx.theme().header);
    ctx.panel(gui::Rect{header.x, header.y + header.height - 1.0F,
                        header.width, 1.0F},
              ctx.theme().divider);

    const scene::EffectInstance* grain{};
    for (const scene::EffectInstance& effect : effects) {
        if (effect.target == selected_layer && effect.enabled &&
            effect.kind == scene::EffectKind::FilmGrain) {
            grain = &effect;
            break;
        }
    }
    if (grain == nullptr) {
        fields_.cancel(ctx);
        ctx.label(gui::Rect{bounds.x + 10.0F, bounds.y + header_height + 10.0F,
                            std::max(bounds.width - 20.0F, 0.0F), 52.0F},
                  selected_layer == doc::NodeId{}
                      ? "Select a layer to edit texture"
                      : "Add Film Grain from the Effects panel",
                  ctx.theme().text_muted);
        return frame;
    }

    const auto normalized = scene::normalized_film_grain(grain->film_grain);
    if (!normalized.has_value()) {
        fields_.cancel(ctx);
        ctx.label(gui::Rect{bounds.x + 10.0F, bounds.y + header_height + 10.0F,
                            std::max(bounds.width - 20.0F, 0.0F), 52.0F},
                  "Film Grain parameters are invalid", ctx.theme().warning);
        return frame;
    }
    const scene::FilmGrainParameters parameters = *normalized;

    constexpr float inset = 8.0F;
    constexpr float row_height = 24.0F;
    constexpr float row_gap = 3.0F;
    float y = bounds.y + header_height + 8.0F;
    const float label_width = std::clamp(bounds.width * 0.42F, 82.0F, 132.0F);

    const auto emit = [&](scene::FilmGrainParameters value,
                          gui::UiId focus, core::u64 property,
                          const NumberFieldEdit* numeric) {
        const auto projected = scene::normalized_film_grain(value);
        if (projected.has_value() && *projected != parameters) {
            frame.edit = TextureEffectEdit{grain->source, *projected};
            if (numeric != nullptr &&
                numeric->kind == NumberFieldEditKind::Nudge) {
                frame.numeric_nudge = NumericNudge{
                    focus, property, numeric->direction, numeric->modifiers};
            }
        }
    };

    const auto numeric_row = [&](std::string_view label, double value,
                                 double speed, double minimum,
                                 double maximum, int decimals,
                                 core::u64 property,
                                 const auto& assign) {
        const gui::Rect row{bounds.x + inset, y,
                            std::max(bounds.width - inset * 2.0F, 0.0F),
                            row_height};
        y += row_height + row_gap;
        ctx.label(gui::Rect{row.x, row.y, label_width, row.height}, label,
                  ctx.theme().text_muted);
        const gui::Rect field{row.x + label_width, row.y,
                              std::max(row.width - label_width, 1.0F),
                              row.height};
        const gui::UiId field_id = mix(id_, property);
        double authored{};
        NumberFieldEdit edit{};
        if (fields_.draw(ctx, field_id, field, {}, value, speed, authored,
                         decimals, &edit) && std::isfinite(authored)) {
            scene::FilmGrainParameters changed = parameters;
            assign(changed, std::clamp(authored, minimum, maximum));
            emit(changed, field_id, property, &edit);
        }
    };

    numeric_row("AMOUNT", parameters.amount, 0.005,
                scene::min_film_grain_amount,
                scene::max_film_grain_amount, 3, 1U,
                [](scene::FilmGrainParameters& value, double authored) {
                    value.amount = static_cast<float>(authored);
                });
    numeric_row("PIXEL SCALE", parameters.pixel_scale, 0.25,
                scene::min_film_grain_pixel_scale,
                scene::max_film_grain_pixel_scale, 2, 2U,
                [](scene::FilmGrainParameters& value, double authored) {
                    value.pixel_scale = static_cast<float>(authored);
                });
    numeric_row("CHROMA RATIO", parameters.chroma_ratio, 0.005,
                scene::min_film_grain_chroma_ratio,
                scene::max_film_grain_chroma_ratio, 3, 3U,
                [](scene::FilmGrainParameters& value, double authored) {
                    value.chroma_ratio = static_cast<float>(authored);
                });
    numeric_row(
        "SEED", static_cast<double>(parameters.seed), 1.0, 0.0,
        static_cast<double>(std::numeric_limits<core::u32>::max()), 0, 4U,
        [](scene::FilmGrainParameters& value, double authored) {
            value.seed = static_cast<core::u32>(std::round(authored));
        });

    const gui::Rect mode_row{bounds.x + inset, y,
                             std::max(bounds.width - inset * 2.0F, 0.0F),
                             row_height};
    y += row_height + row_gap;
    ctx.label(gui::Rect{mode_row.x, mode_row.y, label_width, mode_row.height},
              "CHANNEL MODE", ctx.theme().text_muted);
    const gui::Rect mode_button{
        mode_row.x + label_width, mode_row.y,
        std::max(mode_row.width - label_width, 1.0F), mode_row.height};
    const gui::Interaction mode_it =
        ctx.grab(mix(id_, 5U), mode_button);
    ctx.panel(mode_button, mode_it.hovered ? ctx.theme().widget_hover
                                           : ctx.theme().widget);
    ctx.label(mode_button,
              parameters.mode == scene::FilmGrainMode::RGB
                  ? "RGB" : "MONOCHROME",
              mode_it.hovered ? ctx.theme().accent : ctx.theme().text);
    if (mode_it.clicked) {
        scene::FilmGrainParameters changed = parameters;
        changed.mode = parameters.mode == scene::FilmGrainMode::RGB
            ? scene::FilmGrainMode::Monochrome
            : scene::FilmGrainMode::RGB;
        emit(changed, mix(id_, 5U), 5U, nullptr);
    }

    const gui::Rect reset{bounds.x + inset, y,
                          std::max(bounds.width - inset * 2.0F, 0.0F),
                          row_height};
    const gui::Interaction reset_it = ctx.grab(mix(id_, 6U), reset);
    ctx.panel(reset, reset_it.hovered ? ctx.theme().widget_hover
                                      : ctx.theme().widget);
    ctx.label(reset, "RESET FILM GRAIN",
              reset_it.hovered ? ctx.theme().warning
                               : ctx.theme().text_muted);
    if (reset_it.clicked) {
        emit(scene::FilmGrainParameters{}, mix(id_, 6U), 6U, nullptr);
    }

    frame.editing = fields_.live();
    return frame;
}

} // namespace mediaserver::editor
