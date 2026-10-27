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

// A keyboard nudge passes the coalescing plan so a run of nudges is one
// undo step; every node it moves continues that same step.
[[nodiscard]] bool
schedule_compositor_move(cmd::DirectorScheduler &scheduler,
                         const editor::CompositorMoveIntent &request,
                         core::u64 gesture, cmd::FrameNumber apply_at,
                         EditCoalescingPlan *coalescing = nullptr);

// Sets a node's or comment box's title, or clears a node's back to its
// kind's name.
[[nodiscard]] bool
schedule_compositor_rename(cmd::DirectorScheduler &scheduler,
                           const editor::CompositorRenameIntent &request,
                           core::u64 gesture, cmd::FrameNumber apply_at);

// Creates a comment box at `id` in `scene` when the request names none, or
// changes the one it names. A box smaller than the minimum, out of range, or
// with an unknown colour is ignored.
[[nodiscard]] bool
schedule_compositor_comment(cmd::DirectorScheduler &scheduler,
                            const editor::CompositorCommentIntent &request,
                            doc::NodeId id, doc::NodeId scene,
                            core::u64 gesture, cmd::FrameNumber apply_at);

// Collapses or opens nodes, as one undo step. Layout only.
[[nodiscard]] bool
schedule_compositor_collapse(cmd::DirectorScheduler &scheduler,
                             const editor::CompositorCollapseIntent &request,
                             core::u64 gesture, cmd::FrameNumber apply_at);

// Gives wires new places in their inputs' order, as one undo step. Anything
// that is not a wire is left alone.
[[nodiscard]] bool
schedule_compositor_reorder(cmd::DirectorScheduler &scheduler,
                            const editor::CompositorReorderIntent &request,
                            core::u64 gesture, cmd::FrameNumber apply_at);

// Bypasses nodes or puts them back to work, as one undo step. Nodes that
// cannot be bypassed are left alone.
[[nodiscard]] bool
schedule_compositor_bypass(cmd::DirectorScheduler &scheduler,
                           const editor::CompositorBypassIntent &request,
                           core::u64 gesture, cmd::FrameNumber apply_at);

// Where a pasted snapshot's top-left is now: the smallest x and y of its
// nodes and comment boxes, in graph units.
[[nodiscard]] std::pair<float, float>
compositor_snapshot_origin(std::span<const doc::Node> snapshot) noexcept;

// Puts a planned paste as one undo step, and moves `next_id` past the ids it
// used. Returns false only when the scheduler refuses a node.
[[nodiscard]] bool
schedule_compositor_paste(cmd::DirectorScheduler &scheduler,
                          std::span<const doc::Node> puts, core::u64 gesture,
                          cmd::FrameNumber apply_at, core::u64 &next_id);

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
