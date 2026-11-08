#pragma once

// The Scene outliner panel: the scene's hierarchy beside the viewports.

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/element.hpp>
#include <mediaserver/editor/operator_widgets.hpp>
#include <mediaserver/editor/selection.hpp>
#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/output/feed_map.hpp>
#include <mediaserver/render/hybrid_scene_renderer.hpp>

#include <optional>
#include <span>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace mediaserver::editor {

enum class SceneSettingsTarget : core::u8 { None, Renderer, Environment };

// A scene-tree selection gesture. Shift range requests carry the exact
// filtered/expanded element-row order the operator saw; the shell remains the
// sole owner of canonical selection state.
struct SceneSelectionRequest final {
  doc::NodeId node{};
  SelectionAction action{SelectionAction::Replace};
  std::vector<doc::NodeId> visible_order;
  bool lock_changed{};
  bool locked{};
  // The eye: the element's own visibility, the same bit the Timeline's eye
  // writes.
  bool visibility_changed{};
  bool visible{true};
  enum class StructureAction : core::u8 {
    Group,
    Ungroup,
    // File `moved` under `destination` (zero: the top level), keeping each
    // element where it is on the stage. When `reorder_destination` is set the
    // moved rows land beside that sibling rather than at the front.
    MoveInto,
    // A new, empty group of `group_kind` under `destination` (zero: the top
    // level).
    Create,
  };
  std::optional<StructureAction> structure;
  doc::NodeId destination{};
  scene::GroupKind group_kind{scene::GroupKind::Folder};
  // The context menu's highlight colour, for every selected row: a
  // marker-palette index, or none to clear it.
  bool highlight_changed{};
  std::optional<core::u8> highlight;
  std::vector<doc::NodeId> moved;
  // A drag between two siblings: `node` lands before or after
  // `reorder_destination` in the front-to-back stack.
  doc::NodeId reorder_destination{};
  bool reorder_before{};
  // `node`'s new name, committed from the inline field.
  std::optional<std::string> rename;
  // The context menu's Delete: the shell deletes the current selection.
  bool delete_selection{};
  SceneSettingsTarget settings{SceneSettingsTarget::None};
  // `node` is an LED surface rather than a scene element: a plain click
  // selects it on Stage, and `lock_changed` sets its Feed Mapping lock.
  bool surface{};

  bool operator==(const SceneSelectionRequest &) const = default;
};

// The Scene outliner: the scene's real hierarchy - groups and Blender
// collections holding their children - with the same eye, padlock, icons and
// names the Timeline's layer list uses. Rows rename in place (double-click or
// F2), drag to reorder among siblings or onto a group to file under it, and
// carry a right-click menu with everything else a row can do: grouping, a
// highlight colour, and new empty groups inside a group. A right-click on the
// list's empty space creates groups at the top level. The stage's LED
// surfaces follow the scene's elements, as they do in the Timeline: physical
// geometry rather than scene nodes, so they carry a padlock but no eye and
// never file into the hierarchy.
class ScenePanel final {
public:
  explicit ScenePanel(gui::UiId id) noexcept
      : id_(id), rename_(id ^ 0x52454E414D455343ULL) {}

  [[nodiscard]] std::optional<SceneSelectionRequest>
  draw(gui::UiContext &ctx, const gui::Rect &bounds,
       std::span<const ElementInfo> elements,
       std::span<const doc::NodeId> selection, doc::NodeId primary = {},
       bool show_settings = false,
       SceneSettingsTarget settings_selection = SceneSettingsTarget::None,
       std::span<const output::ProjectionSurface> surfaces = {},
       doc::NodeId selected_surface = {});

  // Opens `element`'s name for editing, as F2 does.
  void begin_rename(gui::UiContext &ctx, const ElementInfo &element);
  [[nodiscard]] bool renaming() const noexcept { return rename_.active(); }

  [[nodiscard]] std::string_view search() const noexcept { return search_; }

private:
  gui::UiId id_{};
  std::string search_;
  gui::ScrollRegion scroll_;
  // Branches the operator closed. Everything starts open; selecting
  // something inside a closed branch opens it again.
  std::set<doc::NodeId> collapsed_;
  doc::NodeId revealed_{};
  InlineRename rename_;
  ClickTracker clicks_;
  // The outliner was the last panel clicked, so F2 is meant for it.
  bool focused_{};
  doc::NodeId drag_node_{};
  float drag_x_{};
  float drag_y_{};
  bool dragging_{};
  // The open context menu's row and entries, fixed when it opened so what
  // the operator reads is what a choice does.
  doc::NodeId menu_node_{};
  // A menu opened on the list's empty space rather than on a row.
  bool menu_background_{};
  std::vector<std::string> menu_entries_;
  std::vector<core::u8> menu_commands_;
  // The highlight colour menu, opened from the row menu at the same point.
  bool color_menu_{};
};

} // namespace mediaserver::editor
