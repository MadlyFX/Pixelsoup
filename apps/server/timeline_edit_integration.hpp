#pragma once

#include "clip_clipboard.hpp"

#include <mediaserver/core/result.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/editor/editor_shell.hpp>
#include <mediaserver/scene/timeline_edit.hpp>

#include <span>
#include <vector>

namespace mediaserver::app {

enum class TimelineEditIntegrationError : core::u8 {
    ClipboardIncompatible,
    TargetLaneUnavailable,
    TimelineRejected,
    AuthoringRejected,
    PasteRejected,
    AllocationFailed,
};

// Complete document replacement for one advanced Timeline intent. Insert and
// Overwrite contain both the displaced/trimmed existing graph and the pasted
// clipboard subgraph, so the caller schedules exactly this Root once.
struct TimelineEditIntegrationPlan final {
    doc::Root document;
    scene::TimelineEditPlan timeline;
    std::vector<doc::NodeId> changed;
    std::vector<doc::NodeId> erased;
    std::vector<doc::Node> created;
    core::u64 next_node{};
};

[[nodiscard]] core::Result<
    TimelineEditIntegrationPlan,
    TimelineEditIntegrationError>
plan_timeline_edit_integration(
    const doc::Root& document,
    std::span<const scene::ClipPlacement> clips,
    const editor::TimelineEditIntent& request,
    const ClipClipboard& clipboard,
    core::u64 next_node);

} // namespace mediaserver::app
