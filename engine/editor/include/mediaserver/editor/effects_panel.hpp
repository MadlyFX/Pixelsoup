#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/scene/scene.hpp>

#include <optional>

namespace mediaserver::editor {

struct EffectApplyRequest final {
    doc::NodeId target{};
    scene::EffectKind effect{scene::EffectKind::Brightness};
    float screen_x{};
    float screen_y{};
    // A click applies to `target`. A real drag reports its release even when no
    // layer is selected so the shell can route a drop over the Compositor into
    // a shared graph node instead.
    bool dragged{};

    bool operator==(const EffectApplyRequest&) const = default;
};

// Pure drop routing used by the shell after every dock has been laid out. A
// click always targets the selected layer; a real drag must finish over Stage
// or either visible Inspector tab. Keeping this rule here makes it possible to
// prove that releasing over an unrelated panel cannot silently apply an
// effect.
[[nodiscard]] bool effect_drop_targets_selected_layer(
    const EffectApplyRequest& request, const gui::Rect* stage_bounds,
    const gui::Rect* inspector_bounds) noexcept;

// The effect browser is intentionally small but real: a click, or a drag that
// ends while a scene layer is selected, asks the shell to author that effect on
// the selected layer. Its parameters then live in the Inspector and timeline.
class EffectsPanel final {
public:
    explicit EffectsPanel(gui::UiId id) noexcept : id_(id) {}

    [[nodiscard]] std::optional<EffectApplyRequest>
    draw(gui::UiContext& ctx, const gui::Rect& bounds,
         doc::NodeId selected_target = {});

    [[nodiscard]] bool dragging() const noexcept {
        return dragging_.has_value();
    }

private:
    gui::UiId id_{};
    float scroll_{};
    std::optional<scene::EffectKind> dragging_;
    float drag_start_x_{};
    float drag_start_y_{};
    bool drag_moved_{};
};

} // namespace mediaserver::editor
