#pragma once

#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/number_field.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/scene/scene.hpp>

#include <span>
#include <optional>
#include <string>
#include <vector>

namespace mediaserver::editor {

struct ColorCorrectionEdit final {
    doc::NodeId effect{};
    std::string property;
    double value{};

    bool operator==(const ColorCorrectionEdit&) const = default;
};

struct ColorCorrectionFrame final {
    std::vector<ColorCorrectionEdit> edits;
    std::optional<NumericNudge> numeric_nudge;
    bool editing{};
};

// A focused tonal-control surface modelled after professional colour panels:
// Master is followed by explicit R/G/B sections, and every authored number is
// visible in predictable sections. It edits the first enabled Levels and
// Lift/Gamma/Gain, Contrast/Pivot, White Balance, Curves, and Colour Matrix
// effects on the selected layer; adding/reordering effects remains the
// Effects/Inspector workflow.
class ColorCorrectionPanel final {
public:
    explicit ColorCorrectionPanel(gui::UiId id) noexcept : id_(id) {}

    [[nodiscard]] ColorCorrectionFrame draw(
        gui::UiContext& ctx, const gui::Rect& bounds,
        doc::NodeId selected_layer,
        std::span<const scene::EffectInstance> effects);

private:
    gui::UiId id_{};
    float scroll_{};
    float content_height_{};
    NumberField fields_;
    doc::NodeId curve_drag_effect_{};
    std::size_t curve_drag_section_{};
    std::size_t curve_drag_point_{};
};

} // namespace mediaserver::editor
