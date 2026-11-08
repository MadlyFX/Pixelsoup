#include <mediaserver/editor/selection_sets_panel.hpp>

#include <mediaserver/editor/operator_widgets.hpp>
#include <mediaserver/gui/rect_cut.hpp>

#include <algorithm>
#include <string>
#include <string_view>

namespace mediaserver::editor {
namespace {

constexpr float title_height = 25.0F;
constexpr float create_height = 27.0F;
constexpr float footer_height = 54.0F;
constexpr float row_height = 23.0F;
constexpr float edit_height = 21.0F;
// Space around and between the strip controls, and a row's text inset.
constexpr float pad = 3.0F;
constexpr float text_inset = 7.0F;
constexpr double double_click_seconds = 0.35;

[[nodiscard]] bool valid_name(std::string_view name) noexcept {
  return !name.empty() && name.size() <= scene::max_selection_set_name_bytes;
}

[[nodiscard]] const scene::SelectionSetSummary *
find_set(std::span<const scene::SelectionSetSummary> sets,
         doc::NodeId id) noexcept {
  const auto found =
      std::find_if(sets.begin(), sets.end(),
                   [id](const auto &set) noexcept { return set.node == id; });
  return found == sets.end() ? nullptr : &*found;
}

} // namespace

SelectionSetPanelFrame SelectionSetsPanel::draw(
    gui::UiContext &ctx, const gui::Rect &bounds,
    std::span<const scene::SelectionSetSummary> sets, doc::NodeId selected_set,
    std::size_t scene_selection_count, bool authoring_enabled) {
  SelectionSetPanelFrame frame{};
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return frame;
  }
  const scene::SelectionSetSummary *selected = find_set(sets, selected_set);
  if (selected_set && selected == nullptr) {
    frame.selection_changed = true;
    frame.selected = {};
    selected_set = {};
  }
  if (selected_set != edit_name_owner_) {
    edit_name_owner_ = selected_set;
    edit_name_ = selected != nullptr ? selected->name : std::string{};
  }

  ctx.push_clip(bounds);
  ctx.panel(bounds);
  gui::Rect area = bounds;
  gui::Rect title = gui::cut_top(area, title_height);
  ctx.panel(title, ctx.theme().background);
  ctx.panel(gui::cut_bottom(title, 1.0F), ctx.theme().divider);

  const gui::Rect create_row = gui::cut_top(area, create_height);
  ctx.panel(create_row, ctx.theme().background);
  constexpr float save_width = 76.0F;
  gui::Rect new_name_bounds = gui::inset(create_row, pad);
  const gui::Rect save_bounds = gui::cut_right(new_name_bounds, save_width);
  static_cast<void>(gui::cut_right(new_name_bounds, pad));
  static_cast<void>(ctx.text_field(gui::ui_id(id_, "selection-set.new-name"),
                                   new_name_bounds, new_name_));
  if (action_button(ctx, gui::ui_id(id_, "selection-set.save-new"), save_bounds,
                    "SAVE NEW",
                    authoring_enabled && scene_selection_count != 0U &&
                        valid_name(new_name_))) {
    frame.intent = SelectionSetPanelIntent{
        SelectionSetPanelAction::SaveNew, {}, new_name_};
  }

  const gui::Rect footer = gui::cut_bottom(area, footer_height);
  const gui::Rect list = area;
  const float scroll = scroll_.begin(ctx, list, row_height * 2.0F);
  if (sets.empty()) {
    gui::Rect first = list;
    ctx.label(gui::inset(gui::cut_top(first, row_height), text_inset, 0.0F),
              "NO SAVED SETS", ctx.theme().text_muted);
  }
  const bool pointer_in_list = list.contains(ctx.pointer_x(), ctx.pointer_y());
  for (std::size_t row_index = 0U; row_index < sets.size(); ++row_index) {
    const scene::SelectionSetSummary &set = sets[row_index];
    const gui::Rect row = gui::list_row(list, row_index, row_height, scroll);
    const gui::Interaction interaction =
        pointer_in_list
            ? ctx.grab(gui::ui_id(id_, "selection-set.row", set.node.value()), row)
            : gui::Interaction{};
    gui::UiColor color =
        row_index % 2U == 0U ? ctx.theme().row : ctx.theme().row_alt;
    if (set.node == selected_set) {
      color = ctx.theme().selection;
    } else if (interaction.hovered) {
      color = ctx.theme().widget_hover;
    }
    ctx.panel(row, color);
    // The name takes a little over half the row; the member count the rest.
    gui::Rect text = gui::inset(row, text_inset, 0.0F);
    ctx.label(gui::cut_left(text, row.width * 0.55F - text_inset), set.name,
              set.node == selected_set ? ctx.theme().text
                                       : ctx.theme().text_muted);
    std::string count = std::to_string(set.member_count) + " MEMBERS";
    if (set.missing_count != 0U) {
      count += " / " + std::to_string(set.missing_count) + " MISSING";
    }
    ctx.label(text, count,
              set.missing_count == 0U ? ctx.theme().text_muted
                                      : ctx.theme().warning);
    if (interaction.clicked) {
      if (set.node != selected_set) {
        frame.selection_changed = true;
        frame.selected = set.node;
        selected_set = set.node;
        edit_name_owner_ = set.node;
        edit_name_ = set.name;
      }
      const double now = ctx.time_seconds();
      if (last_clicked_ == set.node && now >= last_click_time_ &&
          now - last_click_time_ <= double_click_seconds) {
        frame.intent = SelectionSetPanelIntent{
            SelectionSetPanelAction::Recall, set.node, {}};
        last_clicked_ = {};
        last_click_time_ = 0.0;
      } else {
        last_clicked_ = set.node;
        last_click_time_ = now;
      }
    }
  }
  scroll_.end(ctx, row_height * static_cast<float>(sets.size()));

  // The selected set's name, then its four actions in a row.
  ctx.panel(footer, ctx.theme().background);
  gui::Rect rule = footer;
  ctx.panel(gui::cut_top(rule, 1.0F), ctx.theme().divider);
  gui::Rect actions = gui::inset(footer, pad);
  const gui::Rect edit_bounds = gui::cut_top(actions, edit_height);
  static_cast<void>(gui::cut_top(actions, pad));
  if (selected_set) {
    static_cast<void>(ctx.text_field(gui::ui_id(id_, "selection-set.edit-name"),
                                     edit_bounds, edit_name_));
  } else {
    ctx.panel(edit_bounds, ctx.theme().track);
    ctx.label(edit_bounds, "SELECT A SET", ctx.theme().text_muted);
  }

  constexpr std::size_t action_count = 4U;
  const auto button_bounds = [&](std::size_t index) {
    return gui::column(actions, index, action_count, pad);
  };
  if (action_button(
          ctx, gui::ui_id(id_, "selection-set.replace"), button_bounds(0U), "REPLACE",
          authoring_enabled && selected_set && scene_selection_count != 0U)) {
    frame.intent = SelectionSetPanelIntent{
        SelectionSetPanelAction::ReplaceMembers, selected_set, {}};
  }
  if (action_button(ctx, gui::ui_id(id_, "selection-set.recall"), button_bounds(1U),
                    "RECALL", static_cast<bool>(selected_set))) {
    frame.intent = SelectionSetPanelIntent{
        SelectionSetPanelAction::Recall, selected_set, {}};
  }
  if (action_button(
          ctx, gui::ui_id(id_, "selection-set.rename"), button_bounds(2U), "RENAME",
          authoring_enabled && selected_set && valid_name(edit_name_))) {
    frame.intent = SelectionSetPanelIntent{SelectionSetPanelAction::Rename,
                                           selected_set, edit_name_};
  }
  if (action_button(ctx, gui::ui_id(id_, "selection-set.delete"), button_bounds(3U),
                    "DELETE",
                    authoring_enabled && static_cast<bool>(selected_set))) {
    frame.intent = SelectionSetPanelIntent{
        SelectionSetPanelAction::Delete, selected_set, {}};
  }
  ctx.pop_clip();
  return frame;
}

} // namespace mediaserver::editor
