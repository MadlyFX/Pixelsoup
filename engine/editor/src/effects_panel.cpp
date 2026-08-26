#include <mediaserver/editor/effects_panel.hpp>

#include <algorithm>
#include <array>
#include <string_view>

namespace mediaserver::editor {
namespace {

[[nodiscard]] gui::UiId mix(gui::UiId base, core::u64 value) noexcept {
    base ^= value + 0x9E3779B97F4A7C15ULL + (base << 6U) + (base >> 2U);
    return base;
}

struct EffectDescription final {
    scene::EffectKind kind;
    std::string_view name;
    std::string_view description;
    gui::UiColor color;
};

constexpr std::array<EffectDescription, 2U> effects{{
    {scene::EffectKind::Brightness, "Brightness", "Linear exposure multiplier",
     gui::UiColor{0.980F, 0.760F, 0.260F, 1.0F}},
    {scene::EffectKind::Blur, "Blur", "Canvas-space Gaussian radius",
     gui::UiColor{0.420F, 0.710F, 1.000F, 1.0F}},
}};

void draw_effect_icon(gui::UiContext& ctx, const gui::Rect& bounds,
                      const EffectDescription& effect) {
    ctx.panel(bounds, ctx.theme().track);
    ctx.panel(gui::Rect{bounds.x, bounds.y, 2.0F, bounds.height}, effect.color);
    if (effect.kind == scene::EffectKind::Brightness) {
        const float cx = bounds.x + bounds.width * 0.5F;
        const float cy = bounds.y + bounds.height * 0.5F;
        ctx.panel(gui::Rect{cx - 2.0F, cy - 2.0F, 4.0F, 4.0F}, effect.color);
        for (std::size_t index = 0U; index < 4U; ++index) {
            const float dx = index < 2U ? (index == 0U ? -7.0F : 7.0F) : 0.0F;
            const float dy = index >= 2U ? (index == 2U ? -7.0F : 7.0F) : 0.0F;
            ctx.draw_list().add_line(cx + dx * 0.55F, cy + dy * 0.55F,
                                     cx + dx, cy + dy, 1.0F, effect.color);
        }
        return;
    }
    for (std::size_t index = 0U; index < 3U; ++index) {
        const float inset = static_cast<float>(index) * 3.0F;
        ctx.draw_list().add_rect(bounds.x + 5.0F + inset,
                                 bounds.y + 5.0F + inset,
                                 std::max(bounds.width - 10.0F - inset * 2.0F,
                                          1.0F),
                                 1.0F, effect.color);
    }
}

} // namespace

bool effect_drop_targets_selected_layer(
    const EffectApplyRequest& request, const gui::Rect* stage_bounds,
    const gui::Rect* inspector_bounds) noexcept {
    if (request.target == doc::NodeId{}) {
        return false;
    }
    if (!request.dragged) {
        return true;
    }
    return (stage_bounds != nullptr &&
            stage_bounds->contains(request.screen_x, request.screen_y)) ||
        (inspector_bounds != nullptr &&
         inspector_bounds->contains(request.screen_x, request.screen_y));
}

std::optional<EffectApplyRequest>
EffectsPanel::draw(gui::UiContext& ctx, const gui::Rect& bounds,
                   doc::NodeId selected_target) {
    std::optional<EffectApplyRequest> request;
    if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
        return request;
    }

    ctx.push_clip(bounds);
    ctx.panel(bounds, ctx.theme().panel);
    const gui::Rect heading{bounds.x, bounds.y, bounds.width, 24.0F};
    ctx.panel(heading, ctx.theme().header);
    ctx.label(gui::Rect{heading.x + 8.0F, heading.y,
                        std::max(heading.width - 16.0F, 0.0F), heading.height},
              "EFFECTS", ctx.theme().text);
    ctx.panel(gui::Rect{heading.x, heading.y + heading.height - 1.0F,
                        heading.width, 1.0F},
              ctx.theme().divider);

    float y = heading.y + heading.height + 7.0F;
    for (std::size_t index = 0U; index < effects.size(); ++index) {
        const EffectDescription& effect = effects[index];
        const gui::Rect row{bounds.x + 7.0F, y,
                            std::max(bounds.width - 14.0F, 0.0F), 42.0F};
        const gui::Interaction interaction =
            ctx.grab(mix(id_, static_cast<core::u64>(index + 1U)), row);
        ctx.panel(row, interaction.held
                           ? ctx.theme().widget_active
                           : (interaction.hovered ? ctx.theme().widget_hover
                                                  : ctx.theme().row));
        draw_effect_icon(ctx,
                         gui::Rect{row.x + 5.0F, row.y + 7.0F, 28.0F, 28.0F},
                         effect);
        ctx.label(gui::Rect{row.x + 40.0F, row.y + 2.0F,
                            std::max(row.width - 46.0F, 0.0F), 20.0F},
                  effect.name, ctx.theme().text);
        ctx.label(gui::Rect{row.x + 40.0F, row.y + 20.0F,
                            std::max(row.width - 46.0F, 0.0F), 18.0F},
                  effect.description, ctx.theme().text_muted);
        if (interaction.pressed) {
            dragging_ = effect.kind;
            drag_start_x_ = ctx.pointer_x();
            drag_start_y_ = ctx.pointer_y();
            drag_moved_ = false;
        }
        if (interaction.held && dragging_.has_value()) {
            const float dx = ctx.pointer_x() - drag_start_x_;
            const float dy = ctx.pointer_y() - drag_start_y_;
            drag_moved_ = drag_moved_ || dx * dx + dy * dy >= 16.0F;
        }
        if (interaction.released) {
            if (dragging_.has_value() &&
                (drag_moved_ || selected_target.value() != 0U)) {
                request = EffectApplyRequest{
                    selected_target, *dragging_, ctx.pointer_x(),
                    ctx.pointer_y(), drag_moved_};
            }
            dragging_.reset();
            drag_moved_ = false;
        }
        y += 47.0F;
    }

    const gui::Rect help{bounds.x + 8.0F, y + 3.0F,
                         std::max(bounds.width - 16.0F, 0.0F), 34.0F};
    ctx.label(help,
              selected_target.value() == 0U
                  ? "Select a scene layer, then click or drag an effect"
                  : "Click or drag onto the selected scene layer",
              ctx.theme().text_muted);
    ctx.pop_clip();

    if (dragging_.has_value()) {
        const EffectDescription& effect = effects[
            *dragging_ == scene::EffectKind::Brightness ? 0U : 1U];
        const gui::Rect ghost{ctx.pointer_x() + 12.0F, ctx.pointer_y() + 8.0F,
                              116.0F, 24.0F};
        ctx.panel(ghost, ctx.theme().widget_active);
        ctx.panel(gui::Rect{ghost.x, ghost.y, 3.0F, ghost.height}, effect.color);
        ctx.label(gui::Rect{ghost.x + 9.0F, ghost.y,
                            std::max(ghost.width - 12.0F, 0.0F), ghost.height},
                  effect.name, ctx.theme().text);
    }
    return request;
}

} // namespace mediaserver::editor
