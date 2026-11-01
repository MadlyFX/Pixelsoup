#pragma once

// Feed map and projection-surface command authoring.

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/mapping_panels.hpp>
#include <mediaserver/output/feed_map.hpp>
#include <mediaserver/output/perspective_mapping.hpp>

#include <span>
#include <string_view>
#include <vector>

namespace mediaserver::app {

[[nodiscard]] const doc::Node *find_feed_map_node(const doc::Root &root,
                                                  core::u32 node_index);

[[nodiscard]] std::vector<const doc::Node *>
find_feed_map_nodes(const doc::Root &root, core::u32 node_index);

[[nodiscard]] const doc::Node *find_feed_map_node(const doc::Root &root,
                                                  doc::NodeId node) noexcept;

[[nodiscard]] const doc::Node *find_feed_surface_node(const doc::Root &root,
                                                      doc::NodeId id) noexcept;

[[nodiscard]] bool
feed_surface_reference_fits_document(doc::NodeId id) noexcept;

[[nodiscard]] doc::NodeId projection_asset_for_source(const doc::Root &root,
                                                      std::string_view source);

void write_feed_surface_properties(doc::Node &node, std::string_view name,
                                   const output::PhysicalSurface &surface,
                                   output::ProjectionSurfaceShape shape,
                                   float horizontal_arc_turns,
                                   core::u32 mesh_columns,
                                   std::string_view mesh_source,
                                   float mesh_scale, doc::NodeId mesh_asset);

[[nodiscard]] bool
valid_feed_surface_request(const output::PhysicalSurface &surface,
                           output::ProjectionSurfaceShape shape,
                           float horizontal_arc_turns, core::u32 mesh_columns,
                           std::string_view mesh_source,
                           float mesh_scale) noexcept;

[[nodiscard]] doc::Node
make_feed_surface_node(doc::NodeId id,
                       const editor::FeedSurfaceAddRequest &request,
                       doc::NodeId mesh_asset);

[[nodiscard]] bool
schedule_feed_surface_add(cmd::DirectorScheduler &scheduler,
                          const editor::FeedSurfaceAddRequest &request,
                          doc::NodeId allocated_id, cmd::FrameNumber apply_at);

[[nodiscard]] bool
schedule_feed_surface_edit(cmd::DirectorScheduler &scheduler,
                           const editor::FeedSurfaceEditRequest &request,
                           cmd::FrameNumber apply_at);

[[nodiscard]] bool
schedule_feed_surface_delete(cmd::DirectorScheduler &scheduler,
                             const editor::FeedSurfaceDeleteRequest &request,
                             cmd::FrameNumber apply_at);

[[nodiscard]] bool
schedule_feed_projection_edit(cmd::DirectorScheduler &scheduler,
                              const editor::FeedProjectionEdit &edit,
                              cmd::FrameNumber apply_at);

[[nodiscard]] bool schedule_feed_map_edit(cmd::DirectorScheduler &scheduler,
                                          const editor::FeedMapRectEdit &edit,
                                          doc::NodeId allocated_id,
                                          cmd::FrameNumber apply_at);

[[nodiscard]] bool
schedule_feed_map_add(cmd::DirectorScheduler &scheduler,
                      const editor::FeedMapAddRequest &request,
                      doc::NodeId allocated_id, cmd::FrameNumber apply_at,
                      cmd::UndoContextId context = cmd::UndoContextId{4U});

// Renames, reorders or duplicates a viewport or surface. A duplicate takes
// `allocated_id` and is placed directly after its original; a request naming
// something that no longer exists, or an order that is not exactly the
// current list, authors nothing.
[[nodiscard]] bool
schedule_feed_organize(cmd::DirectorScheduler &scheduler,
                       const editor::FeedOrganizeRequest &request,
                       doc::NodeId allocated_id, cmd::FrameNumber apply_at);

[[nodiscard]] bool
schedule_feed_map_delete(cmd::DirectorScheduler &scheduler,
                         const editor::FeedMapDeleteRequest &request,
                         cmd::FrameNumber apply_at);

// Writes the project's stage. The first edit of a project that has none
// creates its output.stage node with `allocated_id` and sets `allocated`; a
// later edit replaces the existing node's value. A non-finite eye authors
// nothing.
[[nodiscard]] bool
schedule_feed_stage_edit(cmd::DirectorScheduler &scheduler,
                         const editor::FeedStageEditRequest &request,
                         doc::NodeId allocated_id, cmd::FrameNumber apply_at,
                         bool &allocated);

// Build from hardware: erases every viewport of the windows the plan lays
// out and adds the plan's, named and in order, in the request's transaction.
// `ids` holds one fresh id per planned viewport. The canvas is the caller's,
// written in the same transaction.
[[nodiscard]] bool
schedule_feed_layout(cmd::DirectorScheduler &scheduler,
                     const editor::FeedLayoutRequest &request,
                     std::span<const doc::NodeId> ids,
                     cmd::FrameNumber apply_at);

} // namespace mediaserver::app
