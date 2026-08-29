#pragma once

#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/number_field.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/scene/scene.hpp>

#include <optional>
#include <span>

namespace mediaserver::editor {

struct TextureEffectEdit final {
    doc::NodeId effect{};
    scene::FilmGrainParameters film_grain{};

    bool operator==(const TextureEffectEdit&) const = default;
};

struct TexturePanelFrame final {
    std::optional<TextureEffectEdit> edit;
    std::optional<NumericNudge> numeric_nudge;
    bool editing{};
};

// A focused texture/noise surface. Film Grain is intentionally not folded
// into the tonal Color Correction panel: its seed, canvas-pixel scale and
// channel mode are spatial texture controls, while only Amount is keyframed
// from the Inspector/Timeline.
class TexturePanel final {
public:
    explicit TexturePanel(gui::UiId id) noexcept : id_(id) {}

    [[nodiscard]] TexturePanelFrame draw(
        gui::UiContext& ctx, const gui::Rect& bounds,
        doc::NodeId selected_layer,
        std::span<const scene::EffectInstance> effects);

private:
    gui::UiId id_{};
    NumberField fields_;
};

} // namespace mediaserver::editor
