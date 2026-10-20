#pragma once

// Feed map and projection-surface command authoring.

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/mapping_panels.hpp>
#include <mediaserver/output/feed_map.hpp>
#include <mediaserver/output/perspective_mapping.hpp>

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

[[nodiscard]] bool
schedule_feed_map_delete(cmd::DirectorScheduler &scheduler,
                         const editor::FeedMapDeleteRequest &request,
                         cmd::FrameNumber apply_at);

} // namespace mediaserver::app
