#pragma once

// One FeedMappingPanel::draw as an object. draw() builds a Frame from its
// arguments and runs the panel's phases in order; state one phase hands to
// another lives here, everything else stays local to its phase. Internal to
// the editor library.

#include "feed_map_support.hpp"

#include <mediaserver/gui/form_layout.hpp>

#include <optional>
#include <utility>
#include <vector>

namespace mediaserver::editor {

struct FeedMappingPanel::Frame final {
  FeedMappingPanel &panel;
  gui::UiContext &ctx;
  const gui::Rect &bounds;
  scene::Canvas canvas;
  const output::FeedMap &feed_map;
  core::u64 show_frame;
  const topology::HardwareMap *hardware;
  const topology::HardwareRuntimeSnapshot *runtime;
  FeedMappingFrame result{};

  // Set by draw_viewport_list(): everything under the title, the work area
  // right of the list, and the list column the viewport and surface lists
  // and the stage run down.
  gui::Rect content{};
  gui::Rect work{};
  std::optional<gui::FormLayout> list_column;

  // A surface dragged far enough out of its row to be dropped somewhere, and
  // what it may be dropped on: the outputs' rows and rectangles, and the
  // other surface rows. Collected as they are drawn and resolved once by
  // resolve_surface_drop().
  bool surface_drag_active{};
  std::vector<std::pair<gui::Rect, doc::NodeId>> surface_drop_targets;
  std::vector<std::pair<gui::Rect, doc::NodeId>> surface_rows;

  // Set by draw_work_area(): the selected viewport, the surface it projects
  // onto, and whether that surface is an imported UV mesh.
  const output::OutputMap *selected_map{};
  const output::ProjectionSurface *mapping_surface{};
  bool uv_mesh_domain{};

  // Points the surface fields at the selected surface, committing the name
  // the operator leaves, and follows the document while nothing is live.
  void sync_surface_details();
  // A full-width button that adds to the list, dimmed when it cannot.
  [[nodiscard]] bool add_button(gui::UiId id, const gui::Rect &button,
                                std::string_view label, bool enabled);
};

} // namespace mediaserver::editor
