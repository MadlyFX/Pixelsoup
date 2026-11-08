#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/scene/selection_set.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace mediaserver::editor {

enum class SelectionSetPanelAction : core::u8 {
  SaveNew,
  ReplaceMembers,
  Recall,
  Rename,
  Delete,
};

struct SelectionSetPanelIntent final {
  SelectionSetPanelAction action{SelectionSetPanelAction::Recall};
  doc::NodeId set{};
  std::string name;

  bool operator==(const SelectionSetPanelIntent &) const = default;
};

struct SelectionSetPanelFrame final {
  // Panel-row selection is its own typed domain. Engaged `selected` matters
  // only when selection_changed is true; a zero id then means clear.
  bool selection_changed{};
  doc::NodeId selected{};
  std::optional<SelectionSetPanelIntent> intent;
};

class SelectionSetsPanel final {
public:
  explicit SelectionSetsPanel(gui::UiId id) noexcept : id_(id) {}

  [[nodiscard]] SelectionSetPanelFrame
  draw(gui::UiContext &ctx, const gui::Rect &bounds,
       std::span<const scene::SelectionSetSummary> sets,
       doc::NodeId selected_set, std::size_t scene_selection_count,
       bool authoring_enabled = true);

  [[nodiscard]] std::string_view edit_name() const noexcept {
    return edit_name_;
  }

private:
  gui::UiId id_{};
  std::string new_name_{"Selection Set"};
  std::string edit_name_;
  doc::NodeId edit_name_owner_{};
  doc::NodeId last_clicked_{};
  double last_click_time_{};
  gui::ScrollRegion scroll_;
};

} // namespace mediaserver::editor
