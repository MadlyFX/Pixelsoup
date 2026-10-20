#pragma once

// Scene command authoring from editor intents: generic and base-property
// edits, locks, project settings, adds, assets, deletes and scene edits.

#include "edit_coalescing_policy.hpp"
#include "project_asset_import.hpp"

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/editor_shell.hpp>
#include <mediaserver/editor/mapping_panels.hpp>
#include <mediaserver/editor/timeline_panel.hpp>
#include <mediaserver/scene/lens.hpp>
#include <mediaserver/scene/mask_authoring.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/scene/scene_project.hpp>
#include <mediaserver/scene/selection_set.hpp>
#include <mediaserver/show/presenter.hpp>
#include <mediaserver/show/show_clock.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mediaserver::app {

[[nodiscard]] std::string
group_edit_error_message(editor::GroupEditAction action,
                         scene::GroupEditError error);

[[nodiscard]] std::string
mask_edit_error_message(scene::MaskAuthoringErrorCode code);

[[nodiscard]] std::string
selection_set_error_message(editor::SelectionSetEditAction action,
                            const scene::SelectionSetError &error);

[[nodiscard]] show::PresenterCommand
map_transport(editor::TransportRequest request) noexcept;

[[nodiscard]] bool schedule_generic_edit(
    cmd::DirectorScheduler &scheduler, const editor::GenericEdit &edit,
    cmd::FrameNumber apply_at, EditCoalescingPlan *coalescing = nullptr,
    std::string* feedback = nullptr);

[[nodiscard]] bool schedule_lock_edit(cmd::DirectorScheduler &scheduler,
                                      const editor::LockEdit &edit,
                                      cmd::FrameNumber apply_at);

[[nodiscard]] bool schedule_property_edit(
    cmd::DirectorScheduler &scheduler, const editor::PropertyEdit &edit,
    cmd::FrameNumber apply_at, EditCoalescingPlan *coalescing = nullptr);

[[nodiscard]] std::string
lens_calibration_property(core::u32 index, std::string_view suffix);

void write_lens_profile(
    doc::Node& node, const std::optional<scene::LensProfile>& profile);

[[nodiscard]] bool schedule_camera_lens_profile(
    cmd::DirectorScheduler& scheduler, const editor::ProjectSettingsEdit& edit,
    cmd::FrameNumber apply_at);

[[nodiscard]] bool
schedule_project_settings(cmd::DirectorScheduler &scheduler,
                          const editor::ProjectSettingsEdit &edit,
                          doc::NodeId new_id, cmd::FrameNumber apply_at);

[[nodiscard]] std::int64_t
placed_clip_duration(const editor::AddIntent &add,
                     show::FrameRate show_rate) noexcept;

[[nodiscard]] doc::Node make_added_node(
    doc::NodeId id, const editor::AddIntent &add, show::FrameRate show_rate,
    const scene::IcvfxCameraSettings &plate_defaults);

[[nodiscard]] bool schedule_add(
    cmd::DirectorScheduler &scheduler, const editor::AddIntent &add,
    doc::NodeId id, cmd::FrameNumber apply_at, show::FrameRate show_rate,
    const scene::IcvfxCameraSettings &plate_defaults = {});

[[nodiscard]] bool
schedule_asset_import(cmd::DirectorScheduler &scheduler,
                      const editor::AssetImportIntent &request, doc::NodeId id,
                      cmd::FrameNumber apply_at);

[[nodiscard]] std::string_view
project_asset_import_error_label(ProjectAssetImportErrorCode code) noexcept;

struct PendingAssetImportAuthoring final {
    core::u64 gesture{};
    std::vector<editor::FeedSurfaceAddRequest> surface_adds;
    std::vector<editor::FeedSurfaceEditRequest> surface_edits;
};

template <typename Request>
void extract_gesture_requests(std::vector<Request> &pending, core::u64 gesture,
                              std::vector<Request> &extracted) {
    const std::size_t count = static_cast<std::size_t>(std::count_if(
        pending.begin(), pending.end(), [gesture](const Request &request) {
            return request.gesture == gesture;
        }));
    extracted.reserve(extracted.size() + count);
    std::size_t retained{};
    for (std::size_t index = 0U; index < pending.size(); ++index) {
        if (pending[index].gesture == gesture) {
            extracted.push_back(std::move(pending[index]));
        } else {
            if (retained != index) {
                pending[retained] = std::move(pending[index]);
            }
            ++retained;
        }
    }
    pending.resize(retained);
}

template <typename Request>
void discard_gesture_requests(std::vector<Request> &pending,
                              core::u64 gesture) {
    std::erase_if(pending, [gesture](const Request &request) {
        return request.gesture == gesture;
    });
}

[[nodiscard]] bool
schedule_asset_delete(cmd::DirectorScheduler &scheduler,
                      const editor::AssetDeleteIntent &request,
                      cmd::FrameNumber apply_at);

[[nodiscard]] bool
schedule_asset_proxy_edit(cmd::DirectorScheduler &scheduler,
                          const editor::AssetProxyEditIntent &request,
                          cmd::FrameNumber apply_at);

[[nodiscard]] bool schedule_delete(cmd::DirectorScheduler &scheduler,
                                   doc::NodeId node, core::u64 gesture,
                                   cmd::FrameNumber apply_at);

[[nodiscard]] bool schedule_scene_edit(cmd::DirectorScheduler &scheduler,
                                       const editor::EditIntent &edit,
                                       cmd::FrameNumber apply_at);

[[nodiscard]] bool schedule_world_edit(cmd::DirectorScheduler &scheduler,
                                       const editor::WorldEdit &edit,
                                       cmd::FrameNumber apply_at);

} // namespace mediaserver::app
