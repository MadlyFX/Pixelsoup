#pragma once

#include <mediaserver/cmd/node_command_bus.hpp>
#include <mediaserver/net/node_session_service.hpp>
#include <mediaserver/show/frame_boundary_driver.hpp>

namespace mediaserver::net {

// Commit seam over a supervised Director connection.
//
// The show clock owns this side of the boundary: it is the only caller that
// publishes, activates a staged checkpoint, or reads the visible root. Every
// readiness question is answered by NodeSessionService::boundary_state(),
// which takes the session locks, so the driver never reads the command bus
// that the network worker is concurrently staging into.
//
// The borrowed service and command bus must outlive this sink.
class NodeServiceFrameCommitSink final : public show::FrameCommitSink {
public:
    NodeServiceFrameCommitSink(
        NodeSessionService& service,
        cmd::NodeCommandBus& node) noexcept
        : service_(&service), node_(&node) {}

    [[nodiscard]] cmd::FrameNumber committed_through() const noexcept override;
    [[nodiscard]] show::FrameCommitOutcome poll(
        cmd::FrameNumber frame) const noexcept override;
    [[nodiscard]] show::FrameCommitReport commit(
        cmd::FrameNumber frame) noexcept override;
    [[nodiscard]] bool activate_pending_checkpoint() noexcept override;

    // Safe from the boundary thread specifically: the visible root is written
    // only by commit_frame() and checkpoint activation, both of which this
    // sink performs, while the network worker only stages prepared roots.
    [[nodiscard]] const doc::Root& visible_root() const noexcept override {
        return node_->document();
    }

    // Latched diagnostics for the operator surface. These describe the commit
    // path rather than the document, so they are reported separately from the
    // frames handed to playback.
    [[nodiscard]] core::u64 checkpoint_activations() const noexcept {
        return checkpoint_activations_;
    }

private:
    NodeSessionService* service_;
    cmd::NodeCommandBus* node_;
    core::u64 checkpoint_activations_{};
};

} // namespace mediaserver::net
