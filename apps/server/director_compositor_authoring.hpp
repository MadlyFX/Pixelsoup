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
