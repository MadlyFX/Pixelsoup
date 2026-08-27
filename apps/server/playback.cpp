#include "playback.hpp"
#include "cluster_observability.hpp"
#include "output_window_config.hpp"
#include "output_deployment.hpp"
#include "project_asset_import.hpp"
#include "project_asset_resolution_worker.hpp"
#include "project_assets.hpp"
#include "system_hardware_probe.hpp"

#include <mediaserver/assets/asset_model.hpp>
#include <mediaserver/assets/asset_schema.hpp>
#include <mediaserver/cmd/command.hpp>
#include <mediaserver/compositor/compositor_model.hpp>
#include <mediaserver/compositor/compositor_schema.hpp>
#include <mediaserver/net/frame_commit_sink.hpp>
#include <mediaserver/output/feed_map_model.hpp>
#include <mediaserver/output/feed_map_schema.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_project.hpp>
#include <mediaserver/scene/scene_schema.hpp>
#include <mediaserver/show/demo_edit_scene.hpp>
#include <mediaserver/show/demo_feed_map.hpp>
#include <mediaserver/show/demo_scene.hpp>
#include <mediaserver/show/frame_boundary_driver.hpp>
#include <mediaserver/show/presenter.hpp>
#include <mediaserver/show/test_pattern.hpp>
#include <mediaserver/show/transport_author.hpp>
#include <mediaserver/show/transport_authority.hpp>
#include <mediaserver/show/switcher_author.hpp>
#include <mediaserver/show/switcher_authority.hpp>
#include <mediaserver/show/switcher_state.hpp>
#include <mediaserver/sync/sync_group.hpp>
#include <mediaserver/sync/sync_policy_model.hpp>
#include <mediaserver/sync/sync_readiness.hpp>
#include <mediaserver/sync/sync_schema.hpp>
#include <mediaserver/topology/hardware_map_model.hpp>
#include <mediaserver/topology/hardware_map_schema.hpp>

#if defined(MEDIASERVER_HAS_GUI)
#include "cluster_lifecycle_controller.hpp"
#include "project_asset_import_worker.hpp"

#include <mediaserver/editor/editor_shell.hpp>
#include <mediaserver/persist/project_file.hpp>
#include <mediaserver/persist/recent_project.hpp>
#include <mediaserver/render/media_source.hpp>
#include <mediaserver/ui/sdl_presenter.hpp>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#endif

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace mediaserver::app {
namespace {

using Clock = show::ShowClock::Clock;

// Shutdown latency ceiling. A boundary further away than this is slept to in
// steps so an interrupt is still observed promptly.
constexpr std::chrono::milliseconds max_sleep{10};

// Bound on the frames one Director iteration will seal after a late wake-up.
constexpr core::u64 max_director_catch_up = 8U;

// Barrier records are diagnostic slots that must be retired once their cohort
// completes; this is the backlog the Director tolerates before refusing to
// seal further.
constexpr std::size_t max_tracked_frames = 64U;

// A render process can start before the synchronized store has received every
// required object. Retrying follows the lifecycle readiness cadence; content
// is normally installed before Start, and a known corrupt large object must
// not turn into continuous background disk saturation.
constexpr std::chrono::seconds asset_resolution_retry_interval{30};

struct AssetResolutionRetrySchedule final {
    Clock::time_point not_before{};
    bool armed{};

    void disarm() noexcept {
        armed = false;
    }

    void after_failure(Clock::time_point now) noexcept {
        armed = true;
        not_before = now + asset_resolution_retry_interval;
    }

    [[nodiscard]] bool due(Clock::time_point now) const noexcept {
        return armed && now >= not_before;
    }
};

[[nodiscard]] core::u64 issue_asset_resolution_request(
    core::u64& last_request_id) noexcept {
    if (last_request_id == std::numeric_limits<core::u64>::max()) {
        return 0U;
    }
    return ++last_request_id;
}

void swap_project_asset_resolution_plan(
    ProjectAssetResolutionPlan& left,
    ProjectAssetResolutionPlan& right) noexcept {
    using std::swap;
    left.manifest.assets.swap(right.manifest.assets);
    swap(left.manifest.unregistered_references,
         right.manifest.unregistered_references);
    swap(left.manifest.kind_mismatches, right.manifest.kind_mismatches);
    swap(left.manifest.unverified_assets,
         right.manifest.unverified_assets);
    swap(left.manifest.dependency_manifests_required,
         right.manifest.dependency_manifests_required);
    swap(left.manifest.non_shared_assets,
         right.manifest.non_shared_assets);
    swap(left.manifest.digest, right.manifest.digest);
    left.bindings.swap(right.bindings);
    left.fail_closed.assets.swap(right.fail_closed.assets);
    left.fail_closed.placements.swap(right.fail_closed.placements);
    left.fail_closed.sources.swap(right.fail_closed.sources);
    left.fail_closed.observations.swap(right.fail_closed.observations);
}

// Playback owns only immutable plans and overlays. All first/full resolution
// and every retry run on the persistent worker; installing the plan itself is
// filesystem-free and immediately suppresses managed authored paths.
struct PlaybackAssetResolution final {
    std::shared_ptr<const ProjectAssetResolutionPlan> plan;
    std::shared_ptr<const ResolvedProjectAssets> overlay;
    // Emergency ownership when allocating the worker's shared immutable plan
    // fails. Its fail-closed overlay remains immediately usable, and the
    // shared allocation is retried on the bounded cadence.
    ProjectAssetResolutionPlan pending_plan;
    bool pending_plan_active{};
    std::unique_ptr<ProjectAssetResolutionWorker> worker;
    ProjectAssetResolutionKey key;
    ProjectAssetResolutionMode retry_mode{
        ProjectAssetResolutionMode::Full};
    AssetResolutionRetrySchedule retry;
    core::u64 last_request_id{};
    bool locally_usable{};
    bool ready{};

    PlaybackAssetResolution() {
        try {
            worker = std::make_unique<ProjectAssetResolutionWorker>();
        } catch (...) {
            // A missing worker is fail-closed. Playback remains responsive and
            // may report the unresolved state, but cannot adopt managed bytes.
        }
    }

    [[nodiscard]] bool install(
        ProjectAssetResolutionPlan&& next,
        const assets::ContentResolutionRoots& roots,
        core::u64 project_generation, Clock::time_point now) noexcept {
        std::shared_ptr<const ProjectAssetResolutionPlan> immutable_plan;
        try {
            auto mutable_plan =
                std::make_shared<ProjectAssetResolutionPlan>();
            // Allocation has succeeded; member-wise container swaps transfer
            // ownership without relying on MSVC's potentially-throwing map
            // move constructor.
            swap_project_asset_resolution_plan(*mutable_plan, next);
            immutable_plan = std::move(mutable_plan);
        } catch (const std::bad_alloc&) {
            if (worker != nullptr) {
                worker->cancel();
            }
            swap_project_asset_resolution_plan(pending_plan, next);
            pending_plan_active = true;
            plan.reset();
            overlay.reset();
            key = ProjectAssetResolutionKey{
                0U, pending_plan.manifest.digest, project_generation, roots};
            locally_usable = false;
            ready = false;
            retry_mode = ProjectAssetResolutionMode::Full;
            retry.after_failure(now);
            return false;
        }
        std::shared_ptr<const ResolvedProjectAssets> deny{
            immutable_plan, &immutable_plan->fail_closed};
        plan = std::move(immutable_plan);
        overlay = std::move(deny);
        // `next` is empty after the transfer above. Swap any obsolete
        // emergency manifest into it so this state does not retain it.
        swap_project_asset_resolution_plan(pending_plan, next);
        pending_plan_active = false;
        locally_usable = false;
        ready = false;
        retry_mode = ProjectAssetResolutionMode::Full;

        const core::u64 request_id =
            issue_asset_resolution_request(last_request_id);
        if (worker == nullptr || request_id == 0U) {
            retry.after_failure(now);
            key = ProjectAssetResolutionKey{
                request_id, plan->manifest.digest, project_generation, roots};
            return false;
        }
        ProjectAssetResolutionKey next_key{
            request_id, plan->manifest.digest, project_generation, roots};
        const auto submitted = worker->submit(ProjectAssetResolutionRequest{
            next_key, plan, overlay, ProjectAssetResolutionMode::Full});
        key = std::move(next_key);
        if (submitted != ProjectAssetResolutionSubmitResult::Accepted) {
            retry.after_failure(now);
            return false;
        }
        retry.disarm();
        return true;
    }

    // Returns true only when an exact-fenced completion installed a new
    // overlay. Callers may publish adoption true after this returns.
    [[nodiscard]] bool poll(Clock::time_point now) {
        if (worker == nullptr) {
            return false;
        }
        auto completed = worker->take_completed();
        if (!completed.has_value() || completed->key != key) {
            return false;
        }
        if (!completed->succeeded || completed->resolved == nullptr) {
            locally_usable = false;
            ready = false;
            retry_mode = ProjectAssetResolutionMode::Full;
            retry.after_failure(now);
            return false;
        }

        // Move/install first. The adoption signal may only become true after
        // this exact overlay is the one consumed by subsequent scene frames.
        overlay = std::move(completed->resolved);
        locally_usable = completed->locally_usable;
        ready = completed->ready;
        if (locally_usable || !overlay->has_retryable_observations()) {
            retry.disarm();
        } else {
            retry_mode = ProjectAssetResolutionMode::RetryUnresolved;
            retry.after_failure(now);
        }
        return true;
    }

    [[nodiscard]] const ResolvedProjectAssets* current_overlay()
        const noexcept {
        if (pending_plan_active) {
            return &pending_plan.fail_closed;
        }
        return overlay.get();
    }

    [[nodiscard]] bool locally_ready_for(
        core::u64 manifest_digest,
        const assets::ContentResolutionRoots& roots,
        core::u64 project_generation) const noexcept {
        return locally_usable && !pending_plan_active && plan != nullptr &&
            overlay != nullptr && key.request_id != 0U &&
            key.manifest_digest == manifest_digest && key.roots == roots &&
            key.project_generation == project_generation;
    }

    void submit_retry_if_due(Clock::time_point now) {
        if (worker == nullptr || !retry.due(now) || !worker->idle()) {
            return;
        }
        if (pending_plan_active) {
            try {
                auto mutable_plan =
                    std::make_shared<ProjectAssetResolutionPlan>();
                swap_project_asset_resolution_plan(
                    *mutable_plan, pending_plan);
                std::shared_ptr<const ProjectAssetResolutionPlan>
                    immutable_plan{std::move(mutable_plan)};
                std::shared_ptr<const ResolvedProjectAssets> deny{
                    immutable_plan, &immutable_plan->fail_closed};
                plan = std::move(immutable_plan);
                overlay = std::move(deny);
                pending_plan_active = false;
                retry_mode = ProjectAssetResolutionMode::Full;
            } catch (const std::bad_alloc&) {
                retry.after_failure(now);
                return;
            }
        }
        if (plan == nullptr || overlay == nullptr) {
            retry.after_failure(now);
            return;
        }
        const core::u64 request_id =
            issue_asset_resolution_request(last_request_id);
        if (request_id == 0U) {
            retry.after_failure(now);
            return;
        }
        ProjectAssetResolutionKey next_key{
            request_id, plan->manifest.digest, key.project_generation,
            key.roots};
        const auto submitted = worker->submit(ProjectAssetResolutionRequest{
            next_key, plan, overlay, retry_mode});
        if (submitted == ProjectAssetResolutionSubmitResult::Accepted) {
            key = std::move(next_key);
            retry.disarm();
        } else {
            retry.after_failure(now);
        }
    }
};

#if defined(MEDIASERVER_HAS_GUI)
[[nodiscard]] std::filesystem::path native_path_from_utf8(
    std::string_view text) {
    std::u8string encoded;
    encoded.reserve(text.size());
    for (const char byte : text) {
        encoded.push_back(
            static_cast<char8_t>(static_cast<unsigned char>(byte)));
    }
    return std::filesystem::path{encoded};
}

[[nodiscard]] std::string native_path_to_utf8(
    const std::filesystem::path& path) {
    const std::u8string encoded = path.u8string();
    std::string text;
    text.reserve(encoded.size());
    for (const char8_t byte : encoded) {
        text.push_back(std::bit_cast<char>(byte));
    }
    return text;
}

[[nodiscard]] bool project_content_changed(const doc::Root& saved,
                                           const doc::Root& current) {
    const doc::Delta delta = doc::diff(saved, current);
    return std::any_of(
        delta.changes.begin(), delta.changes.end(),
        [](const doc::NodeChange& change) {
            // Play/pause/seek re-anchor live transport state; they should not
            // put an asterisk beside an otherwise unchanged project. Timeline
            // range edits set the explicit dirty flag at the editor boundary.
            return change.id != show::transport_node_id;
        });
}

// Recovery may borrow a saved path only when all durable project content
// agrees. Transport playhead/state is live session state, but its direction,
// rate, range, and loop flag are authored project configuration and must match.
[[nodiscard]] bool recovered_project_matches_saved_baseline(
    const doc::Root& saved,
    const doc::Root& recovered) {
    if (project_content_changed(saved, recovered)) {
        return false;
    }
    const doc::Node* const saved_node = saved.get(show::transport_node_id);
    const doc::Node* const recovered_node =
        recovered.get(show::transport_node_id);
    if (saved_node == nullptr || recovered_node == nullptr) {
        return saved_node == nullptr && recovered_node == nullptr;
    }
    const auto saved_transport = show::read_transport(saved);
    const auto recovered_transport = show::read_transport(recovered);
    if (!saved_transport.has_value() || !recovered_transport.has_value()) {
        return false;
    }
    return saved_transport->direction == recovered_transport->direction &&
        saved_transport->rate == recovered_transport->rate &&
        saved_transport->first_frame == recovered_transport->first_frame &&
        saved_transport->last_frame == recovered_transport->last_frame &&
        saved_transport->loop == recovered_transport->loop;
}

[[nodiscard]] std::string project_file_failure(std::string_view operation,
                                               unsigned code) {
    std::string message{"ERROR "};
    message.append(operation);
    message.append(" PROJECT (");
    message.append(std::to_string(code));
    message.push_back(')');
    return message;
}

[[nodiscard]] std::string_view project_file_error_label(
    persist::ProjectFileErrorCode code) noexcept {
    switch (code) {
    case persist::ProjectFileErrorCode::InvalidPath:
        return "INVALID PROJECT PATH";
    case persist::ProjectFileErrorCode::EncodeFailed:
        return "PROJECT DATA CANNOT BE SAVED";
    case persist::ProjectFileErrorCode::OpenFailed:
        return "FILE COULD NOT BE OPENED";
    case persist::ProjectFileErrorCode::ReadFailed:
        return "FILE COULD NOT BE READ";
    case persist::ProjectFileErrorCode::WriteFailed:
        return "FILE COULD NOT BE WRITTEN";
    case persist::ProjectFileErrorCode::SyncFailed:
        return "FILE DURABILITY COULD NOT BE CONFIRMED";
    case persist::ProjectFileErrorCode::ReplaceFailed:
        return "EXISTING FILE COULD NOT BE REPLACED";
    case persist::ProjectFileErrorCode::InvalidFormat:
        return "NOT A MEDIA SERVER PROJECT";
    case persist::ProjectFileErrorCode::UnsupportedVersion:
        return "PROJECT VERSION IS NOT SUPPORTED";
    case persist::ProjectFileErrorCode::CorruptFile:
        return "PROJECT FILE IS DAMAGED OR INCOMPLETE";
    case persist::ProjectFileErrorCode::UnexpectedPayload:
    case persist::ProjectFileErrorCode::InvalidProjectCursor:
        return "NOT A STANDALONE PROJECT";
    case persist::ProjectFileErrorCode::SizeLimitExceeded:
        return "PROJECT EXCEEDS SAFETY LIMITS";
    case persist::ProjectFileErrorCode::AllocationFailed:
        return "NOT ENOUGH MEMORY";
    }
    return "UNKNOWN PROJECT FILE ERROR";
}

[[nodiscard]] std::string project_file_failure(
    std::string_view operation,
    const persist::ProjectFileError& error) {
    std::string message{"ERROR "};
    message.append(operation);
    message.append(" PROJECT: ");
    message.append(project_file_error_label(error.code));
    return message;
}

struct MediaRateEnrichment final {
    doc::Root project;
    std::size_t updated_nodes{};
};

// Projects authored before native video cadence was persisted remain readable
// as 1:1 sequences in SceneModel. At the Director's project boundary, enrich
// only nodes with *neither* rate field. A partial/malformed authored pair is
// deliberately left intact for the schema model to reject, rather than being
// silently repaired. probe_media_frame_rate itself rejects non-video paths, and
// this cache opens each distinct recognized video at most once per load.
[[nodiscard]] MediaRateEnrichment enrich_media_frame_rates(
    const doc::Root& project) {
    struct CachedProbe final {
        std::string source;
        std::optional<render::MediaFrameRate> rate;
    };

    MediaRateEnrichment result{project, 0U};
    std::vector<CachedProbe> probes;
    const auto rate_for = [&](std::string_view source)
        -> std::optional<render::MediaFrameRate> {
        const auto found = std::find_if(
            probes.begin(), probes.end(), [&](const CachedProbe& entry) {
                return entry.source == source;
            });
        if (found != probes.end()) {
            return found->rate;
        }
        const std::string owned{source};
        const auto rate = render::probe_media_frame_rate(owned);
        probes.push_back(CachedProbe{owned, rate});
        return rate;
    };

    const doc::Delta contents = doc::diff(doc::Root{}, project);
    for (const doc::NodeChange& change : contents.changes) {
        if (!change.after) {
            continue;
        }
        const doc::Node& node = *change.after;
        const bool placement =
            std::string_view{node.type} == scene::schema::media_type;
        const bool asset =
            std::string_view{node.type} == assets::schema::media_type;
        if (!placement && !asset) {
            continue;
        }

        const std::string_view numerator_name = placement
            ? scene::schema::source_rate_numerator
            : assets::schema::media_rate_numerator;
        const std::string_view denominator_name = placement
            ? scene::schema::source_rate_denominator
            : assets::schema::media_rate_denominator;
        // Any authored member makes this pair intentional. In particular, do
        // not hide a partial pair by filling in its missing half.
        if (node.properties.find(numerator_name) != node.properties.end() ||
            node.properties.find(denominator_name) != node.properties.end()) {
            continue;
        }
        const auto source = node.properties.find(
            placement ? scene::schema::source : assets::schema::source);
        if (source == node.properties.end()) {
            continue;
        }
        const auto* const path = std::get_if<std::string>(&source->second);
        if (path == nullptr || path->empty()) {
            continue;
        }
        const auto rate = rate_for(*path);
        if (!rate.has_value()) {
            continue;
        }

        doc::Node enriched = node;
        enriched.properties.emplace(
            std::string{numerator_name},
            doc::PropertyValue{static_cast<std::int64_t>(rate->numerator)});
        enriched.properties.emplace(
            std::string{denominator_name},
            doc::PropertyValue{static_cast<std::int64_t>(rate->denominator)});
        result.project = result.project.set(std::move(enriched));
        ++result.updated_nodes;
    }
    return result;
}

[[nodiscard]] bool restore_project_transport(
    show::TransportAuthority& transport,
    const show::TransportAnchor& loaded,
    cmd::FrameNumber at) noexcept {
    if (!show::valid_transport_anchor(loaded)) {
        return false;
    }
    if (transport.state() == show::TransportState::Playing &&
        !transport.pause(at)) {
        return false;
    }
    if (!transport.set_rate(loaded.rate, at) ||
        !transport.set_range(loaded.first_frame, loaded.last_frame,
                              loaded.loop, at) ||
        !transport.set_direction(loaded.direction, at) ||
        !transport.seek(loaded.anchor_show_frame, at)) {
        return false;
    }
    switch (loaded.state) {
    case show::TransportState::Stopped:
        return static_cast<bool>(transport.stop(at));
    case show::TransportState::Paused:
        if (transport.state() == show::TransportState::Paused) {
            return true;
        }
        if (transport.state() == show::TransportState::Stopped &&
            !transport.play(at)) {
            return false;
        }
        return static_cast<bool>(transport.pause(at));
    case show::TransportState::Playing:
        return transport.state() == show::TransportState::Playing ||
            static_cast<bool>(transport.play(at));
    }
    return false;
}

// A switcher dissolve is transient control-frame state, not a timeline asset.
// Loading it verbatim can leave Rehearsal locked until a control-frame ordinal
// from another run is reached. Preserve the stable Program/Rehearsal choices,
// validate that both still name scenes, and settle the transition at this
// Director's load boundary.
[[nodiscard]] bool normalize_project_switcher(
    doc::Root& project,
    cmd::FrameNumber at) {
    const doc::Node* const raw = project.get(show::switcher_node_id);
    const auto loaded = show::read_switcher(project);
    if (!loaded.has_value()) {
        return raw == nullptr ||
            std::string_view{raw->type} != show::switcher_node_type;
    }

    scene::SceneModel scenes;
    scenes.reset(project);
    const auto scene_infos = scenes.scenes();
    const auto contains = [&](scene::SceneId id) {
        return std::any_of(
            scene_infos.begin(), scene_infos.end(),
            [id](const scene::SceneInfo& info) { return info.id == id; });
    };
    if (!contains(loaded->program_scene) ||
        !contains(loaded->rehearsal_scene)) {
        return false;
    }

    auto settled = show::SwitcherAuthority::create(
        loaded->program_scene, loaded->rehearsal_scene, at);
    if (!settled) {
        return false;
    }
    project = project.set(show::make_switcher_node(
        std::move(settled).value().anchor()));
    return true;
}

enum class ProjectLoadStageErrorCode {
    ScheduleFailed,
    PrepareFailed,
    WirePreflightFailed,
    WalPreflightFailed,
    AbortFailed,
    AllocationFailed,
};

struct ProjectLoadStageError final {
    ProjectLoadStageErrorCode code{};
    std::optional<cmd::ScheduleError> schedule_error;
    std::optional<cmd::SealError> seal_error;
    std::optional<persist::CommandLogError> command_log_error;
};

using ProjectLoadStageResult =
    core::Result<cmd::DirectorScheduler, ProjectLoadStageError>;

[[nodiscard]] bool command_log_limit_error(
    const persist::CommandLogError& error) noexcept {
    if (const auto* const failure =
            std::get_if<persist::LogFailure>(&error)) {
        return failure->error.code == persist::LogErrorCode::LimitExceeded;
    }
    if (const auto* const failure =
            std::get_if<persist::EncodeFailure>(&error)) {
        return failure->error == wire::EncodeError::SizeLimitExceeded;
    }
    if (const auto* const failure =
            std::get_if<persist::DecodeFailure>(&error)) {
        return failure->error == wire::DecodeError::SizeLimitExceeded;
    }
    return false;
}

// Build the replacement and its exact next-frame seal away from the live
// scheduler. EditsOnly verifies the replicated wire batch without imposing a
// stale WAL tail; EveryFrame instead runs the complete CommandLog append gate.
// The prepared seal is aborted before returning so the normal barrier remains
// the sole live seal owner.
[[nodiscard]] ProjectLoadStageResult stage_project_load(
    const cmd::DirectorScheduler& current,
    const doc::Root& replacement,
    cmd::FrameNumber frame,
    persist::CommandLog& command_log,
    persist::FrameDurability frame_durability) {
    try {
        cmd::DirectorScheduler staged = current;
        auto replaced = staged.schedule_document_replace(
            replacement,
            cmd::UndoContextId{7U},
            cmd::TransactionId{frame.value()},
            frame);
        if (!replaced) {
            return ProjectLoadStageResult::failure(ProjectLoadStageError{
                ProjectLoadStageErrorCode::ScheduleFailed,
                replaced.error(),
                std::nullopt,
                std::nullopt,
            });
        }

        staged.clear_undo_history();
        auto prepared = staged.prepare_seal(frame);
        if (!prepared) {
            return ProjectLoadStageResult::failure(ProjectLoadStageError{
                ProjectLoadStageErrorCode::PrepareFailed,
                std::nullopt,
                prepared.error(),
                std::nullopt,
            });
        }

        const auto token = prepared.value().token;
        auto preflight =
            frame_durability == persist::FrameDurability::EveryFrame
            ? command_log.preflight(
                  staged.commands_for(frame), prepared.value().seal)
            : command_log.preflight_wire(
                  staged.commands_for(frame), prepared.value().seal);
        if (!preflight) {
            return ProjectLoadStageResult::failure(ProjectLoadStageError{
                frame_durability == persist::FrameDurability::EveryFrame
                    ? ProjectLoadStageErrorCode::WalPreflightFailed
                    : ProjectLoadStageErrorCode::WirePreflightFailed,
                std::nullopt,
                std::nullopt,
                preflight.error(),
            });
        }
        if (!staged.abort_prepared_seal(token)) {
            return ProjectLoadStageResult::failure(ProjectLoadStageError{
                ProjectLoadStageErrorCode::AbortFailed,
                std::nullopt,
                std::nullopt,
                std::nullopt,
            });
        }
        return ProjectLoadStageResult::success(std::move(staged));
    } catch (const std::bad_alloc&) {
        return ProjectLoadStageResult::failure(ProjectLoadStageError{
            ProjectLoadStageErrorCode::AllocationFailed,
            std::nullopt,
            std::nullopt,
            std::nullopt,
        });
    } catch (const std::length_error&) {
        return ProjectLoadStageResult::failure(ProjectLoadStageError{
            ProjectLoadStageErrorCode::AllocationFailed,
            std::nullopt,
            std::nullopt,
            std::nullopt,
        });
    }
}
#endif

// Schedules the built-in scene as one transaction of node creations applied at
// `apply_at`. Every edit is an ordinary command, so it replicates, journals and
// undoes through the paths everything else does - there is no second mechanism
// for 3D. Returns false if the scheduler refuses any edit, which is a
// programming error here rather than a runtime condition.
[[nodiscard]] bool author_scene_document(
    cmd::DirectorScheduler& scheduler,
    cmd::FrameNumber apply_at) {
    const cmd::UndoContextId undo_context{1U};
    const cmd::TransactionId transaction{1U};
    for (const auto& node : show::demo_scene()) {
        const auto node_id = node.id;
        auto scheduled = scheduler.schedule_edit(
            cmd::PutNode{node},
            cmd::EraseNode{node_id},
            undo_context,
            transaction,
            apply_at);
        if (!scheduled) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] doc::Node editor_canvas_feed_map_node(
    core::u32 width,
    core::u32 height) {
    namespace schema = output::schema;

    doc::Node node{};
    node.id = doc::NodeId{show::demo_feed_map_node_base};
    node.type = std::string{schema::map_type};
    node.properties.emplace(
        std::string{schema::node_index}, doc::PropertyValue{std::int64_t{0}});
    node.properties.emplace(
        std::string{schema::source_x}, doc::PropertyValue{std::int64_t{0}});
    node.properties.emplace(
        std::string{schema::source_y}, doc::PropertyValue{std::int64_t{0}});
    node.properties.emplace(
        std::string{schema::source_width},
        doc::PropertyValue{static_cast<std::int64_t>(width)});
    node.properties.emplace(
        std::string{schema::source_height},
        doc::PropertyValue{static_cast<std::int64_t>(height)});
    return node;
}

// The initial feed map, scheduled the same way as the scene: ordinary
// node-creation commands in one transaction, so it replicates, journals, and
// undoes through the same paths. Explicit output demos keep their two slices;
// a fresh editor gets one full-canvas node-0 slice for its supervised output.
[[nodiscard]] bool author_feed_map_document(
    cmd::DirectorScheduler& scheduler,
    cmd::FrameNumber apply_at,
    InitialFeedMapPlan plan) {
    std::vector<doc::Node> nodes;
    switch (plan.kind) {
    case InitialFeedMapKind::None:
        return true;
    case InitialFeedMapKind::OutputDemo:
        nodes = show::demo_feed_map();
        break;
    case InitialFeedMapKind::EditorCanvas:
        nodes.push_back(
            editor_canvas_feed_map_node(plan.source_width, plan.source_height));
        break;
    }

    const cmd::UndoContextId undo_context{1U};
    const cmd::TransactionId transaction{1U};
    for (const auto& node : nodes) {
        const auto node_id = node.id;
        auto scheduled = scheduler.schedule_edit(
            cmd::PutNode{node},
            cmd::EraseNode{node_id},
            undo_context,
            transaction,
            apply_at);
        if (!scheduled) {
            return false;
        }
    }
    return true;
}

#if defined(MEDIASERVER_HAS_GUI)
// Writes the demo clip as a short image sequence of TGAs to a temp directory and
// returns a '#'-run template the renderer resolves per frame. A stand-in for
// real media the operator would drop in: enough to have something on the wall to
// drag while the show plays. TGA because it needs no encoder library.
[[nodiscard]] std::string write_demo_clip() {
    namespace fs = std::filesystem;
    const auto directory = fs::temp_directory_path() / "mediaserver-editclip";
    std::error_code ec;
    fs::create_directories(directory, ec);

    constexpr core::u32 width = 320U;
    constexpr core::u32 height = 180U;
    std::array<core::u8, 18U> header{};
    header[2U] = 2U; // uncompressed true-colour
    header[12U] = static_cast<core::u8>(width & 0xffU);
    header[13U] = static_cast<core::u8>((width >> 8U) & 0xffU);
    header[14U] = static_cast<core::u8>(height & 0xffU);
    header[15U] = static_cast<core::u8>((height >> 8U) & 0xffU);
    header[16U] = 32U;
    header[17U] = 0x28U; // top-left origin, 8 alpha bits

    std::vector<core::u8> pixels(
        static_cast<std::size_t>(width) * height * 4U, 0U);
    for (core::u32 frame = 0U; frame < show::demo_clip_frames; ++frame) {
        const auto bar = (frame * width) / show::demo_clip_frames;
        for (core::u32 y = 0U; y < height; ++y) {
            for (core::u32 x = 0U; x < width; ++x) {
                const auto index = (static_cast<std::size_t>(y) * width + x) * 4U;
                auto r = static_cast<core::u8>((x * 255U) / (width - 1U));
                auto g = static_cast<core::u8>((y * 255U) / (height - 1U));
                auto b = static_cast<core::u8>(
                    (frame * 255U) / (show::demo_clip_frames - 1U));
                if (x >= bar && x < bar + 14U) {
                    r = 255U;
                    g = 255U;
                    b = 255U;
                }
                pixels[index + 0U] = b; // TGA is BGRA
                pixels[index + 1U] = g;
                pixels[index + 2U] = r;
                pixels[index + 3U] = 255U;
            }
        }
        char name[32]{};
        std::snprintf(name, sizeof(name), "editclip_%04u.tga", frame);
        std::ofstream out{directory / name, std::ios::binary | std::ios::trunc};
        out.write(
            reinterpret_cast<const char*>(header.data()),
            static_cast<std::streamsize>(header.size()));
        out.write(
            reinterpret_cast<const char*>(pixels.data()),
            static_cast<std::streamsize>(pixels.size()));
    }
    return (directory / "editclip_####.tga").string();
}

// Authors the edit scene the same way the other demos are authored: one
// transaction of node creations, replicating and undoing through the ordinary
// paths.
[[nodiscard]] bool author_edit_document(
    cmd::DirectorScheduler& scheduler,
    cmd::FrameNumber apply_at,
    const std::string& clip_source) {
    for (const auto& node : show::demo_edit_scene(clip_source)) {
        const auto node_id = node.id;
        auto scheduled = scheduler.schedule_edit(
            cmd::PutNode{node},
            cmd::EraseNode{node_id},
            cmd::UndoContextId{1U},
            cmd::TransactionId{1U},
            apply_at);
        if (!scheduled) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string_view transform_property(
    scene::TransformChannel component) noexcept;

// A deterministic node id for the channel driving one transform component
// (0=PositionX .. 8=ScaleZ) of `target`. Deterministic so keying finds-or-
// creates a channel without scanning the document; the high bit keeps it clear
// of authored node ids, and the component fits the low nibble.
[[nodiscard]] doc::NodeId channel_node_id(
    doc::NodeId target, core::u64 component_index) noexcept {
    constexpr core::u64 channel_id_base = 0x7C00'0000'0000'0000ULL;
    return doc::NodeId{
        channel_id_base | (target.value() << 4U) | component_index};
}

[[nodiscard]] doc::Node make_channel_node(
    doc::NodeId id, doc::NodeId target, std::string_view component) {
    doc::Node node{};
    node.id = id;
    node.type = std::string{scene::schema::channel_type};
    node.properties.insert_or_assign(
        std::string{scene::schema::channel_target},
        doc::PropertyValue{static_cast<std::int64_t>(target.value())});
    node.properties.insert_or_assign(
        std::string{scene::schema::channel_component},
        doc::PropertyValue{std::string{component}});
    return node;
}

// Inserts or updates the key at `frame` in a channel node's indexed key
// properties. Keys are kept contiguous, so the first missing index is the next
// free slot and a matching frame just has its value replaced.
void upsert_channel_key(doc::Node& node, core::u64 frame, double value) {
    const std::string prefix{scene::schema::channel_key_prefix};
    const std::string frame_suffix{scene::schema::channel_key_frame_suffix};
    const std::string value_suffix{scene::schema::channel_key_value_suffix};
    core::u64 next_index = 0U;
    for (core::u64 i = 0U;; ++i) {
        const auto found = node.properties.find(
            prefix + std::to_string(i) + frame_suffix);
        if (found == node.properties.end()) {
            next_index = i;
            break;
        }
        const auto* const held = std::get_if<std::int64_t>(&found->second);
        if (held != nullptr && *held == static_cast<std::int64_t>(frame)) {
            node.properties.insert_or_assign(
                prefix + std::to_string(i) + value_suffix,
                doc::PropertyValue{value});
            return;
        }
    }
    const auto index = std::to_string(next_index);
    node.properties.insert_or_assign(
        prefix + index + frame_suffix,
        doc::PropertyValue{static_cast<std::int64_t>(frame)});
    node.properties.insert_or_assign(
        prefix + index + value_suffix, doc::PropertyValue{value});
    node.properties.insert_or_assign(
        prefix + index + std::string{scene::schema::channel_key_interp_suffix},
        doc::PropertyValue{std::string{scene::schema::interp_linear}});
}

// Removes one indexed key and compacts every later key down by one slot. The
// channel schema stops parsing at its first missing index, so leaving a hole
// would silently hide all later animation from every node.
[[nodiscard]] bool erase_channel_key(doc::Node& node, core::u64 frame) {
    constexpr core::u64 maximum =
        static_cast<core::u64>(std::numeric_limits<core::i64>::max());
    if (frame > maximum) {
        return false;
    }
    const auto wanted = static_cast<std::int64_t>(frame);
    const std::string prefix{scene::schema::channel_key_prefix};
    const std::string frame_suffix{scene::schema::channel_key_frame_suffix};
    const std::string value_suffix{scene::schema::channel_key_value_suffix};
    const std::string interp_suffix{
        scene::schema::channel_key_interp_suffix};

    core::u64 count = 0U;
    core::u64 remove_at = std::numeric_limits<core::u64>::max();
    for (core::u64 index = 0U;; ++index) {
        const auto found = node.properties.find(
            prefix + std::to_string(index) + frame_suffix);
        if (found == node.properties.end()) {
            count = index;
            break;
        }
        const auto* const held = std::get_if<std::int64_t>(&found->second);
        if (held != nullptr && *held == wanted &&
            remove_at == std::numeric_limits<core::u64>::max()) {
            remove_at = index;
        }
    }
    if (remove_at == std::numeric_limits<core::u64>::max()) {
        return false;
    }

    const std::array<std::string_view, 3U> suffixes{
        frame_suffix, value_suffix, interp_suffix};
    for (core::u64 index = remove_at; index + 1U < count; ++index) {
        for (const std::string_view suffix : suffixes) {
            const std::string destination =
                prefix + std::to_string(index) + std::string{suffix};
            const std::string source =
                prefix + std::to_string(index + 1U) + std::string{suffix};
            const auto found = node.properties.find(source);
            if (found != node.properties.end()) {
                node.properties.insert_or_assign(destination, found->second);
            } else {
                node.properties.erase(destination);
            }
        }
    }
    const std::string last_index = std::to_string(count - 1U);
    for (const std::string_view suffix : suffixes) {
        node.properties.erase(prefix + last_index + std::string{suffix});
    }
    return true;
}

// Finds the key at `from` in a channel node's indexed properties and sets its
// frame to `to`. Returns false when there is no key at that frame. Value and
// interp stay; only the frame moves, which the model re-sorts on parse.
[[nodiscard]] bool retime_channel_key(
    doc::Node& node, core::i64 from, core::i64 to) {
    const std::string prefix{scene::schema::channel_key_prefix};
    const std::string frame_suffix{scene::schema::channel_key_frame_suffix};
    for (core::u64 i = 0U;; ++i) {
        const auto name = prefix + std::to_string(i) + frame_suffix;
        const auto found = node.properties.find(name);
        if (found == node.properties.end()) {
            return false;
        }
        const auto* const held = std::get_if<std::int64_t>(&found->second);
        if (held != nullptr && *held == from) {
            node.properties.insert_or_assign(
                name, doc::PropertyValue{static_cast<std::int64_t>(to)});
            return true;
        }
    }
}

// Authors one keyframe on a single transform component: create-or-update the
// component's channel with a key at the frame, or clear the channel. On the
// same scene undo context as moves; the gesture groups a multi-component key.
[[nodiscard]] bool schedule_key_edit(
    cmd::DirectorScheduler& scheduler,
    const editor::KeyIntent& key,
    cmd::FrameNumber apply_at) {
    const auto id =
        channel_node_id(key.target, static_cast<core::u64>(key.component));
    const auto* const existing = scheduler.projected_document().get(id);
    if (key.clear) {
        if (existing == nullptr) {
            return true;
        }
        auto scheduled = scheduler.schedule_edit(
            cmd::EraseNode{id},
            cmd::PutNode{*existing},
            cmd::UndoContextId{2U},
            cmd::TransactionId{key.gesture},
            apply_at);
        return static_cast<bool>(scheduled);
    }
    if (key.erase) {
        if (existing == nullptr) {
            return true;
        }
        doc::Node node = *existing;
        if (!erase_channel_key(node, key.frame)) {
            return true;
        }
        const std::string first_frame =
            std::string{scene::schema::channel_key_prefix} + "0" +
            std::string{scene::schema::channel_key_frame_suffix};
        cmd::Payload payload = node.properties.contains(first_frame)
            ? cmd::Payload{cmd::PutNode{std::move(node)}}
            : cmd::Payload{cmd::EraseNode{id}};
        auto scheduled = scheduler.schedule_edit(
            std::move(payload),
            cmd::PutNode{*existing},
            cmd::UndoContextId{2U},
            cmd::TransactionId{key.gesture},
            apply_at);
        return static_cast<bool>(scheduled);
    }
    doc::Node node = existing != nullptr
        ? *existing
        : make_channel_node(id, key.target, transform_property(key.component));
    upsert_channel_key(node, key.frame, key.value);
    cmd::Payload inverse = existing != nullptr
        ? cmd::Payload{cmd::PutNode{*existing}}
        : cmd::Payload{cmd::EraseNode{id}};
    auto scheduled = scheduler.schedule_edit(
        cmd::PutNode{std::move(node)},
        std::move(inverse),
        cmd::UndoContextId{2U},
        cmd::TransactionId{key.gesture},
        apply_at);
    return static_cast<bool>(scheduled);
}

// Authors a retime: move the key at `from_frame` on (target, component) to
// `to_frame`. A missing channel or key is a no-op, not a failure.
[[nodiscard]] bool schedule_key_move(
    cmd::DirectorScheduler& scheduler,
    const editor::KeyMove& move,
    cmd::FrameNumber apply_at) {
    const auto id =
        channel_node_id(move.target, static_cast<core::u64>(move.component));
    const auto* const existing = scheduler.projected_document().get(id);
    if (existing == nullptr) {
        return true;
    }
    doc::Node node = *existing;
    if (!retime_channel_key(node, move.from_frame, move.to_frame)) {
        return true;
    }
    auto scheduled = scheduler.schedule_edit(
        cmd::PutNode{std::move(node)},
        cmd::PutNode{*existing},
        cmd::UndoContextId{2U},
        cmd::TransactionId{move.gesture},
        apply_at);
    return static_cast<bool>(scheduled);
}

// Maps an editor transport request to the operator command the transport
// already understands, so timeline buttons and keyboard controls share one path.
[[nodiscard]] show::PresenterCommand map_transport(
    editor::TransportRequest request) noexcept {
    switch (request) {
    case editor::TransportRequest::TogglePlay:
        return show::PresenterCommand::TogglePlay;
    case editor::TransportRequest::StepBack:
        return show::PresenterCommand::StepBack;
    case editor::TransportRequest::StepForward:
        return show::PresenterCommand::StepForward;
    case editor::TransportRequest::JumpBack:
        return show::PresenterCommand::JumpBack;
    case editor::TransportRequest::JumpForward:
        return show::PresenterCommand::JumpForward;
    case editor::TransportRequest::GoToStart:
        return show::PresenterCommand::GoToStart;
    case editor::TransportRequest::None:
        break;
    }
    return show::PresenterCommand::None;
}

// The document property a transform channel maps to. Position/rotation/scale
// reuse the object transform names, so a channel and a base edit name the same
// property.
[[nodiscard]] std::string_view transform_property(
    scene::TransformChannel component) noexcept {
    switch (component) {
    case scene::TransformChannel::PositionX:
        return scene::schema::position_x;
    case scene::TransformChannel::PositionY:
        return scene::schema::position_y;
    case scene::TransformChannel::PositionZ:
        return scene::schema::position_z;
    case scene::TransformChannel::RotationYaw:
        return scene::schema::rotation_yaw;
    case scene::TransformChannel::RotationPitch:
        return scene::schema::rotation_pitch;
    case scene::TransformChannel::RotationRoll:
        return scene::schema::rotation_roll;
    case scene::TransformChannel::ScaleX:
        return scene::schema::scale_x;
    case scene::TransformChannel::ScaleY:
        return scene::schema::scale_y;
    case scene::TransformChannel::ScaleZ:
        return scene::schema::scale_z;
    case scene::PropertyChannel::Opacity:
        return scene::schema::opacity;
    case scene::PropertyChannel::Brightness:
        return scene::schema::brightness;
    case scene::PropertyChannel::BlurRadius:
        return scene::schema::blur_radius;
    case scene::TransformChannel::Count:
        break;
    }
    return {};
}

[[nodiscard]] double read_transform(
    const doc::Node& node, std::string_view name, double fallback) noexcept {
    const auto found = node.properties.find(name);
    if (found == node.properties.end()) {
        return fallback;
    }
    if (const auto* const real = std::get_if<double>(&found->second)) {
        return *real;
    }
    if (const auto* const whole = std::get_if<std::int64_t>(&found->second)) {
        return static_cast<double>(*whole);
    }
    return fallback;
}

// The authored transform of each scene element, for the editor's Inspector.
// Read from the projected document (the base values, before channels), so the
// Inspector shows and edits what an edit changes rather than the animated result.
[[nodiscard]] std::vector<editor::ElementInfo> scene_elements(
    const cmd::DirectorScheduler& scheduler, const scene::Scene& scene) {
    std::vector<editor::ElementInfo> elements;
    const auto read = [&scheduler](doc::NodeId node, editor::ElementKind kind) {
        editor::ElementInfo info{};
        info.node = node;
        info.kind = kind;
        const auto* const doc_node = scheduler.projected_document().get(node);
        if (doc_node == nullptr) {
            return info;
        }
        const auto number = [doc_node](std::string_view name, double fallback) {
            return static_cast<float>(read_transform(*doc_node, name, fallback));
        };
        info.position = scene::Vec3{
            number(scene::schema::position_x, 0.0),
            number(scene::schema::position_y, 0.0),
            number(scene::schema::position_z, 0.0)};
        info.rotation = scene::Vec3{
            number(scene::schema::rotation_yaw, 0.0),
            number(scene::schema::rotation_pitch, 0.0),
            number(scene::schema::rotation_roll, 0.0)};
        info.scale = scene::Vec3{
            number(scene::schema::scale_x, 1.0),
            number(scene::schema::scale_y, 1.0),
            number(scene::schema::scale_z, 1.0)};
        info.color = scene::Vec3{
            number(scene::schema::color_r, 1.0),
            number(scene::schema::color_g, 1.0),
            number(scene::schema::color_b, 1.0)};
        info.intensity = number(scene::schema::intensity, 1.0);
        info.direction = scene::Vec3{
            number(scene::schema::direction_x, 0.0),
            number(scene::schema::direction_y, -1.0),
            number(scene::schema::direction_z, 0.0)};
        info.range = number(scene::schema::range, 10.0);
        info.material.base_color = info.color;
        info.material.roughness = number(scene::schema::roughness, 0.5);
        info.material.metallic = number(scene::schema::metallic, 0.0);
        info.material.emissive = scene::Vec3{
            number(scene::schema::emissive_r, 0.0),
            number(scene::schema::emissive_g, 0.0),
            number(scene::schema::emissive_b, 0.0)};
        info.style.opacity = std::clamp(
            number(scene::schema::opacity, 1.0), 0.0F, 1.0F);
        const int blend = static_cast<int>(
            number(scene::schema::blend_mode, 0.0));
        if (blend >= 0 && blend <= 3) {
            info.style.blend = static_cast<scene::BlendMode>(blend);
        }
        info.style.effects.brightness_enabled =
            number(scene::schema::brightness_enabled, 0.0) >= 0.5F;
        info.style.effects.brightness =
            number(scene::schema::brightness, 1.0);
        info.style.effects.blur_enabled =
            number(scene::schema::blur_enabled, 0.0) >= 0.5F;
        info.style.effects.blur_radius =
            number(scene::schema::blur_radius, 0.0);
        const auto text = [doc_node](std::string_view name) {
            const auto found = doc_node->properties.find(name);
            if (found == doc_node->properties.end()) {
                return std::string{};
            }
            if (const auto* const value =
                    std::get_if<std::string>(&found->second)) {
                return *value;
            }
            return std::string{};
        };
        const auto integer = [doc_node](std::string_view name,
                                        core::u32 fallback) {
            const auto found = doc_node->properties.find(name);
            if (found == doc_node->properties.end()) {
                return fallback;
            }
            if (const auto* const value =
                    std::get_if<std::int64_t>(&found->second);
                value != nullptr && *value >= 0 &&
                *value <= static_cast<std::int64_t>(UINT32_MAX)) {
                return static_cast<core::u32>(*value);
            }
            return fallback;
        };
        info.source = text(scene::schema::source);
        info.sequence_start = integer(scene::schema::sequence_start, 0U);
        info.sequence_count = integer(scene::schema::sequence_count, 1U);
        return info;
    };
    for (const auto& clip : scene.media) {
        auto info = read(clip.source_node, editor::ElementKind::Media);
        info.style = clip.style;
        elements.push_back(std::move(info));
    }
    for (const auto& object : scene.instances) {
        auto info = read(object.source, editor::ElementKind::Object);
        info.style = object.style;
        elements.push_back(std::move(info));
    }
    for (const auto& model : scene.models) {
        auto info = read(model.source_node, editor::ElementKind::Model);
        info.style = model.style;
        elements.push_back(std::move(info));
    }
    for (const auto& light : scene.directional_lights) {
        elements.push_back(
            read(light.source, editor::ElementKind::DirectionalLight));
    }
    for (const auto& light : scene.point_lights) {
        elements.push_back(read(light.source, editor::ElementKind::PointLight));
    }
    return elements;
}

// Authors one Inspector edit of a named property (a light field). Reads the
// node, sets the property, PutNode with the restoring inverse.
[[nodiscard]] bool schedule_generic_edit(
    cmd::DirectorScheduler& scheduler,
    const editor::GenericEdit& edit,
    cmd::FrameNumber apply_at) {
    if (edit.property.empty()) {
        return true;
    }
    const auto* const existing = scheduler.projected_document().get(edit.node);
    if (existing == nullptr) {
        return true;
    }
    doc::Node updated = *existing;
    updated.properties.insert_or_assign(
        edit.property, doc::PropertyValue{edit.value});
    auto scheduled = scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)},
        cmd::PutNode{*existing},
        cmd::UndoContextId{2U},
        cmd::TransactionId{edit.gesture},
        apply_at);
    return static_cast<bool>(scheduled);
}

// Authors one Inspector edit: set a single transform component's base property,
// PutNode with the inverse that restores it. The gesture groups a drag's edits.
[[nodiscard]] bool schedule_property_edit(
    cmd::DirectorScheduler& scheduler,
    const editor::PropertyEdit& edit,
    cmd::FrameNumber apply_at) {
    const auto name = transform_property(edit.component);
    if (name.empty()) {
        return true;
    }
    const auto* const existing = scheduler.projected_document().get(edit.node);
    if (existing == nullptr) {
        return true;
    }
    doc::Node updated = *existing;
    updated.properties.insert_or_assign(
        std::string{name}, doc::PropertyValue{edit.value});
    auto scheduled = scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)},
        cmd::PutNode{*existing},
        cmd::UndoContextId{2U},
        cmd::TransactionId{edit.gesture},
        apply_at);
    return static_cast<bool>(scheduled);
}

// A default node for a newly-added element. Objects sit at the origin with a
// neutral material; lights carry the same defaults the demo scene uses. The
// operator refines them through the Inspector afterwards.
[[nodiscard]] doc::Node make_added_node(
    doc::NodeId id, const editor::AddIntent& add) {
    doc::Node node{};
    node.id = id;
    const auto kind = add.kind;
    const auto real = [](double value) { return doc::PropertyValue{value}; };
    const auto whole = [](std::int64_t value) {
        return doc::PropertyValue{value};
    };
    const auto text = [](std::string_view value) {
        return doc::PropertyValue{std::string{value}};
    };
    const auto add_links = [&]() {
        if (add.scene != scene::SceneId{}) {
            node.properties.emplace(
                std::string{scene::schema::scene_id},
                whole(static_cast<std::int64_t>(add.scene.value())));
        }
        if (add.asset != doc::NodeId{}) {
            node.properties.emplace(
                std::string{scene::schema::asset_id},
                whole(static_cast<std::int64_t>(add.asset.value())));
        }
        if (add.position.has_value()) {
            node.properties.insert_or_assign(
                std::string{scene::schema::position_x},
                real(static_cast<double>(add.position->x)));
            node.properties.insert_or_assign(
                std::string{scene::schema::position_y},
                real(static_cast<double>(add.position->y)));
            node.properties.insert_or_assign(
                std::string{scene::schema::position_z},
                real(static_cast<double>(add.position->z)));
        }
    };
    if (kind == editor::AddKind::Model) {
        node.type = std::string{scene::schema::model_type};
        node.properties.emplace(
            std::string{scene::schema::source}, text(add.source));
        add_links();
        return node;
    }
    if (kind == editor::AddKind::Media) {
        node.type = std::string{scene::schema::media_type};
        node.properties.emplace(
            std::string{scene::schema::source}, text(add.source));
        node.properties.emplace(
            std::string{scene::schema::sequence_start}, whole(0));
        node.properties.emplace(
            std::string{scene::schema::sequence_count},
            whole(static_cast<std::int64_t>(std::max(add.count, 1U))));
        if (add.frame_rate.has_value()) {
            node.properties.emplace(
                std::string{scene::schema::source_rate_numerator},
                whole(static_cast<std::int64_t>(
                    add.frame_rate->numerator)));
            node.properties.emplace(
                std::string{scene::schema::source_rate_denominator},
                whole(static_cast<std::int64_t>(
                    add.frame_rate->denominator)));
        }
        // Two metres tall, and as wide as the source's aspect ratio, so a clip
        // is never stretched.
        const double aspect = add.aspect > 0.0F
            ? static_cast<double>(add.aspect)
            : (16.0 / 9.0);
        node.properties.emplace(
            std::string{scene::schema::scale_x}, real(2.0 * aspect));
        node.properties.emplace(std::string{scene::schema::scale_y}, real(2.0));
        add_links();
        return node;
    }
    if (kind == editor::AddKind::DirectionalLight
        || kind == editor::AddKind::PointLight) {
        node.type = std::string{scene::schema::light_type};
        node.properties.emplace(
            std::string{scene::schema::color_r}, real(1.0));
        node.properties.emplace(
            std::string{scene::schema::color_g}, real(1.0));
        node.properties.emplace(
            std::string{scene::schema::color_b}, real(1.0));
        if (kind == editor::AddKind::PointLight) {
            node.properties.emplace(
                std::string{scene::schema::light_kind}, text("point"));
            node.properties.emplace(
                std::string{scene::schema::position_y}, real(3.0));
            node.properties.emplace(
                std::string{scene::schema::range}, real(25.0));
            node.properties.emplace(
                std::string{scene::schema::intensity}, real(12.0));
        } else {
            node.properties.emplace(
                std::string{scene::schema::light_kind}, text("directional"));
            node.properties.emplace(
                std::string{scene::schema::direction_x}, real(-0.3));
            node.properties.emplace(
                std::string{scene::schema::direction_y}, real(-1.0));
            node.properties.emplace(
                std::string{scene::schema::direction_z}, real(-0.4));
            node.properties.emplace(
                std::string{scene::schema::intensity}, real(2.5));
        }
        add_links();
        return node;
    }

    node.type = std::string{scene::schema::object_type};
    const std::string_view primitive = kind == editor::AddKind::Plane
        ? "plane"
        : (kind == editor::AddKind::Sphere ? "sphere" : "box");
    node.properties.emplace(std::string{scene::schema::primitive}, text(primitive));
    node.properties.emplace(std::string{scene::schema::color_r}, real(0.60));
    node.properties.emplace(std::string{scene::schema::color_g}, real(0.60));
    node.properties.emplace(std::string{scene::schema::color_b}, real(0.63));
    add_links();
    return node;
}

// Creates one element: a PutNode whose inverse erases it, on the scene undo
// context, so an add undoes cleanly and replicates like any other edit.
[[nodiscard]] bool schedule_add(
    cmd::DirectorScheduler& scheduler,
    const editor::AddIntent& add,
    doc::NodeId id,
    cmd::FrameNumber apply_at) {
    auto scheduled = scheduler.schedule_edit(
        cmd::PutNode{make_added_node(id, add)},
        cmd::EraseNode{id},
        cmd::UndoContextId{2U},
        cmd::TransactionId{add.gesture},
        apply_at);
    return static_cast<bool>(scheduled);
}


[[nodiscard]] bool schedule_asset_import(
    cmd::DirectorScheduler& scheduler,
    const editor::AssetImportIntent& request,
    doc::NodeId id,
    cmd::FrameNumber apply_at) {
    ProjectAssetNodeUpsert upsert = make_project_asset_upsert(
        scheduler.projected_document(), request.asset, id);
    if (upsert.before.has_value()) {
        return static_cast<bool>(scheduler.schedule_edit(
            cmd::PutNode{std::move(upsert.after)},
            cmd::PutNode{std::move(upsert.before.value())},
            cmd::UndoContextId{4U},
            cmd::TransactionId{request.gesture}, apply_at));
    }
    const doc::NodeId target = upsert.after.id;
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(upsert.after)}, cmd::EraseNode{target},
        cmd::UndoContextId{4U}, cmd::TransactionId{request.gesture}, apply_at));
}

[[nodiscard]] std::string_view project_asset_import_error_label(
    ProjectAssetImportErrorCode code) noexcept {
    switch (code) {
    case ProjectAssetImportErrorCode::InvalidSource:
        return "INVALID SOURCE";
    case ProjectAssetImportErrorCode::ContentIdentityFailed:
        return "SOURCE COULD NOT BE HASHED";
    case ProjectAssetImportErrorCode::ContentTooLarge:
        return "SOURCE IS TOO LARGE";
    case ProjectAssetImportErrorCode::SharedStoreRequired:
        return "ASSET STORE IS NOT CONFIGURED";
    case ProjectAssetImportErrorCode::StoreOpenFailed:
        return "ASSET STORE COULD NOT BE OPENED";
    case ProjectAssetImportErrorCode::SourceReadFailed:
        return "SOURCE COULD NOT BE READ";
    case ProjectAssetImportErrorCode::StoreWriteFailed:
        return "ASSET STORE WRITE FAILED";
    case ProjectAssetImportErrorCode::StoreFinalizeFailed:
        return "ASSET STORE VERIFICATION FAILED";
    case ProjectAssetImportErrorCode::AllocationFailed:
        return "NOT ENOUGH MEMORY";
    }
    return "UNKNOWN ERROR";
}

struct PendingAssetImportAuthoring final {
    core::u64 token{};
    core::u64 project_generation{};
    core::u64 gesture{};
    std::vector<editor::FeedSurfaceAddRequest> surface_adds;
    std::vector<editor::FeedSurfaceEditRequest> surface_edits;
};

template <typename Request>
void extract_gesture_requests(std::vector<Request>& pending,
                              core::u64 gesture,
                              std::vector<Request>& extracted) {
    const std::size_t count = static_cast<std::size_t>(std::count_if(
        pending.begin(), pending.end(), [gesture](const Request& request) {
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
void discard_gesture_requests(std::vector<Request>& pending,
                              core::u64 gesture) {
    std::erase_if(pending, [gesture](const Request& request) {
        return request.gesture == gesture;
    });
}

[[nodiscard]] bool schedule_asset_delete(
    cmd::DirectorScheduler& scheduler,
    const editor::AssetDeleteIntent& request,
    cmd::FrameNumber apply_at) {
    const doc::Node* const existing =
        scheduler.projected_document().get(request.asset);
    if (existing == nullptr) {
        return true;
    }
    if (existing->type != assets::schema::media_type &&
        existing->type != assets::schema::model_type &&
        existing->type != assets::schema::projection_mesh_type) {
        // A stale or malformed UI request must never erase a non-asset node.
        return true;
    }
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::EraseNode{request.asset}, cmd::PutNode{*existing},
        // Library removal is a direct destructive UI action, so keep it in
        // the editor's globally reachable Undo context. Undo restores the
        // complete asset node without touching any scene placement.
        cmd::UndoContextId{2U}, cmd::TransactionId{request.gesture},
        apply_at));
}

[[nodiscard]] std::string_view compositor_type(
    compositor::NodeKind kind) noexcept {
    switch (kind) {
    case compositor::NodeKind::SceneLayer:
        return compositor::schema::scene_layer_type;
    case compositor::NodeKind::Brightness:
        return compositor::schema::brightness_type;
    case compositor::NodeKind::Blur:
        return compositor::schema::blur_type;
    case compositor::NodeKind::RadialArray:
        return compositor::schema::radial_array_type;
    case compositor::NodeKind::Composite:
        return compositor::schema::composite_type;
    case compositor::NodeKind::Output:
        return compositor::schema::output_type;
    }
    return compositor::schema::brightness_type;
}

[[nodiscard]] doc::Node make_compositor_node(
    doc::NodeId id, const editor::CompositorAddIntent& request) {
    doc::Node node{};
    node.id = id;
    node.type = std::string{compositor_type(request.kind)};
    node.properties.emplace(
        std::string{compositor::schema::label},
        doc::PropertyValue{
            std::string{compositor::node_kind_name(request.kind)}});
    node.properties.emplace(std::string{compositor::schema::editor_x},
                            doc::PropertyValue{
                                static_cast<double>(request.editor_x)});
    node.properties.emplace(std::string{compositor::schema::editor_y},
                            doc::PropertyValue{
                                static_cast<double>(request.editor_y)});
    switch (request.kind) {
    case compositor::NodeKind::SceneLayer:
        node.properties.emplace(
            std::string{compositor::schema::scene_node},
            doc::PropertyValue{static_cast<std::int64_t>(
                request.scene_node.value())});
        break;
    case compositor::NodeKind::Brightness:
        node.properties.emplace(std::string{compositor::schema::amount},
                                doc::PropertyValue{1.0});
        break;
    case compositor::NodeKind::Blur:
        node.properties.emplace(std::string{compositor::schema::radius},
                                doc::PropertyValue{8.0});
        break;
    case compositor::NodeKind::RadialArray:
        node.properties.emplace(std::string{compositor::schema::count},
                                doc::PropertyValue{std::int64_t{9}});
        node.properties.emplace(std::string{compositor::schema::radius},
                                doc::PropertyValue{4.0});
        break;
    case compositor::NodeKind::Composite:
    case compositor::NodeKind::Output:
        break;
    }
    return node;
}

[[nodiscard]] bool schedule_compositor_add(
    cmd::DirectorScheduler& scheduler,
    const editor::CompositorAddIntent& request,
    doc::NodeId id,
    core::u64 gesture,
    cmd::FrameNumber apply_at) {
    if (request.kind == compositor::NodeKind::SceneLayer &&
        request.scene_node == doc::NodeId{}) {
        return true;
    }
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{make_compositor_node(id, request)},
        cmd::EraseNode{id}, cmd::UndoContextId{6U},
        cmd::TransactionId{gesture}, apply_at));
}

[[nodiscard]] bool schedule_compositor_move(
    cmd::DirectorScheduler& scheduler,
    const editor::CompositorMoveIntent& request,
    core::u64 gesture,
    cmd::FrameNumber apply_at) {
    const doc::Node* const existing =
        scheduler.projected_document().get(request.node);
    if (existing == nullptr) {
        return true;
    }
    doc::Node moved = *existing;
    moved.properties.insert_or_assign(
        std::string{compositor::schema::editor_x},
        doc::PropertyValue{static_cast<double>(request.editor_x)});
    moved.properties.insert_or_assign(
        std::string{compositor::schema::editor_y},
        doc::PropertyValue{static_cast<double>(request.editor_y)});
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(moved)}, cmd::PutNode{*existing},
        cmd::UndoContextId{6U}, cmd::TransactionId{gesture}, apply_at));
}

[[nodiscard]] bool schedule_compositor_property(
    cmd::DirectorScheduler& scheduler,
    const editor::CompositorPropertyEditIntent& request,
    core::u64 gesture,
    cmd::FrameNumber apply_at) {
    const doc::Node* const existing =
        scheduler.projected_document().get(request.node);
    if (existing == nullptr) {
        return true;
    }

    std::string_view property;
    bool integer_value = false;
    switch (request.property) {
    case editor::CompositorProperty::BrightnessAmount:
        property = compositor::schema::amount;
        break;
    case editor::CompositorProperty::BlurRadius:
    case editor::CompositorProperty::RadialRadius:
        property = compositor::schema::radius;
        break;
    case editor::CompositorProperty::RadialCount:
        property = compositor::schema::count;
        integer_value = true;
        break;
    case editor::CompositorProperty::RadialStartAngleTurns:
        property = compositor::schema::start_angle_turns;
        break;
    case editor::CompositorProperty::RadialCenterX:
        property = compositor::schema::center_x;
        break;
    case editor::CompositorProperty::RadialCenterY:
        property = compositor::schema::center_y;
        break;
    }

    doc::PropertyValue value;
    if (integer_value) {
        const auto* const number =
            std::get_if<core::u32>(&request.value);
        if (number == nullptr) {
            return true;
        }
        value = static_cast<std::int64_t>(*number);
    } else {
        const auto* const number = std::get_if<float>(&request.value);
        if (number == nullptr || !std::isfinite(*number)) {
            return true;
        }
        value = static_cast<double>(*number);
    }

    doc::Node changed = *existing;
    changed.properties.insert_or_assign(std::string{property},
                                        std::move(value));
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(changed)}, cmd::PutNode{*existing},
        cmd::UndoContextId{6U}, cmd::TransactionId{gesture}, apply_at));
}

[[nodiscard]] bool schedule_compositor_connect(
    cmd::DirectorScheduler& scheduler,
    const editor::CompositorConnectIntent& request,
    doc::NodeId id,
    core::u64 gesture,
    cmd::FrameNumber apply_at) {
    constexpr core::u64 max_reference =
        static_cast<core::u64>(std::numeric_limits<std::int64_t>::max());
    if (request.from.node.value() > max_reference ||
        request.to.node.value() > max_reference) {
        return true;
    }
    const cmd::UndoContextId context{6U};
    const cmd::TransactionId transaction{gesture};
    if (request.replace_edge.has_value()) {
        const doc::Node* const replaced =
            scheduler.projected_document().get(*request.replace_edge);
        if (replaced != nullptr) {
            const auto target = replaced->properties.find(
                compositor::schema::to_node);
            const auto port = replaced->properties.find(
                compositor::schema::to_port);
            const auto* const target_value =
                target == replaced->properties.end()
                ? nullptr
                : std::get_if<std::int64_t>(&target->second);
            const auto* const port_value =
                port == replaced->properties.end()
                ? nullptr
                : std::get_if<std::int64_t>(&port->second);
            const bool matches_target =
                replaced->type == compositor::schema::edge_type &&
                target_value != nullptr && *target_value > 0 &&
                static_cast<core::u64>(*target_value) ==
                    request.to.node.value() &&
                port_value != nullptr && *port_value >= 0 &&
                static_cast<core::u64>(*port_value) == request.to.port;
            if (!matches_target) {
                return true;
            }
            if (!scheduler.schedule_edit(
                    cmd::EraseNode{replaced->id}, cmd::PutNode{*replaced},
                    context, transaction, apply_at)) {
                return false;
            }
        }
    }
    doc::Node edge{};
    edge.id = id;
    edge.type = std::string{compositor::schema::edge_type};
    edge.properties.emplace(
        std::string{compositor::schema::from_node},
        doc::PropertyValue{static_cast<std::int64_t>(
            request.from.node.value())});
    edge.properties.emplace(
        std::string{compositor::schema::from_port},
        doc::PropertyValue{static_cast<std::int64_t>(request.from.port)});
    edge.properties.emplace(
        std::string{compositor::schema::to_node},
        doc::PropertyValue{static_cast<std::int64_t>(
            request.to.node.value())});
    edge.properties.emplace(
        std::string{compositor::schema::to_port},
        doc::PropertyValue{static_cast<std::int64_t>(request.to.port)});
    edge.properties.emplace(std::string{compositor::schema::order},
                            doc::PropertyValue{std::int64_t{0}});
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(edge)}, cmd::EraseNode{id},
        context, transaction, apply_at));
}

[[nodiscard]] std::optional<doc::NodeId>
node_reference(const doc::Node& node, std::string_view property) noexcept {
    const auto found = node.properties.find(property);
    if (found == node.properties.end()) {
        return std::nullopt;
    }
    const auto* const value = std::get_if<std::int64_t>(&found->second);
    if (value == nullptr || *value <= 0) {
        return std::nullopt;
    }
    return doc::NodeId{static_cast<core::u64>(*value)};
}

[[nodiscard]] bool schedule_erase_node(
    cmd::DirectorScheduler& scheduler, doc::NodeId id,
    cmd::UndoContextId context, cmd::TransactionId transaction,
    cmd::FrameNumber apply_at) {
    const doc::Node* const existing = scheduler.projected_document().get(id);
    if (existing == nullptr) {
        return true;
    }
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::EraseNode{id}, cmd::PutNode{*existing}, context, transaction,
        apply_at));
}

[[nodiscard]] bool schedule_compositor_delete(
    cmd::DirectorScheduler& scheduler,
    const editor::CompositorDeleteIntent& request, core::u64 gesture,
    cmd::FrameNumber apply_at) {
    if (request.target.kind == editor::CompositorSelectionKind::None ||
        request.target.node == doc::NodeId{}) {
        return true;
    }
    const doc::Root& root = scheduler.projected_document();
    const doc::Node* const target = root.get(request.target.node);
    if (target == nullptr) {
        return true;
    }
    const doc::NodeId target_id = target->id;
    const std::string target_type = target->type;
    const cmd::UndoContextId context{6U};
    const cmd::TransactionId transaction{gesture};
    if (request.target.kind == editor::CompositorSelectionKind::Edge) {
        return target_type != compositor::schema::edge_type ||
            schedule_erase_node(scheduler, target_id, context, transaction,
                                apply_at);
    }
    const bool compositor_node =
        target_type == compositor::schema::scene_layer_type ||
        target_type == compositor::schema::brightness_type ||
        target_type == compositor::schema::blur_type ||
        target_type == compositor::schema::radial_array_type ||
        target_type == compositor::schema::composite_type ||
        target_type == compositor::schema::output_type;
    if (!compositor_node) {
        return true;
    }

    try {
        std::vector<doc::NodeId> incident;
        const doc::Delta contents = doc::diff(doc::Root{}, root);
        for (const doc::NodeChange& change : contents.changes) {
            if (!change.after ||
                change.after->type != compositor::schema::edge_type) {
                continue;
            }
            const auto from = node_reference(
                *change.after, compositor::schema::from_node);
            const auto to = node_reference(
                *change.after, compositor::schema::to_node);
            if ((from && *from == target_id) ||
                (to && *to == target_id)) {
                incident.push_back(change.after->id);
            }
        }
        for (const doc::NodeId edge : incident) {
            if (!schedule_erase_node(scheduler, edge, context, transaction,
                                     apply_at)) {
                return false;
            }
        }
        return schedule_erase_node(scheduler, target_id, context, transaction,
                                   apply_at);
    } catch (const std::bad_alloc&) {
        return false;
    }
}

[[nodiscard]] doc::Node make_effect_node(
    doc::NodeId id, const editor::EffectIntent& effect) {
    doc::Node node{};
    node.id = id;
    node.type = std::string{scene::schema::effect_type};
    node.properties.emplace(
        std::string{scene::schema::effect_target},
        doc::PropertyValue{
            static_cast<std::int64_t>(effect.target.value())});
    node.properties.emplace(
        std::string{scene::schema::effect_kind},
        doc::PropertyValue{std::string{
            effect.kind == scene::EffectKind::Brightness
                ? scene::schema::brightness_kind
                : scene::schema::blur_kind}});
    node.properties.emplace(
        std::string{scene::schema::effect_order},
        doc::PropertyValue{std::int64_t{0}});
    node.properties.emplace(
        std::string{scene::schema::effect_enabled},
        doc::PropertyValue{true});
    if (effect.kind == scene::EffectKind::Brightness) {
        node.properties.emplace(
            std::string{scene::schema::brightness},
            doc::PropertyValue{1.0});
    } else {
        node.properties.emplace(
            std::string{scene::schema::blur_radius},
            doc::PropertyValue{8.0});
    }
    return node;
}

[[nodiscard]] bool schedule_effect(
    cmd::DirectorScheduler& scheduler,
    const editor::EffectIntent& effect,
    doc::NodeId id,
    cmd::FrameNumber apply_at) {
    if (!scheduler.projected_document().contains(effect.target) ||
        effect.target.value() >
            static_cast<core::u64>(std::numeric_limits<std::int64_t>::max())) {
        return true;
    }
    auto scheduled = scheduler.schedule_edit(
        cmd::PutNode{make_effect_node(id, effect)},
        cmd::EraseNode{id},
        cmd::UndoContextId{2U},
        cmd::TransactionId{effect.gesture},
        apply_at);
    return static_cast<bool>(scheduled);
}

[[nodiscard]] bool schedule_effect_delete(
    cmd::DirectorScheduler& scheduler,
    const editor::EffectDeleteIntent& request,
    cmd::FrameNumber apply_at) {
    const auto planned = scene::plan_delete_effect(
        scheduler.projected_document(), request.effect);
    if (!planned) {
        // A stale UI request is harmless. Allocation failure is different: do
        // not partially author a cascade whose complete dependency list could
        // not be produced.
        return planned.error() !=
            scene::EffectDeletePlanError::AllocationFailed;
    }

    const cmd::UndoContextId context{2U};
    const cmd::TransactionId transaction{request.gesture};
    for (const doc::NodeId node : planned.value().erases) {
        if (!schedule_erase_node(scheduler, node, context, transaction,
                                 apply_at)) {
            return false;
        }
    }
    return true;
}

// Deletes one element: an EraseNode whose inverse restores it. A missing node is
// a no-op. All of a multi-select delete share the gesture, so it undoes at once.
[[nodiscard]] bool schedule_delete(
    cmd::DirectorScheduler& scheduler,
    doc::NodeId node,
    core::u64 gesture,
    cmd::FrameNumber apply_at) {
    const auto* const existing = scheduler.projected_document().get(node);
    if (existing == nullptr) {
        return true;
    }
    auto scheduled = scheduler.schedule_edit(
        cmd::EraseNode{node},
        cmd::PutNode{*existing},
        cmd::UndoContextId{2U},
        cmd::TransactionId{gesture},
        apply_at);
    return static_cast<bool>(scheduled);
}

// Authors one operator move as an ordinary command: read the node's current
// authored state, set its position, and schedule a PutNode whose inverse
// restores the old one. A separate undo context from the transport's, so
// undoing a move never touches playback. The editor produced the position; this
// is the only place a document changes.
[[nodiscard]] bool schedule_scene_edit(
    cmd::DirectorScheduler& scheduler,
    const editor::EditIntent& edit,
    cmd::FrameNumber apply_at) {
    const auto* const existing = scheduler.projected_document().get(edit.node);
    if (existing == nullptr) {
        // The node is gone; there is nothing to move and nothing to fail.
        return true;
    }
    doc::Node updated = *existing;
    updated.properties.insert_or_assign(
        std::string{scene::schema::position_x},
        doc::PropertyValue{static_cast<double>(edit.position.x)});
    updated.properties.insert_or_assign(
        std::string{scene::schema::position_y},
        doc::PropertyValue{static_cast<double>(edit.position.y)});
    updated.properties.insert_or_assign(
        std::string{scene::schema::position_z},
        doc::PropertyValue{static_cast<double>(edit.position.z)});

    // The transaction is the drag gesture, not the frame: every sealed frame of
    // one continuous drag appends to the same transaction, so the clip moves on
    // the nodes each frame but the whole drag undoes as a single step. A fresh
    // gesture (the next drag) starts a new transaction and becomes the undo top.
    auto scheduled = scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)},
        cmd::PutNode{*existing},
        cmd::UndoContextId{2U},
        cmd::TransactionId{edit.gesture},
        apply_at);
    return static_cast<bool>(scheduled);
}

[[nodiscard]] std::string_view hardware_kind_property(
    topology::HardwareDeviceKind kind) noexcept {
    switch (kind) {
    case topology::HardwareDeviceKind::RenderServer:
        return topology::schema::render_server_kind;
    case topology::HardwareDeviceKind::OperatorMachine:
        return topology::schema::operator_machine_kind;
    case topology::HardwareDeviceKind::LedProcessor:
        return topology::schema::led_processor_kind;
    case topology::HardwareDeviceKind::LedWall:
        return topology::schema::led_wall_kind;
    }
    return topology::schema::render_server_kind;
}

[[nodiscard]] doc::Node make_hardware_device(
    doc::NodeId id, const editor::HardwarePlaceRequest& request) {
    doc::Node node{};
    node.id = id;
    node.type = std::string{topology::schema::device_type};
    node.properties.emplace(
        std::string{topology::schema::kind},
        doc::PropertyValue{std::string{hardware_kind_property(request.kind)}});
    node.properties.emplace(
        std::string{topology::schema::name},
        doc::PropertyValue{std::string{topology::device_kind_name(request.kind)}});
    node.properties.emplace(std::string{topology::schema::grid_x},
                            doc::PropertyValue{static_cast<std::int64_t>(
                                request.grid_x)});
    node.properties.emplace(std::string{topology::schema::grid_y},
                            doc::PropertyValue{static_cast<std::int64_t>(
                                request.grid_y)});
    node.properties.emplace(std::string{topology::schema::address},
                            doc::PropertyValue{std::string{}});
    if (request.kind == topology::HardwareDeviceKind::RenderServer) {
        node.properties.emplace(
            std::string{topology::schema::lifecycle_endpoint},
            doc::PropertyValue{std::string{}});
        node.properties.emplace(std::string{topology::schema::node_index},
                                doc::PropertyValue{std::int64_t{0}});
    }
    node.properties.emplace(std::string{topology::schema::width},
                            doc::PropertyValue{std::int64_t{1920}});
    node.properties.emplace(std::string{topology::schema::height},
                            doc::PropertyValue{std::int64_t{1080}});
    node.properties.emplace(
        std::string{topology::schema::refresh_numerator},
        doc::PropertyValue{std::int64_t{60}});
    node.properties.emplace(
        std::string{topology::schema::refresh_denominator},
        doc::PropertyValue{std::int64_t{1}});
    if (request.kind == topology::HardwareDeviceKind::RenderServer) {
        node.properties.emplace(
            std::string{topology::schema::window_mode},
            doc::PropertyValue{
                std::string{topology::schema::inherit_window_mode}});
        node.properties.emplace(std::string{topology::schema::desktop_x},
                                doc::PropertyValue{std::int64_t{0}});
        node.properties.emplace(std::string{topology::schema::desktop_y},
                                doc::PropertyValue{std::int64_t{0}});
    }
    return node;
}

[[nodiscard]] bool schedule_hardware_place(
    cmd::DirectorScheduler& scheduler,
    const editor::HardwarePlaceRequest& request,
    doc::NodeId id,
    cmd::FrameNumber apply_at) {
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{make_hardware_device(id, request)}, cmd::EraseNode{id},
        cmd::UndoContextId{3U}, cmd::TransactionId{request.gesture}, apply_at));
}

[[nodiscard]] bool schedule_hardware_move(
    cmd::DirectorScheduler& scheduler,
    const editor::HardwareMoveRequest& request,
    cmd::FrameNumber apply_at) {
    const doc::Node* const existing =
        scheduler.projected_document().get(request.device);
    if (existing == nullptr ||
        existing->type != topology::schema::device_type) {
        return true;
    }
    doc::Node updated = *existing;
    updated.properties.insert_or_assign(
        std::string{topology::schema::grid_x},
        doc::PropertyValue{static_cast<std::int64_t>(request.grid_x)});
    updated.properties.insert_or_assign(
        std::string{topology::schema::grid_y},
        doc::PropertyValue{static_cast<std::int64_t>(request.grid_y)});
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)}, cmd::PutNode{*existing},
        cmd::UndoContextId{3U}, cmd::TransactionId{request.gesture}, apply_at));
}

[[nodiscard]] bool schedule_hardware_link(
    cmd::DirectorScheduler& scheduler,
    const editor::HardwareLinkRequest& request,
    doc::NodeId id,
    cmd::FrameNumber apply_at) {
    constexpr core::u64 max_document_node =
        static_cast<core::u64>(std::numeric_limits<std::int64_t>::max());
    if (request.from.device.value() > max_document_node ||
        request.to.device.value() > max_document_node) {
        return true;
    }
    if (request.output_binding.has_value() &&
        (request.output_binding->render_server != request.from.device ||
         request.output_binding->logical_port != request.from.port ||
         request.output_region.width == 0U ||
         request.output_region.height == 0U)) {
        return true;
    }
    doc::Node node{};
    node.id = id;
    node.type = std::string{topology::schema::link_type};
    node.properties.emplace(
        std::string{topology::schema::from_device},
        doc::PropertyValue{
            static_cast<std::int64_t>(request.from.device.value())});
    node.properties.emplace(
        std::string{topology::schema::from_port},
        doc::PropertyValue{static_cast<std::int64_t>(request.from.port)});
    node.properties.emplace(
        std::string{topology::schema::to_device},
        doc::PropertyValue{
            static_cast<std::int64_t>(request.to.device.value())});
    node.properties.emplace(
        std::string{topology::schema::to_port},
        doc::PropertyValue{static_cast<std::int64_t>(request.to.port)});
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(node)}, cmd::EraseNode{id},
        cmd::UndoContextId{3U}, cmd::TransactionId{request.gesture}, apply_at));
}

[[nodiscard]] bool schedule_hardware_property(
    cmd::DirectorScheduler& scheduler,
    const editor::HardwarePropertyEdit& edit,
    cmd::FrameNumber apply_at) {
    const bool allowed =
        edit.property == topology::schema::name ||
        edit.property == topology::schema::address ||
        edit.property == topology::schema::lifecycle_endpoint ||
        edit.property == topology::schema::node_index ||
        edit.property == topology::schema::width ||
        edit.property == topology::schema::height ||
        edit.property == topology::schema::refresh_numerator ||
        edit.property == topology::schema::refresh_denominator ||
        edit.property == topology::schema::window_mode ||
        edit.property == topology::schema::desktop_x ||
        edit.property == topology::schema::desktop_y;
    const doc::Node* const existing =
        scheduler.projected_document().get(edit.device);
    if (!allowed || existing == nullptr ||
        existing->type != topology::schema::device_type) {
        return true;
    }
    const bool lifecycle_property =
        edit.property == topology::schema::lifecycle_endpoint ||
        edit.property == topology::schema::node_index ||
        edit.property == topology::schema::window_mode ||
        edit.property == topology::schema::desktop_x ||
        edit.property == topology::schema::desktop_y;
    const auto kind = existing->properties.find(
        std::string{topology::schema::kind});
    const auto* const kind_text =
        kind != existing->properties.end()
            ? std::get_if<std::string>(&kind->second)
            : nullptr;
    if (lifecycle_property &&
        (kind_text == nullptr ||
         *kind_text != topology::schema::render_server_kind)) {
        return true;
    }
    doc::Node updated = *existing;
    updated.properties.insert_or_assign(edit.property, edit.value);
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)}, cmd::PutNode{*existing},
        cmd::UndoContextId{3U}, cmd::TransactionId{edit.gesture}, apply_at));
}

[[nodiscard]] bool schedule_hardware_control_port(
    cmd::DirectorScheduler& scheduler,
    const editor::HardwareControlPortEdit& edit,
    doc::NodeId allocated_id,
    cmd::FrameNumber apply_at) {
    if (edit.base_port == 0U ||
        edit.base_port > topology::max_hardware_control_base_port) {
        return true;
    }

    const doc::Root& root = scheduler.projected_document();
    const doc::Node* existing{};
    if (edit.settings != doc::NodeId{}) {
        existing = root.get(edit.settings);
        // A stale request from a replaced project must not create a second
        // settings object or edit a node whose identity was reused.
        if (existing == nullptr ||
            existing->type != topology::schema::settings_type) {
            return true;
        }
    } else {
        // The panel asks to create when its evaluated map has no provenance.
        // Re-check the authoritative projected document at the authoring
        // boundary and update the lowest-id settings node if one arrived in
        // the meantime, preserving the model's deterministic winner.
        const doc::Delta contents = doc::diff(doc::Root{}, root);
        for (const doc::NodeChange& change : contents.changes) {
            if (!change.after ||
                change.after->type != topology::schema::settings_type) {
                continue;
            }
            if (existing == nullptr || change.after->id < existing->id) {
                existing = change.after.get();
            }
        }
    }

    doc::Node updated{};
    updated.id = existing != nullptr ? existing->id : allocated_id;
    updated.type = std::string{topology::schema::settings_type};
    if (existing != nullptr) {
        updated = *existing;
    }
    updated.properties.insert_or_assign(
        std::string{topology::schema::control_base_port},
        doc::PropertyValue{static_cast<std::int64_t>(edit.base_port)});
    cmd::Payload inverse = existing != nullptr
        ? cmd::Payload{cmd::PutNode{*existing}}
        : cmd::Payload{cmd::EraseNode{allocated_id}};
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)}, std::move(inverse),
        cmd::UndoContextId{3U}, cmd::TransactionId{edit.gesture}, apply_at));
}

// Authors the project-wide synchronization policy exactly like every other
// document edit, so it replicates, journals, and undoes through the same
// paths. A zero policy provenance creates sync.policy; an existing node is
// replaced whole, which keeps a half-edited raster from ever being visible.
[[nodiscard]] bool schedule_sync_policy(
    cmd::DirectorScheduler& scheduler,
    const editor::HardwareSyncPolicyEdit& edit,
    doc::NodeId allocated_id,
    cmd::FrameNumber apply_at) {
    if (!sync::valid_policy(edit.value)) {
        return true;
    }

    const doc::Root& root = scheduler.projected_document();
    const doc::Node* existing{};
    if (edit.policy != doc::NodeId{}) {
        existing = root.get(edit.policy);
        // A stale request from a replaced project must not create a second
        // policy object or edit a node whose identity was reused.
        if (existing == nullptr ||
            existing->type != sync::schema::policy_type) {
            return true;
        }
    } else {
        // Re-check the authoritative projected document at the authoring
        // boundary and update the lowest-id policy node if one arrived in the
        // meantime, preserving the model deterministic winner.
        const doc::Delta contents = doc::diff(doc::Root{}, root);
        for (const doc::NodeChange& change : contents.changes) {
            if (!change.after ||
                change.after->type != sync::schema::policy_type) {
                continue;
            }
            if (existing == nullptr || change.after->id < existing->id) {
                existing = change.after.get();
            }
        }
    }

    doc::Node updated{};
    updated.id = existing != nullptr ? existing->id : allocated_id;
    sync::write_sync_policy_properties(updated, edit.value);
    cmd::Payload inverse = existing != nullptr
        ? cmd::Payload{cmd::PutNode{*existing}}
        : cmd::Payload{cmd::EraseNode{allocated_id}};
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)}, std::move(inverse),
        cmd::UndoContextId{3U}, cmd::TransactionId{edit.gesture}, apply_at));
}

[[nodiscard]] std::string_view hardware_window_mode_property(
    topology::HardwareWindowMode mode) noexcept {
    switch (mode) {
    case topology::HardwareWindowMode::Inherit:
        return topology::schema::inherit_window_mode;
    case topology::HardwareWindowMode::Windowed:
        return topology::schema::windowed_window_mode;
    case topology::HardwareWindowMode::Borderless:
        return topology::schema::borderless_window_mode;
    case topology::HardwareWindowMode::Fullscreen:
        return topology::schema::fullscreen_window_mode;
    }
    return topology::schema::fullscreen_window_mode;
}

[[nodiscard]] doc::Node make_hardware_output_binding(
    doc::NodeId id,
    const topology::HardwareOutputBinding& binding) {
    doc::Node node{};
    node.id = id;
    node.type = std::string{topology::schema::output_binding_type};
    node.properties.emplace(
        std::string{topology::schema::render_server},
        doc::PropertyValue{static_cast<std::int64_t>(
            binding.render_server.value())});
    node.properties.emplace(
        std::string{topology::schema::logical_port},
        doc::PropertyValue{static_cast<std::int64_t>(binding.logical_port)});
    node.properties.emplace(
        std::string{topology::schema::feed_index},
        doc::PropertyValue{static_cast<std::int64_t>(binding.feed_index)});
    node.properties.emplace(
        std::string{topology::schema::display_adapter_id},
        doc::PropertyValue{
            topology::format_display_adapter_id(binding.target.adapter_id)});
    node.properties.emplace(
        std::string{topology::schema::display_target_id},
        doc::PropertyValue{static_cast<std::int64_t>(binding.target.target_id)});
    node.properties.emplace(
        std::string{topology::schema::connector_instance},
        doc::PropertyValue{static_cast<std::int64_t>(
            binding.target.connector_instance)});
    node.properties.emplace(
        std::string{topology::schema::width},
        doc::PropertyValue{static_cast<std::int64_t>(binding.width)});
    node.properties.emplace(
        std::string{topology::schema::height},
        doc::PropertyValue{static_cast<std::int64_t>(binding.height)});
    node.properties.emplace(
        std::string{topology::schema::refresh_numerator},
        doc::PropertyValue{static_cast<std::int64_t>(
            binding.refresh_numerator)});
    node.properties.emplace(
        std::string{topology::schema::refresh_denominator},
        doc::PropertyValue{static_cast<std::int64_t>(
            binding.refresh_denominator)});
    node.properties.emplace(
        std::string{topology::schema::window_mode},
        doc::PropertyValue{std::string{
            hardware_window_mode_property(binding.window_mode)}});
    return node;
}

[[nodiscard]] bool schedule_hardware_output_binding(
    cmd::DirectorScheduler& scheduler,
    const editor::HardwareOutputBindingEdit& edit,
    doc::NodeId allocated_id, cmd::FrameNumber apply_at) {
    const topology::HardwareOutputBinding& binding = edit.binding;
    if (binding.render_server == doc::NodeId{} ||
        binding.logical_port < topology::first_render_server_video_port ||
        binding.logical_port > topology::last_render_server_video_port ||
        binding.feed_index > topology::max_hardware_node_index ||
        !binding.target.valid() || binding.width == 0U ||
        binding.width > topology::max_hardware_output_width ||
        binding.height == 0U ||
        binding.height > topology::max_hardware_output_height ||
        binding.refresh_numerator == 0U ||
        binding.refresh_denominator == 0U ||
        binding.window_mode == topology::HardwareWindowMode::Inherit) {
        return true;
    }

    const doc::Root& root = scheduler.projected_document();
    topology::HardwareMapModel model;
    model.reset(root);
    const topology::HardwareMap map = model.evaluate();
    const topology::HardwareDevice* const server =
        map.device(binding.render_server);
    if (server == nullptr ||
        server->kind != topology::HardwareDeviceKind::RenderServer) {
        return true;
    }
    const doc::NodeId id =
        binding.node != doc::NodeId{} ? binding.node : allocated_id;
    const doc::Node* const existing = root.get(id);
    if (existing != nullptr &&
        existing->type != topology::schema::output_binding_type) {
        return true;
    }
    for (const topology::HardwareOutputBinding& other :
         map.output_bindings) {
        if (other.node == id || other.render_server != binding.render_server) {
            continue;
        }
        if (other.logical_port == binding.logical_port ||
            other.feed_index == binding.feed_index ||
            other.target == binding.target) {
            return true;
        }
    }
    cmd::Payload inverse = existing != nullptr
        ? cmd::Payload{cmd::PutNode{*existing}}
        : cmd::Payload{cmd::EraseNode{id}};
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{make_hardware_output_binding(id, binding)},
        std::move(inverse), cmd::UndoContextId{3U},
        cmd::TransactionId{edit.gesture}, apply_at));
}

[[nodiscard]] const doc::Node* find_feed_map_node(const doc::Root& root,
                                                  core::u32 node_index);

[[nodiscard]] bool schedule_hardware_output_binding_delete(
    cmd::DirectorScheduler& scheduler,
    const editor::HardwareOutputBindingDelete& request,
    cmd::FrameNumber apply_at) {
    const doc::Node* const existing =
        scheduler.projected_document().get(request.binding);
    if (existing == nullptr ||
        existing->type != topology::schema::output_binding_type) {
        return true;
    }
    topology::HardwareMapModel model;
    model.reset(scheduler.projected_document());
    const topology::HardwareMap map = model.evaluate();
    const topology::HardwareOutputBinding* const binding =
        map.output_binding(request.binding);
    if (binding == nullptr) {
        return true;
    }
    const bool feed_is_shared = std::any_of(
        map.output_bindings.begin(), map.output_bindings.end(),
        [&](const topology::HardwareOutputBinding& other) {
            return other.node != binding->node &&
                other.feed_index == binding->feed_index;
        });
    if (!feed_is_shared) {
        if (const doc::Node* const feed = find_feed_map_node(
                scheduler.projected_document(), binding->feed_index)) {
            if (!schedule_erase_node(
                    scheduler, feed->id, cmd::UndoContextId{3U},
                    cmd::TransactionId{request.gesture}, apply_at)) {
                return false;
            }
        }
    }
    return schedule_erase_node(
        scheduler, request.binding, cmd::UndoContextId{3U},
        cmd::TransactionId{request.gesture}, apply_at);
}

[[nodiscard]] bool schedule_hardware_delete(
    cmd::DirectorScheduler& scheduler,
    const editor::HardwareDeleteRequest& request,
    cmd::FrameNumber apply_at) {
    if (request.target.kind == editor::HardwareSelectionKind::None ||
        request.target.node == doc::NodeId{}) {
        return true;
    }
    const doc::Root& root = scheduler.projected_document();
    const doc::Node* const target = root.get(request.target.node);
    if (target == nullptr) {
        return true;
    }
    const doc::NodeId target_id = target->id;
    const std::string target_type = target->type;
    const cmd::UndoContextId context{3U};
    const cmd::TransactionId transaction{request.gesture};
    if (request.target.kind == editor::HardwareSelectionKind::Link) {
        if (target_type != topology::schema::link_type) {
            return true;
        }
        topology::HardwareMapModel model;
        model.reset(root);
        const topology::HardwareMap map = model.evaluate();
        if (const topology::HardwareLink* const link = map.link(target_id)) {
            if (const topology::HardwareOutputBinding* const binding =
                    map.output_binding_for(link->from.device,
                                           link->from.port)) {
                if (!schedule_hardware_output_binding_delete(
                        scheduler,
                        editor::HardwareOutputBindingDelete{
                            binding->node, request.gesture},
                        apply_at)) {
                    return false;
                }
            }
        }
        return schedule_erase_node(scheduler, target_id, context, transaction,
                                   apply_at);
    }
    if (target_type != topology::schema::device_type) {
        return true;
    }

    try {
        std::vector<doc::NodeId> incident;
        std::vector<core::u32> feeds;
        topology::HardwareMapModel model;
        model.reset(root);
        const topology::HardwareMap hardware = model.evaluate();
        for (const topology::HardwareOutputBinding& binding :
             hardware.output_bindings) {
            if (binding.render_server == target_id) {
                feeds.push_back(binding.feed_index);
            }
        }
        const doc::Delta contents = doc::diff(doc::Root{}, root);
        for (const doc::NodeChange& change : contents.changes) {
            if (!change.after) {
                continue;
            }
            if (change.after->type == topology::schema::output_binding_type) {
                const auto server = node_reference(
                    *change.after, topology::schema::render_server);
                if (server && *server == target_id) {
                    incident.push_back(change.after->id);
                }
                continue;
            }
            if (change.after->type != topology::schema::link_type) {
                continue;
            }
            const auto from = node_reference(
                *change.after, topology::schema::from_device);
            const auto to = node_reference(
                *change.after, topology::schema::to_device);
            if ((from && *from == target_id) ||
                (to && *to == target_id)) {
                incident.push_back(change.after->id);
            }
        }
        for (const core::u32 feed_index : feeds) {
            const bool shared = std::any_of(
                hardware.output_bindings.begin(),
                hardware.output_bindings.end(),
                [&](const topology::HardwareOutputBinding& binding) {
                    return binding.render_server != target_id &&
                        binding.feed_index == feed_index;
                });
            if (!shared) {
                if (const doc::Node* const feed =
                        find_feed_map_node(scheduler.projected_document(),
                                           feed_index)) {
                    if (!schedule_erase_node(scheduler, feed->id, context,
                                             transaction, apply_at)) {
                        return false;
                    }
                }
            }
        }
        for (const doc::NodeId link : incident) {
            if (!schedule_erase_node(scheduler, link, context, transaction,
                                     apply_at)) {
                return false;
            }
        }
        return schedule_erase_node(scheduler, target_id, context, transaction,
                                   apply_at);
    } catch (const std::bad_alloc&) {
        return false;
    }
}

[[nodiscard]] const doc::Node* find_feed_map_node(const doc::Root& root,
                                                  core::u32 node_index) {
    const doc::Delta contents = doc::diff(doc::Root{}, root);
    for (const doc::NodeChange& change : contents.changes) {
        if (!change.after || change.after->type != output::schema::map_type) {
            continue;
        }
        const auto found = change.after->properties.find(
            output::schema::node_index);
        if (found == change.after->properties.end()) {
            continue;
        }
        const auto* const value = std::get_if<std::int64_t>(&found->second);
        if (value != nullptr && *value >= 0 &&
            static_cast<core::u64>(*value) == node_index) {
            return change.after.get();
        }
    }
    return nullptr;
}

[[nodiscard]] const doc::Node* find_feed_surface_node(
    const doc::Root& root, doc::NodeId id) noexcept {
    const doc::Node* const node = root.get(id);
    return node != nullptr && node->type == output::schema::surface_type
        ? node
        : nullptr;
}

[[nodiscard]] bool feed_surface_reference_fits_document(
    doc::NodeId id) noexcept {
    return id.value() <= static_cast<core::u64>(
        std::numeric_limits<std::int64_t>::max());
}

[[nodiscard]] doc::NodeId projection_asset_for_source(
    const doc::Root& root, std::string_view source) {
    if (source.empty()) {
        return {};
    }
    assets::AssetModel model;
    model.reset(root);
    const assets::AssetRegistry registry = model.evaluate();
    const assets::Asset* const asset =
        registry.asset_for_source(source, assets::AssetKind::ProjectionMesh);
    return asset != nullptr ? assets::asset_id(*asset) : doc::NodeId{};
}

void write_feed_surface_properties(
    doc::Node& node, std::string_view name,
    const output::PhysicalSurface& surface,
    output::ProjectionSurfaceShape shape,
    float horizontal_arc_turns,
    core::u32 mesh_columns,
    std::string_view mesh_source,
    float mesh_scale,
    doc::NodeId mesh_asset) {
    std::string_view shape_property = output::schema::shape_planar;
    switch (shape) {
    case output::ProjectionSurfaceShape::Planar:
        break;
    case output::ProjectionSurfaceShape::HorizontalArc:
        shape_property = output::schema::shape_horizontal_arc;
        break;
    case output::ProjectionSurfaceShape::ImportedMesh:
        shape_property = output::schema::shape_imported_mesh;
        break;
    }
    node.properties.insert_or_assign(
        std::string{output::schema::surface_name},
        doc::PropertyValue{std::string{name}});
    node.properties.insert_or_assign(
        std::string{output::schema::surface_shape},
        doc::PropertyValue{std::string{shape_property}});
    node.properties.insert_or_assign(
        std::string{output::schema::center_x},
        doc::PropertyValue{static_cast<double>(surface.center.x)});
    node.properties.insert_or_assign(
        std::string{output::schema::center_y},
        doc::PropertyValue{static_cast<double>(surface.center.y)});
    node.properties.insert_or_assign(
        std::string{output::schema::center_z},
        doc::PropertyValue{static_cast<double>(surface.center.z)});
    node.properties.insert_or_assign(
        std::string{output::schema::right_x},
        doc::PropertyValue{static_cast<double>(surface.right.x)});
    node.properties.insert_or_assign(
        std::string{output::schema::right_y},
        doc::PropertyValue{static_cast<double>(surface.right.y)});
    node.properties.insert_or_assign(
        std::string{output::schema::right_z},
        doc::PropertyValue{static_cast<double>(surface.right.z)});
    node.properties.insert_or_assign(
        std::string{output::schema::up_x},
        doc::PropertyValue{static_cast<double>(surface.up.x)});
    node.properties.insert_or_assign(
        std::string{output::schema::up_y},
        doc::PropertyValue{static_cast<double>(surface.up.y)});
    node.properties.insert_or_assign(
        std::string{output::schema::up_z},
        doc::PropertyValue{static_cast<double>(surface.up.z)});
    node.properties.insert_or_assign(
        std::string{output::schema::physical_width},
        doc::PropertyValue{static_cast<double>(surface.width)});
    node.properties.insert_or_assign(
        std::string{output::schema::physical_height},
        doc::PropertyValue{static_cast<double>(surface.height)});
    node.properties.insert_or_assign(
        std::string{output::schema::raster_width},
        doc::PropertyValue{
            static_cast<std::int64_t>(surface.raster.width)});
    node.properties.insert_or_assign(
        std::string{output::schema::raster_height},
        doc::PropertyValue{
            static_cast<std::int64_t>(surface.raster.height)});
    node.properties.insert_or_assign(
        std::string{output::schema::horizontal_arc_turns},
        doc::PropertyValue{static_cast<double>(horizontal_arc_turns)});
    node.properties.insert_or_assign(
        std::string{output::schema::mesh_columns},
        doc::PropertyValue{static_cast<std::int64_t>(mesh_columns)});
    node.properties.insert_or_assign(
        std::string{output::schema::mesh_source},
        doc::PropertyValue{std::string{mesh_source}});
    if (shape == output::ProjectionSurfaceShape::ImportedMesh && mesh_asset &&
        feed_surface_reference_fits_document(mesh_asset)) {
        node.properties.insert_or_assign(
            std::string{output::schema::mesh_asset_id},
            doc::PropertyValue{
                static_cast<std::int64_t>(mesh_asset.value())});
    } else {
        node.properties.erase(std::string{output::schema::mesh_asset_id});
    }
    node.properties.insert_or_assign(
        std::string{output::schema::mesh_scale},
        doc::PropertyValue{static_cast<double>(mesh_scale)});
}

[[nodiscard]] bool valid_feed_surface_request(
    const output::PhysicalSurface& surface,
    output::ProjectionSurfaceShape shape,
    float horizontal_arc_turns,
    core::u32 mesh_columns,
    std::string_view mesh_source,
    float mesh_scale) noexcept {
    // The model parses every persisted scalar even when its shape does not
    // currently use it. Refuse dormant non-finite shape parameters here too,
    // so a node accepted for authoring cannot be rejected when it is evaluated
    // or loaded from a saved project.
    if (!output::valid(surface) ||
        !std::isfinite(horizontal_arc_turns) ||
        !std::isfinite(mesh_scale)) {
        return false;
    }
    switch (shape) {
    case output::ProjectionSurfaceShape::Planar:
        return true;
    case output::ProjectionSurfaceShape::HorizontalArc:
        return std::abs(horizontal_arc_turns) > 1.0e-5F &&
            std::abs(horizontal_arc_turns) < 0.5F &&
            mesh_columns >= output::min_horizontal_arc_columns &&
            mesh_columns <= output::max_horizontal_arc_columns;
    case output::ProjectionSurfaceShape::ImportedMesh:
        return !mesh_source.empty() && mesh_scale > 0.0F &&
            mesh_scale <= output::max_imported_mesh_scale;
    }
    return false;
}

[[nodiscard]] doc::Node make_feed_surface_node(
    doc::NodeId id, const editor::FeedSurfaceAddRequest& request,
    doc::NodeId mesh_asset) {
    doc::Node node{};
    node.id = id;
    node.type = std::string{output::schema::surface_type};
    write_feed_surface_properties(
        node, request.name, request.surface, request.shape,
        request.horizontal_arc_turns, request.mesh_columns,
        request.mesh_source, request.mesh_scale, mesh_asset);
    return node;
}

[[nodiscard]] bool schedule_feed_surface_add(
    cmd::DirectorScheduler& scheduler,
    const editor::FeedSurfaceAddRequest& request,
    doc::NodeId allocated_id,
    cmd::FrameNumber apply_at) {
    if (!valid_feed_surface_request(
            request.surface, request.shape, request.horizontal_arc_turns,
            request.mesh_columns, request.mesh_source,
            request.mesh_scale)) {
        return true;
    }
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{make_feed_surface_node(
            allocated_id, request,
            projection_asset_for_source(scheduler.projected_document(),
                                        request.mesh_source))},
        cmd::EraseNode{allocated_id}, cmd::UndoContextId{4U},
        cmd::TransactionId{request.gesture}, apply_at));
}

[[nodiscard]] bool schedule_feed_surface_edit(
    cmd::DirectorScheduler& scheduler,
    const editor::FeedSurfaceEditRequest& request,
    cmd::FrameNumber apply_at) {
    const doc::Node* const existing = find_feed_surface_node(
        scheduler.projected_document(), request.node);
    if (existing == nullptr ||
        !valid_feed_surface_request(
            request.surface, request.shape, request.horizontal_arc_turns,
            request.mesh_columns, request.mesh_source,
            request.mesh_scale)) {
        return true;
    }
    doc::Node updated = *existing;
    write_feed_surface_properties(
        updated, request.name, request.surface, request.shape,
        request.horizontal_arc_turns, request.mesh_columns,
        request.mesh_source, request.mesh_scale,
        projection_asset_for_source(scheduler.projected_document(),
                                    request.mesh_source));

    // Surface views receive evaluated geometry. Preserve every transform
    // property the originating control did not actually change, otherwise an
    // edit to an unanimated axis (or to mesh calibration) would bake another
    // axis's animated value into the authored base beneath its channel.
    const auto preserve = [&](std::string_view property) {
        const auto authored = existing->properties.find(property);
        if (authored != existing->properties.end()) {
            updated.properties.insert_or_assign(
                std::string{property}, authored->second);
            return;
        }
        const auto generated = updated.properties.find(property);
        if (generated != updated.properties.end()) {
            updated.properties.erase(generated);
        }
    };
    if (!editor::has_transform(request.transform_components,
                               editor::FeedSurfaceTransform::PositionX)) {
        preserve(output::schema::center_x);
    }
    if (!editor::has_transform(request.transform_components,
                               editor::FeedSurfaceTransform::PositionY)) {
        preserve(output::schema::center_y);
    }
    if (!editor::has_transform(request.transform_components,
                               editor::FeedSurfaceTransform::PositionZ)) {
        preserve(output::schema::center_z);
    }
    if (!editor::has_transform(request.transform_components,
                               editor::FeedSurfaceTransform::Yaw)) {
        preserve(output::schema::right_x);
        preserve(output::schema::right_y);
        preserve(output::schema::right_z);
        preserve(output::schema::up_x);
        preserve(output::schema::up_y);
        preserve(output::schema::up_z);
    }
    const cmd::UndoContextId context{
        request.transform_components == editor::FeedSurfaceTransform::None
            ? 4U
            : 2U};
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)}, cmd::PutNode{*existing},
        context, cmd::TransactionId{request.gesture}, apply_at));
}

[[nodiscard]] bool schedule_feed_surface_delete(
    cmd::DirectorScheduler& scheduler,
    const editor::FeedSurfaceDeleteRequest& request,
    cmd::FrameNumber apply_at) {
    const doc::Node* const existing = find_feed_surface_node(
        scheduler.projected_document(), request.node);
    if (existing == nullptr) {
        return true;
    }
    // Output-map references deliberately survive this erase. Their unresolved
    // surface id is a visible, repairable project error and undo restores the
    // exact surface without having to reconstruct assignments.
    return schedule_erase_node(
        scheduler, existing->id, cmd::UndoContextId{4U},
        cmd::TransactionId{request.gesture}, apply_at);
}

[[nodiscard]] bool schedule_feed_projection_edit(
    cmd::DirectorScheduler& scheduler,
    const editor::FeedProjectionEdit& edit,
    cmd::FrameNumber apply_at) {
    const doc::Root& root = scheduler.projected_document();
    const doc::Node* const existing =
        find_feed_map_node(root, edit.node_index);
    if (existing == nullptr || edit.source.width == 0U ||
        edit.source.height == 0U) {
        return true;
    }

    std::string_view projection;
    doc::NodeId surface{};
    switch (edit.projection) {
    case output::FeedProjection::CanvasSlice:
        projection = output::schema::projection_canvas;
        break;
    case output::FeedProjection::PlanarSurface: {
        projection = output::schema::projection_planar_surface;
        surface = edit.surface;
        if (!surface || !feed_surface_reference_fits_document(surface) ||
            find_feed_surface_node(root, surface) == nullptr) {
            return true;
        }
        output::FeedMapModel model;
        model.reset(root);
        const output::FeedMap evaluated = model.evaluate();
        const output::ProjectionSurface* const resolved =
            evaluated.surface_for(surface);
        if (resolved == nullptr ||
            !scene::contains(resolved->geometry.raster, edit.source)) {
            return true;
        }
        break;
    }
    default:
        return true;
    }

    doc::Node updated = *existing;
    updated.properties.insert_or_assign(
        std::string{output::schema::projection},
        doc::PropertyValue{std::string{projection}});
    updated.properties.insert_or_assign(
        std::string{output::schema::surface_id},
        doc::PropertyValue{static_cast<std::int64_t>(surface.value())});
    updated.properties.insert_or_assign(
        std::string{output::schema::source_x},
        doc::PropertyValue{static_cast<std::int64_t>(edit.source.x)});
    updated.properties.insert_or_assign(
        std::string{output::schema::source_y},
        doc::PropertyValue{static_cast<std::int64_t>(edit.source.y)});
    updated.properties.insert_or_assign(
        std::string{output::schema::source_width},
        doc::PropertyValue{static_cast<std::int64_t>(edit.source.width)});
    updated.properties.insert_or_assign(
        std::string{output::schema::source_height},
        doc::PropertyValue{static_cast<std::int64_t>(edit.source.height)});
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)}, cmd::PutNode{*existing},
        cmd::UndoContextId{4U}, cmd::TransactionId{edit.gesture}, apply_at));
}

[[nodiscard]] bool schedule_feed_map_edit(
    cmd::DirectorScheduler& scheduler,
    const editor::FeedMapRectEdit& edit,
    doc::NodeId allocated_id,
    cmd::FrameNumber apply_at) {
    const doc::Node* const existing =
        find_feed_map_node(scheduler.projected_document(), edit.node_index);
    doc::Node updated{};
    updated.id = existing != nullptr ? existing->id : allocated_id;
    updated.type = std::string{output::schema::map_type};
    if (existing != nullptr) {
        updated = *existing;
    }
    updated.properties.insert_or_assign(
        std::string{output::schema::node_index},
        doc::PropertyValue{static_cast<std::int64_t>(edit.node_index)});
    updated.properties.insert_or_assign(
        std::string{output::schema::source_x},
        doc::PropertyValue{static_cast<std::int64_t>(edit.source.x)});
    updated.properties.insert_or_assign(
        std::string{output::schema::source_y},
        doc::PropertyValue{static_cast<std::int64_t>(edit.source.y)});
    updated.properties.insert_or_assign(
        std::string{output::schema::source_width},
        doc::PropertyValue{static_cast<std::int64_t>(edit.source.width)});
    updated.properties.insert_or_assign(
        std::string{output::schema::source_height},
        doc::PropertyValue{static_cast<std::int64_t>(edit.source.height)});
    cmd::Payload inverse = existing != nullptr
        ? cmd::Payload{cmd::PutNode{*existing}}
        : cmd::Payload{cmd::EraseNode{allocated_id}};
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)}, std::move(inverse),
        cmd::UndoContextId{4U}, cmd::TransactionId{edit.gesture}, apply_at));
}

[[nodiscard]] bool schedule_feed_map_add(
    cmd::DirectorScheduler& scheduler,
    const editor::FeedMapAddRequest& request, doc::NodeId allocated_id,
    cmd::FrameNumber apply_at,
    cmd::UndoContextId context = cmd::UndoContextId{4U}) {
    if (request.source.width == 0U || request.source.height == 0U ||
        find_feed_map_node(scheduler.projected_document(), request.node_index)
            != nullptr) {
        return true;
    }
    doc::Node node{};
    node.id = allocated_id;
    node.type = std::string{output::schema::map_type};
    node.properties.emplace(
        std::string{output::schema::node_index},
        doc::PropertyValue{static_cast<std::int64_t>(request.node_index)});
    node.properties.emplace(
        std::string{output::schema::source_x},
        doc::PropertyValue{static_cast<std::int64_t>(request.source.x)});
    node.properties.emplace(
        std::string{output::schema::source_y},
        doc::PropertyValue{static_cast<std::int64_t>(request.source.y)});
    node.properties.emplace(
        std::string{output::schema::source_width},
        doc::PropertyValue{static_cast<std::int64_t>(request.source.width)});
    node.properties.emplace(
        std::string{output::schema::source_height},
        doc::PropertyValue{static_cast<std::int64_t>(request.source.height)});
    node.properties.emplace(
        std::string{output::schema::projection},
        doc::PropertyValue{std::string{output::schema::projection_canvas}});
    node.properties.emplace(
        std::string{output::schema::surface_id},
        doc::PropertyValue{std::int64_t{0}});
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(node)}, cmd::EraseNode{allocated_id},
        context, cmd::TransactionId{request.gesture}, apply_at));
}

[[nodiscard]] bool schedule_feed_map_delete(
    cmd::DirectorScheduler& scheduler,
    const editor::FeedMapDeleteRequest& request,
    cmd::FrameNumber apply_at) {
    const doc::Node* const existing =
        find_feed_map_node(scheduler.projected_document(), request.node_index);
    if (existing == nullptr) {
        return true;
    }
    return schedule_erase_node(
        scheduler, existing->id, cmd::UndoContextId{4U},
        cmd::TransactionId{request.gesture}, apply_at);
}
#endif

[[nodiscard]] bool parse_u64(std::string_view value, core::u64& parsed) noexcept {
    if (value.empty()) {
        return false;
    }
    const auto* const begin = value.data();
    const auto* const end = begin + value.size();
    const auto result = std::from_chars(begin, end, parsed, 10);
    return result.ec == std::errc{} && result.ptr == end;
}

void sleep_until_boundary(Clock::time_point boundary) noexcept {
    const auto now = Clock::now();
    if (boundary <= now) {
        std::this_thread::yield();
        return;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(boundary - now);
    std::this_thread::sleep_for(std::min(remaining, max_sleep));
}

[[nodiscard]] const char* transport_label(show::TransportState state) noexcept {
    switch (state) {
    case show::TransportState::Playing:
        return "playing";
    case show::TransportState::Paused:
        return "paused";
    case show::TransportState::Stopped:
        break;
    }
    return "stopped";
}

[[nodiscard]] const char* outcome_label(show::FrameCommitOutcome outcome) noexcept {
    switch (outcome) {
    case show::FrameCommitOutcome::Idle:
        return "idle";
    case show::FrameCommitOutcome::Ready:
        return "ready";
    case show::FrameCommitOutcome::Committed:
        return "committed";
    case show::FrameCommitOutcome::NotApproved:
        return "waiting";
    case show::FrameCommitOutcome::CheckpointPending:
        return "checkpoint";
    case show::FrameCommitOutcome::NeedsResync:
        return "resync";
    case show::FrameCommitOutcome::Unavailable:
        break;
    }
    return "disconnected";
}

[[nodiscard]] const char* sync_mode_label(sync::SyncMode mode) noexcept {
    switch (mode) {
    case sync::SyncMode::Unsynced:
        return "unsynced";
    case sync::SyncMode::Framelock:
        return "framelock";
    case sync::SyncMode::ExternalGenlock:
        return "external-genlock";
    }
    return "unsynced";
}

[[nodiscard]] const char* runtime_barrier_label(
    net::RuntimeBarrierState state) noexcept {
    switch (state) {
    case net::RuntimeBarrierState::Unknown:
        return "unknown";
    case net::RuntimeBarrierState::Unsupported:
        return "unsupported";
    case net::RuntimeBarrierState::NotJoined:
        return "not-joined";
    case net::RuntimeBarrierState::Joining:
        return "joining";
    case net::RuntimeBarrierState::Joined:
        return "joined";
    case net::RuntimeBarrierState::Left:
        return "left";
    case net::RuntimeBarrierState::Failed:
        return "failed";
    }
    return "unknown";
}

[[nodiscard]] const char* presenter_error_text(
    show::PresenterError error) noexcept {
    switch (error) {
    case show::PresenterError::InvalidConfiguration:
        return "invalid surface configuration";
    case show::PresenterError::AllocationFailed:
        return "allocation failed";
    case show::PresenterError::ExactDisplayModeUnavailable:
        return "the selected display does not support the exact saved "
               "resolution and refresh rate";
    case show::PresenterError::DisplayUnavailable:
        break;
    }
    return "the platform refused a window";
}

[[nodiscard]] show::ShowClockConfig clock_config(
    const PlaybackOptions& options) noexcept {
    return show::ShowClockConfig{
        options.rate, options.first_frame, options.last_frame, options.loop};
}

// Per-frame presentation cost, reported only where there is one to report.
// The CPU path has no submission to time and says nothing rather than zero.
[[nodiscard]] std::string gpu_timing_text(
    const show::PresenterTiming& timing) {
    if (!timing.gpu) {
        return {};
    }
    return " present-us=" + std::to_string(timing.last_present_ns / 1'000U)
        + " present-peak-us=" + std::to_string(timing.peak_present_ns / 1'000U)
        + " gpu-observed-us="
        + std::to_string(timing.last_submit_to_observed_ns / 1'000U)
        + " gpu-skipped=" + std::to_string(timing.skipped_presents)
        + " gpu-present=" + (timing.vsync_limited ? "vsync" : "unthrottled");
}

// Control frames the wall clock says are owed but the seal loop has not
// produced. Under an anchored transport a Director that cannot keep up makes
// the show run slow rather than skip positions, so this is the number that
// says so instead of the old skipped-position count.
[[nodiscard]] core::u64 pacer_lag(
    const show::ShowClock& pacer,
    core::u64 sealed) noexcept {
    const auto due = pacer.frame().value();
    return due > sealed ? due - sealed : 0U;
}

[[nodiscard]] core::u64 status_interval(
    const PlaybackOptions& options,
    show::FrameRate rate) noexcept {
    if (options.status_interval != 0U) {
        return options.status_interval;
    }
    const auto per_second =
        static_cast<core::u64>(show::frames_per_second(rate));
    return per_second == 0U ? 1U : per_second;
}

// Builds the output for one role. A windowed request in a build without the
// shell is refused rather than silently downgraded: an operator who asked for a
// window and got a headless run would have no way to tell.
[[nodiscard]] std::unique_ptr<show::Presenter> make_presenter(
    const PlaybackOptions& options,
    const std::string& title,
    bool accepts_commands,
    const PresenterWindowSettings* applied_window = nullptr) {
    if (options.windowed) {
#if defined(MEDIASERVER_HAS_GUI)
        ui::SdlPresenterConfig config{};
        config.title = title;
        config.width = options.surface_width;
        config.height = options.surface_height;
        config.position_x = options.window_x;
        config.position_y = options.window_y;
        config.position_explicit = options.window_position_explicit;
        config.fullscreen = options.fullscreen;
        config.borderless = options.borderless;
        if (applied_window != nullptr) {
            config.refresh_numerator = applied_window->refresh_numerator;
            config.refresh_denominator = applied_window->refresh_denominator;
            config.require_exact_display_mode =
                applied_window->require_exact_display_mode;
        }
        config.accepts_commands = accepts_commands;
        config.gpu = options.gpu;
        config.scene = options.scene;
        config.output = options.output;

        auto created = ui::SdlPresenter::create(config);
        if (!created) {
            std::cerr << "could not open the output window: "
                      << presenter_error_text(created.error()) << '\n';
            return nullptr;
        }
        return std::move(created).value();
#else
        static_cast<void>(title);
        static_cast<void>(accepts_commands);
        static_cast<void>(applied_window);
        std::cerr << "this build has no window output; configure with "
                     "MEDIASERVER_ENABLE_GUI=ON and the vcpkg gui feature\n";
        return nullptr;
#endif
    }

    auto created = show::HeadlessPresenter::create(
        options.surface_width, options.surface_height);
    if (!created) {
        std::cerr << "could not allocate the presentation surface: "
                  << presenter_error_text(created.error()) << '\n';
        return nullptr;
    }
    return std::move(created).value();
}

// Applies one operator command to the Director transport, re-anchoring it at
// the control frame the result will be sealed into. Every command is exact
// integer state, and the nodes learn about it through the ordinary
// sealed-frame path.
void apply_command(
    show::TransportAuthority& transport,
    show::PresenterCommand command,
    cmd::FrameNumber at) noexcept {
    const auto per_second =
        static_cast<core::u64>(
            show::frames_per_second(transport.anchor().rate));
    // The live range from the transport, so a range change from the editor is
    // respected by the step and jump clamps immediately.
    const auto first = transport.anchor().first_frame.value();
    const auto last = transport.anchor().last_frame.value();
    const auto current = transport.frame_at(at).value();

    const auto clamped = [first, last](core::u64 target) noexcept {
        return show::ShowFrame{std::min(std::max(target, first), last)};
    };
    const auto back = [current, first](core::u64 delta) noexcept {
        return current - first >= delta ? current - delta : first;
    };
    const auto forward = [current, last](core::u64 delta) noexcept {
        return last - current >= delta ? current + delta : last;
    };

    switch (command) {
    case show::PresenterCommand::TogglePlay:
        if (transport.state() == show::TransportState::Playing) {
            static_cast<void>(transport.pause(at));
        } else {
            static_cast<void>(transport.play(at));
        }
        return;
    case show::PresenterCommand::StepBack:
    case show::PresenterCommand::StepForward:
    case show::PresenterCommand::JumpBack:
    case show::PresenterCommand::JumpForward:
    case show::PresenterCommand::GoToStart:
        break;
    case show::PresenterCommand::None:
    case show::PresenterCommand::Quit:
        return;
    }

    // Scrubbing is a paused operation: stepping while playing would be
    // immediately overwritten by the next boundary.
    if (transport.state() == show::TransportState::Playing) {
        static_cast<void>(transport.pause(at));
    }

    switch (command) {
    case show::PresenterCommand::StepBack:
        static_cast<void>(transport.seek(clamped(back(1U)), at));
        return;
    case show::PresenterCommand::StepForward:
        static_cast<void>(transport.seek(clamped(forward(1U)), at));
        return;
    case show::PresenterCommand::JumpBack:
        static_cast<void>(transport.seek(clamped(back(per_second)), at));
        return;
    case show::PresenterCommand::JumpForward:
        static_cast<void>(transport.seek(clamped(forward(per_second)), at));
        return;
    case show::PresenterCommand::GoToStart:
        static_cast<void>(
            transport.seek(transport.anchor().first_frame, at));
        return;
    default:
        return;
    }
}

#if defined(MEDIASERVER_HAS_GUI)
// Fills the per-output physical-timing row of Output Monitor.
//
// Each row answers for one supervised render process: how many of its outputs
// the authored sync groups expect, how many actually report locked, whether
// its swapchains joined the expected barrier, and what its frame accounting
// looks like. A row with nothing to go on reports SYNC UNVERIFIED rather than
// an absence that could be mistaken for health.
void apply_node_sync_rows(
    std::vector<editor::OutputMonitorState::Node>& rows,
    const sync::SyncPolicy& policy,
    const sync::SyncTopology& topology,
    const sync::SyncStatusSnapshot& status) {
    using RowSync = editor::OutputMonitorState::Node::SyncStatus;
    for (editor::OutputMonitorState::Node& row : rows) {
        row.sync = RowSync::Unsynced;
        row.sync_locked_outputs = 0U;
        row.sync_expected_outputs = 0U;
        row.barrier_joined_outputs = 0U;
        row.barrier_expected_outputs = 0U;
        row.missed_frames = 0U;
        row.late_frames = 0U;
        row.held_frames = 0U;
        row.repeated_frames = 0U;
        row.loss_of_sync_events = 0U;
        row.sync_detail.clear();
        if (!policy.requires_physical_sync()) {
            continue;
        }

        const sync::NodeSyncStatus* const observed =
            status.node(row.node_index);
        for (const sync::SyncGroup& group : topology.groups) {
            for (const sync::SyncGroupMember& member : group.members) {
                if (member.node_index != row.node_index) {
                    continue;
                }
                ++row.sync_expected_outputs;
                if (observed == nullptr) {
                    continue;
                }
                for (const sync::SyncOutputStatus& output :
                     observed->outputs) {
                    if (output.target == member.target &&
                        output.lock == sync::SyncLockState::Locked) {
                        ++row.sync_locked_outputs;
                        break;
                    }
                }
            }
        }

        if (observed == nullptr ||
            (!observed->machine_reported && !observed->runtime_reported)) {
            row.sync = RowSync::Unverified;
            continue;
        }
        row.sync_detail = observed->detail;
        row.barrier_joined_outputs = observed->barrier_joined_outputs;
        row.barrier_expected_outputs = observed->barrier_expected_outputs;
        row.missed_frames = observed->counters.missed;
        row.late_frames = observed->counters.late;
        row.held_frames = observed->counters.held;
        row.repeated_frames = observed->counters.repeated;
        row.loss_of_sync_events = observed->counters.loss_of_sync_events;

        // Ordered from the most fundamental missing fact to the most
        // specific, so the row names the first thing an operator must fix.
        switch (observed->board) {
        case sync::SyncDeviceAvailability::Absent:
        case sync::SyncDeviceAvailability::Failed:
            row.sync = RowSync::SyncDeviceMissing;
            continue;
        case sync::SyncDeviceAvailability::Unknown:
        case sync::SyncDeviceAvailability::Unsupported:
            row.sync = RowSync::Unverified;
            continue;
        case sync::SyncDeviceAvailability::Present:
            break;
        }
        if (policy.requires_external_reference()) {
            switch (observed->reference) {
            case sync::SyncReferenceState::Absent:
            case sync::SyncReferenceState::Unstable:
                row.sync = RowSync::ReferenceLost;
                continue;
            case sync::SyncReferenceState::Present:
                row.sync = RowSync::ReferenceNotInUse;
                continue;
            case sync::SyncReferenceState::Unknown:
            case sync::SyncReferenceState::Unsupported:
                row.sync = RowSync::Unverified;
                continue;
            case sync::SyncReferenceState::InUse:
                break;
            }
        }
        if (row.sync_locked_outputs < row.sync_expected_outputs) {
            row.sync = RowSync::Unlocked;
            continue;
        }
        if (observed->barrier != sync::PresentBarrierState::Joined ||
            observed->barrier_group != policy.group ||
            observed->barrier_id != policy.barrier ||
            observed->barrier_joined_outputs <
                observed->barrier_expected_outputs) {
            row.sync = observed->barrier == sync::PresentBarrierState::Unknown
                ? RowSync::Unverified
                : RowSync::BarrierIncomplete;
            continue;
        }
        row.sync = observed->simulated ? RowSync::Simulated : RowSync::Locked;
    }
}
#endif

} // namespace

bool window_output_available() noexcept {
#if defined(MEDIASERVER_HAS_GUI)
    return true;
#else
    return false;
#endif
}

bool gpu_output_available() noexcept {
#if defined(MEDIASERVER_HAS_GUI)
    return ui::gpu_output_available();
#else
    return false;
#endif
}

bool scene_output_available() noexcept {
#if defined(MEDIASERVER_HAS_GUI)
    return ui::scene_output_available();
#else
    return false;
#endif
}

bool output_output_available() noexcept {
#if defined(MEDIASERVER_HAS_GUI)
    return ui::output_output_available();
#else
    return false;
#endif
}

bool editor_available() noexcept {
#if defined(MEDIASERVER_HAS_GUI)
    return editor::editor_available();
#else
    return false;
#endif
}

bool parse_frame_rate(std::string_view value, show::FrameRate& rate) noexcept {
    const auto separator = value.find('/');
    core::u64 numerator{};
    core::u64 denominator{1U};
    if (separator == std::string_view::npos) {
        if (!parse_u64(value, numerator)) {
            return false;
        }
    } else if (
        !parse_u64(value.substr(0U, separator), numerator)
        || !parse_u64(value.substr(separator + 1U), denominator)) {
        return false;
    }

    if (numerator == 0U || numerator > show::max_rate_numerator
        || denominator == 0U || denominator > show::max_rate_denominator) {
        return false;
    }
    rate = show::FrameRate{
        static_cast<core::u32>(numerator), static_cast<core::u32>(denominator)};
    return true;
}

bool parse_show_range(
    std::string_view value,
    show::ShowFrame& first,
    show::ShowFrame& last) noexcept {
    const auto separator = value.find(':');
    if (separator == std::string_view::npos) {
        return false;
    }
    core::u64 begin{};
    core::u64 end{};
    if (!parse_u64(value.substr(0U, separator), begin)
        || !parse_u64(value.substr(separator + 1U), end) || end < begin
        || end - begin == std::numeric_limits<core::u64>::max()) {
        return false;
    }
    first = show::ShowFrame{begin};
    last = show::ShowFrame{end};
    return true;
}

bool valid_playback_options(const PlaybackOptions& options) noexcept {
    return show::ShowClock::valid_configuration(clock_config(options))
        && options.surface_width != 0U && options.surface_height != 0U
        && options.surface_width <= 7'680U && options.surface_height <= 4'320U;
}

int run_node_playback(
    net::NodeSessionService& service,
    cmd::NodeCommandBus& node,
    const PlaybackOptions& options,
    const volatile std::sig_atomic_t& stop_requested) {
    // A node output window is input-inert on purpose: what it shows is
    // replicated state, so keyboard transport there would be a lie.
    auto presenter = make_presenter(
        options,
        "media-server node " + std::to_string(options.node_index),
        false);
    if (presenter == nullptr) {
        return 1;
    }
    PresenterWindowSettings active_presenter_settings{
        options.surface_width, options.surface_height, options.window_x,
        options.window_y, options.window_position_explicit, options.windowed,
        options.fullscreen, options.borderless};
    struct NodeOutputPresenter final {
        DeployedOutput output;
        std::unique_ptr<show::Presenter> presenter;
    };
    std::vector<NodeOutputPresenter> output_presenters;
    output_presenters.push_back(NodeOutputPresenter{
        DeployedOutput{
            {}, topology::first_render_server_video_port,
            options.node_index, {}, active_presenter_settings,
            "launch output"},
        std::move(presenter)});
    OutputDeployment active_deployment{};
    active_deployment.node_index = options.node_index;
    active_deployment.legacy_single_output = true;
    active_deployment.outputs.push_back(output_presenters.front().output);
    bool node_deployment_runtime_ready = true;
    bool node_feed_bindings_runtime_ready = true;
    bool node_asset_runtime_ready = false;

    net::NodeServiceFrameCommitSink sink{service, node};
    show::FrameBoundaryDriverConfig config{};
    config.clock = clock_config(options);
    config.mailbox_capacity = 16U;
    config.max_commits_per_tick = 8U;

    auto created =
        show::FrameBoundaryDriver::create(config, sink, Clock::now());
    if (!created) {
        std::cerr << "invalid node frame-boundary driver configuration\n";
        return 1;
    }
    auto driver = std::move(created).value();

    auto retirement = show::RootRetirementRing::create(32U);
    if (!retirement) {
        std::cerr << "could not allocate the node root retirement ring\n";
        return 1;
    }
    auto retired = std::move(retirement).value();

    // The node clock is a pacer, not the transport: it decides when a boundary
    // occurs, while what is presented comes from the committed document.
    if (!driver.play(Clock::now())) {
        std::cerr << "could not start the node frame pacer\n";
        return 1;
    }

    auto interval = status_interval(options, driver.config().clock.rate);
    show::PublishedFrame presented{};
    core::u64 boundaries = 0U;
    core::u64 presented_frames = 0U;
    core::u64 since_status = 0U;
    core::u64 present_failures = 0U;
    auto last_outcome = show::FrameCommitOutcome::Idle;
    auto closed = false;

    // 3D scene state, used only when the presenter renders scenes. The model is
    // reset when the committed document changes and evaluated every frame, so a
    // playing show re-parses nothing and two nodes evaluate byte-identical
    // scenes from the same show frame.
    scene::SceneModel scene_model;
    core::u64 scene_version = 0U;
    bool scene_loaded = false;
    compositor::CompositorModel compositor_model;
    core::u64 compositor_version = 0U;
    bool compositor_loaded = false;

    // Feed-map state, used only on the output path. Reset on a document change
    // like the scene, and evaluated the same way, so two nodes derive the same
    // map from the same committed revision.
    output::FeedMapModel feed_model;
    core::u64 feed_version = 0U;
    bool feed_loaded = false;

    assets::AssetModel node_asset_model;
    core::u64 node_asset_version = 0U;
    core::u64 node_asset_manifest_digest = 0U;
    bool node_assets_loaded = false;
    PlaybackAssetResolution node_asset_resolution;
    const assets::ContentResolutionRoots node_asset_roots{
        {}, options.asset_store_root};

    // Output-window placement is authored in the replicated Hardware Mapping
    // document. It is evaluated on version changes and only rebuilds the
    // presenter when the effective settings for this exact node index change.
    topology::HardwareMapModel node_hardware_model;
    core::u64 node_hardware_version{};
    bool node_hardware_loaded{};
    topology::HardwareMap node_hardware;
    SystemHardwareProbe local_hardware_probe;
    core::u64 local_inventory_generation{};
    auto next_inventory_check = Clock::time_point::min();

    // The project-wide synchronization policy is replicated document state
    // like everything else, so a node adopts a changed policy at a frame
    // boundary rather than being told out of band.
    sync::SyncPolicyModel node_sync_model;
    core::u64 node_sync_version{};
    bool node_sync_loaded{};
    sync::SyncPolicy node_sync_policy{};
    show::PresenterSyncRequest node_sync_request{};
    bool node_sync_request_pending{true};
    net::RuntimeOutputSync node_output_sync{};
    net::RuntimeOutputSync published_output_sync{};
    // Boundaries where the group late-frame policy repeated the previous
    // complete frame instead of advancing part of a wall.
    core::u64 policy_held_boundaries{};
    core::u64 late_boundaries{};
    core::u64 missed_boundaries{};
    // Transitions out of an aggregate Joined state observed here. Kept apart
    // from the presenter leave counts, which are already cumulative totals:
    // folding the two into one running sum would count each loss again on
    // every refresh.
    core::u64 barrier_loss_transitions{};

    const auto publish_runtime_adoption = [&]() noexcept {
        service.publish_runtime_asset_adoption(net::RuntimeAssetAdoption{
            node_asset_manifest_digest,
            node_asset_runtime_ready && node_deployment_runtime_ready &&
                node_feed_bindings_runtime_ready});
    };

    // The worst state across this process's outputs. A group is only joined
    // when every one of its members is, so any output that is not joined
    // decides the report; the ordering below is by how badly it fails.
    const auto aggregate_barrier =
        [](net::RuntimeBarrierState current,
           show::PresenterBarrierState observed) noexcept {
            const auto mapped = [observed]() noexcept {
                switch (observed) {
                case show::PresenterBarrierState::Unknown:
                    return net::RuntimeBarrierState::Unknown;
                case show::PresenterBarrierState::Unsupported:
                    return net::RuntimeBarrierState::Unsupported;
                case show::PresenterBarrierState::NotJoined:
                    return net::RuntimeBarrierState::NotJoined;
                case show::PresenterBarrierState::Joining:
                    return net::RuntimeBarrierState::Joining;
                case show::PresenterBarrierState::Joined:
                    return net::RuntimeBarrierState::Joined;
                case show::PresenterBarrierState::Left:
                    return net::RuntimeBarrierState::Left;
                case show::PresenterBarrierState::Failed:
                    return net::RuntimeBarrierState::Failed;
                }
                return net::RuntimeBarrierState::Unknown;
            }();
            const auto severity =
                [](net::RuntimeBarrierState state) noexcept -> int {
                switch (state) {
                case net::RuntimeBarrierState::Failed:
                    return 6;
                case net::RuntimeBarrierState::Left:
                    return 5;
                case net::RuntimeBarrierState::Unsupported:
                    return 4;
                case net::RuntimeBarrierState::NotJoined:
                    return 3;
                case net::RuntimeBarrierState::Joining:
                    return 2;
                case net::RuntimeBarrierState::Unknown:
                    return 1;
                case net::RuntimeBarrierState::Joined:
                    return 0;
                }
                return 1;
            };
            return severity(mapped) > severity(current) ? mapped : current;
        };

    const auto refresh_output_sync = [&]() noexcept {
        const auto previous = node_output_sync.barrier;
        net::RuntimeOutputSync sync{};
        sync.barrier = net::RuntimeBarrierState::Joined;
        sync.group = node_sync_policy.group;
        sync.barrier_id = node_sync_policy.barrier;
        sync.expected_outputs =
            static_cast<core::u32>(output_presenters.size());
        if (output_presenters.empty()) {
            sync.barrier = net::RuntimeBarrierState::Unknown;
        }
        for (const NodeOutputPresenter& slot : output_presenters) {
            const show::PresenterSyncReport report =
                slot.presenter->sync_report();
            sync.barrier = aggregate_barrier(sync.barrier, report.barrier);
            if (report.barrier == show::PresenterBarrierState::Joined) {
                ++sync.joined_outputs;
            }
            sync.repeated_frames += report.repeated;
            sync.loss_of_sync_events += report.leave_events;
        }
        sync.presented_frames = presented_frames;
        sync.missed_frames = missed_boundaries;
        sync.late_frames = late_boundaries;
        sync.held_frames = policy_held_boundaries;
        if (previous == net::RuntimeBarrierState::Joined &&
            sync.barrier != net::RuntimeBarrierState::Joined) {
            ++barrier_loss_transitions;
        }
        sync.loss_of_sync_events += barrier_loss_transitions;
        node_output_sync = sync;
    };

    const auto publish_output_sync = [&](bool force) noexcept {
        refresh_output_sync();
        // Publishing takes the session resource lock, so the per-frame path
        // only does it when the state an operator acts on actually changed.
        const bool changed =
            node_output_sync.barrier != published_output_sync.barrier ||
            node_output_sync.group != published_output_sync.group ||
            node_output_sync.barrier_id != published_output_sync.barrier_id ||
            node_output_sync.joined_outputs !=
                published_output_sync.joined_outputs ||
            node_output_sync.expected_outputs !=
                published_output_sync.expected_outputs ||
            node_output_sync.loss_of_sync_events !=
                published_output_sync.loss_of_sync_events;
        if (!force && !changed) {
            return;
        }
        published_output_sync = node_output_sync;
        service.publish_runtime_output_sync(node_output_sync);
    };

    const auto apply_sync_policy = [&]() noexcept {
        node_sync_request = show::PresenterSyncRequest{
            node_sync_policy.requires_physical_sync(), node_sync_policy.group,
            node_sync_policy.barrier};
        for (NodeOutputPresenter& slot : output_presenters) {
            static_cast<void>(slot.presenter->apply_sync(node_sync_request));
        }
        node_sync_request_pending = false;
        publish_output_sync(true);
    };

    while (stop_requested == 0 && !closed) {
        const auto now = Clock::now();
        auto ticked = driver.tick(now);
        if (!ticked) {
            // A stall long enough to invalidate the pacer is a local timing
            // fault, never a reason to present unapproved state. Re-anchor and
            // keep holding whatever the Director last approved.
            if (ticked.error() == show::ShowClockError::TimeOverflow) {
                static_cast<void>(driver.seek(driver.clock().frame(), now));
                continue;
            }
            std::cerr << "node frame pacer refused a time observation (code "
                      << static_cast<unsigned>(ticked.error()) << ")\n";
            return 1;
        }

        const auto report = std::move(ticked).value();
        boundaries += report.boundaries;
        last_outcome = report.last_outcome;
        // The loop wakes more often than the show rate so shutdown stays
        // responsive, so frame accounting only counts on an actual boundary.
        // Counting per iteration would report thousands of missed frames a
        // second on a perfectly healthy node.
        const bool at_boundary = report.boundaries != 0U;
        // More than one boundary crossed by a single wake-up means this
        // process missed a raster deadline it should have hit. That is the
        // one late-frame signal available without a scan-out timestamp, and
        // it is reported as such rather than being folded into "held".
        if (report.boundaries > 1U) {
            late_boundaries += report.boundaries - 1U;
        }

        std::size_t skipped = 0U;
        show::PublishedFrame incoming{};
        if (driver.mailbox().consume_latest(incoming, skipped, &retired)) {
            // The outgoing root is parked rather than released here; the ring
            // is drained below, away from the boundary.
            static_cast<void>(retired.retire(std::move(presented.root)));
            presented = std::move(incoming);
            ++presented_frames;
        }

        if (!node_sync_loaded ||
            presented.version.value() != node_sync_version) {
            node_sync_model.reset(presented.root);
            node_sync_version = presented.version.value();
            node_sync_loaded = true;
            const sync::SyncPolicy updated = node_sync_model.evaluate();
            if (updated != node_sync_policy) {
                node_sync_policy = updated;
                node_sync_request_pending = true;
            }
        }

        const bool hardware_changed = !node_hardware_loaded ||
            presented.version.value() != node_hardware_version;
        if (hardware_changed) {
            node_hardware_model.reset(presented.root);
            node_hardware_version = presented.version.value();
            node_hardware_loaded = true;
            node_hardware = node_hardware_model.evaluate();
        }

        std::shared_ptr<const net::ClusterMachineInfo> local_inventory;
        bool inventory_changed{};
        if (options.output && now >= next_inventory_check) {
            next_inventory_check = now + std::chrono::seconds{1};
            local_inventory = local_hardware_probe.snapshot();
            const core::u64 generation =
                local_inventory != nullptr
                ? local_inventory->sample_generation
                : 0U;
            inventory_changed = generation != local_inventory_generation;
            local_inventory_generation = generation;
        }
        if (hardware_changed || inventory_changed) {
            if (options.output) {
                if (local_inventory == nullptr) {
                    local_inventory = local_hardware_probe.snapshot();
                }
                OutputDeployment wanted = resolve_output_deployment(
                    options, node_hardware, local_inventory.get());
                if (!wanted.ready()) {
                    for (const auto& problem : wanted.issues) {
                        std::cerr << "output deployment for node "
                                  << options.node_index << ": "
                                  << problem.message << '\n';
                    }
                    output_presenters.clear();
                    active_deployment = std::move(wanted);
                    node_deployment_runtime_ready = false;
                    node_feed_bindings_runtime_ready = false;
                    node_sync_request_pending = true;
                    publish_runtime_adoption();
                } else if (wanted != active_deployment) {
                    std::vector<NodeOutputPresenter> replacements;
                    bool created_all = true;
                    try {
                        replacements.reserve(wanted.outputs.size());
                    } catch (const std::bad_alloc&) {
                        created_all = false;
                    }
                    for (const DeployedOutput& deployed : wanted.outputs) {
                        if (!created_all) {
                            break;
                        }
                        const PlaybackOptions applied =
                            presenter_options(options, deployed.window);
                        auto replacement = make_presenter(
                            applied,
                            "media-server node " +
                                std::to_string(options.node_index) +
                                " output " +
                                std::to_string(deployed.logical_port) +
                                " feed " +
                                std::to_string(deployed.feed_index),
                            false, &deployed.window);
                        if (replacement == nullptr) {
                            created_all = false;
                            break;
                        }
                        replacements.push_back(NodeOutputPresenter{
                            deployed, std::move(replacement)});
                    }
                    if (!created_all) {
                        // Never retain an old physical binding after the
                        // project requests another target or mode. A black/
                        // absent output is safer than driving the wrong wall.
                        output_presenters.clear();
                        node_deployment_runtime_ready = false;
                        node_feed_bindings_runtime_ready = false;
                        node_sync_request_pending = true;
                        std::cerr << "could not apply the complete output "
                                     "deployment for node "
                                  << options.node_index << '\n';
                    } else {
                        output_presenters = std::move(replacements);
                        active_deployment = std::move(wanted);
                        node_deployment_runtime_ready = true;
                        // A new swapchain is not a barrier member. Rejoin on
                        // the next tick rather than assuming the replacement
                        // inherited the old window's membership.
                        node_sync_request_pending = true;
                        // Explicit feed references are verified against the
                        // evaluated Feed Map later in this same render tick.
                        node_feed_bindings_runtime_ready =
                            active_deployment.legacy_single_output;
                    }
                    publish_runtime_adoption();
                }
            } else {
                const auto wanted = presenter_window_update(
                    options, node_hardware,
                    node_hardware_model.counters().rejected);
                if (!wanted.has_value()) {
                    std::cerr << "retained output window because authored "
                                 "hardware for node "
                              << options.node_index
                              << " is rejected or ambiguous\n";
                } else if (*wanted != active_presenter_settings) {
                    auto replacement = make_presenter(
                        presenter_options(options, *wanted),
                        "media-server node " +
                            std::to_string(options.node_index),
                        false, &*wanted);
                    if (replacement == nullptr) {
                        std::cerr << "could not apply authored output window "
                                     "configuration for node "
                                  << options.node_index << '\n';
                    } else {
                        output_presenters.front().presenter =
                            std::move(replacement);
                        output_presenters.front().output.window = *wanted;
                        active_presenter_settings = *wanted;
                        node_sync_request_pending = true;
                    }
                }
            }
        }

        const auto transport = presented.transport;
        show::TestPatternInput input{};
        // The position was derived from the anchor at the exact control frame
        // that carried it, so the two can never be a mismatched pair.
        input.show_frame = transport ? presented.show_frame : options.first_frame;
        input.control_frame = presented.control_frame;
        input.rate = transport ? transport->rate : options.rate;
        input.state =
            transport ? transport->state : show::TransportState::Stopped;
        input.node_index = options.node_index;
        input.held = report.held;
        input.needs_resync =
            report.last_outcome == show::FrameCommitOutcome::NeedsResync;
        if (transport && driver.config().clock.rate != transport->rate) {
            if (!driver.reconfigure_rate(transport->rate, now)) {
                std::cerr << "node frame pacer refused the replicated show "
                             "rate\n";
                return 1;
            }
            interval = status_interval(options, transport->rate);
            since_status = 0U;
        }

        // A node draws its slice of the replicated scene through its feed map,
        // or draws the raw slice, or shows the test pattern - decided once at
        // start-up by which renderers the presenter was built with. A node that
        // could not build the path it was asked for never reaches here:
        // make_presenter refuses it. The output path implies the scene path,
        // which is the source it transforms.
        const bool render_output = options.output;
        const bool render_scene = options.scene;
        if ((render_scene || render_output) &&
            (!node_assets_loaded ||
             presented.version.value() != node_asset_version)) {
            node_asset_model.reset(presented.root);
            const assets::AssetRegistry registry =
                node_asset_model.evaluate();
            ProjectAssetResolutionPlan plan =
                prepare_project_asset_resolution(presented.root, registry);
            if (!node_assets_loaded ||
                plan.manifest.digest != node_asset_manifest_digest) {
                node_asset_manifest_digest = plan.manifest.digest;
                static_cast<void>(node_asset_resolution.install(
                    std::move(plan), node_asset_roots, 0U, now));
                // This is deliberately immediate: until an exact completion
                // installs its verified overlay, the running process has not
                // adopted the new manifest bytes.
                node_asset_runtime_ready = false;
                publish_runtime_adoption();
            }
            node_asset_version = presented.version.value();
            node_assets_loaded = true;
        }
        if (render_scene || render_output) {
            if (node_asset_resolution.poll(now)) {
                node_asset_runtime_ready = node_asset_resolution.ready;
                publish_runtime_adoption();
            }
            node_asset_resolution.submit_retry_if_due(now);
        }
        scene::Scene scene_frame;
        scene::Scene transition_from_frame;
        scene::Scene transition_to_frame;
        compositor::CompositorGraph compositor_graph;
        show::SwitcherProgress switcher_progress{};
        output::FeedMap feed_map;
        show::PresenterStatus status{show::PresenterStatus::Skipped};
        // Barrier membership is applied before this boundary presents, so a
        // policy change or a rebuilt swapchain never spends a frame silently
        // outside the group it is supposed to be in.
        if (node_sync_request_pending) {
            apply_sync_policy();
        }
        if (render_scene) {
            if (!scene_loaded || presented.version.value() != scene_version) {
                scene_model.reset(presented.root);
                scene_version = presented.version.value();
                scene_loaded = true;
            }
            if (!compositor_loaded ||
                presented.version.value() != compositor_version) {
                compositor_model.reset(presented.root);
                compositor_version = presented.version.value();
                compositor_loaded = true;
            }
            compositor_graph = compositor_model.evaluate();
            const scene::SceneTime time{
                input.show_frame.value(),
                input.rate.numerator,
                input.rate.denominator};
            const auto switcher = show::read_switcher(presented.root);
            if (switcher.has_value()) {
                switcher_progress = show::derive_switcher_progress(
                    *switcher, presented.control_frame);
                scene_frame =
                    scene_model.evaluate(switcher->program_scene, time);
                if (switcher_progress.active) {
                    transition_from_frame =
                        scene_model.evaluate(switcher_progress.from, time);
                    transition_to_frame =
                        scene_model.evaluate(switcher_progress.to, time);
                    // Older documents may contain a dissolve authored before
                    // canvas compatibility was enforced. Never hand an
                    // impossible pair to Presenter; promote Program as a cut.
                    if (!show::dissolve_canvases_compatible(
                            transition_from_frame.canvas,
                            transition_to_frame.canvas)) {
                        switcher_progress.active = false;
                    }
                }
            } else {
                scene_frame = scene_model.evaluate(time);
            }
            const ResolvedProjectAssets* const node_overlay =
                node_asset_resolution.current_overlay();
            if (node_overlay != nullptr) {
                node_overlay->apply(scene_frame);
            }
            if (switcher_progress.active) {
                if (node_overlay != nullptr) {
                    node_overlay->apply(transition_from_frame);
                    node_overlay->apply(transition_to_frame);
                }
            }
        }

        if (render_output) {
            if (!feed_loaded || presented.version.value() != feed_version) {
                feed_model.reset(presented.root);
                feed_version = presented.version.value();
                feed_loaded = true;
            }
            feed_map = feed_model.evaluate(scene::SceneTime{
                input.show_frame.value(), input.rate.numerator,
                input.rate.denominator});
            if (const ResolvedProjectAssets* const node_overlay =
                    node_asset_resolution.current_overlay()) {
                node_overlay->apply(feed_map);
            }
            const bool feeds_ready = active_deployment.legacy_single_output ||
                std::all_of(
                    active_deployment.outputs.begin(),
                    active_deployment.outputs.end(),
                    [&](const DeployedOutput& deployed) {
                        return feed_map.map_for(deployed.feed_index) != nullptr;
                    });
            if (feeds_ready != node_feed_bindings_runtime_ready) {
                node_feed_bindings_runtime_ready = feeds_ready;
                if (!feeds_ready) {
                    std::cerr << "output deployment for node "
                              << options.node_index
                              << " references a missing Feed Map index\n";
                }
                publish_runtime_adoption();
            }
            // The group late-frame rule. When one output of this process has
            // no complete frame, HOLD GROUP repeats the previous complete
            // frame on every output instead of advancing the rest: on an LED
            // wall one repeated frame everywhere is far less visible than one
            // segment moving while its neighbour stands still.
            //
            // This holds the outputs one render process owns. Holding across
            // machines is what the present barrier and the existing frame
            // barrier are for; neither is replaced by this rule.
            const bool outputs_complete = std::all_of(
                output_presenters.begin(), output_presenters.end(),
                [&](const NodeOutputPresenter& slot) {
                    return feed_map.map_for(slot.output.feed_index) != nullptr;
                });
            const bool hold_group = !outputs_complete &&
                node_sync_policy.late_frame == sync::LateFramePolicy::HoldGroup;
            status = show::PresenterStatus::Skipped;
            if (hold_group) {
                if (at_boundary) {
                    policy_held_boundaries += report.boundaries;
                    missed_boundaries += report.boundaries;
                }
                for (NodeOutputPresenter& slot : output_presenters) {
                    if (slot.presenter->hold() ==
                        show::PresenterStatus::Closed) {
                        status = show::PresenterStatus::Closed;
                        break;
                    }
                }
            } else {
                for (NodeOutputPresenter& slot : output_presenters) {
                    const auto* const map =
                        feed_map.map_for(slot.output.feed_index);
                    show::PresenterStatus slot_status =
                        show::PresenterStatus::Skipped;
                    if (map != nullptr && switcher_progress.active) {
                        slot_status = slot.presenter->present(
                            transition_from_frame, transition_to_frame,
                            static_cast<float>(switcher_progress.mix()),
                            compositor_graph, *map);
                    } else if (map != nullptr) {
                        slot_status = slot.presenter->present(
                            scene_frame, compositor_graph, *map);
                    }
                    if (slot_status == show::PresenterStatus::Closed) {
                        status = slot_status;
                        break;
                    }
                    if (slot_status == show::PresenterStatus::Failed ||
                        (slot_status == show::PresenterStatus::Presented &&
                         status == show::PresenterStatus::Skipped)) {
                        status = slot_status;
                    }
                }
            }
        } else if (render_scene) {
            // The node's own rectangle of the canvas, from the document. A node
            // the feed map does not bind draws the whole canvas rather than
            // guessing.
            const auto* const bound = scene_frame.slice_for(options.node_index);
            const auto slice =
                bound != nullptr ? *bound : scene::whole(scene_frame.canvas);
            show::Presenter& scene_presenter =
                *output_presenters.front().presenter;
            status = switcher_progress.active
                ? scene_presenter.present(
                      transition_from_frame, transition_to_frame,
                      static_cast<float>(switcher_progress.mix()),
                      compositor_graph, slice)
                : scene_presenter.present(
                      scene_frame, compositor_graph, slice);
        } else {
            status = output_presenters.front().presenter->present(input);
        }

        switch (status) {
        case show::PresenterStatus::Closed:
            node_deployment_runtime_ready = false;
            publish_runtime_adoption();
            closed = true;
            break;
        case show::PresenterStatus::Failed:
            ++present_failures;
            if (at_boundary) {
                missed_boundaries += report.boundaries;
            }
            break;
        case show::PresenterStatus::Presented:
        case show::PresenterStatus::Skipped:
            break;
        }

        // Only on a real boundary: a vendor barrier query is a driver call,
        // and this loop wakes far more often than the show rate. Only a
        // changed barrier state then takes the session lock; the counters
        // ride along at the status cadence below.
        if (at_boundary) {
            publish_output_sync(false);
        }

        since_status += report.boundaries;
        if (since_status >= interval) {
            since_status = 0U;
            publish_output_sync(true);
            std::cout << "node " << options.node_index
                      << " show-frame=" << input.show_frame.value()
                      << " control-frame=" << presented.control_frame.value()
                      << " version=" << presented.version.value()
                      << " transport=" << transport_label(input.state)
                      << " state=" << outcome_label(report.last_outcome)
                      << " boundaries=" << boundaries
                      << " presented=" << presented_frames
                      << " held=" << driver.counters().held_boundaries
                      << " skipped=" << skipped
                      << " overflows=" << driver.counters().mailbox_overflows
                      // The scene digest compares the replicated state itself,
                      // not a rendering of it, so it agrees across nodes drawing
                      // different slices - which the frame-buffer digest cannot.
                      << " digest="
                      << (render_scene
                              ? scene::digest(scene_frame)
                              : show::frame_buffer_digest(
                                    output_presenters.front()
                                        .presenter->surface()));
            // The feed-map digest agrees across nodes for the same reason the
            // scene one does: it is the replicated parameters, not a rendering.
            if (render_output) {
                std::cout << " feed=" << output::digest(feed_map);
            }
            if (!output_presenters.empty()) {
                std::cout << gpu_timing_text(
                    output_presenters.front().presenter->timing());
            }
            std::cout << " sync=" << sync_mode_label(node_sync_policy.mode)
                      << " barrier="
                      << runtime_barrier_label(node_output_sync.barrier) << '/'
                      << node_output_sync.joined_outputs << 'x'
                      << node_output_sync.expected_outputs
                      << " missed=" << node_output_sync.missed_frames
                      << " late=" << node_output_sync.late_frames
                      << " policy-held=" << node_output_sync.held_frames
                      << " repeated=" << node_output_sync.repeated_frames
                      << " sync-loss=" << node_output_sync.loss_of_sync_events;
            std::cout << '\n'
                      << std::flush;
        }

        // Off-boundary housekeeping: this is the only place a superseded trie
        // is actually released.
        static_cast<void>(retired.drain());

        if (options.run_frames != 0U && boundaries >= options.run_frames) {
            break;
        }
        sleep_until_boundary(driver.clock().next_boundary_at());
    }

    std::cout << "node " << options.node_index << " stopping"
              << " show-frame="
              << (presented.transport ? presented.show_frame.value()
                                      : options.first_frame.value())
              << " control-frame=" << presented.control_frame.value()
              << " version=" << presented.version.value()
              << " boundaries=" << boundaries
              << " presented=" << presented_frames
              << " committed=" << driver.counters().committed_frames
              << " held=" << driver.counters().held_boundaries
              << " checkpoints=" << driver.counters().checkpoint_activations
              << " present-failures=" << present_failures
              << " outputs=" << output_presenters.size();
    if (!output_presenters.empty()) {
        std::cout << " window="
                  << (output_presenters.front().presenter->windowed()
                          ? "closed" : "headless")
                  << gpu_timing_text(
                         output_presenters.front().presenter->timing());
    }
    std::cout
              << " last=" << outcome_label(last_outcome) << '\n'
              << std::flush;
    return 0;
}

int run_director_show(
    cmd::DirectorScheduler& scheduler,
    persist::CommandLog& command_log,
    persist::FrameDurability frame_durability,
    net::DirectorSessionHub& hub,
    net::DirectorFrameBarrier& barrier,
    const net::DirectorSessionService* session_service,
    const PlaybackOptions& options,
    const volatile std::sig_atomic_t& stop_requested) {
    // Recovery affects whether either build may author demo fallback content;
    // it is not a GUI-only project-loading concern.
    const bool recovered_session =
        command_log.recovery_report().entry_count != 0U;
#if !defined(MEDIASERVER_HAS_GUI)
    // Project loading is a GUI-only authoring path, but keeping these seams in
    // the common entry point lets both builds use the same durable coordinator.
    static_cast<void>(frame_durability);
#endif
    // The editor replaces the preview presenter when asked for: it is the
    // operator's surface, not an output window. Everything else - the seal loop,
    // the transport, replication - is identical either way.
    std::unique_ptr<show::Presenter> presenter;
    std::string clip_source;
#if defined(MEDIASERVER_HAS_GUI)
    std::unique_ptr<editor::EditorShell> editor;
    std::unique_ptr<ClusterLifecycleController> lifecycle_controller;
    if (options.edit) {
        editor::EditorConfig editor_config{};
        editor_config.title = "media-server editor";
        // Honour an explicit --surface, but keep the editor usably large when
        // the caller left the small test-pattern default in place.
        editor_config.width =
            options.surface_width >= 1024U ? options.surface_width : 1400U;
        editor_config.height =
            options.surface_height >= 640U ? options.surface_height : 900U;
        editor_config.position_x = options.window_x;
        editor_config.position_y = options.window_y;
        auto created_editor = editor::EditorShell::create(editor_config);
        if (!created_editor) {
            std::cerr << "could not open the operator editor window\n";
            return 1;
        }
        editor = std::move(created_editor).value();
        try {
            ClusterLifecycleControllerConfig lifecycle_config{};
            lifecycle_config.shared_secret = options.lifecycle_secret;
            lifecycle_controller =
                std::make_unique<ClusterLifecycleController>(
                    std::move(lifecycle_config));
        } catch (const std::bad_alloc&) {
            std::cerr << "could not initialize cluster lifecycle controller: "
                         "allocation failed\n";
            return 1;
        } catch (const std::system_error& error) {
            std::cerr << "could not initialize cluster lifecycle controller: "
                      << error.what() << '\n';
            return 1;
        }
        clip_source = write_demo_clip();
    }
#endif
    if (!options.edit) {
        presenter = make_presenter(options, "media-server director", true);
        if (presenter == nullptr) {
            return 1;
        }
    }

    const auto start = Clock::now();

    // One clock, and it is a pacer. It free-runs regardless of transport state
    // and decides only when the next control frame is due, so pausing never
    // stops the frame stream that carries transport changes and keeps the
    // cohort acknowledging.
    //
    // Where the show is, is not a clock reading at all. It is the replicated
    // anchor evaluated at the control-frame ordinal, which is what makes the
    // Director and every node compute the same position from the same state.
    auto paced = show::ShowClock::create(
        show::ShowClockConfig{
            options.rate,
            show::ShowFrame{0U},
            show::ShowFrame{std::numeric_limits<core::u64>::max() - 1U},
            false},
        start);
    auto created = show::TransportAuthority::create(
        clock_config(options), scheduler.sealed_through());
    if (!paced || !created) {
        std::cerr << "invalid Director show clock configuration\n";
        return 1;
    }
    auto pacer = std::move(paced).value();
    auto transport = std::move(created).value();
    bool restored_recovered_transport = false;

    if (!pacer.play(start)) {
        std::cerr << "could not start the Director frame pacer\n";
        return 1;
    }
#if defined(MEDIASERVER_HAS_GUI)
    if (options.edit && recovered_session) {
        const doc::Node* const raw_transport =
            scheduler.projected_document().get(show::transport_node_id);
        const auto recovered_transport =
            show::read_transport(scheduler.projected_document());
        if (raw_transport != nullptr &&
            std::string_view{raw_transport->type} == show::transport_node_type &&
            !recovered_transport.has_value()) {
            std::cerr << "recovered project transport state is invalid\n";
            return 1;
        }
        if (recovered_transport.has_value()) {
            const cmd::FrameNumber through = scheduler.sealed_through();
            if (!restore_project_transport(transport, *recovered_transport,
                                           through) ||
                !pacer.seek(show::ShowFrame{through.value()}, start) ||
                !pacer.reconfigure_rate(recovered_transport->rate, start)) {
                std::cerr << "could not restore recovered project timing\n";
                return 1;
            }
            restored_recovered_transport = true;
        }
    }
#endif

    // The built-in scene and feed map are authored once, as ordinary
    // node-creation commands on the first frame this Director will seal. From
    // there they are document state like any other: they replicate to the
    // cohort, and a playing show adds no further edits because every motion is a
    // rate the nodes integrate against the show frame themselves, and the feed
    // map does not move at all.
    if (options.scene || options.output || options.edit) {
        const cmd::FrameNumber first{scheduler.sealed_through().value() + 1U};
        if (options.scene && !author_scene_document(scheduler, first)) {
            std::cerr << "could not author the built-in scene document\n";
            return 1;
        }
        const InitialFeedMapPlan feed_map_plan =
            initial_feed_map_plan(options, recovered_session);
        if (feed_map_plan.kind != InitialFeedMapKind::None &&
            !author_feed_map_document(scheduler, first, feed_map_plan)) {
            std::cerr << "could not author the built-in feed map document\n";
            return 1;
        }
#if defined(MEDIASERVER_HAS_GUI)
        if (options.edit && !recovered_session &&
            !author_edit_document(scheduler, first, clip_source)) {
            std::cerr << "could not author the edit scene document\n";
            return 1;
        }
#endif
    }

    std::deque<net::DirectorFrameBarrierId> tracked;
    std::vector<show::PresenterCommand> operator_commands;
#if defined(MEDIASERVER_HAS_GUI)
    // Operator moves from the editor, coalesced to the latest per node, authored
    // at the next frame boundary like the transport commands.
    std::vector<editor::EditIntent> pending_edits;
    // A pending scene-edit undo (its transaction id, or 0 for none), authored at
    // the boundary after the edits so it reverses whatever the edits just left on
    // the stack.
    core::u64 pending_undo{};
    // A pending timeline scrub: the show frame the operator dragged the playhead
    // to, applied to the transport (paused) at the boundary.
    std::optional<core::u64> pending_seek;
    // A pending show range/loop change from the timeline.
    std::optional<editor::RangeRequest> pending_range;
    // A pending exact project cadence. The transport anchor and the Director
    // pacer change on the same sealed boundary.
    std::optional<editor::ShowRateRequest> pending_show_rate;
    // Pending keyframe requests, authored as scene.channel edits at the boundary.
    std::vector<editor::KeyIntent> pending_keys;
    // Pending keyframe retimes (a diamond dragged to a new time).
    std::vector<editor::KeyMove> pending_key_moves;
    // Pending Inspector edits, authored as base-property edits at the boundary.
    std::vector<editor::PropertyEdit> pending_properties;
    // Pending Inspector edits of named properties (light fields).
    std::vector<editor::GenericEdit> pending_generics;
    // Pending element creations and deletions from the editor's Add/Edit menus.
    std::vector<editor::AddIntent> pending_adds;
    std::vector<editor::AssetImportIntent> pending_asset_imports;
    std::vector<editor::AssetDeleteIntent> pending_asset_deletes;
    std::vector<editor::EffectIntent> pending_effects;
    std::vector<editor::EffectDeleteIntent> pending_effect_deletes;
    std::optional<editor::SwitcherRequest> pending_switcher;
    std::vector<editor::HardwarePlaceRequest> pending_hardware_places;
    std::vector<editor::HardwareMoveRequest> pending_hardware_moves;
    std::vector<editor::HardwareLinkRequest> pending_hardware_links;
    std::vector<editor::HardwarePropertyEdit> pending_hardware_properties;
    std::optional<editor::HardwareControlPortEdit>
        pending_hardware_control_port;
    std::optional<editor::HardwareSyncPolicyEdit> pending_sync_policy;
    std::vector<editor::HardwareOutputBindingEdit>
        pending_hardware_output_binding_edits;
    std::vector<editor::HardwareOutputBindingDelete>
        pending_hardware_output_binding_deletes;
    std::vector<editor::HardwareDeleteRequest> pending_hardware_deletes;
    std::vector<editor::FeedMapAddRequest> pending_feed_map_adds;
    std::vector<editor::FeedMapRectEdit> pending_feed_map_edits;
    std::vector<editor::FeedMapDeleteRequest> pending_feed_map_deletes;
    std::vector<editor::FeedProjectionEdit> pending_feed_projection_edits;
    std::vector<editor::FeedSurfaceAddRequest> pending_feed_surface_adds;
    std::vector<editor::FeedSurfaceEditRequest> pending_feed_surface_edits;
    std::vector<editor::FeedSurfaceDeleteRequest> pending_feed_surface_deletes;
    std::optional<editor::CompositorAddIntent> pending_compositor_add;
    std::optional<editor::CompositorMoveIntent> pending_compositor_move;
    std::optional<editor::CompositorConnectIntent>
        pending_compositor_connect;
    std::optional<editor::CompositorPropertyEditIntent>
        pending_compositor_property;
    std::optional<editor::CompositorDeleteIntent>
        pending_compositor_delete;
    core::u64 pending_compositor_gesture{};
    std::vector<doc::NodeId> pending_deletes;
    core::u64 pending_delete_gesture{};
    std::optional<editor::ProjectFileRequest> pending_project_save;
    std::optional<editor::ProjectFileRequest> pending_project_load;
    // Startup validates the recent project before the frame loop, then hands
    // the already-decoded root to the ordinary frame-boundary staging path.
    // Manual Open requests leave this empty and continue to read at the
    // boundary, so both paths share transport/switcher/WAL validation.
    std::optional<doc::Root> pending_preloaded_project;
    std::optional<std::filesystem::path> recent_project_settings_file;
    std::string project_path;
    std::string project_message = recovered_session
        ? std::string{"RECOVERED UNSAVED CHANGES"}
        : std::string{};
    doc::Root saved_project_snapshot = scheduler.projected_document();
    // Without a verified saved-file baseline, a recovered WAL is an unsaved
    // untitled session.
    bool project_explicitly_dirty{recovered_session};
    core::u64 project_generation{};
    constexpr std::size_t max_pending_asset_imports = 16U;
    std::unique_ptr<ProjectAssetImportWorker> asset_import_worker;
    try {
        if (options.edit) {
            asset_import_worker = std::make_unique<ProjectAssetImportWorker>(
                options.asset_store_root, max_pending_asset_imports);
        }
    } catch (const std::bad_alloc&) {
        project_message = "ASSET IMPORTS UNAVAILABLE - NOT ENOUGH MEMORY";
        std::cerr << "could not initialize the asset import worker: "
                     "allocation failed\n";
    } catch (const std::system_error& error) {
        project_message = "ASSET IMPORTS UNAVAILABLE";
        std::cerr << "could not initialize the asset import worker: "
                  << error.what() << '\n';
    }
    std::vector<PendingAssetImportAuthoring> asset_import_authoring;
    core::u64 next_asset_import_token{1U};
    const auto report_recent_project_error = [](
        std::string_view operation,
        const persist::RecentProjectError& error) {
        std::cerr << operation << " recent-project setting (code "
                  << static_cast<unsigned>(error.code) << ')';
        if (error.system_error) {
            std::cerr << ": " << error.system_error.message();
        }
        std::cerr << '\n';
    };
    // A recovered Director WAL can contain edits made after the last explicit
    // project save. It is the newer, lossless source of truth, so never replace
    // it merely because a recent-project preference exists. The default WAL
    // may recover its path only when the saved content is identical; an
    // explicit WAL never borrows the global recent path. A fresh log may safely
    // stage the remembered project.
    if (options.edit) {
        const auto settings_file =
            persist::default_recent_project_settings_file();
        if (!settings_file) {
            report_recent_project_error(
                "could not locate", settings_file.error());
        } else {
            recent_project_settings_file = settings_file.value();
            auto recent = persist::open_recent_project(
                *recent_project_settings_file);
            if (!recent) {
                if (recent.error().settings_error.has_value()) {
                    report_recent_project_error(
                        "could not read", *recent.error().settings_error);
                } else if (recent.error().project_error.has_value()) {
                    const auto& error = *recent.error().project_error;
                    std::cerr << "recent project is unavailable or invalid "
                                 "(code "
                              << static_cast<unsigned>(error.code) << ')';
                    if (error.system_error) {
                        std::cerr << ": " << error.system_error.message();
                    }
                    std::cerr << '\n';
                }
            } else if (recent.value().has_value()) {
                auto candidate = std::move(*recent.value());
                if (!recovered_session) {
                    pending_project_load = editor::ProjectFileRequest{
                        native_path_to_utf8(candidate.path)};
                    pending_preloaded_project =
                        std::move(candidate.project);
                    project_message = "REOPENING PROJECT...";
                } else if (
                    options.associate_recent_project_after_recovery &&
                    recovered_project_matches_saved_baseline(
                        candidate.project, scheduler.projected_document())) {
                    // The recovered document agrees with the complete saved
                    // project content (apart from transient transport state),
                    // so this is a safe baseline and ordinary Ctrl+S target.
                    project_path = native_path_to_utf8(candidate.path);
                    saved_project_snapshot = std::move(candidate.project);
                    project_explicitly_dirty = false;
                    project_message = "RECOVERED PROJECT";
                    std::cerr << "recovered Director session matched the "
                                 "recent saved-project baseline\n";
                } else {
                    // A global recent pointer is not proof that a recovered WAL
                    // belongs to that file: the last recent-setting update may
                    // have failed after a successful save of another project.
                    // Keep recovery untitled and dirty so Ctrl+S cannot
                    // overwrite an unrelated project.
                    std::cerr << "recovered WAL left untitled; recent-project "
                                 "content/ownership was not verified\n";
                }
            }
        }
    }
    if (options.edit && recovered_session) {
        try {
            MediaRateEnrichment enrichment = enrich_media_frame_rates(
                scheduler.projected_document());
            if (enrichment.updated_nodes != 0U) {
                const cmd::FrameNumber at{
                    scheduler.sealed_through().value() + 1U};
                auto scheduled = scheduler.schedule_document_replace(
                    enrichment.project, cmd::UndoContextId{7U},
                    cmd::TransactionId{at.value()}, at);
                if (scheduled) {
                    project_explicitly_dirty = true;
                    project_message = "RECOVERED PROJECT - MEDIA FPS UPDATED (" +
                        std::to_string(enrichment.updated_nodes) + ')';
                    std::cerr << "enriched " << enrichment.updated_nodes
                              << " recovered media node(s) with native FPS; "
                                 "save the project to persist metadata\n";
                } else {
                    std::cerr << "could not schedule recovered media FPS "
                                 "metadata enrichment\n";
                }
            }
        } catch (const std::bad_alloc&) {
            std::cerr << "could not enrich recovered media FPS metadata: "
                         "allocation failed\n";
        } catch (const std::length_error&) {
            std::cerr << "could not enrich recovered media FPS metadata: "
                         "project limits exceeded\n";
        }
    }
    const auto remember_recent_project = [
        &recent_project_settings_file,
        &report_recent_project_error](std::string_view path) {
        if (!recent_project_settings_file.has_value()) {
            return;
        }
        auto remembered = persist::save_recent_project_path(
            *recent_project_settings_file,
            native_path_from_utf8(path));
        if (!remembered && !remembered.error().committed) {
            report_recent_project_error(
                "could not update", remembered.error());
        } else if (!remembered) {
            report_recent_project_error(
                "updated without confirmed durability",
                remembered.error());
        }
    };
    // The next id to try for a created element; advanced past any occupied id so
    // an add never collides with an existing node (content, channel, or demo).
    core::u64 next_new_node{100'000U};
    const auto clear_pending_project_authoring = [&]() {
        pending_edits.clear();
        pending_undo = 0U;
        pending_seek.reset();
        pending_range.reset();
        pending_show_rate.reset();
        pending_keys.clear();
        pending_key_moves.clear();
        pending_properties.clear();
        pending_generics.clear();
        pending_adds.clear();
        pending_asset_imports.clear();
        pending_asset_deletes.clear();
        pending_effects.clear();
        pending_effect_deletes.clear();
        pending_switcher.reset();
        pending_hardware_places.clear();
        pending_hardware_moves.clear();
        pending_hardware_links.clear();
        pending_hardware_properties.clear();
        pending_hardware_control_port.reset();
        pending_sync_policy.reset();
        pending_hardware_output_binding_edits.clear();
        pending_hardware_output_binding_deletes.clear();
        pending_hardware_deletes.clear();
        pending_feed_map_adds.clear();
        pending_feed_map_edits.clear();
        pending_feed_map_deletes.clear();
        pending_feed_projection_edits.clear();
        pending_feed_surface_adds.clear();
        pending_feed_surface_edits.clear();
        pending_feed_surface_deletes.clear();
        pending_compositor_add.reset();
        pending_compositor_move.reset();
        pending_compositor_connect.reset();
        pending_compositor_property.reset();
        pending_compositor_delete.reset();
        pending_compositor_gesture = 0U;
        pending_deletes.clear();
        pending_delete_gesture = 0U;
        // Hash/copy jobs already running cannot safely be interrupted inside
        // filesystem APIs. Their document-side companions are discarded now;
        // generation metadata makes the eventual completions harmless.
        if (asset_import_worker != nullptr) {
            asset_import_worker->discard_waiting();
        }
        asset_import_authoring.clear();
    };
#endif
    bool rehearsal_mode{};
    std::optional<show::TransportAnchor> authored;
    auto interval = status_interval(options, transport.anchor().rate);
    core::u64 sealed = 0U;
    core::u64 since_status = 0U;
    core::u64 anchors = 0U;
    core::u64 present_failures = 0U;
    core::u32 configured_cluster_nodes = 0U;
    bool cluster_configuration_authored = false;
    ClusterDiagnosticLog cluster_diagnostics{
        cluster_diagnostics_path(command_log.path())};
    if (!cluster_diagnostics.available()) {
        std::cerr << "cluster diagnostics unavailable at "
                  << cluster_diagnostics.path().string() << '\n';
    }
    // The latest evaluated physical-timing picture. It is refreshed once per
    // Director tick and read by both the operator surface and the diagnostic
    // stream, so those two can never disagree about what was proven.
    struct DirectorSyncView final {
        sync::SyncPolicy policy{};
        sync::SyncTopology topology{};
        sync::SyncStatusSnapshot status{};
        sync::SyncReadiness readiness{};
    } director_sync;
    sync::SyncPolicyModel director_sync_model;
    core::u64 director_sync_version{};
    bool director_sync_loaded{};

    // One place where every physical-timing condition is combined, so the
    // Program gate, the operator surface, and the diagnostic stream can never
    // disagree about why an output is or is not ready.
    const auto evaluate_sync_gate = [&](bool assets_adopted) {
        sync::SyncReadinessInput gate{};
        gate.policy = &director_sync.policy;
        gate.topology = &director_sync.topology;
        gate.nodes = director_sync.status.nodes;
        gate.exact_show_frame_ready = true;
        gate.assets_adopted = assets_adopted;
        gate.render_nodes_live = std::all_of(
            director_sync.topology.node_indices.begin(),
            director_sync.topology.node_indices.end(),
            [&](core::u32 node_index) {
                const sync::NodeSyncStatus* const node =
                    director_sync.status.node(node_index);
                return node != nullptr && node->runtime_reported;
            });
        return sync::evaluate_sync_readiness(gate);
    };

    const auto observe_cluster = [&](core::u64 show_frame) {
        ClusterObservation observation{};
        const auto barrier_diagnostics = barrier.observer_diagnostics();
        observation.unix_time_ms = static_cast<core::u64>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        observation.control_frame = scheduler.sealed_through().value();
        observation.show_frame = show_frame;
        const std::size_t observed_sessions =
            barrier_diagnostics.live_sessions +
            barrier_diagnostics.catching_up_sessions;
        const core::u32 observed_node_count = static_cast<core::u32>(
            std::min<std::size_t>(observed_sessions,
                                  std::numeric_limits<core::u32>::max()));
        if (!cluster_configuration_authored) {
            configured_cluster_nodes =
                std::max(configured_cluster_nodes, observed_node_count);
        }
        observation.configured_nodes = configured_cluster_nodes;
        observation.live_sessions = static_cast<core::u32>(
            std::min<std::size_t>(barrier_diagnostics.live_sessions,
                                  std::numeric_limits<core::u32>::max()));
        observation.catching_up_sessions = static_cast<core::u32>(
            std::min<std::size_t>(barrier_diagnostics.catching_up_sessions,
                                  std::numeric_limits<core::u32>::max()));
        const core::u64 released =
            barrier.released_through().through_frame.value();
        observation.release_lag_frames =
            observation.control_frame > released
                ? observation.control_frame - released
                : 0U;
        observation.pacer_lag_frames =
            pacer_lag(pacer, observation.control_frame);
        observation.open_barriers = static_cast<core::u64>(
            barrier.open_frames());
        observation.present_failures = present_failures;
        if (session_service != nullptr) {
            observation.sessions = session_service->counters();
        }
        observation.barrier = barrier_diagnostics;
        observation.diagnostics_recording = cluster_diagnostics.available();
        observation.sync_mode =
            std::string{sync::sync_mode_name(director_sync.policy.mode)};
        observation.sync_state =
            std::string{director_sync.readiness.physical_headline()};
        observation.program_state =
            std::string{director_sync.readiness.headline()};
        observation.sync_required =
            director_sync.policy.requires_physical_sync();
        observation.sync_satisfied = director_sync.readiness.satisfied;
        observation.sync_degraded = director_sync.readiness.degraded;
        observation.sync_groups = director_sync.readiness.groups;
        observation.sync_expected_outputs =
            director_sync.readiness.expected_outputs;
        observation.sync_locked_outputs =
            director_sync.readiness.locked_outputs;
        observation.sync_barrier_joined_outputs =
            director_sync.readiness.barrier_joined_outputs;
        observation.sync_configuration_issues = static_cast<core::u32>(
            director_sync.topology.diagnostics.size());
        for (const sync::NodeSyncStatus& node : director_sync.status.nodes) {
            observation.sync_simulated =
                observation.sync_simulated || node.simulated;
            observation.sync_missed_frames += node.counters.missed;
            observation.sync_late_frames += node.counters.late;
            observation.sync_held_frames += node.counters.held;
            observation.sync_repeated_frames += node.counters.repeated;
            observation.sync_loss_events += node.counters.loss_of_sync_events;
        }
        // Physical timing is proven, never assumed. Only a non-simulated
        // verdict that satisfied a policy demanding it may set these.
        observation.hardware_frame_lock_verified =
            director_sync.readiness.verified;
        observation.external_time_sync_verified =
            director_sync.readiness.verified &&
            director_sync.policy.requires_external_reference();
        observation.health = cluster_control_health(observation);
        return observation;
    };
    auto autoplay_pending = options.autoplay && !restored_recovered_transport;
    auto closed = false;

    // The Director previews the whole canvas of the same replicated scene. It
    // evaluates from its own committed document, so it needs no mailbox; the
    // model is reset when that document's version changes.
    scene::SceneModel scene_model;
    core::u64 scene_version = 0U;
    bool scene_loaded = false;

    // The feed map, evaluated only to report its digest: the Director previews
    // the whole canvas, not a node's slice, so it does not transform. Reported
    // so an operator can confirm the Director and every node agree on the map.
    output::FeedMapModel feed_model;
    core::u64 feed_version = 0U;
    bool feed_loaded = false;

    topology::HardwareMapModel hardware_model;
    core::u64 hardware_version = 0U;
    bool hardware_loaded = false;

    assets::AssetModel asset_model;
    core::u64 asset_version = 0U;
    core::u64 asset_manifest_digest = 0U;
    bool assets_loaded = false;
    PlaybackAssetResolution director_asset_resolution;
    // Narrow local seam for Program gating. This names the exact manifest
    // whose verified overlay is installed in this Director process.
    bool director_asset_runtime_ready{};

    compositor::CompositorModel compositor_model;
    core::u64 compositor_version = 0U;
    bool compositor_loaded = false;

    while (stop_requested == 0 && !closed) {
        const auto now = Clock::now();

        auto paced_tick = pacer.advance_to(now);
        if (!paced_tick) {
            if (paced_tick.error() == show::ShowClockError::TimeOverflow) {
                static_cast<void>(pacer.seek(pacer.frame(), now));
                continue;
            }
            std::cerr << "Director frame pacer refused a time observation\n";
            return 1;
        }

        const auto due = std::min(
            std::move(paced_tick).value().advanced, max_director_catch_up);
        core::u64 sealed_this_iteration = 0U;
        std::optional<show::FrameRate> repace_after_seal;
        for (core::u64 index = 0U; index < due; ++index) {
            if (tracked.size() >= max_tracked_frames) {
                std::cerr << "Director cohort backlog reached " << tracked.size()
                          << " unretired frames; refusing to seal further\n";
                return 1;
            }

            const auto through = scheduler.sealed_through().value();
            if (through == std::numeric_limits<core::u64>::max()) {
                std::cerr << "Director control frame numbering is exhausted\n";
                return 1;
            }
            const cmd::FrameNumber frame{through + 1U};

            // Operator input is applied to the first frame of this iteration,
            // because an anchor has to name the exact control frame it takes
            // effect on.
            if (index == 0U) {
                if (autoplay_pending) {
                    autoplay_pending = false;
                    if (!transport.play(frame)) {
                        std::cerr << "could not start the Director transport\n";
                        return 1;
                    }
                }
                bool project_loaded_this_frame = false;
#if defined(MEDIASERVER_HAS_GUI)
                if (pending_project_load.has_value()) {
                    const editor::ProjectFileRequest request =
                        std::move(*pending_project_load);
                    pending_project_load.reset();

                    auto loaded = pending_preloaded_project.has_value()
                        ? persist::ProjectLoadResult::success(
                              std::move(*pending_preloaded_project))
                        : persist::load_project_file(
                              native_path_from_utf8(request.path));
                    pending_preloaded_project.reset();
                    if (!loaded) {
                        project_message =
                            project_file_failure("LOADING", loaded.error());
                        std::cerr << "could not load project '" << request.path
                                  << "' (code "
                                  << static_cast<unsigned>(loaded.error().code)
                                  << ')';
                        if (loaded.error().system_error) {
                            std::cerr << ": "
                                      << loaded.error().system_error.message();
                        }
                        std::cerr << '\n';
                    } else {
                        doc::Root replacement = std::move(loaded).value();
                        std::size_t enriched_media_nodes{};
                        try {
                            MediaRateEnrichment enrichment =
                                enrich_media_frame_rates(replacement);
                            replacement = std::move(enrichment.project);
                            enriched_media_nodes = enrichment.updated_nodes;
                        } catch (const std::bad_alloc&) {
                            std::cerr
                                << "could not enrich loaded media FPS metadata: "
                                   "allocation failed\n";
                        } catch (const std::length_error&) {
                            std::cerr
                                << "could not enrich loaded media FPS metadata: "
                                   "project limits exceeded\n";
                        }
                        show::TransportAuthority replacement_transport =
                            transport;
                        const doc::Node* const raw_transport =
                            replacement.get(show::transport_node_id);
                        const auto loaded_transport =
                            show::read_transport(replacement);
                        const bool malformed_transport =
                            raw_transport != nullptr &&
                            std::string_view{raw_transport->type} ==
                                show::transport_node_type &&
                            !loaded_transport.has_value();
                        bool transport_valid = !malformed_transport;
                        if (transport_valid && loaded_transport.has_value()) {
                            transport_valid = restore_project_transport(
                                replacement_transport, *loaded_transport,
                                frame);
                        } else if (transport_valid) {
                            // A project predating persisted transport settings
                            // gets the launch/default cadence (50 fps unless an
                            // explicit --show-rate was supplied), never the
                            // settings of whichever project happened to be open.
                            auto legacy_transport =
                                show::TransportAuthority::create(
                                    clock_config(options), frame);
                            if (legacy_transport) {
                                replacement_transport =
                                    std::move(legacy_transport).value();
                            } else {
                                transport_valid = false;
                            }
                        }
                        if (!transport_valid) {
                            project_message = "ERROR LOADING PROJECT TRANSPORT";
                            std::cerr << "project transport state is invalid\n";
                        } else {
                            // Settle before staging so the preflight batch is
                            // still exact when the ordinary end-of-frame
                            // transport settlement runs below.
                            static_cast<void>(
                                replacement_transport.settle(frame));
                            // Re-anchor the saved playhead to this Director's
                            // current control frame. This preserves range,
                            // direction, state, and position without importing a
                            // stale control-frame ordinal from another run.
                            replacement = replacement.set(
                                show::make_transport_node(
                                    replacement_transport.anchor()));
                            if (!normalize_project_switcher(
                                    replacement, frame)) {
                                project_message =
                                    "ERROR LOADING PROJECT SWITCHER";
                                std::cerr << "project switcher state is invalid\n";
                            } else {
                                auto staged = stage_project_load(
                                    scheduler, replacement, frame, command_log,
                                    frame_durability);
                                if (!staged) {
                                    const auto& error = staged.error();
                                    switch (error.code) {
                                    case ProjectLoadStageErrorCode::ScheduleFailed:
                                        project_message =
                                            "ERROR LOADING PROJECT: SCHEDULER REJECTED REPLACEMENT";
                                        break;
                                    case ProjectLoadStageErrorCode::PrepareFailed:
                                        project_message =
                                            "ERROR LOADING PROJECT: FRAME COULD NOT BE PREPARED";
                                        break;
                                    case ProjectLoadStageErrorCode::WirePreflightFailed:
                                        project_message =
                                            error.command_log_error.has_value() &&
                                                command_log_limit_error(
                                                    *error.command_log_error)
                                            ? "ERROR LOADING PROJECT: PROJECT EXCEEDS FRAME TRANSFER LIMITS"
                                            : "ERROR LOADING PROJECT: FRAME TRANSFER PREFLIGHT REJECTED";
                                        break;
                                    case ProjectLoadStageErrorCode::WalPreflightFailed:
                                        project_message =
                                            error.command_log_error.has_value() &&
                                                command_log_limit_error(
                                                    *error.command_log_error)
                                            ? "ERROR LOADING PROJECT: PROJECT EXCEEDS WAL LIMITS"
                                            : "ERROR LOADING PROJECT: WAL PREFLIGHT REJECTED";
                                        break;
                                    case ProjectLoadStageErrorCode::AbortFailed:
                                        project_message =
                                            "ERROR LOADING PROJECT: FRAME PREFLIGHT FAILED";
                                        break;
                                    case ProjectLoadStageErrorCode::AllocationFailed:
                                        project_message =
                                            "ERROR LOADING PROJECT: NOT ENOUGH MEMORY";
                                        break;
                                    }
                                    std::cerr << "could not stage loaded project for frame "
                                              << frame.value() << " (code "
                                              << static_cast<unsigned>(error.code)
                                              << ")\n";
                                } else {
                                    static_assert(
                                        std::is_nothrow_move_assignable_v<
                                            cmd::DirectorScheduler>);
                                    scheduler = std::move(staged).value();
                                    transport = replacement_transport;
                                    repace_after_seal =
                                        transport.anchor().rate;
                                    authored = transport.anchor();
                                    saved_project_snapshot =
                                        scheduler.projected_document();
                                    project_explicitly_dirty =
                                        enriched_media_nodes != 0U;
                                    project_path = request.path;
                                    remember_recent_project(project_path);
                                    project_message = enriched_media_nodes == 0U
                                        ? "PROJECT LOADED"
                                        : "PROJECT LOADED - MEDIA FPS UPDATED (" +
                                              std::to_string(
                                                  enriched_media_nodes) +
                                              ')';
                                    if (enriched_media_nodes != 0U) {
                                        std::cerr
                                            << "enriched "
                                            << enriched_media_nodes
                                            << " loaded media node(s) with "
                                               "native FPS; save the project "
                                               "to persist metadata\n";
                                    }
                                    rehearsal_mode = false;
                                    next_new_node = 100'000U;
                                    ++project_generation;
                                    // A replacement project may carry the
                                    // same document version as the previous
                                    // one, while project-relative asset
                                    // resolution depends on the newly chosen
                                    // project path. Force the runtime asset
                                    // overlay to be rebuilt on the next draw.
                                    assets_loaded = false;
                                    clear_pending_project_authoring();
                                    project_loaded_this_frame = true;
                                }
                            }
                        }
                    }
                }
#endif
                if (!project_loaded_this_frame) {
                    for (const auto command : operator_commands) {
                        apply_command(transport, command, frame);
                    }
                }
                operator_commands.clear();
#if defined(MEDIASERVER_HAS_GUI)
                const auto projected_switcher =
                    show::read_switcher(scheduler.projected_document());
                bool scene_edits_locked = false;
                if (projected_switcher.has_value() &&
                    show::derive_switcher_progress(*projected_switcher,
                                                   frame)
                        .active) {
                    scene::SceneModel projected_scenes;
                    projected_scenes.reset(scheduler.projected_document());
                    const auto infos = projected_scenes.scenes();
                    const auto from = std::find_if(
                        infos.begin(), infos.end(),
                        [&](const scene::SceneInfo& info) {
                            return info.id ==
                                projected_switcher->transition_from;
                        });
                    const auto to = std::find_if(
                        infos.begin(), infos.end(),
                        [&](const scene::SceneInfo& info) {
                            return info.id ==
                                projected_switcher->transition_to;
                        });
                    scene_edits_locked = from != infos.end() &&
                        to != infos.end() &&
                        show::dissolve_canvases_compatible(
                            from->canvas, to->canvas);
                }
                if (scene_edits_locked) {
                    pending_edits.clear();
                    pending_keys.clear();
                    pending_key_moves.clear();
                    pending_properties.clear();
                    pending_generics.clear();
                    pending_adds.clear();
                    pending_effects.clear();
                    pending_effect_deletes.clear();
                    pending_deletes.clear();
                    pending_compositor_add.reset();
                    pending_compositor_move.reset();
                    pending_compositor_connect.reset();
                    pending_compositor_property.reset();
                    pending_compositor_delete.reset();
                    pending_undo = 0U;
                } else if (rehearsal_mode) {
                    // Compositor nodes are project-global in the current
                    // document schema. The editor presents them read-only in
                    // Rehearsal; this boundary-side guard ensures a stale
                    // queued gesture can never leak into live Program.
                    pending_compositor_add.reset();
                    pending_compositor_move.reset();
                    pending_compositor_connect.reset();
                    pending_compositor_property.reset();
                    pending_compositor_delete.reset();
                    pending_compositor_gesture = 0U;
                }
                for (const auto& edit : pending_edits) {
                    if (!schedule_scene_edit(scheduler, edit, frame)) {
                        std::cerr << "could not author a scene edit for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_edits.clear();
                for (const auto& key : pending_keys) {
                    if (!schedule_key_edit(scheduler, key, frame)) {
                        std::cerr << "could not author a keyframe for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_keys.clear();
                // Hashing and SharedStore installation can take many frames.
                // Submit them to one bounded worker, and retain any projection
                // surface mutations with the job rather than exposing an
                // unverified mesh path to the replicated document.
                for (auto& imported : pending_asset_imports) {
                    PendingAssetImportAuthoring authoring{};
                    authoring.token = next_asset_import_token++;
                    if (next_asset_import_token == 0U) {
                        next_asset_import_token = 1U;
                    }
                    authoring.project_generation = project_generation;
                    authoring.gesture = imported.gesture;
                    try {
                        extract_gesture_requests(
                            pending_feed_surface_adds, imported.gesture,
                            authoring.surface_adds);
                        extract_gesture_requests(
                            pending_feed_surface_edits, imported.gesture,
                            authoring.surface_edits);
                        asset_import_authoring.push_back(
                            std::move(authoring));
                    } catch (const std::bad_alloc&) {
                        discard_gesture_requests(
                            pending_feed_surface_adds, imported.gesture);
                        discard_gesture_requests(
                            pending_feed_surface_edits, imported.gesture);
                        project_message =
                            "ASSET IMPORT FAILED - NOT ENOUGH MEMORY";
                        std::cerr
                            << "could not queue an asset import: allocation "
                               "failed\n";
                        continue;
                    }

                    const auto submitted = asset_import_worker != nullptr
                        ? asset_import_worker->submit(ProjectAssetImportJob{
                              asset_import_authoring.back().token,
                              project_generation, imported.gesture,
                              std::move(imported.asset)})
                        : ProjectAssetImportSubmitResult::Stopping;
                    if (submitted ==
                        ProjectAssetImportSubmitResult::Accepted) {
                        project_message = "IMPORTING ASSET...";
                    } else {
                        asset_import_authoring.pop_back();
                        if (submitted ==
                            ProjectAssetImportSubmitResult::QueueFull) {
                            project_message =
                                "ASSET IMPORT QUEUE FULL - TRY AGAIN";
                        } else if (
                            submitted == ProjectAssetImportSubmitResult::
                                             AllocationFailed) {
                            project_message =
                                "ASSET IMPORT FAILED - NOT ENOUGH MEMORY";
                        } else {
                            project_message = "ASSET IMPORTS UNAVAILABLE";
                        }
                        std::cerr << "could not queue an asset import (code "
                                  << static_cast<unsigned>(submitted)
                                  << ")\n";
                    }
                }
                pending_asset_imports.clear();

                // Drain without waiting. The worker's outstanding count stays
                // bounded until each completion is consumed here.
                if (asset_import_worker != nullptr) {
                    for (auto completed =
                             asset_import_worker->take_completed();
                         completed.has_value();
                         completed = asset_import_worker->take_completed()) {
                        const auto authoring_entry = std::find_if(
                            asset_import_authoring.begin(),
                            asset_import_authoring.end(),
                            [&](const PendingAssetImportAuthoring& pending) {
                                return pending.token == completed->token;
                            });
                        if (authoring_entry ==
                            asset_import_authoring.end()) {
                            // A successful project replacement already
                            // discarded this job's document-side intent.
                            continue;
                        }
                        PendingAssetImportAuthoring authoring =
                            std::move(*authoring_entry);
                        asset_import_authoring.erase(authoring_entry);
                        if (completed->project_generation !=
                                project_generation ||
                            authoring.project_generation !=
                                project_generation) {
                            continue;
                        }
                        if (!completed->result) {
                            const auto& error = completed->result.error();
                            project_message = "ASSET IMPORT FAILED - ";
                            project_message.append(
                                project_asset_import_error_label(error.code));
                            std::cerr
                                << "asset import preparation failed (code "
                                << static_cast<unsigned>(error.code) << ')';
                            if (error.system_error) {
                                std::cerr << ": "
                                          << error.system_error.message();
                            }
                            std::cerr << '\n';
                            // Surface companions die with authoring here.
                            continue;
                        }

                        while (scheduler.projected_document().contains(
                            doc::NodeId{next_new_node})) {
                            ++next_new_node;
                        }
                        const doc::NodeId asset_id{next_new_node++};
                        const editor::AssetImportIntent verified{
                            std::move(completed->result).value(),
                            completed->gesture};
                        if (!schedule_asset_import(
                                scheduler, verified, asset_id, frame)) {
                            project_message =
                                "ASSET IMPORT FAILED - SCHEDULER REJECTED ASSET";
                            std::cerr
                                << "could not author a completed asset import "
                                   "for frame "
                                << frame.value() << '\n';
                            continue;
                        }

                        bool companion_failed = false;
                        for (const auto& request : authoring.surface_edits) {
                            if (!schedule_feed_surface_edit(
                                    scheduler, request, frame)) {
                                companion_failed = true;
                                break;
                            }
                        }
                        if (!companion_failed) {
                            for (const auto& request :
                                 authoring.surface_adds) {
                                while (scheduler.projected_document().contains(
                                    doc::NodeId{next_new_node})) {
                                    ++next_new_node;
                                }
                                const doc::NodeId surface_id{next_new_node++};
                                if (!schedule_feed_surface_add(
                                        scheduler, request, surface_id,
                                        frame)) {
                                    companion_failed = true;
                                    break;
                                }
                                if (request.assign_node_index.has_value()) {
                                    const editor::FeedProjectionEdit assignment{
                                        *request.assign_node_index,
                                        output::FeedProjection::PlanarSurface,
                                        surface_id,
                                        scene::whole(request.surface.raster),
                                        request.gesture};
                                    if (!schedule_feed_projection_edit(
                                            scheduler, assignment, frame)) {
                                        companion_failed = true;
                                        break;
                                    }
                                }
                            }
                        }
                        if (companion_failed) {
                            project_message =
                                "ASSET IMPORTED - SURFACE UPDATE FAILED";
                            std::cerr
                                << "could not author the projection surface "
                                   "paired with an asset import for frame "
                                << frame.value() << '\n';
                        } else {
                            project_message = "ASSET IMPORTED";
                        }
                    }
                }
                // A mixed surface drag can emit animated-axis keys and one
                // masked base edit for the remaining axes. Schedule that base
                // immediately after its keys so their shared scene transaction
                // stays on top and undoes atomically.
                for (const auto& request : pending_feed_surface_edits) {
                    if (!schedule_feed_surface_edit(scheduler, request,
                                                    frame)) {
                        std::cerr
                            << "could not edit a projection surface for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_feed_surface_edits.clear();
                for (const auto& moved : pending_key_moves) {
                    if (!schedule_key_move(scheduler, moved, frame)) {
                        std::cerr << "could not author a keyframe move for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_key_moves.clear();
                for (const auto& edit : pending_properties) {
                    if (!schedule_property_edit(scheduler, edit, frame)) {
                        std::cerr << "could not author an inspector edit for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_properties.clear();
                for (const auto& edit : pending_generics) {
                    if (!schedule_generic_edit(scheduler, edit, frame)) {
                        std::cerr << "could not author an inspector edit for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_generics.clear();
                for (const auto& add : pending_adds) {
                    while (scheduler.projected_document().contains(
                        doc::NodeId{next_new_node})) {
                        ++next_new_node;
                    }
                    const doc::NodeId id{next_new_node++};
                    if (!schedule_add(scheduler, add, id, frame)) {
                        std::cerr << "could not author an element add for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_adds.clear();
                for (const auto& request : pending_asset_deletes) {
                    if (!schedule_asset_delete(scheduler, request, frame)) {
                        std::cerr << "could not delete an asset for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_asset_deletes.clear();
                for (const auto& effect : pending_effects) {
                    while (scheduler.projected_document().contains(
                        doc::NodeId{next_new_node})) {
                        ++next_new_node;
                    }
                    const doc::NodeId id{next_new_node++};
                    if (!schedule_effect(scheduler, effect, id, frame)) {
                        std::cerr << "could not author an effect for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_effects.clear();
                for (const auto& request : pending_effect_deletes) {
                    if (!schedule_effect_delete(scheduler, request, frame)) {
                        std::cerr << "could not delete an effect for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_effect_deletes.clear();
                for (const auto& request : pending_hardware_places) {
                    while (scheduler.projected_document().contains(
                        doc::NodeId{next_new_node})) {
                        ++next_new_node;
                    }
                    const doc::NodeId id{next_new_node++};
                    if (!schedule_hardware_place(scheduler, request, id,
                                                 frame)) {
                        std::cerr << "could not author a hardware device for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_hardware_places.clear();
                for (const auto& request : pending_hardware_moves) {
                    if (!schedule_hardware_move(scheduler, request, frame)) {
                        std::cerr << "could not author a hardware move for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_hardware_moves.clear();
                for (const auto& edit : pending_hardware_properties) {
                    if (!schedule_hardware_property(scheduler, edit, frame)) {
                        std::cerr << "could not author hardware properties for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_hardware_properties.clear();
                if (pending_hardware_control_port.has_value()) {
                    while (scheduler.projected_document().contains(
                        doc::NodeId{next_new_node})) {
                        ++next_new_node;
                    }
                    const doc::NodeId id{next_new_node++};
                    if (!schedule_hardware_control_port(
                            scheduler, *pending_hardware_control_port, id,
                            frame)) {
                        std::cerr
                            << "could not author hardware control settings for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                    pending_hardware_control_port.reset();
                }
                if (pending_sync_policy.has_value()) {
                    while (scheduler.projected_document().contains(
                        doc::NodeId{next_new_node})) {
                        ++next_new_node;
                    }
                    const doc::NodeId id{next_new_node++};
                    if (!schedule_sync_policy(scheduler, *pending_sync_policy,
                                              id, frame)) {
                        std::cerr << "could not author the sync policy for "
                                     "frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                    pending_sync_policy.reset();
                }
                for (const auto& edit :
                     pending_hardware_output_binding_edits) {
                    while (scheduler.projected_document().contains(
                        doc::NodeId{next_new_node})) {
                        ++next_new_node;
                    }
                    const doc::NodeId id{next_new_node++};
                    if (!schedule_hardware_output_binding(
                            scheduler, edit, id, frame)) {
                        std::cerr << "could not author an output binding for "
                                     "frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_hardware_output_binding_edits.clear();
                for (const auto& request :
                     pending_hardware_output_binding_deletes) {
                    if (!schedule_hardware_output_binding_delete(
                            scheduler, request, frame)) {
                        std::cerr << "could not remove an output binding for "
                                     "frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_hardware_output_binding_deletes.clear();
                for (const auto& request : pending_hardware_links) {
                    if (request.output_binding.has_value()) {
                        const topology::HardwareOutputBinding& binding =
                            *request.output_binding;
                        if (binding.node == doc::NodeId{}) {
                            while (scheduler.projected_document().contains(
                                doc::NodeId{next_new_node})) {
                                ++next_new_node;
                            }
                            const doc::NodeId binding_id{next_new_node++};
                            if (!schedule_hardware_output_binding(
                                    scheduler,
                                    editor::HardwareOutputBindingEdit{
                                        binding, request.gesture},
                                    binding_id, frame)) {
                                std::cerr
                                    << "could not bind a wired hardware output for frame "
                                    << frame.value() << '\n';
                                return 1;
                            }
                        }
                        while (scheduler.projected_document().contains(
                            doc::NodeId{next_new_node})) {
                            ++next_new_node;
                        }
                        const doc::NodeId feed_id{next_new_node++};
                        const editor::FeedMapAddRequest feed{
                            binding.feed_index, request.output_region,
                            request.gesture};
                        if (!schedule_feed_map_add(
                                scheduler, feed, feed_id, frame,
                                cmd::UndoContextId{3U})) {
                            std::cerr
                                << "could not create a region for a wired hardware output at frame "
                                << frame.value() << '\n';
                            return 1;
                        }
                    }
                    while (scheduler.projected_document().contains(
                        doc::NodeId{next_new_node})) {
                        ++next_new_node;
                    }
                    const doc::NodeId id{next_new_node++};
                    if (!schedule_hardware_link(scheduler, request, id,
                                                frame)) {
                        std::cerr << "could not author a hardware link for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_hardware_links.clear();
                for (const auto& request : pending_hardware_deletes) {
                    if (!schedule_hardware_delete(scheduler, request,
                                                  frame)) {
                        std::cerr
                            << "could not delete hardware for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_hardware_deletes.clear();
                for (const auto& request : pending_feed_surface_adds) {
                    while (scheduler.projected_document().contains(
                        doc::NodeId{next_new_node})) {
                        ++next_new_node;
                    }
                    const doc::NodeId id{next_new_node++};
                    if (!schedule_feed_surface_add(scheduler, request, id,
                                                   frame)) {
                        std::cerr
                            << "could not add a projection surface for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                    if (request.assign_node_index.has_value()) {
                        const editor::FeedProjectionEdit assignment{
                            *request.assign_node_index,
                            output::FeedProjection::PlanarSurface,
                            id,
                            scene::whole(request.surface.raster),
                            request.gesture};
                        if (!schedule_feed_projection_edit(
                                scheduler, assignment, frame)) {
                            std::cerr
                                << "could not assign a UV projection mesh for frame "
                                << frame.value() << '\n';
                            return 1;
                        }
                    }
                }
                pending_feed_surface_adds.clear();
                for (const auto& request : pending_feed_map_adds) {
                    while (scheduler.projected_document().contains(
                        doc::NodeId{next_new_node})) {
                        ++next_new_node;
                    }
                    const doc::NodeId id{next_new_node++};
                    if (!schedule_feed_map_add(scheduler, request, id,
                                               frame)) {
                        std::cerr
                            << "could not add a feed map for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_feed_map_adds.clear();
                for (const auto& edit : pending_feed_map_edits) {
                    while (scheduler.projected_document().contains(
                        doc::NodeId{next_new_node})) {
                        ++next_new_node;
                    }
                    const doc::NodeId id{next_new_node++};
                    if (!schedule_feed_map_edit(scheduler, edit, id, frame)) {
                        std::cerr << "could not author a feed map edit for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_feed_map_edits.clear();
                for (const auto& edit : pending_feed_projection_edits) {
                    if (!schedule_feed_projection_edit(scheduler, edit,
                                                       frame)) {
                        std::cerr
                            << "could not edit a feed projection for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_feed_projection_edits.clear();
                for (const auto& request : pending_feed_surface_deletes) {
                    if (!schedule_feed_surface_delete(scheduler, request,
                                                      frame)) {
                        std::cerr
                            << "could not delete a projection surface for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_feed_surface_deletes.clear();
                for (const auto& request : pending_feed_map_deletes) {
                    if (!schedule_feed_map_delete(scheduler, request,
                                                  frame)) {
                        std::cerr
                            << "could not delete a feed map for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_feed_map_deletes.clear();
                if (pending_compositor_add.has_value()) {
                    while (scheduler.projected_document().contains(
                        doc::NodeId{next_new_node})) {
                        ++next_new_node;
                    }
                    const doc::NodeId id{next_new_node++};
                    if (!schedule_compositor_add(
                            scheduler, *pending_compositor_add, id,
                            pending_compositor_gesture, frame)) {
                        std::cerr << "could not add a compositor node for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                    pending_compositor_add.reset();
                }
                if (pending_compositor_move.has_value()) {
                    if (!schedule_compositor_move(
                            scheduler, *pending_compositor_move,
                            pending_compositor_gesture, frame)) {
                        std::cerr << "could not move a compositor node for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                    pending_compositor_move.reset();
                }
                if (pending_compositor_property.has_value()) {
                    if (!schedule_compositor_property(
                            scheduler, *pending_compositor_property,
                            pending_compositor_gesture, frame)) {
                        std::cerr
                            << "could not edit a compositor parameter for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                    pending_compositor_property.reset();
                }
                if (pending_compositor_connect.has_value()) {
                    while (scheduler.projected_document().contains(
                        doc::NodeId{next_new_node})) {
                        ++next_new_node;
                    }
                    const doc::NodeId id{next_new_node++};
                    if (!schedule_compositor_connect(
                            scheduler, *pending_compositor_connect, id,
                            pending_compositor_gesture, frame)) {
                        std::cerr
                            << "could not connect compositor nodes for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                    pending_compositor_connect.reset();
                }
                if (pending_compositor_delete.has_value()) {
                    if (!schedule_compositor_delete(
                            scheduler, *pending_compositor_delete,
                            pending_compositor_gesture, frame)) {
                        std::cerr
                            << "could not delete a compositor item for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                    pending_compositor_delete.reset();
                }
                pending_compositor_gesture = 0U;
                for (const auto node : pending_deletes) {
                    if (!schedule_delete(
                            scheduler, node, pending_delete_gesture, frame)) {
                        std::cerr
                            << "could not author an element delete for frame "
                            << frame.value() << '\n';
                        return 1;
                    }
                }
                pending_deletes.clear();
                if (pending_switcher.has_value()) {
                    const editor::SwitcherRequest request = *pending_switcher;
                    pending_switcher.reset();
                    const cmd::UndoContextId switcher_context{5U};
                    const cmd::TransactionId switcher_transaction{
                        frame.value()};

                    scene::SceneModel project_scenes;
                    project_scenes.reset(scheduler.projected_document());
                    const std::vector<scene::SceneInfo> scenes =
                        project_scenes.scenes();
                    std::optional<show::SwitcherAnchor> anchor =
                        show::read_switcher(scheduler.projected_document());

                    const auto scene_for =
                        [&](scene::SceneId id) -> const scene::SceneInfo* {
                        const auto found = std::find_if(
                            scenes.begin(), scenes.end(),
                            [id](const scene::SceneInfo& info) {
                                return info.id == id;
                            });
                        return found == scenes.end() ? nullptr : &*found;
                    };
                    const auto dissolve_pair_compatible =
                        [&](scene::SceneId program,
                            scene::SceneId rehearsal) {
                        const scene::SceneInfo* const program_info =
                            scene_for(program);
                        const scene::SceneInfo* const rehearsal_info =
                            scene_for(rehearsal);
                        return program_info != nullptr &&
                            rehearsal_info != nullptr &&
                            show::dissolve_canvases_compatible(
                                program_info->canvas,
                                rehearsal_info->canvas);
                    };

                    const auto author_switcher =
                        [&](const show::SwitcherAnchor& next) {
                            return static_cast<bool>(
                                show::schedule_switcher_anchor(
                                    scheduler, next, frame,
                                    switcher_context,
                                    switcher_transaction));
                        };

                    if (request.action ==
                        editor::SwitcherAction::ToggleRehearse) {
                        if (rehearsal_mode) {
                            rehearsal_mode = false;
                        } else if (anchor.has_value()) {
                            rehearsal_mode = true;
                        } else if (!scenes.empty()) {
                            auto clone = scene::plan_clone_scene(
                                scheduler.projected_document(),
                                scenes.front().id,
                                doc::NodeId{next_new_node});
                            if (!clone) {
                                std::cerr
                                    << "could not clone Program into Rehearsal\n";
                                return 1;
                            }
                            scene::SceneClonePlan plan =
                                std::move(clone).value();
                            for (doc::Node& node : plan.puts) {
                                if (node.id == plan.destination) {
                                    const std::string rehearsal_name =
                                        scenes.front().name.empty()
                                        ? "Rehearsal"
                                        : scenes.front().name + " Rehearsal";
                                    node.properties.insert_or_assign(
                                        std::string{
                                            scene::schema::scene_name},
                                        doc::PropertyValue{rehearsal_name});
                                }
                                if (!scheduler.schedule_edit(
                                        cmd::PutNode{node},
                                        cmd::EraseNode{node.id},
                                        switcher_context,
                                        switcher_transaction, frame)) {
                                    std::cerr
                                        << "could not author Rehearsal clone\n";
                                    return 1;
                                }
                            }
                            for (const auto& [source, destination] :
                                 plan.remap) {
                                static_cast<void>(source);
                                next_new_node = std::max(
                                    next_new_node,
                                    destination.value() + 1U);
                            }
                            auto switcher_created =
                                show::SwitcherAuthority::create(
                                plan.source, plan.destination, frame);
                            if (!switcher_created || !author_switcher(
                                    std::move(switcher_created)
                                        .value()
                                        .anchor())) {
                                std::cerr
                                    << "could not author Program/Rehearsal slots\n";
                                return 1;
                            }
                            rehearsal_mode = true;
                        }
                    } else if (request.action ==
                               editor::SwitcherAction::Swap) {
                        const doc::Root& projected_document =
                            scheduler.projected_document();
                        assets::AssetModel projected_assets;
                        projected_assets.reset(projected_document);
                        const ProjectAssetManifest projected_manifest =
                            collect_project_asset_manifest(
                                projected_document,
                                projected_assets.evaluate());
                        assets::ContentResolutionRoots projected_roots{
                            {}, options.asset_store_root};
                        if (!project_path.empty()) {
                            projected_roots.project_file =
                                native_path_from_utf8(project_path);
                        }
                        bool program_assets_ready =
                            director_asset_resolution.locally_ready_for(
                                projected_manifest.digest,
                                projected_roots,
                                project_generation);
                        topology::HardwareMapModel projected_hardware;
                        projected_hardware.reset(projected_document);
                        const topology::HardwareMap projected_hardware_map =
                            projected_hardware.evaluate();
                        if (program_assets_ready && lifecycle_controller) {
                            program_assets_ready =
                                lifecycle_controller->program_ready(
                                    projected_manifest,
                                    projected_hardware_map,
                                    project_generation,
                                    barrier);
                        }
                        // The same gate the surface reports, evaluated against
                        // the exact document this swap would promote. A swap
                        // is the moment the claim becomes visible, so it is
                        // the moment the claim must actually hold.
                        sync::SyncPolicyModel projected_sync_model;
                        projected_sync_model.reset(projected_document);
                        const sync::SyncPolicy projected_policy =
                            projected_sync_model.evaluate();
                        const sync::SyncTopology projected_topology =
                            sync::derive_sync_topology(projected_hardware_map,
                                                       projected_policy);
                        sync::SyncReadinessInput projected_gate{};
                        projected_gate.policy = &projected_policy;
                        projected_gate.topology = &projected_topology;
                        projected_gate.nodes = director_sync.status.nodes;
                        projected_gate.assets_adopted = program_assets_ready;
                        projected_gate.render_nodes_live = std::all_of(
                            projected_topology.node_indices.begin(),
                            projected_topology.node_indices.end(),
                            [&](core::u32 node_index) {
                                const sync::NodeSyncStatus* const node =
                                    director_sync.status.node(node_index);
                                return node != nullptr &&
                                    node->runtime_reported;
                            });
                        const sync::SyncReadiness projected_readiness =
                            sync::evaluate_sync_readiness(projected_gate);
                        program_assets_ready = projected_readiness.satisfied;
                        if (!program_assets_ready) {
                            project_message = std::string{
                                sync::sync_readiness_headline(
                                    projected_readiness.code)};
                            if (projected_readiness.code ==
                                sync::SyncReadinessCode::AssetsNotAdopted) {
                                project_message =
                                    "ASSETS NOT READY OR ADOPTED FOR PROGRAM";
                            }
                        } else if (anchor.has_value() &&
                            !show::derive_switcher_progress(*anchor, frame)
                                 .active &&
                            (request.transition_control_frames == 0U ||
                             dissolve_pair_compatible(
                                 anchor->program_scene,
                                 anchor->rehearsal_scene))) {
                            auto authority = show::SwitcherAuthority::create(
                                anchor->program_scene,
                                anchor->rehearsal_scene, frame);
                            auto swapped = authority
                                ? std::move(authority).value().swap(
                                      frame,
                                      request.transition_control_frames)
                                : core::Result<show::SwitcherAnchor,
                                               show::SwitcherCommandError>::failure(
                                      show::SwitcherCommandError::InvalidScene);
                            if (!swapped ||
                                !author_switcher(std::move(swapped).value())) {
                                std::cerr << "could not swap Program and "
                                             "Rehearsal\n";
                                return 1;
                            }
                        }
                    } else if (request.action ==
                               editor::SwitcherAction::SelectRehearsal) {
                        const scene::SceneId program = anchor.has_value()
                            ? anchor->program_scene
                            : (!scenes.empty() ? scenes.front().id
                                               : scene::SceneId{});
                        if (dissolve_pair_compatible(program,
                                                     request.scene)) {
                            if (request.scene != program &&
                                (!anchor.has_value() ||
                                 !show::derive_switcher_progress(*anchor,
                                                                 frame)
                                      .active)) {
                                auto authority =
                                    show::SwitcherAuthority::create(
                                        program,
                                        anchor.has_value()
                                            ? anchor->rehearsal_scene
                                            : request.scene,
                                        frame);
                                std::optional<show::SwitcherAnchor> selected;
                                if (authority) {
                                    auto state = std::move(authority).value();
                                    if (!anchor.has_value() ||
                                        request.scene ==
                                            anchor->rehearsal_scene) {
                                        selected = state.anchor();
                                    } else {
                                        auto changed = state.set_rehearsal(
                                            request.scene, frame);
                                        if (changed) {
                                            selected =
                                                std::move(changed).value();
                                        }
                                    }
                                }
                                if (!selected.has_value() ||
                                    !author_switcher(*selected)) {
                                    std::cerr
                                        << "could not select Rehearsal scene\n";
                                    return 1;
                                }
                                rehearsal_mode = true;
                            }
                        }
                    } else if (request.action ==
                                   editor::SwitcherAction::DuplicateScene &&
                               (!anchor.has_value() ||
                                !show::derive_switcher_progress(
                                     *anchor, frame)
                                     .active)) {
                        const scene::SceneInfo* const source_info =
                            scene_for(request.scene);
                        if (source_info != nullptr) {
                            auto cloned = scene::plan_clone_scene(
                                scheduler.projected_document(), request.scene,
                                doc::NodeId{next_new_node});
                            if (!cloned) {
                                std::cerr
                                    << "could not plan a scene copy\n";
                                return 1;
                            }
                            scene::SceneClonePlan plan =
                                std::move(cloned).value();
                            for (doc::Node& node : plan.puts) {
                                if (node.id == plan.destination) {
                                    const std::string copy_name =
                                        source_info->name.empty()
                                        ? "Scene Copy"
                                        : source_info->name + " Copy";
                                    node.properties.insert_or_assign(
                                        std::string{
                                            scene::schema::scene_name},
                                        doc::PropertyValue{copy_name});
                                }
                                if (!scheduler.schedule_edit(
                                        cmd::PutNode{node},
                                        cmd::EraseNode{node.id},
                                        switcher_context,
                                        switcher_transaction, frame)) {
                                    std::cerr
                                        << "could not author a scene copy\n";
                                    return 1;
                                }
                            }
                            for (const auto& [source, destination] :
                                 plan.remap) {
                                static_cast<void>(source);
                                next_new_node = std::max(
                                    next_new_node,
                                    destination.value() + 1U);
                            }
                            const scene::SceneId program =
                                anchor.has_value()
                                ? anchor->program_scene
                                : (!scenes.empty() ? scenes.front().id
                                                   : request.scene);
                            auto copied_switcher =
                                show::SwitcherAuthority::create(
                                    program, plan.destination, frame);
                            if (!copied_switcher ||
                                !author_switcher(
                                    std::move(copied_switcher)
                                        .value()
                                        .anchor())) {
                                std::cerr
                                    << "could not assign the scene copy to "
                                       "Rehearsal\n";
                                return 1;
                            }
                            rehearsal_mode = true;
                        }
                    } else if (request.action ==
                                   editor::SwitcherAction::CreateScene &&
                               (!anchor.has_value() ||
                                !show::derive_switcher_progress(
                                     *anchor, frame)
                                     .active)) {
                        const scene::SceneId existing_program =
                            anchor.has_value()
                            ? anchor->program_scene
                            : (!scenes.empty() ? scenes.front().id
                                               : scene::SceneId{});
                        const scene::SceneInfo* const program_info =
                            scene_for(existing_program);
                        const scene::Canvas created_canvas =
                            program_info != nullptr
                            ? program_info->canvas
                            : scene::Canvas{1'920U, 1'080U};
                        while (scheduler.projected_document().contains(
                            doc::NodeId{next_new_node})) {
                            ++next_new_node;
                        }
                        const scene::SceneId created_scene{next_new_node++};
                        while (scheduler.projected_document().contains(
                            doc::NodeId{next_new_node})) {
                            ++next_new_node;
                        }
                        const doc::NodeId camera_id{next_new_node++};
                        doc::Node stage{};
                        stage.id = created_scene;
                        stage.type = std::string{scene::schema::stage_type};
                        stage.properties.emplace(
                            std::string{scene::schema::scene_name},
                            doc::PropertyValue{
                                "Scene " + std::to_string(scenes.size() + 1U)});
                        stage.properties.emplace(
                            std::string{scene::schema::canvas_width},
                            doc::PropertyValue{static_cast<std::int64_t>(
                                created_canvas.width)});
                        stage.properties.emplace(
                            std::string{scene::schema::canvas_height},
                            doc::PropertyValue{static_cast<std::int64_t>(
                                created_canvas.height)});
                        doc::Node camera{};
                        camera.id = camera_id;
                        camera.type = std::string{scene::schema::camera_type};
                        camera.properties.emplace(
                            std::string{scene::schema::scene_id},
                            doc::PropertyValue{static_cast<std::int64_t>(
                                created_scene.value())});
                        camera.properties.emplace(
                            std::string{scene::schema::eye_z},
                            doc::PropertyValue{6.0});
                        for (const doc::Node& node : {stage, camera}) {
                            if (!scheduler.schedule_edit(
                                    cmd::PutNode{node},
                                    cmd::EraseNode{node.id},
                                    switcher_context,
                                    switcher_transaction, frame)) {
                                std::cerr << "could not create a scene\n";
                                return 1;
                            }
                        }
                        const scene::SceneId program =
                            existing_program != scene::SceneId{}
                            ? existing_program
                            : created_scene;
                        if (program != created_scene) {
                            auto scene_switcher =
                                show::SwitcherAuthority::create(
                                program, created_scene, frame);
                            if (!scene_switcher || !author_switcher(
                                    std::move(scene_switcher)
                                        .value()
                                        .anchor())) {
                                std::cerr << "could not assign the new scene "
                                             "to Rehearsal\n";
                                return 1;
                            }
                            rehearsal_mode = true;
                        }
                    }
                }
                if (pending_range) {
                    // Set the show length and loop, then let the ordinary anchor
                    // authoring below replicate the new bounds.
                    static_cast<void>(transport.set_range(
                        show::ShowFrame{pending_range->first},
                        show::ShowFrame{pending_range->last},
                        pending_range->loop,
                        frame));
                    project_explicitly_dirty = true;
                    pending_range.reset();
                }
                if (pending_show_rate.has_value()) {
                    const show::FrameRate requested{
                        pending_show_rate->numerator,
                        pending_show_rate->denominator};
                    if (!transport.set_rate(requested, frame)) {
                        std::cerr << "could not apply the requested show rate\n";
                        return 1;
                    }
                    repace_after_seal = requested;
                    project_explicitly_dirty = true;
                    pending_show_rate.reset();
                }
                if (pending_seek) {
                    // Scrubbing is a paused operation, like the keyboard step and
                    // jump commands: pause, then seek to the dragged frame,
                    // clamped to the live show range. The new anchor replicates
                    // like any other transport change.
                    if (transport.state() == show::TransportState::Playing) {
                        static_cast<void>(transport.pause(frame));
                    }
                    const auto target = std::min(
                        std::max(
                            *pending_seek,
                            transport.anchor().first_frame.value()),
                        transport.anchor().last_frame.value());
                    static_cast<void>(
                        transport.seek(show::ShowFrame{target}, frame));
                    pending_seek.reset();
                }
                if (pending_undo != 0U) {
                    // Undo the top of the scene-edit context - a whole drag's
                    // worth of commands, reversed as one transaction. NoHistory
                    // just means there was nothing to undo, which is not an error.
                    auto undone = scheduler.schedule_undo(
                        cmd::UndoContextId{2U},
                        cmd::TransactionId{pending_undo},
                        frame);
                    if (!undone
                        && undone.error()
                            != cmd::DirectorUndoError::NoHistory) {
                        std::cerr << "could not author a scene-edit undo for frame "
                                  << frame.value() << '\n';
                        return 1;
                    }
                    pending_undo = 0U;
                }
                if (pending_project_save.has_value()) {
                    const editor::ProjectFileRequest request =
                        std::move(*pending_project_save);
                    pending_project_save.reset();
                    doc::Root snapshot = scheduler.projected_document();
                    show::TransportAnchor saved_transport = transport.anchor();
                    saved_transport.anchor_show_frame = transport.frame_at(frame);
                    saved_transport.anchor_control_frame = frame;
                    snapshot = snapshot.set(
                        show::make_transport_node(saved_transport));
                    auto saved =
                        persist::save_project_file(
                            native_path_from_utf8(request.path), snapshot);
                    const bool installed =
                        !saved && saved.error().committed;
                    if (!saved && !installed) {
                        project_message =
                            project_file_failure("SAVING", saved.error());
                        std::cerr << "could not save project '" << request.path
                                  << "' (code "
                                  << static_cast<unsigned>(saved.error().code)
                                  << ')';
                        if (saved.error().system_error) {
                            std::cerr << ": "
                                      << saved.error().system_error.message();
                        }
                        std::cerr << '\n';
                    } else {
                        saved_project_snapshot =
                            scheduler.projected_document();
                        project_explicitly_dirty = false;
                        const bool asset_base_changed =
                            project_path != request.path;
                        project_path = request.path;
                        // Save As changes the base of project-relative asset
                        // paths even though authored content is unchanged.
                        if (asset_base_changed) {
                            assets_loaded = false;
                        }
                        remember_recent_project(project_path);
                        project_message = installed
                            ? "ERROR SAVE: DURABILITY NOT CONFIRMED"
                            : "PROJECT SAVED";
                        if (installed) {
                            std::cerr
                                << "project '" << request.path
                                << "' was replaced, but durable directory "
                                   "sync failed";
                            if (saved.error().system_error) {
                                std::cerr
                                    << ": "
                                    << saved.error().system_error.message();
                            }
                            std::cerr << '\n';
                        }
                    }
                }
#endif
            }
            static_cast<void>(transport.settle(frame));

            // The anchor travels as an ordinary command inside a sealed frame,
            // so every node derives the position from the same revision. An
            // unchanged anchor authors nothing, so a playing show costs the
            // document no edits at all -- which is what removes the per-frame
            // durable write behind it.
            if (!authored || *authored != transport.anchor()) {
                const auto command = show::schedule_transport_anchor(
                    scheduler,
                    transport.anchor(),
                    frame,
                    cmd::UndoContextId{1U},
                    cmd::TransactionId{frame.value()});
                if (!command) {
                    std::cerr
                        << "could not author the transport anchor for frame "
                        << frame.value() << " (code "
                        << static_cast<unsigned>(command.error()) << ")\n";
                    return 1;
                }
                authored = transport.anchor();
                ++anchors;
            }

            auto sealed_frame = barrier.seal_and_track(hub, frame);
            if (!sealed_frame) {
                std::cerr << "Director could not seal frame " << frame.value()
                          << " (code "
                          << static_cast<unsigned>(sealed_frame.error().code)
                          << ")\n";
                return 1;
            }
            tracked.push_back(sealed_frame.value().barrier);
            ++sealed;
            ++sealed_this_iteration;
            if (repace_after_seal.has_value()) {
                // `due` was calculated at the old cadence. Once the boundary
                // carrying the new rate is sealed, discard the remaining old-
                // rate debt and make the next boundary one complete period at
                // the new exact rational rate after this same observation.
                if (!pacer.seek(show::ShowFrame{frame.value()}, now) ||
                    !pacer.reconfigure_rate(*repace_after_seal, now)) {
                    std::cerr << "could not re-anchor the Director show rate\n";
                    return 1;
                }
                interval = status_interval(options, *repace_after_seal);
                since_status = 0U;
                break;
            }
        }

        // Retire in order. A frame whose cohort has not finished stays, and so
        // does everything behind it, which is what bounds the backlog above.
        while (!tracked.empty() && barrier.retire(tracked.front())) {
            tracked.pop_front();
        }

        const auto control_frame = scheduler.sealed_through();
        show::TestPatternInput input{};
        input.show_frame = transport.frame_at(control_frame);
        input.control_frame = control_frame;
        input.rate = transport.anchor().rate;
        input.state = transport.state();
        input.node_index = options.node_index;
        input.held = sealed_this_iteration == 0U;

        // The Director previews the whole canvas of the replicated scene, so it
        // shows what the wall shows between its nodes. It evaluates from its own
        // committed document rather than a mailbox.
        const bool render_scene =
            presenter != nullptr && presenter->renders_scenes();
        // The scene is evaluated for the preview or the editor: both work on the
        // same committed scene at the show frame.
        const bool needs_scene = render_scene || options.edit;
        scene::Scene scene_frame;
        scene::Scene stage_scene_frame;
        scene::Scene transition_from_frame;
        scene::Scene transition_to_frame;
        std::vector<scene::SceneInfo> scene_infos;
        std::optional<show::SwitcherAnchor> switcher_anchor;
        show::SwitcherProgress switcher_progress{};
        output::FeedMap feed_map;
        topology::HardwareMap hardware_map;
        assets::AssetRegistry asset_registry;
        ProjectAssetManifest asset_manifest;
        compositor::CompositorGraph compositor_graph;
        show::PresenterStatus status{show::PresenterStatus::Skipped};
        if (needs_scene) {
            const auto& document = scheduler.sealed_document();
            assets::ContentResolutionRoots director_asset_roots{
                {}, options.asset_store_root};
            core::u64 director_project_generation{};
#if defined(MEDIASERVER_HAS_GUI)
            if (!project_path.empty()) {
                director_asset_roots.project_file =
                    native_path_from_utf8(project_path);
            }
            director_project_generation = project_generation;
#endif
            if (!scene_loaded || document.version().value() != scene_version) {
                scene_model.reset(document);
                scene_version = document.version().value();
                scene_loaded = true;
            }
            if (!compositor_loaded ||
                document.version().value() != compositor_version) {
                compositor_model.reset(document);
                compositor_version = document.version().value();
                compositor_loaded = true;
            }
            if (!assets_loaded ||
                document.version().value() != asset_version) {
                asset_model.reset(document);
                const assets::AssetRegistry registry =
                    asset_model.evaluate();
                ProjectAssetResolutionPlan plan =
                    prepare_project_asset_resolution(document, registry);
                if (!assets_loaded ||
                    plan.manifest.digest != asset_manifest_digest ||
                    director_asset_resolution.key.roots !=
                        director_asset_roots ||
                    director_asset_resolution.key.project_generation !=
                        director_project_generation) {
                    asset_manifest_digest = plan.manifest.digest;
                    director_asset_runtime_ready = false;
                    static_cast<void>(director_asset_resolution.install(
                        std::move(plan), director_asset_roots,
                        director_project_generation, now));
                }
                asset_version = document.version().value();
                assets_loaded = true;
            }
            static_cast<void>(director_asset_resolution.poll(now));
            director_asset_resolution.submit_retry_if_due(now);
            director_asset_runtime_ready =
                director_asset_resolution.locally_ready_for(
                    asset_manifest_digest, director_asset_roots,
                    director_project_generation);
            compositor_graph = compositor_model.evaluate();
            const scene::SceneTime time{
                input.show_frame.value(),
                input.rate.numerator,
                input.rate.denominator};
            scene_infos = scene_model.scenes();
            switcher_anchor = show::read_switcher(document);
            if (switcher_anchor.has_value()) {
                switcher_progress = show::derive_switcher_progress(
                    *switcher_anchor, control_frame);
                scene_frame = scene_model.evaluate(
                    switcher_anchor->program_scene, time);
                stage_scene_frame = scene_model.evaluate(
                    rehearsal_mode ? switcher_anchor->rehearsal_scene
                                   : switcher_anchor->program_scene,
                    time);
                if (switcher_progress.active) {
                    transition_from_frame =
                        scene_model.evaluate(switcher_progress.from, time);
                    transition_to_frame =
                        scene_model.evaluate(switcher_progress.to, time);
                    if (!show::dissolve_canvases_compatible(
                            transition_from_frame.canvas,
                            transition_to_frame.canvas)) {
                        switcher_progress.active = false;
                    }
                }
            } else {
                scene_frame = scene_model.evaluate(time);
                stage_scene_frame = scene_frame;
            }
            if (const ResolvedProjectAssets* const director_overlay =
                    director_asset_resolution.current_overlay()) {
                director_overlay->apply(scene_frame);
                director_overlay->apply(stage_scene_frame);
                if (switcher_progress.active) {
                    director_overlay->apply(transition_from_frame);
                    director_overlay->apply(transition_to_frame);
                }
            }
        }

        if (options.edit) {
            const auto& document = scheduler.sealed_document();
            if (!feed_loaded || document.version().value() != feed_version) {
                feed_model.reset(document);
                feed_version = document.version().value();
                feed_loaded = true;
            }
            feed_map = feed_model.evaluate(scene::SceneTime{
                input.show_frame.value(), input.rate.numerator,
                input.rate.denominator});
            if (const ResolvedProjectAssets* const director_overlay =
                    director_asset_resolution.current_overlay()) {
                director_overlay->apply(feed_map);
            }
            if (!hardware_loaded ||
                document.version().value() != hardware_version) {
                hardware_model.reset(document);
                hardware_version = document.version().value();
                hardware_loaded = true;
            }
            hardware_map = hardware_model.evaluate();
            asset_registry = asset_model.evaluate();
            asset_manifest = collect_project_asset_manifest(
                document, asset_registry);
        }

        // The synchronization policy and the groups derived from the authored
        // cabling are pure document projections, so they are refreshed even on
        // a Director with no editor: the diagnostic stream should record what
        // the project asked for whether or not anyone is watching.
        {
            const doc::Root& sync_document = scheduler.sealed_document();
            if (!director_sync_loaded ||
                sync_document.version().value() != director_sync_version) {
                director_sync_model.reset(sync_document);
                director_sync_version = sync_document.version().value();
                director_sync_loaded = true;
                director_sync.policy = director_sync_model.evaluate();
            }
            director_sync.topology =
                sync::derive_sync_topology(hardware_map, director_sync.policy);
            director_sync.readiness = evaluate_sync_gate(true);
        }

        bool handled = false;
#if defined(MEDIASERVER_HAS_GUI)
        if (options.edit && editor) {
            handled = true;
            const editor::EditorTimeline timeline{
                input.show_frame.value(),
                transport.anchor().first_frame.value(),
                transport.anchor().last_frame.value(),
                transport.state() == show::TransportState::Playing,
                transport.anchor().loop,
                transport.anchor().rate.numerator,
                transport.anchor().rate.denominator};
            const auto elements =
                scene_elements(scheduler, stage_scene_frame);
            const auto all_effects = scene_model.effects();
            std::vector<scene::EffectInstance> effects;
            for (const scene::EffectInstance& effect : all_effects) {
                const bool in_stage = std::any_of(
                    elements.begin(), elements.end(),
                    [&](const editor::ElementInfo& element) {
                        return element.node == effect.target;
                    });
                if (in_stage) {
                    effects.push_back(effect);
                }
            }
            const auto all_keyframes = scene_model.channel_keyframes();
            std::vector<scene::ChannelKeyframes> keyframes;
            for (const scene::ChannelKeyframes& channel : all_keyframes) {
                const bool element_channel = std::any_of(
                    elements.begin(), elements.end(),
                    [&](const editor::ElementInfo& element) {
                        return element.node == channel.target;
                    });
                const bool effect_channel = std::any_of(
                    effects.begin(), effects.end(),
                    [&](const scene::EffectInstance& effect) {
                        return effect.source == channel.target;
                    });
                const bool surface_channel =
                    feed_map.surface_for(channel.target) != nullptr;
                if (element_channel || effect_channel || surface_channel) {
                    keyframes.push_back(channel);
                }
            }
            editor::EditorSwitcherState switcher_view{};
            switcher_view.scenes = scene_infos;
            switcher_view.program_scene = switcher_anchor.has_value()
                ? switcher_anchor->program_scene
                : scene_frame.id;
            switcher_view.rehearsal_scene = switcher_anchor.has_value()
                ? switcher_anchor->rehearsal_scene
                : scene::SceneId{};
            switcher_view.stage_scene = &stage_scene_frame;
            switcher_view.transition_from = switcher_progress.active
                ? &transition_from_frame
                : nullptr;
            switcher_view.transition_to = switcher_progress.active
                ? &transition_to_frame
                : nullptr;
            switcher_view.transition_mix =
                static_cast<float>(switcher_progress.mix());
            switcher_view.transition_active = switcher_progress.active;
            switcher_view.rehearsal_mode = rehearsal_mode;
            switcher_view.rehearsal_locked = switcher_progress.active;
            switcher_view.dissolve_compatible = false;
            if (switcher_anchor.has_value()) {
                const auto program_info = std::find_if(
                    scene_infos.begin(), scene_infos.end(),
                    [&](const scene::SceneInfo& info) {
                        return info.id == switcher_anchor->program_scene;
                    });
                const auto rehearsal_info = std::find_if(
                    scene_infos.begin(), scene_infos.end(),
                    [&](const scene::SceneInfo& info) {
                        return info.id == switcher_anchor->rehearsal_scene;
                    });
                switcher_view.dissolve_compatible =
                    program_info != scene_infos.end() &&
                    rehearsal_info != scene_infos.end() &&
                    show::dissolve_canvases_compatible(
                        program_info->canvas, rehearsal_info->canvas);
            }
            const editor::EditorProjectState project_view{
                project_path,
                project_message,
                project_explicitly_dirty || project_content_changed(
                                                saved_project_snapshot,
                                                scheduler.projected_document()),
                project_generation};
            std::vector<editor::OutputMonitorState::Node> output_nodes;
            topology::HardwareRuntimeSnapshot hardware_runtime;
            if (lifecycle_controller) {
                cluster_configuration_authored = true;
                try {
                    lifecycle_controller->sync(
                        hardware_map, asset_manifest, project_generation);
                    output_nodes = lifecycle_controller->snapshot();
                    hardware_runtime =
                        lifecycle_controller->hardware_snapshot();
                } catch (const std::bad_alloc&) {
                    std::cerr << "could not build hardware runtime snapshot\n";
                    return 1;
                }
            }
            configured_cluster_nodes = static_cast<core::u32>(
                std::min<std::size_t>(output_nodes.size(),
                                      std::numeric_limits<core::u32>::max()));
            switcher_view.content_ready = director_asset_runtime_ready;
            if (switcher_view.content_ready && lifecycle_controller) {
                switcher_view.content_ready =
                    lifecycle_controller->program_ready(
                        asset_manifest, hardware_map, project_generation,
                        barrier);
            }
            if (lifecycle_controller) {
                director_sync.status =
                    lifecycle_controller->sync_snapshot(&barrier);
            }
            // Program readiness is the conjunction of every gate, not the
            // asset gate with a timing note beside it.
            director_sync.readiness =
                evaluate_sync_gate(switcher_view.content_ready);
            switcher_view.content_ready = director_sync.readiness.satisfied;
            editor::OutputMonitorState output_monitor{
                timeline.show_frame, scene_frame.canvas.width,
                scene_frame.canvas.height,
                static_cast<float>(timeline.rate_numerator) /
                    static_cast<float>(
                        std::max(timeline.rate_denominator, 1U)),
                timeline.playing, output_nodes};
            const ClusterObservation cluster =
                observe_cluster(timeline.show_frame);
            using UiHealth = editor::OutputMonitorState::Cluster::Health;
            switch (cluster.health) {
            case ClusterControlHealth::Unmonitored:
                output_monitor.cluster.health = UiHealth::Unmonitored;
                break;
            case ClusterControlHealth::Healthy:
                output_monitor.cluster.health = UiHealth::Healthy;
                break;
            case ClusterControlHealth::Degraded:
                output_monitor.cluster.health = UiHealth::Degraded;
                break;
            case ClusterControlHealth::Faulted:
                output_monitor.cluster.health = UiHealth::Faulted;
                break;
            }
            output_monitor.cluster.configured_nodes =
                cluster.configured_nodes;
            output_monitor.cluster.live_sessions = cluster.live_sessions;
            output_monitor.cluster.catching_up_sessions =
                cluster.catching_up_sessions;
            output_monitor.cluster.release_lag_frames =
                cluster.release_lag_frames;
            output_monitor.cluster.pacer_lag_frames =
                cluster.pacer_lag_frames;
            output_monitor.cluster.heartbeat_timeouts =
                cluster.sessions.heartbeat_timeouts;
            output_monitor.cluster.session_failures =
                cluster.sessions.session_failures;
            output_monitor.cluster.barrier_rejections =
                cluster.barrier.rejected_registrations +
                cluster.barrier.rejected_acknowledgements;
            output_monitor.cluster.present_failures =
                cluster.present_failures;
            output_monitor.cluster.diagnostics_recording =
                cluster.diagnostics_recording;
            output_monitor.cluster.external_time_sync_verified =
                cluster.external_time_sync_verified;
            output_monitor.cluster.hardware_frame_lock_verified =
                cluster.hardware_frame_lock_verified;
            output_monitor.cluster.sync_mode =
                sync::sync_mode_name(director_sync.policy.mode);
            // The timing line reports the timing fact. An unrelated hold -
            // assets not adopted, a node still catching up - has its own
            // indicators and must not be dressed up as a raster problem.
            output_monitor.cluster.sync_headline =
                director_sync.readiness.physical_headline();
            output_monitor.cluster.sync_required =
                director_sync.policy.requires_physical_sync();
            output_monitor.cluster.sync_satisfied =
                director_sync.readiness.satisfied;
            output_monitor.cluster.sync_degraded =
                director_sync.readiness.degraded;
            output_monitor.cluster.sync_groups =
                director_sync.readiness.groups;
            output_monitor.cluster.sync_locked_outputs =
                director_sync.readiness.locked_outputs;
            output_monitor.cluster.sync_expected_outputs =
                director_sync.readiness.expected_outputs;
            output_monitor.cluster.barrier_joined_outputs =
                director_sync.readiness.barrier_joined_outputs;
            output_monitor.cluster.sync_configuration_issues =
                static_cast<core::u32>(
                    director_sync.topology.diagnostics.size());
            apply_node_sync_rows(output_nodes, director_sync.policy,
                                 director_sync.topology,
                                 director_sync.status);
            auto editor_frame =
                editor->frame(scene_frame, timeline, keyframes, elements,
                              effects, hardware_map, feed_map, switcher_view,
                              asset_registry, compositor_graph, project_view,
                              output_monitor, hardware_runtime,
                              director_sync.policy);
            if (editor_frame.output_lifecycle.has_value() &&
                lifecycle_controller) {
                lifecycle_controller->submit(
                    *editor_frame.output_lifecycle);
            }
            if (editor_frame.status == editor::EditorStatus::Closed) {
                closed = true;
            }
            // Coalesce to the latest edit per node before the next boundary
            // authors them, so a drag is one command per frame, not per event.
            for (const auto& edit : editor_frame.edits) {
                bool merged = false;
                for (auto& queued : pending_edits) {
                    if (queued.node == edit.node) {
                        queued = edit;
                        merged = true;
                        break;
                    }
                }
                if (!merged) {
                    try {
                        pending_edits.push_back(edit);
                    } catch (const std::bad_alloc&) {
                        std::cerr << "could not queue a scene edit\n";
                        return 1;
                    }
                }
            }
            if (editor_frame.undo_transaction != 0U) {
                pending_undo = editor_frame.undo_transaction;
            }
            if (editor_frame.seek_to_frame) {
                pending_seek = editor_frame.seek_to_frame;
            }
            if (editor_frame.range) {
                pending_range = editor_frame.range;
            }
            if (editor_frame.show_rate) {
                pending_show_rate = editor_frame.show_rate;
            }
            for (const auto& key : editor_frame.keys) {
                try {
                    pending_keys.push_back(key);
                } catch (const std::bad_alloc&) {
                    std::cerr << "could not queue a keyframe\n";
                    return 1;
                }
            }
            for (const auto& moved : editor_frame.key_moves) {
                try {
                    pending_key_moves.push_back(moved);
                } catch (const std::bad_alloc&) {
                    std::cerr << "could not queue a keyframe move\n";
                    return 1;
                }
            }
            for (const auto& edit : editor_frame.property_edits) {
                try {
                    pending_properties.push_back(edit);
                } catch (const std::bad_alloc&) {
                    std::cerr << "could not queue an inspector edit\n";
                    return 1;
                }
            }
            for (const auto& edit : editor_frame.generic_edits) {
                try {
                    pending_generics.push_back(edit);
                } catch (const std::bad_alloc&) {
                    std::cerr << "could not queue an inspector edit\n";
                    return 1;
                }
            }
            for (const auto& add : editor_frame.adds) {
                try {
                    pending_adds.push_back(add);
                } catch (const std::bad_alloc&) {
                    std::cerr << "could not queue an element add\n";
                    return 1;
                }
            }
            for (const auto& imported : editor_frame.asset_imports) {
                try {
                    pending_asset_imports.push_back(imported);
                } catch (const std::bad_alloc&) {
                    std::cerr << "could not queue an asset import\n";
                    return 1;
                }
            }
            for (const auto& request : editor_frame.asset_deletes) {
                try {
                    pending_asset_deletes.push_back(request);
                } catch (const std::bad_alloc&) {
                    std::cerr << "could not queue an asset removal\n";
                    return 1;
                }
            }
            for (const auto& effect : editor_frame.effects) {
                try {
                    pending_effects.push_back(effect);
                } catch (const std::bad_alloc&) {
                    std::cerr << "could not queue an effect\n";
                    return 1;
                }
            }
            for (const auto& effect : editor_frame.effect_deletes) {
                try {
                    pending_effect_deletes.push_back(effect);
                } catch (const std::bad_alloc&) {
                    std::cerr << "could not queue an effect removal\n";
                    return 1;
                }
            }
            if (editor_frame.switcher.has_value()) {
                pending_switcher = editor_frame.switcher;
            }
            try {
                pending_hardware_places.insert(
                    pending_hardware_places.end(),
                    editor_frame.hardware_places.begin(),
                    editor_frame.hardware_places.end());
                pending_hardware_moves.insert(
                    pending_hardware_moves.end(),
                    editor_frame.hardware_moves.begin(),
                    editor_frame.hardware_moves.end());
                pending_hardware_links.insert(
                    pending_hardware_links.end(),
                    editor_frame.hardware_links.begin(),
                    editor_frame.hardware_links.end());
                pending_hardware_deletes.insert(
                    pending_hardware_deletes.end(),
                    editor_frame.hardware_deletes.begin(),
                    editor_frame.hardware_deletes.end());
                pending_hardware_properties.insert(
                    pending_hardware_properties.end(),
                    editor_frame.hardware_property_edits.begin(),
                    editor_frame.hardware_property_edits.end());
                pending_hardware_output_binding_edits.insert(
                    pending_hardware_output_binding_edits.end(),
                    editor_frame.hardware_output_binding_edits.begin(),
                    editor_frame.hardware_output_binding_edits.end());
                pending_hardware_output_binding_deletes.insert(
                    pending_hardware_output_binding_deletes.end(),
                    editor_frame.hardware_output_binding_deletes.begin(),
                    editor_frame.hardware_output_binding_deletes.end());
                pending_feed_map_adds.insert(
                    pending_feed_map_adds.end(),
                    editor_frame.feed_map_adds.begin(),
                    editor_frame.feed_map_adds.end());
                pending_feed_map_edits.insert(
                    pending_feed_map_edits.end(),
                    editor_frame.feed_map_edits.begin(),
                    editor_frame.feed_map_edits.end());
                pending_feed_map_deletes.insert(
                    pending_feed_map_deletes.end(),
                    editor_frame.feed_map_deletes.begin(),
                    editor_frame.feed_map_deletes.end());
                pending_feed_projection_edits.insert(
                    pending_feed_projection_edits.end(),
                    editor_frame.feed_projection_edits.begin(),
                    editor_frame.feed_projection_edits.end());
                pending_feed_surface_adds.insert(
                    pending_feed_surface_adds.end(),
                    editor_frame.feed_surface_adds.begin(),
                    editor_frame.feed_surface_adds.end());
                pending_feed_surface_edits.insert(
                    pending_feed_surface_edits.end(),
                    editor_frame.feed_surface_edits.begin(),
                    editor_frame.feed_surface_edits.end());
                pending_feed_surface_deletes.insert(
                    pending_feed_surface_deletes.end(),
                    editor_frame.feed_surface_deletes.begin(),
                    editor_frame.feed_surface_deletes.end());
            } catch (const std::bad_alloc&) {
                std::cerr << "could not queue a mapping edit\n";
                return 1;
            }
            if (editor_frame.hardware_control_port_edit.has_value()) {
                pending_hardware_control_port =
                    editor_frame.hardware_control_port_edit;
            }
            if (editor_frame.sync_policy_edit.has_value()) {
                pending_sync_policy = editor_frame.sync_policy_edit;
            }
            if (editor_frame.compositor_add.has_value()) {
                pending_compositor_add = editor_frame.compositor_add;
                pending_compositor_gesture =
                    editor_frame.compositor_gesture;
            }
            if (editor_frame.compositor_move.has_value()) {
                pending_compositor_move = editor_frame.compositor_move;
                pending_compositor_gesture =
                    editor_frame.compositor_gesture;
            }
            if (editor_frame.compositor_connect.has_value()) {
                pending_compositor_connect = editor_frame.compositor_connect;
                pending_compositor_gesture =
                    editor_frame.compositor_gesture;
            }
            if (editor_frame.compositor_property_edit.has_value()) {
                pending_compositor_property =
                    editor_frame.compositor_property_edit;
                pending_compositor_gesture =
                    editor_frame.compositor_gesture;
            }
            if (editor_frame.compositor_delete.has_value()) {
                pending_compositor_delete = editor_frame.compositor_delete;
                pending_compositor_gesture =
                    editor_frame.compositor_gesture;
            }
            if (!editor_frame.deletes.empty()) {
                try {
                    pending_deletes.insert(
                        pending_deletes.end(),
                        editor_frame.deletes.begin(),
                        editor_frame.deletes.end());
                    pending_delete_gesture = editor_frame.delete_gesture;
                } catch (const std::bad_alloc&) {
                    std::cerr << "could not queue an element delete\n";
                    return 1;
                }
            }
            if (editor_frame.transport != editor::TransportRequest::None) {
                try {
                    operator_commands.push_back(
                        map_transport(editor_frame.transport));
                } catch (const std::bad_alloc&) {
                    std::cerr << "could not queue a transport command\n";
                    return 1;
                }
            }
            if (editor_frame.load_project.has_value()) {
                pending_project_save.reset();
                // An explicit Open supersedes the startup recent-project
                // candidate, including its already-decoded Root. Never pair a
                // newly chosen path with stale preloaded project contents.
                pending_preloaded_project.reset();
                pending_project_load =
                    std::move(editor_frame.load_project);
                project_message = "LOADING PROJECT...";
            } else if (editor_frame.save_project.has_value()) {
                // Explicit Save means "save this current project", not "load
                // the remembered project at the next boundary first".
                pending_project_load.reset();
                pending_preloaded_project.reset();
                pending_project_save =
                    std::move(editor_frame.save_project);
                project_message = "SAVING PROJECT...";
            }
        }
#endif
        if (!handled) {
            if (render_scene && switcher_progress.active) {
                status = presenter->present(
                    transition_from_frame, transition_to_frame,
                    static_cast<float>(switcher_progress.mix()),
                    compositor_graph, scene::whole(scene_frame.canvas));
            } else {
                status = render_scene
                    ? presenter->present(
                          scene_frame, compositor_graph,
                          scene::whole(scene_frame.canvas))
                    : presenter->present(input);
            }
        }

        // Evaluated for its digest only, so the Director's status line can show
        // it agrees with the nodes' feed maps.
        if (options.output) {
            const auto& document = scheduler.sealed_document();
            if (!feed_loaded || document.version().value() != feed_version) {
                feed_model.reset(document);
                feed_version = document.version().value();
                feed_loaded = true;
            }
            feed_map = feed_model.evaluate(scene::SceneTime{
                input.show_frame.value(), input.rate.numerator,
                input.rate.denominator});
            if (const ResolvedProjectAssets* const director_overlay =
                    director_asset_resolution.current_overlay()) {
                director_overlay->apply(feed_map);
            }
        }

        switch (status) {
        case show::PresenterStatus::Closed:
            closed = true;
            break;
        case show::PresenterStatus::Failed:
            ++present_failures;
            break;
        case show::PresenterStatus::Presented:
        case show::PresenterStatus::Skipped:
            break;
        }

        if (presenter != nullptr) {
            for (auto command = presenter->take_command();
                 command != show::PresenterCommand::None;
                 command = presenter->take_command()) {
                try {
                    operator_commands.push_back(command);
                } catch (const std::bad_alloc&) {
                    std::cerr << "could not queue an operator transport command\n";
                    return 1;
                }
            }
        }

        since_status += sealed_this_iteration;
        if (sealed_this_iteration != 0U && since_status >= interval) {
            since_status = 0U;
            ClusterObservation cluster =
                observe_cluster(input.show_frame.value());
            cluster.diagnostics_recording = cluster_diagnostics.available();
            static_cast<void>(cluster_diagnostics.append(cluster));
            std::cout << "director show-frame=" << input.show_frame.value()
                      << " control-frame=" << control_frame.value()
                      << " transport=" << transport_label(transport.state())
                      << " sealed=" << sealed << " anchors=" << anchors
                      << " pacer-lag=" << cluster.pacer_lag_frames
                      << " tracked=" << tracked.size()
                      << " sessions=" << hub.active_subscriptions()
                      << " live=" << cluster.live_sessions
                      << " catching-up=" << cluster.catching_up_sessions
                      << " release-lag=" << cluster.release_lag_frames
                      << " heartbeat-timeouts="
                      << cluster.sessions.heartbeat_timeouts
                      << " released-through="
                      << barrier.released_through().through_frame.value();
            // The same scene digest the nodes report. Printed here so an
            // operator can confirm the Director and every node agree on the
            // replicated scene at the same show frame.
            if (needs_scene) {
                std::cout << " digest=" << scene::digest(scene_frame);
            }
            // And the same feed-map digest, so the operator can confirm the map
            // itself is replicated, not just the scene.
            if (options.output) {
                std::cout << " feed=" << output::digest(feed_map);
            }
            if (presenter != nullptr) {
                std::cout << gpu_timing_text(presenter->timing());
            }
            std::cout << '\n' << std::flush;
        }

        if (options.run_frames != 0U && sealed >= options.run_frames) {
            break;
        }
        sleep_until_boundary(pacer.next_boundary_at());
    }

    ClusterObservation final_cluster = observe_cluster(
        transport.frame_at(scheduler.sealed_through()).value());
    final_cluster.diagnostics_recording = cluster_diagnostics.available();
    static_cast<void>(cluster_diagnostics.append(final_cluster));
    std::cout << "director stopping"
              << " show-frame="
              << transport.frame_at(scheduler.sealed_through()).value()
              << " control-frame=" << scheduler.sealed_through().value()
              << " transport=" << transport_label(transport.state())
              << " sealed=" << sealed << " anchors=" << anchors
              << " pacer-lag=" << final_cluster.pacer_lag_frames
              << " tracked=" << tracked.size()
              << " present-failures=" << present_failures
              << " surface="
              << (options.edit ? "editor"
                               : (presenter && presenter->windowed() ? "window"
                                                                     : "headless"))
              << " released-through="
              << barrier.released_through().through_frame.value() << '\n'
              << std::flush;
    return 0;
}

} // namespace mediaserver::app
