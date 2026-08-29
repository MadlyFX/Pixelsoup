#include <mediaserver/editor/selection_sets_panel.hpp>

#include <algorithm>
#include <string>
#include <string_view>

namespace mediaserver::editor {
namespace {

constexpr float title_height = 25.0F;
constexpr float create_height = 27.0F;
constexpr float footer_height = 54.0F;
constexpr float row_height = 23.0F;
constexpr double double_click_seconds = 0.35;

[[nodiscard]] gui::UiId mix(gui::UiId base, std::string_view name,
                            core::u64 index = 0U) noexcept {
  core::u64 hash = base ^ 0x9e3779b97f4a7c15ULL;
  for (const char character : name) {
    hash ^= static_cast<core::u8>(character);
    hash *= 1099511628211ULL;
  }
  hash ^= index + 0x9e3779b9ULL;
  hash *= 1099511628211ULL;
  return hash;
}

void border(gui::UiContext &ctx, const gui::Rect &bounds, gui::UiColor color) {
  auto &draw = ctx.draw_list();
  draw.add_rect(bounds.x, bounds.y, bounds.width, 1.0F, color);
  draw.add_rect(bounds.x, bounds.y + bounds.height - 1.0F, bounds.width, 1.0F,
                color);
  draw.add_rect(bounds.x, bounds.y, 1.0F, bounds.height, color);
  draw.add_rect(bounds.x + bounds.width - 1.0F, bounds.y, 1.0F, bounds.height,
                color);
}

[[nodiscard]] bool action_button(gui::UiContext &ctx, gui::UiId id,
                                 const gui::Rect &bounds,
                                 std::string_view label, bool enabled) {
  const gui::Interaction interaction =
      enabled ? ctx.grab(id, bounds) : gui::Interaction{};
  ctx.panel(bounds, !enabled
                        ? ctx.theme().track
                        : (interaction.held
                               ? ctx.theme().widget_active
                               : (interaction.hovered ? ctx.theme().widget_hover
                                                      : ctx.theme().widget)));
  border(ctx, bounds, enabled ? ctx.theme().header : ctx.theme().divider);
  ctx.label(bounds, label, enabled ? ctx.theme().text : ctx.theme().text_muted);
  return enabled && interaction.clicked;
}

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
  const gui::Rect title{bounds.x, bounds.y, bounds.width,
                        std::min(bounds.height, title_height)};
  ctx.panel(title, ctx.theme().background);
  ctx.panel(
      gui::Rect{title.x, title.y + title.height - 1.0F, title.width, 1.0F},
      ctx.theme().divider);
  ctx.label(gui::Rect{title.x + 7.0F, title.y,
                      std::max(title.width - 14.0F, 0.0F), title.height},
            "SELECTION SETS", ctx.theme().text);

  const gui::Rect create_row{
      bounds.x, bounds.y + title_height, bounds.width,
      std::min(create_height, std::max(bounds.height - title_height, 0.0F))};
  ctx.panel(create_row, ctx.theme().background);
  constexpr float save_width = 76.0F;
  const gui::Rect save_bounds{
      create_row.x + std::max(create_row.width - save_width - 3.0F, 0.0F),
      create_row.y + 3.0F,
      std::min(save_width, std::max(create_row.width - 6.0F, 0.0F)),
      std::max(create_row.height - 6.0F, 0.0F)};
  const gui::Rect new_name_bounds{
      create_row.x + 3.0F, create_row.y + 3.0F,
      std::max(save_bounds.x - create_row.x - 6.0F, 0.0F),
      std::max(create_row.height - 6.0F, 0.0F)};
  static_cast<void>(ctx.text_field(mix(id_, "selection-set.new-name"),
                                   new_name_bounds, new_name_));
  if (action_button(ctx, mix(id_, "selection-set.save-new"), save_bounds,
                    "SAVE NEW",
                    authoring_enabled && scene_selection_count != 0U &&
                        valid_name(new_name_))) {
    frame.intent = SelectionSetPanelIntent{
        SelectionSetPanelAction::SaveNew, {}, new_name_};
  }

  const gui::Rect footer{
      bounds.x,
      bounds.y + std::max(bounds.height - footer_height, title_height),
      bounds.width,
      std::min(footer_height, std::max(bounds.height - title_height, 0.0F))};
  const gui::Rect list{
      bounds.x, create_row.y + create_row.height, bounds.width,
      std::max(footer.y - create_row.y - create_row.height, 0.0F)};
  const float content_height = row_height * static_cast<float>(sets.size());
  const float maximum_scroll = std::max(content_height - list.height, 0.0F);
  if (list.contains(ctx.pointer_x(), ctx.pointer_y())) {
    scroll_ -= ctx.wheel() * row_height * 2.0F;
  }
  scroll_ = std::clamp(scroll_, 0.0F, maximum_scroll);

  ctx.push_clip(list);
  float y = list.y - scroll_;
  if (sets.empty()) {
    ctx.label(gui::Rect{list.x + 7.0F, list.y,
                        std::max(list.width - 14.0F, 0.0F),
                        std::min(list.height, row_height)},
              "NO SAVED SETS", ctx.theme().text_muted);
  }
  std::size_t row_index{};
  for (const scene::SelectionSetSummary &set : sets) {
    const gui::Rect row{list.x, y, list.width, row_height};
    const gui::Interaction interaction =
        list.contains(ctx.pointer_x(), ctx.pointer_y())
            ? ctx.grab(mix(id_, "selection-set.row", set.node.value()), row)
            : gui::Interaction{};
    gui::UiColor color =
        row_index % 2U == 0U ? ctx.theme().row : ctx.theme().row_alt;
    if (set.node == selected_set) {
      color = ctx.theme().selection;
    } else if (interaction.hovered) {
      color = ctx.theme().widget_hover;
    }
    ctx.panel(row, color);
    ctx.label(gui::Rect{row.x + 7.0F, row.y,
                        std::max(row.width * 0.55F - 7.0F, 0.0F), row.height},
              set.name,
              set.node == selected_set ? ctx.theme().text
                                       : ctx.theme().text_muted);
    std::string count = std::to_string(set.member_count) + " MEMBERS";
    if (set.missing_count != 0U) {
      count += " / " + std::to_string(set.missing_count) + " MISSING";
    }
    ctx.label(gui::Rect{row.x + row.width * 0.55F, row.y,
                        std::max(row.width * 0.45F - 7.0F, 0.0F), row.height},
              count,
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
    y += row_height;
    ++row_index;
  }
  ctx.pop_clip();

  ctx.panel(footer, ctx.theme().background);
  ctx.panel(gui::Rect{footer.x, footer.y, footer.width, 1.0F},
            ctx.theme().divider);
  const gui::Rect edit_bounds{footer.x + 3.0F, footer.y + 3.0F,
                              std::max(footer.width - 6.0F, 0.0F), 21.0F};
  if (selected_set) {
    static_cast<void>(ctx.text_field(mix(id_, "selection-set.edit-name"),
                                     edit_bounds, edit_name_));
  } else {
    ctx.panel(edit_bounds, ctx.theme().track);
    ctx.label(edit_bounds, "SELECT A SET", ctx.theme().text_muted);
  }

  constexpr float gap = 3.0F;
  const float button_width =
      std::max((footer.width - 6.0F - gap * 3.0F) / 4.0F, 0.0F);
  const float button_y = footer.y + 27.0F;
  const float button_height = std::max(footer.height - 30.0F, 0.0F);
  const auto button_bounds = [&](std::size_t index) {
    return gui::Rect{footer.x + 3.0F +
                         static_cast<float>(index) * (button_width + gap),
                     button_y, button_width, button_height};
  };
  if (action_button(
          ctx, mix(id_, "selection-set.replace"), button_bounds(0U), "REPLACE",
          authoring_enabled && selected_set && scene_selection_count != 0U)) {
    frame.intent = SelectionSetPanelIntent{
        SelectionSetPanelAction::ReplaceMembers, selected_set, {}};
  }
  if (action_button(ctx, mix(id_, "selection-set.recall"), button_bounds(1U),
                    "RECALL", static_cast<bool>(selected_set))) {
    frame.intent = SelectionSetPanelIntent{
        SelectionSetPanelAction::Recall, selected_set, {}};
  }
  if (action_button(
          ctx, mix(id_, "selection-set.rename"), button_bounds(2U), "RENAME",
          authoring_enabled && selected_set && valid_name(edit_name_))) {
    frame.intent = SelectionSetPanelIntent{SelectionSetPanelAction::Rename,
                                           selected_set, edit_name_};
  }
  if (action_button(ctx, mix(id_, "selection-set.delete"), button_bounds(3U),
                    "DELETE",
                    authoring_enabled && static_cast<bool>(selected_set))) {
    frame.intent = SelectionSetPanelIntent{
        SelectionSetPanelAction::Delete, selected_set, {}};
  }
  ctx.pop_clip();
  return frame;
}

} // namespace mediaserver::editor
