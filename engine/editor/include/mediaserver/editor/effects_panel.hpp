#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/scene/scene.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace mediaserver::editor {

struct EffectApplyRequest final {
    doc::NodeId target{};
    scene::EffectKind effect{scene::EffectKind::Brightness};
    float screen_x{};
    float screen_y{};

    bool operator==(const EffectApplyRequest&) const = default;
};

// Pure drop routing used by the shell after every dock has been laid out. A
// drag must finish over Stage or either visible Inspector tab. Keeping this
// rule here makes it possible to prove that releasing over an unrelated panel
// cannot silently apply an effect.
[[nodiscard]] bool effect_drop_targets_selected_layer(
    const EffectApplyRequest& request, const gui::Rect* stage_bounds,
    const gui::Rect* inspector_bounds) noexcept;

// Draw one truthful effect-application target after the dock layout is known.
// Every valid Inspector is outlined while an effect is in flight; the target
// strengthens under the pointer and names the selected layer before release.
void draw_effect_drop_target(gui::UiContext& ctx, const gui::Rect& bounds,
                             std::string_view target_name);

// A contextual picker uses the browser's catalogue and leaves the workspace
// in place. The chosen kind is authored by the caller's document authority.
[[nodiscard]] std::optional<scene::EffectKind>
draw_effect_picker(gui::UiContext& ctx, gui::UiId id, const gui::Rect& bounds);

// The effect browser is intentionally small but real: dragging an effect onto
// a valid target asks the shell to author it. Its parameters then live in the
// Inspector and timeline.
class EffectsPanel final {
public:
    explicit EffectsPanel(gui::UiId id) noexcept : id_(id) {}

    // `target_name` is what the drop will land on. The palette states it in the
    // heading and carries it on the drag itself, so a drag says what it will do
    // before it is let go rather than a paragraph explaining the rule.
    [[nodiscard]] std::optional<EffectApplyRequest>
    draw(gui::UiContext& ctx, const gui::Rect& bounds,
         doc::NodeId selected_target = {},
         std::string_view target_name = {});

    [[nodiscard]] bool dragging() const noexcept {
        return dragging_.has_value();
    }
    [[nodiscard]] std::string_view dragging_name() const noexcept;
    // The effect being dragged, so a drop target can say what it will make.
    [[nodiscard]] std::optional<scene::EffectKind> dragging_effect() const noexcept {
        return dragging_;
    }
    [[nodiscard]] std::string_view search() const noexcept { return search_; }

private:
    gui::UiId id_{};
    gui::ScrollRegion scroll_;
    std::string search_;
    std::optional<scene::EffectKind> dragging_;
    float drag_start_x_{};
    float drag_start_y_{};
    bool drag_moved_{};
};

} // namespace mediaserver::editor
