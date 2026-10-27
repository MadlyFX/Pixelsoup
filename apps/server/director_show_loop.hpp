#pragma once

// The Director's frame loop, split into named phases.
//
// run_director_show owns every piece of show state and builds it in the order
// start-up needs. DirectorShowLoop names that state by reference, so each
// phase below reads and writes exactly what the single loop body used to, and
// the phases run in the order the body ran them: pace, seal (operator input is
// authored onto the first sealed frame), evaluate the committed frame, refresh
// the editor, present, report. Values that only live for one iteration are in
// DirectorTick; the editor projection's inputs are in EditorViewInputs.

#include "camera_prediction.hpp"
#include "clip_clipboard.hpp"
#include "cluster_observability.hpp"
#include "edit_coalescing_policy.hpp"
#include "key_clipboard.hpp"
#include "playback.hpp"
#include "playback_media.hpp"
#include "playback_runtime.hpp"
#include "project_assets.hpp"
#include "proxy_playback.hpp"
#include "renderer_lockstep.hpp"
#include "tracker_listener.hpp"

#include <mediaserver/assets/asset_model.hpp>
#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/compositor/compositor_model.hpp>
#include <mediaserver/core/asset_publication.hpp>
#include <mediaserver/net/director_frame_barrier.hpp>
#include <mediaserver/net/director_session_hub.hpp>
#include <mediaserver/net/director_session_service.hpp>
#include <mediaserver/output/feed_map_model.hpp>
#include <mediaserver/persist/command_log.hpp>
#include <mediaserver/persist/durable_director.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/selection_set.hpp>
#include <mediaserver/show/presenter.hpp>
#include <mediaserver/show/show_clock.hpp>
#include <mediaserver/show/switcher_state.hpp>
#include <mediaserver/show/test_pattern.hpp>
#include <mediaserver/show/transport_authority.hpp>
#include <mediaserver/show/transport_state.hpp>
#include <mediaserver/sync/sync_policy_model.hpp>
#include <mediaserver/sync/sync_readiness.hpp>
#include <mediaserver/sync/sync_status.hpp>
#include <mediaserver/topology/hardware_map_model.hpp>
#include <mediaserver/topology/hardware_runtime.hpp>

#if defined(MEDIASERVER_HAS_GUI)
#include "blender_links.hpp"
#include "cluster_lifecycle_controller.hpp"
#include "director_live_data.hpp"
#include "thumbnail_service.hpp"
#include <mediaserver/editor/editor_shell.hpp>
#include <mediaserver/net/cluster_discovery.hpp>
#include <mediaserver/render/blender_discovery.hpp>
#include <mediaserver/render/linked_publication.hpp>
#include <mediaserver/render/linked_scene_ingest.hpp>
#include <mediaserver/render/media_source.hpp>
#include <mediaserver/render/render_conformance.hpp>
#endif

#include <chrono>
#include <csignal>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace mediaserver::app {

#if defined(MEDIASERVER_HAS_GUI)
// How often the editor is redrawn when the show itself does not ask sooner.
constexpr auto editor_refresh_interval = std::chrono::nanoseconds{16'666'667};

struct PendingClipPaste final {
    editor::ClipClipboardRequest request;
    ClipClipboard snapshot;
};

struct PendingKeyPaste final {
    editor::KeyClipboardRequest request;
    KeyClipboard snapshot;
};
#endif

// The latest evaluated physical-timing picture. It is refreshed once per
// Director tick and read by both the operator surface and the diagnostic
// stream, so those two can never disagree about what was proven.
struct DirectorSyncView final {
    sync::SyncPolicy policy{};
    sync::SyncTopology topology{};
    sync::SyncStatusSnapshot status{};
    sync::SyncReadiness readiness{};
};

// Values one loop iteration derives and several phases read: the pacing
// decision, then the committed frame the Director evaluated after sealing.
struct DirectorTick final {
    Clock::time_point now{};
    core::u64 due{};
    core::u64 sealed_this_iteration = 0U;
    std::optional<show::FrameRate> repace_after_seal;

    cmd::FrameNumber control_frame{};
    show::TestPatternInput input{};
    show::ShowFrame stage_show_frame{};
    bool stage_playing{};
    bool render_scene{};
    bool needs_scene{};
    scene::Scene scene_frame;
    core::u64 authored_scene_digest{};
    scene::Scene stage_scene_frame;
    scene::Scene transition_from_frame;
    scene::Scene transition_to_frame;
    std::vector<scene::SceneInfo> scene_infos;
    std::optional<show::SwitcherAnchor> switcher_anchor;
    show::SwitcherProgress switcher_progress{};
    // A Loop cue's crossfade on Program, mixed through the same frames
    // as a switcher dissolve without being one.
    std::optional<show::TransportBlend> loop_blend;
    output::FeedMap feed_map;
    topology::HardwareMap hardware_map;
    ProjectAssetManifest asset_manifest;
    // The graph of the scene on the Stage - the one the Compositor panel
    // edits. Program renders its own, which is the same object except while
    // Preview is showing a different scene.
    compositor::CompositorGraph compositor_graph;
    compositor::CompositorGraph program_compositor_graph;
    compositor::CompositorGraph transition_from_graph;
    compositor::CompositorGraph transition_to_graph;
    scene::SceneId stage_scene{};
    show::PresenterStatus status{show::PresenterStatus::Skipped};
    bool handled = false;
    bool fast_show{};
};

// What operator input is authored against on the first frame of a batch.
struct OperatorFrame final {
    cmd::FrameNumber frame{};
    core::u64 through{};
    bool project_loaded_this_frame = false;
    // The switcher as authored before any of this frame's edits.
    std::optional<show::SwitcherAnchor> projected_switcher;
    bool scene_edits_locked = false;
};

#if defined(MEDIASERVER_HAS_GUI)
// Everything one editor refresh hands to EditorShell::frame. The shell
// borrows these (and views into them) for the duration of the call, so they
// live together until it returns.
struct EditorViewInputs final {
    editor::EditorTimeline timeline{};
    std::vector<scene::ClipPlacement> stage_clips;
    std::vector<scene::TimelineLane> stage_lanes;
    std::vector<scene::TimelineMarker> stage_markers;
    std::vector<scene::SelectionSetSummary> selection_set_summaries;
    std::vector<scene::TrackingSource> authored_tracking_sources;
    std::vector<editor::ElementInfo> elements;
    std::vector<scene::EffectInstance> effects;
    std::vector<scene::ChannelKeyframes> keyframes;
    editor::EditorSwitcherState switcher_view{};
    std::shared_ptr<const render::BlenderDiscoveryState> blender_state;
    std::string blender_preview_instance;
    editor::EditorProjectState project_view{};
    std::vector<editor::OutputMonitorState::Node> output_nodes;
    topology::HardwareRuntimeSnapshot hardware_runtime;
    scene::RendererSettings renderer{};
    editor::OutputMonitorState output_monitor{};
    editor::ProjectSettingsState project_settings_view{};
    std::vector<editor::LibraryThumbnailObservation> thumbnail_observations;
    std::vector<editor::TimelineFilmstripObservation> filmstrip_observations;
};
#endif

enum class PaceOutcome : core::u8 {
    Ready,
    Retry,
    Failed,
};

// A phase that can end the show returns the process exit code it ends with.
using PhaseExit = std::optional<int>;

struct DirectorShowLoop final {
    cmd::DirectorScheduler &scheduler;
    persist::CommandLog &command_log;
    persist::FrameDurability &frame_durability;
    net::DirectorSessionHub &hub;
    net::DirectorFrameBarrier &barrier;
    net::DirectorSessionService *const session_service;
    const PlaybackOptions &options;
    const volatile std::sig_atomic_t &stop_requested;

    std::unique_ptr<show::Presenter> &presenter;
    std::unique_ptr<ProxyGenerationWorker> &director_proxy_generator;
    ProxyPlaybackService &director_proxy_playback;
    std::vector<ProxyPlacementObservation> &director_proxy_observations;
    server::TrackerListenerPool &tracker_listeners;
    TrackedCameraChannels &director_pose_channels;
    server::DirectorPosePredictors &director_pose_predictors;
    core::u64 &director_poses_sent;
    core::u64 &last_replicated_pose_frame;
#if defined(MEDIASERVER_HAS_GUI)
    std::unique_ptr<editor::EditorShell> &editor;
    std::unique_ptr<ClusterLifecycleController> &lifecycle_controller;
    std::unique_ptr<net::ClusterDiscoveryBrowser> &discovery_browser;
    std::error_code &discovery_error;
    std::unique_ptr<server::ThumbnailService> &thumbnail_service;
    std::vector<assets::DerivativeKey> &retained_thumbnail_keys;
    std::vector<assets::DerivativeKey> &retained_filmstrip_keys;
    std::vector<editor::LibraryProxyRuntimeObservation> &library_proxy_runtime;
    Clock::time_point &next_output_media_projection;
    Clock::time_point &next_editor_refresh;
    std::optional<cmd::FrameNumber> &last_editor_control_frame;
    core::u64 &editor_frame_calls;
    std::optional<std::filesystem::path> &workspace_layout_file;
#endif
    show::ShowClock &pacer;
    sync::DirectorClockStatus &director_clock;
    bool &director_reference_seen;
    core::u32 &director_last_reference_frame;
    show::TransportAuthority &transport;
    std::deque<net::DirectorFrameBarrierId> &tracked;
    std::vector<show::PresenterCommand> &operator_commands;
#if defined(MEDIASERVER_HAS_GUI)
    std::vector<editor::EditIntent> &pending_edits;
    std::vector<editor::WorldEdit> &pending_world_edits;
    std::optional<editor::HistoryRequest> &pending_history;
    std::optional<core::u64> &pending_seek;
    std::optional<core::u64> &pending_preview_seek;
    std::vector<show::PresenterCommand> &preview_commands;
    std::optional<editor::RangeRequest> &pending_range;
    std::optional<editor::ShowRateRequest> &pending_show_rate;
    std::vector<editor::KeyIntent> &pending_keys;
    std::vector<editor::KeyMove> &pending_key_moves;
    std::optional<editor::KeyDuplicateRequest> &pending_key_duplicate;
    std::optional<editor::KeyBatchRequest> &pending_key_batch;
    std::optional<editor::TimelineKeyScalePreview> &key_scale_preview;
    std::optional<editor::ProjectSettingsEdit> &pending_project_settings;
    std::vector<editor::ClipTimingEdit> &pending_clip_timings;
    std::vector<editor::ClipSplitIntent> &pending_clip_splits;
    std::vector<editor::TimelineLaneEditIntent> &pending_lane_edits;
    std::vector<editor::TimelineMarkerEditIntent> &pending_marker_edits;
    std::optional<bool> &pending_cues_armed;
    std::vector<editor::TimelineLayerOrderIntent> &pending_layer_orders;
    std::vector<editor::ElementFlagIntent> &pending_element_flags;
    std::vector<editor::ElementRenameIntent> &pending_renames;
    std::vector<editor::ElementHighlightIntent> &pending_highlights;
    bool &pending_asset_rescan;
    std::vector<editor::TimelineEditIntent> &pending_timeline_edits;
    std::optional<editor::GroupEditIntent> &pending_group_edit;
    std::optional<editor::SelectionSetEditIntent> &pending_selection_set_edit;
    std::optional<editor::MaskEditIntent> &pending_mask_edit;
    ClipClipboard &clip_clipboard;
    std::optional<doc::Root> &linked_clipboard;
    std::optional<editor::ClipClipboardRequest> &pending_linked_paste;
    std::optional<PendingClipPaste> &pending_clip_paste;
    KeyClipboard &key_clipboard;
    std::optional<PendingKeyPaste> &pending_key_paste;
    std::vector<editor::PropertyEdit> &pending_properties;
    std::vector<editor::GenericEdit> &pending_generics;
    std::vector<editor::LockEdit> &pending_locks;
    std::vector<editor::AddIntent> &pending_adds;
    std::vector<editor::AssetImportIntent> &pending_asset_imports;
    std::vector<editor::AssetRevisionIntent> &pending_asset_revisions;
    std::vector<editor::AssetDeleteIntent> &pending_asset_deletes;
    std::vector<editor::AssetProxyEditIntent> &pending_asset_proxy_edits;
    std::vector<editor::AssetLibraryEditIntent> &pending_asset_library_edits;
    std::vector<editor::SourceReplacementIntent> &pending_source_replacements;
    std::vector<editor::EffectIntent> &pending_effects;
    std::vector<editor::FilmGrainEffectEditIntent> &pending_film_grain_effect_edits;
    std::vector<editor::EffectDeleteIntent> &pending_effect_deletes;
    std::optional<editor::SwitcherRequest> &pending_switcher;
    std::vector<editor::HardwarePlaceRequest> &pending_hardware_places;
    std::vector<editor::HardwareMoveRequest> &pending_hardware_moves;
    std::vector<editor::HardwareLinkRequest> &pending_hardware_links;
    std::vector<editor::HardwarePropertyEdit> &pending_hardware_properties;
    std::optional<editor::HardwareControlPortEdit> &pending_hardware_control_port;
    std::optional<editor::HardwareCanvasEdit> &pending_hardware_canvas;
    std::optional<editor::HardwareSyncPolicyEdit> &pending_sync_policy;
    std::vector<editor::HardwareOutputBindingEdit> &pending_hardware_output_binding_edits;
    std::vector<editor::HardwareOutputBindingDelete> &pending_hardware_output_binding_deletes;
    std::vector<editor::HardwareDeleteRequest> &pending_hardware_deletes;
    std::vector<editor::FeedMapAddRequest> &pending_feed_map_adds;
    std::vector<editor::FeedMapRectEdit> &pending_feed_map_edits;
    std::vector<editor::FeedMapDeleteRequest> &pending_feed_map_deletes;
    std::vector<editor::FeedProjectionEdit> &pending_feed_projection_edits;
    std::vector<editor::FeedOrganizeRequest> &pending_feed_organize;
    std::vector<editor::FeedSurfaceAddRequest> &pending_feed_surface_adds;
    std::vector<editor::FeedSurfaceEditRequest> &pending_feed_surface_edits;
    std::vector<editor::FeedSurfaceDeleteRequest> &pending_feed_surface_deletes;
    std::vector<editor::CompositorAddIntent> &pending_compositor_adds;
    std::vector<editor::CompositorMoveIntent> &pending_compositor_moves;
    std::optional<editor::CompositorClipboardIntent> &pending_compositor_clipboard;
    std::optional<editor::CompositorRenameIntent> &pending_compositor_rename;
    std::optional<editor::CompositorCommentIntent> &pending_compositor_comment;
    std::optional<editor::CompositorCollapseIntent> &pending_compositor_collapse;
    std::optional<editor::CompositorBypassIntent> &pending_compositor_bypass;
    std::optional<editor::CompositorReorderIntent> &pending_compositor_reorder;
    // The Compositor's copied nodes, wires and comment boxes: Director-local,
    // never replicated, and cleared when another project is loaded.
    std::vector<doc::Node> &compositor_clipboard;
    std::optional<editor::CompositorConnectIntent> &pending_compositor_connect;
    std::optional<editor::CompositorRewireIntent> &pending_compositor_rewire;
    std::optional<editor::CompositorWaypointIntent> &pending_compositor_waypoints;
    std::optional<editor::CompositorPropertyEditIntent> &pending_compositor_property;
    std::optional<editor::CompositorDeleteIntent> &pending_compositor_delete;
    core::u64 &pending_compositor_gesture;
    std::vector<doc::NodeId> &pending_deletes;
    core::u64 &pending_delete_gesture;
    std::optional<editor::ProjectFileRequest> &pending_project_save;
    std::optional<editor::ProjectFileRequest> &pending_project_load;
    EditCoalescingPolicy &editor_coalescing_policy;
    std::map<core::u64, EditCoalescingPlan> &pending_editor_coalescing;
    bool &pending_editor_coalescing_close;
    std::optional<doc::Root> &pending_preloaded_project;
    std::optional<std::filesystem::path> &recent_project_settings_file;
    std::vector<std::string> &recent_project_paths;
    std::string &project_path;
    std::string &project_message;
    doc::Root &saved_project_snapshot;
    bool &project_explicitly_dirty;
    core::u64 &project_generation;
    render::LinkedSceneIngest &linked_ingest;
    BlenderLinks &blender_links;
    std::string &blender_store_project;
    core::u64 &blender_link_transactions;
    std::string &unsaved_blender_name;
    std::deque<BlenderLinks::Created> &blender_scene_requests;
    std::optional<BlenderLinks::Created> &blender_scene_creating;
    std::vector<BlenderAssociation> &blender_associations;
    doc::Root &blender_association_root;
    std::string &deferred_blender_connect;
    std::string &deferred_blender_resync;
    std::optional<scene::SceneId> &deferred_blender_open;
    std::string &blender_connection_error;
    render::LinkedPublication &linked_publication;
    render::PublicationVerification &director_publication_verification;
    std::optional<std::string> &pending_publication_storage;
    bool &pending_publication;
    bool &pending_publication_cancel;
    bool &pending_publication_rollback;
    bool &publication_staging;
    core::u64 &publication_project;
    core::u64 &publication_activation;
    doc::NodeId &publication_program;
    std::string &publication_message;
    std::string &publication_storage;
    std::string &publication_message_shown;
    std::string &scene_link_details;
    std::string &scene_link_diagnostics;
    std::vector<BlenderLinks::View> &blender_views;
    std::vector<scene::SceneInfo> &listed_scenes;
    std::vector<editor::BlenderSceneState> &blender_scene_states;
    std::vector<std::pair<std::string, std::string>> &blender_bound_names;
    std::string &publication_default_storage;
    doc::Root &linked_ingest_base;
    scene::SceneId &linked_ingest_target;
    core::u64 &linked_ingest_project;
    core::u64 &linked_ingest_gesture;
    core::u64 &next_new_node;
#endif
    bool &rehearsal_mode;
    std::optional<show::TransportAuthority> &preview_transport;
    std::optional<show::TransportAnchor> &authored;
    core::u64 &interval;
    core::u64 &sealed;
    core::u64 &since_status;
    core::u64 &anchors;
    core::u64 &present_failures;
    core::u32 &configured_cluster_nodes;
    bool &cluster_configuration_authored;
    ClusterDiagnosticLog &cluster_diagnostics;
    DirectorSyncView &director_sync;
    std::vector<editor::NodeTrackingObservation> &director_node_tracking;
    sync::SyncPolicyModel &director_sync_model;
    core::u64 &director_sync_version;
    bool &director_sync_loaded;
    net::RuntimeRendererReadiness &observed_local_renderer;
    server::RendererLockstep &director_renderer_lockstep;
    core::u64 &conformance_request;
#if defined(MEDIASERVER_HAS_GUI)
    render::RenderConformanceService &director_conformance;
#endif
    std::optional<std::pair<doc::NodeId, core::u64>> &pending_asset_commit;
    core::u64 &last_asset_commit_frame;
    core::u64 &last_asset_commit_digest;
    doc::NodeId &last_asset_commit_stage;
    core::AssetCohort &director_asset_cohort;
    std::optional<core::u32> &asset_waiting_node;
    core::AssetCohort &logged_asset_cohort;
    bool &autoplay_pending;
    bool &closed;
    scene::SceneModel &scene_model;
    core::u64 &scene_version;
    bool &scene_loaded;
    std::optional<core::u64> &cue_version;
    bool &cued_preview;
    show::FrameRate &cued_rate;
    output::FeedMapModel &feed_model;
    core::u64 &feed_version;
    bool &feed_loaded;
    topology::HardwareMapModel &hardware_model;
    core::u64 &hardware_version;
    bool &hardware_loaded;
    assets::AssetModel &asset_model;
    assets::AssetRegistry &director_asset_registry;
    assets::AssetRegistry &director_playback_asset_registry;
    core::u64 &asset_version;
    core::u64 &asset_registry_digest;
    core::u64 &asset_manifest_digest;
    bool &assets_loaded;
    ProjectAssetAvailability &director_asset_availability;
    // Library sources found changed by the last RESCAN, and those of them
    // the document still records at their old revision.
    std::vector<ChangedLibrarySource> &changed_library_sources;
    std::vector<doc::NodeId> &changed_library_assets;
#if defined(MEDIASERVER_HAS_GUI)
    editor::EditorPreferencesState &preferences_view;
    std::string &stored_password_key;
#endif
    compositor::CompositorModel &compositor_model;
    core::u64 &compositor_version;
    bool &compositor_loaded;

    // Owned by the loop rather than bound to playback.cpp locals.
    show::FrameWaiter frame_waiter{};
    show::BoundaryLateness boundary_lateness{};
    ControlBacklogWatch backlog_watch{};
    core::u64 reported_lockstep_demotions{};
#if defined(MEDIASERVER_HAS_GUI)
    // The Timeline's cache bar. A placement off air has no evaluated media
    // to say which file it decodes, so the last one seen on air is kept.
    struct TimelineDecodeSource final {
        std::string document_source;
        std::string decode_source;
        core::i64 offset{};
    };
    std::map<doc::NodeId, TimelineDecodeSource> timeline_decode_sources{};
    // The newest decode-residency snapshot, held while the scheduler is busy.
    std::vector<std::string> timeline_ready_sources{};
    std::vector<render::MediaReadyFrames> timeline_ready_frames{};
#endif

    // Runs until stopped, closed or out of run frames; returns the exit code.
    [[nodiscard]] int run();

    // director_show_loop.cpp: pacing, sealing and the shared evaluations.
    [[nodiscard]] PaceOutcome pace(DirectorTick &tick);
    [[nodiscard]] PhaseExit seal_due_frames(DirectorTick &tick);
    [[nodiscard]] PhaseExit apply_operator_input(DirectorTick &tick,
                                                 cmd::FrameNumber frame,
                                                 core::u64 through);
    [[nodiscard]] show::VideoReferenceSample
    read_director_video_reference() noexcept;
    [[nodiscard]] server::RendererLockstep
    evaluate_renderer_lockstep(const scene::RendererSettings &source,
                               const topology::HardwareMap &hardware) noexcept;
    [[nodiscard]] sync::SyncReadiness evaluate_sync_gate(bool assets_adopted,
                                                         bool renderer_ready);
    [[nodiscard]] bool program_sync_held() noexcept;
    [[nodiscard]] ClusterObservation observe_cluster(core::u64 show_frame);

    // director_frame_evaluation.cpp: the committed frame after sealing.
    void begin_frame(DirectorTick &tick);
    [[nodiscard]] PhaseExit evaluate_scene(DirectorTick &tick);
    void evaluate_feed_and_sync(DirectorTick &tick);
    void observe_asset_cohort(DirectorTick &tick);
    [[nodiscard]] PhaseExit present_output(DirectorTick &tick);

    // director_show_status.cpp: console status lines.
    void print_status(DirectorTick &tick);
    void report_backlog(ControlBacklogEvent event);
    void report_lockstep_demotions();
    void report_stopping();

#if defined(MEDIASERVER_HAS_GUI)
    // director_operator_input.cpp: project, transport and switcher requests.
    [[nodiscard]] EditCoalescingPlan *coalescing_for(core::u64 gesture);
    void remember_recent_project(std::string_view path);
    void clear_pending_project_authoring();
    void enter_preview(cmd::FrameNumber at);
    void leave_preview() noexcept;
    [[nodiscard]] doc::NodeId scene_being_authored(const OperatorFrame &op);
    [[nodiscard]] PhaseExit apply_project_load(DirectorTick &tick,
                                               OperatorFrame &op);
    void apply_preview_transport(OperatorFrame &op);
    void prepare_authoring_context(OperatorFrame &op);
    [[nodiscard]] PhaseExit apply_switcher_request(OperatorFrame &op);
    [[nodiscard]] PhaseExit apply_show_timing_requests(DirectorTick &tick,
                                                       OperatorFrame &op);
    [[nodiscard]] PhaseExit apply_history_request(OperatorFrame &op);
    [[nodiscard]] PhaseExit apply_project_save(OperatorFrame &op);

    // director_linked_scenes.cpp: publication and Blender links.
    [[nodiscard]] PhaseExit apply_publication_requests(OperatorFrame &op);
    [[nodiscard]] PhaseExit apply_blender_links(OperatorFrame &op);

    // director_operator_edits.cpp: queued editor edits, by domain.
    [[nodiscard]] PhaseExit apply_scene_edits(OperatorFrame &op);
    [[nodiscard]] PhaseExit apply_asset_imports(OperatorFrame &op);
    [[nodiscard]] PhaseExit apply_key_requests(OperatorFrame &op);
    [[nodiscard]] PhaseExit apply_clip_requests(OperatorFrame &op);
    [[nodiscard]] PhaseExit apply_element_and_lane_requests(OperatorFrame &op);
    [[nodiscard]] PhaseExit apply_group_selection_and_mask_requests(
        OperatorFrame &op);
    [[nodiscard]] PhaseExit apply_timeline_edits(OperatorFrame &op);
    [[nodiscard]] PhaseExit apply_property_and_effect_requests(
        OperatorFrame &op);
    [[nodiscard]] PhaseExit apply_hardware_requests(OperatorFrame &op);
    [[nodiscard]] PhaseExit apply_feed_map_requests(OperatorFrame &op);
    [[nodiscard]] PhaseExit apply_compositor_and_delete_requests(
        OperatorFrame &op);

    // director_editor_view.cpp: the editor projection and its results.
    [[nodiscard]] PhaseExit refresh_editor(DirectorTick &tick);
    [[nodiscard]] PhaseExit project_stage_contents(DirectorTick &tick,
                                                   EditorViewInputs &inputs);
    void project_switcher_and_links(DirectorTick &tick, EditorViewInputs &inputs);
    [[nodiscard]] PhaseExit project_output_monitor(DirectorTick &tick,
                                                   EditorViewInputs &inputs);
    void project_stage_settings(DirectorTick &tick, EditorViewInputs &inputs);
    [[nodiscard]] PhaseExit observe_library_thumbnails(EditorViewInputs &inputs);
    [[nodiscard]] PhaseExit
    observe_timeline_filmstrips(EditorViewInputs &inputs);
    void observe_timeline_playback(DirectorTick &tick, EditorViewInputs &inputs);
    [[nodiscard]] PhaseExit project_output_media(DirectorTick &tick);
    [[nodiscard]] PhaseExit
    apply_editor_services(EditorViewInputs &inputs,
                          editor::EditorFrame &editor_frame);
    [[nodiscard]] PhaseExit
    request_library_thumbnails(editor::EditorFrame &editor_frame);
    [[nodiscard]] PhaseExit
    apply_preferences_request(editor::EditorFrame &editor_frame);
    [[nodiscard]] PhaseExit
    apply_proxy_cache_clear(editor::EditorFrame &editor_frame);

    // director_editor_intents.cpp: editor intents queued for the next frame.
    [[nodiscard]] PhaseExit
    queue_editor_edits(editor::EditorFrame &editor_frame);
    [[nodiscard]] PhaseExit
    queue_editor_requests(DirectorTick &tick,
                          editor::EditorFrame &editor_frame);
#endif
};

} // namespace mediaserver::app
