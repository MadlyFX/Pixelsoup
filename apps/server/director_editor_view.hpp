#pragma once

// Editor-view projection helpers: the element list and Output Monitor sync
// rows.

#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/editor/element.hpp>
#include <mediaserver/editor/workspace_panels.hpp>
#include <mediaserver/scene/linked_scene.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/sync/sync_group.hpp>
#include <mediaserver/sync/sync_policy.hpp>
#include <mediaserver/sync/sync_status.hpp>

#include <span>
#include <string_view>
#include <vector>

namespace mediaserver::app {

[[nodiscard]] double read_transform(const doc::Node &node,
                                    std::string_view name,
                                    double fallback) noexcept;

[[nodiscard]] std::vector<editor::ElementInfo> scene_elements(
    const cmd::DirectorScheduler &scheduler, const scene::Scene &scene,
    std::span<const scene::ClipPlacement> clips = {}, core::u64 show_frame = 0U,
    std::span<const scene::TimelineLane> lanes = {},
    std::span<const scene::CameraPlacement> cameras = {},
    std::span<const scene::TrackingSource> tracking_sources = {},
    const scene::LinkedSceneState* linked = nullptr);

void apply_node_sync_rows(std::vector<editor::OutputMonitorState::Node> &rows,
                          const sync::SyncPolicy &policy,
                          const sync::SyncTopology &topology,
                          const sync::SyncStatusSnapshot &status);

} // namespace mediaserver::app
