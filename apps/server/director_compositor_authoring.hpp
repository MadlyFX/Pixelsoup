#pragma once

// Compositor graph command authoring: nodes, connections, properties and
// deletes.

#include "edit_coalescing_policy.hpp"

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/compositor/compositor_graph.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/workspace_panels.hpp>

#include <optional>
#include <span>
#include <string_view>

namespace mediaserver::app {

[[nodiscard]] std::string_view
compositor_type(compositor::NodeKind kind) noexcept;

[[nodiscard]] doc::Node
make_compositor_node(doc::NodeId id, const editor::CompositorAddIntent &request,
                     doc::NodeId scene);

[[nodiscard]] bool
schedule_compositor_add(cmd::DirectorScheduler &scheduler,
                        const editor::CompositorAddIntent &request,
                        doc::NodeId id, doc::NodeId scene, core::u64 gesture,
                        cmd::FrameNumber apply_at);

[[nodiscard]] bool
schedule_compositor_move(cmd::DirectorScheduler &scheduler,
                         const editor::CompositorMoveIntent &request,
                         core::u64 gesture, cmd::FrameNumber apply_at);

[[nodiscard]] bool schedule_compositor_property(
    cmd::DirectorScheduler &scheduler,
    const editor::CompositorPropertyEditIntent &request, core::u64 gesture,
    cmd::FrameNumber apply_at, EditCoalescingPlan *coalescing = nullptr);

[[nodiscard]] bool
schedule_compositor_connect(cmd::DirectorScheduler &scheduler,
                            const editor::CompositorConnectIntent &request,
                            doc::NodeId id, core::u64 gesture,
                            cmd::FrameNumber apply_at);

// The document node of one wire, with no reroute points.
[[nodiscard]] doc::Node make_compositor_edge(doc::NodeId id,
                                             compositor::EdgeEndpoint from,
                                             compositor::EdgeEndpoint to);

// Adds a node with the wires it arrives with, removing the wires the request
// names, as one undo step. `edge_ids` holds a fresh id for each link. When
// the node cannot be added - a Scene Layer naming nothing - nothing is.
[[nodiscard]] bool schedule_compositor_add_linked(
    cmd::DirectorScheduler &scheduler,
    const editor::CompositorAddIntent &request, doc::NodeId id,
    doc::NodeId scene, std::span<const doc::NodeId> edge_ids,
    core::u64 gesture, cmd::FrameNumber apply_at);

// Removes and adds wires as one undo step. `edge_ids` holds a fresh id for
// each added wire. A wire to a node that no longer exists is skipped.
[[nodiscard]] bool
schedule_compositor_rewire(cmd::DirectorScheduler &scheduler,
                           const editor::CompositorRewireIntent &request,
                           std::span<const doc::NodeId> edge_ids,
                           core::u64 gesture, cmd::FrameNumber apply_at);

// Replaces a wire's reroute points. A list past the cap, or with a
// non-finite or out-of-range point, is ignored whole.
[[nodiscard]] bool
schedule_compositor_waypoints(cmd::DirectorScheduler &scheduler,
                              const editor::CompositorWaypointIntent &request,
                              core::u64 gesture, cmd::FrameNumber apply_at);

[[nodiscard]] std::optional<doc::NodeId>
node_reference(const doc::Node &node, std::string_view property) noexcept;

[[nodiscard]] bool schedule_erase_node(cmd::DirectorScheduler &scheduler,
                                       doc::NodeId id,
                                       cmd::UndoContextId context,
                                       cmd::TransactionId transaction,
                                       cmd::FrameNumber apply_at);

[[nodiscard]] bool
schedule_compositor_delete(cmd::DirectorScheduler &scheduler,
                           const editor::CompositorDeleteIntent &request,
                           core::u64 gesture, cmd::FrameNumber apply_at);

} // namespace mediaserver::app
