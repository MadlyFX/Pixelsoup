#include "playback.hpp"
#include "director_show_loop.hpp"

#include "asset_activation.hpp"
#include "director_documents.hpp"
#include "output_deployment.hpp"
#include "output_frame_submission.hpp"
#include "output_playback_buffer.hpp"
#include "output_window_config.hpp"
#include "proxy_generation_service.hpp"
#include "system_hardware_probe.hpp"
#include "tracked_camera_bridge.hpp"

#include <mediaserver/assets/derivative.hpp>
#include <mediaserver/assets/derivative_cache.hpp>
#include <mediaserver/cmd/command.hpp>
#include <mediaserver/cmd/node_command_bus.hpp>
#include <mediaserver/compositor/compositor_graph.hpp>
#include <mediaserver/core/output_limits.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/net/cluster_lifecycle.hpp>
#include <mediaserver/net/frame_commit_sink.hpp>
#include <mediaserver/net/node_session_service.hpp>
#include <mediaserver/net/session_protocol.hpp>
#include <mediaserver/output/feed_map.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/linked_publication.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/show/frame_boundary_driver.hpp>
#include <mediaserver/show/frame_root_mailbox.hpp>
#include <mediaserver/sync/sync_policy.hpp>
#include <mediaserver/topology/hardware_map.hpp>
#include <mediaserver/track/pose_channel.hpp>

#if defined(MEDIASERVER_HAS_GUI)
#include "director_project_load.hpp"
#include <mediaserver/editor/live_data_panel.hpp>
#include <mediaserver/editor/mapping_panels.hpp>
#include <mediaserver/editor/source_replacement.hpp>
#include <mediaserver/editor/timeline_panel.hpp>
#include <mediaserver/editor/workspace_panels.hpp>
#include <mediaserver/persist/settings.hpp>
#include <mediaserver/ui/sdl_presenter.hpp>
#endif

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <variant>

namespace mediaserver::app {

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

bool parse_frame_rate(std::string_view value, show::FrameRate &rate) noexcept {
    const auto separator = value.find('/');
    core::u64 numerator{};
    core::u64 denominator{1U};
    if (separator == std::string_view::npos) {
        if (!parse_u64(value, numerator)) {
            return false;
        }
    } else if (!parse_u64(value.substr(0U, separator), numerator) ||
               !parse_u64(value.substr(separator + 1U), denominator)) {
        return false;
    }

    if (numerator == 0U || numerator > show::max_rate_numerator ||
        denominator == 0U || denominator > show::max_rate_denominator) {
        return false;
    }
    rate = show::FrameRate{static_cast<core::u32>(numerator),
                           static_cast<core::u32>(denominator)};
    return true;
}

bool parse_show_range(std::string_view value, show::ShowFrame &first,
                      show::ShowFrame &last) noexcept {
    const auto separator = value.find(':');
    if (separator == std::string_view::npos) {
        return false;
    }
    core::u64 begin{};
    core::u64 end{};
    if (!parse_u64(value.substr(0U, separator), begin) ||
        !parse_u64(value.substr(separator + 1U), end) || end < begin ||
        end - begin == std::numeric_limits<core::u64>::max()) {
        return false;
    }
    first = show::ShowFrame{begin};
    last = show::ShowFrame{end};
    return true;
}

bool valid_playback_options(const PlaybackOptions &options) noexcept {
    return show::ShowClock::valid_configuration(clock_config(options)) &&
           options.surface_width != 0U && options.surface_height != 0U &&
           options.surface_width <= core::max_output_dimension &&
           options.surface_height <= core::max_output_dimension;
}

int run_node_playback(net::NodeSessionService &service,
                      cmd::NodeCommandBus &node, const PlaybackOptions &options,
                      const volatile std::sig_atomic_t &stop_requested) {
    // A node output window is input-inert on purpose: what it shows is
    // replicated state, so keyboard transport there would be a lie.
    auto presenter = make_presenter(
        options, "media-server node " + std::to_string(options.node_index),
        false);
    if (presenter == nullptr) {
        return 1;
    }
    PresenterWindowSettings active_presenter_settings{
        options.surface_width,
        options.surface_height,
        options.window_x,
        options.window_y,
        options.window_position_explicit,
        options.windowed,
        options.fullscreen,
        options.borderless};
    struct NodeOutputPresenter final {
        DeployedOutput output;
        std::unique_ptr<show::Presenter> presenter;
        OutputFrameSubmission submission{};
    };
    std::vector<NodeOutputPresenter> output_presenters;
    output_presenters.push_back(NodeOutputPresenter{
        DeployedOutput{{},
                       topology::first_render_server_video_port,
                       options.node_index,
                       {},
                       active_presenter_settings,
                       "launch output"},
        std::move(presenter)});
    OutputDeployment active_deployment{};
    active_deployment.node_index = options.node_index;
    active_deployment.legacy_single_output = true;
    active_deployment.outputs.push_back(output_presenters.front().output);
    bool node_deployment_runtime_ready = true;
    // Reported separately from readiness so the Director can say which
    // physical thing failed instead of inferring "not ready yet" from a
    // process that has no presenters to report a renderer for.
    net::RuntimeOutputDeploymentState node_output_deployment =
        options.output ? net::RuntimeOutputDeploymentState::Unknown
                       : net::RuntimeOutputDeploymentState::Ready;
    bool node_feed_bindings_runtime_ready = true;
    bool node_asset_runtime_ready = false;
    bool node_proxy_runtime_ready = true;

    std::unique_ptr<server::ProxyGenerationService> node_proxy_artifacts;
    if (options.scene || options.output) {
        node_proxy_artifacts = open_proxy_playback_artifacts(options, "node");
    }
    std::unique_ptr<ProxyGenerationWorker> node_proxy_generator;
    if (options.output) {
        auto generation_artifacts =
            open_proxy_playback_artifacts(options, "node proxy generator");
        if (generation_artifacts != nullptr) {
            node_proxy_generator = std::make_unique<ProxyGenerationWorker>(
                std::move(generation_artifacts), true);
        }
    }
    MediaDecodeSupportWorker node_decode_support;
    ProxyPlaybackService node_proxy_playback{node_proxy_artifacts.get()};
    std::vector<ProxyPlacementObservation> node_proxy_observations;
    std::array<net::RuntimeMediaAssetStatus, net::max_runtime_media_assets>
        node_media_status{};
    std::size_t node_media_status_count{};
    auto next_node_media_status_refresh = Clock::time_point::min();

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
    // The pose channel for this node.
    //
    // Fed from the session, never from a socket: there is no tracker here, and
    // that is structural rather than incidental. A frame with no pose holds
    // the previous one - every node holds the same one, so the wall stays
    // whole - and never extrapolates, because two nodes guessing would guess
    // differently.
    TrackedCameraChannels node_pose_channels;
    // The Director's pose-stream ordinal this node last saw, and how many it
    // never saw at all. A gap is a pose that was replaced in this node's
    // mailbox before the session drained it: not a fault, but it means this
    // node's plate is running behind its neighbours', and nothing else on
    // either end can tell.
    core::u64 node_last_pose_sequence = 0U;
    core::u64 node_missed_poses = 0U;
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
    bool node_checkpoint_waiting = false;
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
    assets::AssetRegistry node_asset_registry;
    core::u64 node_asset_version = 0U;
    core::u64 node_asset_registry_digest = 0U;
    core::u64 node_asset_manifest_digest = 0U;
    bool node_assets_loaded = false;
    ProjectAssetAvailability node_asset_availability;

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
    net::RuntimeTracking node_tracking{};
    net::RuntimeTracking published_tracking{};
    net::RuntimePlayhead published_playhead{};
#if defined(MEDIASERVER_HAS_GUI)
    render::RenderConformanceService node_conformance;
    render::PublicationVerification node_publication_verification;
#endif
    net::RuntimeRendererReadiness node_renderer_readiness{};
    core::AssetCommit node_asset_commit{};
    net::RuntimeRendererReadiness published_renderer_readiness{};
    // Boundaries where the group late-frame policy repeated the previous
    // complete frame instead of advancing part of a wall.
    core::u64 policy_held_boundaries{};
    core::u64 late_boundaries{};
    core::u64 missed_boundaries{};
    OutputPlaybackBuffer output_playback_buffer{
        options.active_preferences.output_buffer_frames};
    // Transitions out of an aggregate Joined state observed here. Kept apart
    // from the presenter leave counts, which are already cumulative totals:
    // folding the two into one running sum would count each loss again on
    // every refresh.
    core::u64 barrier_loss_transitions{};
    show::VideoReferenceSample latest_video_reference{};

    const auto publish_runtime_adoption = [&]() noexcept {
        try {
            net::RuntimeAssetAdoption adoption{
                node_asset_manifest_digest,
                node_asset_runtime_ready && node_proxy_runtime_ready &&
                    node_deployment_runtime_ready &&
                    node_feed_bindings_runtime_ready};
            adoption.deployment = node_output_deployment;
            adoption.media_assets.assign(
                node_media_status.begin(),
                node_media_status.begin() +
                    static_cast<std::ptrdiff_t>(node_media_status_count));
            service.publish_runtime_asset_adoption(std::move(adoption));
        } catch (const std::bad_alloc &) {
        }
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

    const auto read_video_reference = [&]() noexcept {
        show::VideoReferenceSample result{};
        bool have_output = false;
        bool locked_counter_seen = false;
        core::u32 locked_counter{};
        bool counter_disagreement = false;
        const auto map = [](show::PresenterVideoReference observed) noexcept {
            show::VideoReferenceSample mapped{};
            mapped.frame_counter = observed.frame_counter;
            switch (observed.state) {
            case show::PresenterVideoReferenceState::Unsupported:
                mapped.state = show::VideoReferenceState::Unsupported;
                break;
            case show::PresenterVideoReferenceState::Waiting:
                mapped.state = show::VideoReferenceState::Waiting;
                break;
            case show::PresenterVideoReferenceState::Locked:
                mapped.state = show::VideoReferenceState::Locked;
                break;
            case show::PresenterVideoReferenceState::Lost:
                mapped.state = show::VideoReferenceState::Lost;
                break;
            case show::PresenterVideoReferenceState::Failed:
                mapped.state = show::VideoReferenceState::Failed;
                break;
            case show::PresenterVideoReferenceState::Unknown:
                mapped.state = show::VideoReferenceState::Unknown;
                break;
            }
            return mapped;
        };
        const auto severity = [](show::VideoReferenceState state)
            noexcept -> int {
            switch (state) {
            case show::VideoReferenceState::Failed:
                return 5;
            case show::VideoReferenceState::Lost:
                return 4;
            case show::VideoReferenceState::Unsupported:
                return 3;
            case show::VideoReferenceState::Waiting:
                return 2;
            case show::VideoReferenceState::Unknown:
                return 1;
            case show::VideoReferenceState::Locked:
                return 0;
            }
            return 1;
        };
        for (const NodeOutputPresenter& slot : output_presenters) {
            const show::VideoReferenceSample observed =
                map(slot.presenter->video_reference());
            if (observed.state == show::VideoReferenceState::Locked) {
                if (!locked_counter_seen) {
                    locked_counter_seen = true;
                    locked_counter = observed.frame_counter;
                } else {
                    // The reads are sequential and can straddle one hardware
                    // edge. More than that one-frame ambiguity is not a
                    // common source, so do not pick one and let the other
                    // free-run: the show clock must hold.
                    const core::u32 delta =
                        observed.frame_counter - locked_counter;
                    if (delta != 0U && delta != 1U &&
                        delta != std::numeric_limits<core::u32>::max()) {
                        counter_disagreement = true;
                    }
                }
            }
            if (!have_output || severity(observed.state) > severity(result.state)) {
                have_output = true;
                result = observed;
            }
        }
        if (counter_disagreement) {
            return show::VideoReferenceSample{
                show::VideoReferenceState::Lost, 0U};
        }
        return result;
    };

    const auto runtime_reference_state = [](show::VideoReferenceState state)
        noexcept {
        switch (state) {
        case show::VideoReferenceState::Unsupported:
            return net::RuntimeReferenceClockState::Unsupported;
        case show::VideoReferenceState::Waiting:
            return net::RuntimeReferenceClockState::Waiting;
        case show::VideoReferenceState::Locked:
            return net::RuntimeReferenceClockState::Locked;
        case show::VideoReferenceState::Lost:
            return net::RuntimeReferenceClockState::Lost;
        case show::VideoReferenceState::Failed:
            return net::RuntimeReferenceClockState::Failed;
        case show::VideoReferenceState::Unknown:
            return net::RuntimeReferenceClockState::Unknown;
        }
        return net::RuntimeReferenceClockState::Unknown;
    };

    const auto refresh_output_sync = [&]() noexcept {
        const auto previous = node_output_sync.barrier;
        net::RuntimeOutputSync sync{};
        sync.barrier = net::RuntimeBarrierState::Joined;
        sync.group = node_sync_policy.group;
        sync.barrier_id = node_sync_policy.barrier;
        sync.expected_outputs =
            static_cast<core::u32>(output_presenters.size());
        sync.reference_clock = runtime_reference_state(latest_video_reference.state);
        sync.reference_frame_counter = latest_video_reference.frame_counter;
        if (output_presenters.empty()) {
            sync.barrier = net::RuntimeBarrierState::Unknown;
        }
        for (const NodeOutputPresenter &slot : output_presenters) {
            const show::PresenterSyncReport report =
                slot.presenter->sync_report();
            sync.barrier = aggregate_barrier(sync.barrier, report.barrier);
            if (report.barrier == show::PresenterBarrierState::Joined) {
                ++sync.joined_outputs;
            }
            if (report.presents != 0U &&
                report.presents_in_sync < report.presents) {
                sync.barrier = aggregate_barrier(
                    sync.barrier, show::PresenterBarrierState::Left);
            }
            sync.barrier_presents += report.presents;
            sync.barrier_presents_in_sync += report.presents_in_sync;
            sync.barrier_flips_in_sync += report.flips_in_sync;
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
                published_output_sync.loss_of_sync_events ||
            node_output_sync.barrier_presents !=
                published_output_sync.barrier_presents ||
            node_output_sync.barrier_presents_in_sync !=
                published_output_sync.barrier_presents_in_sync ||
            node_output_sync.reference_clock !=
                published_output_sync.reference_clock;
        if (!force && !changed) {
            return;
        }
        published_output_sync = node_output_sync;
        service.publish_runtime_output_sync(node_output_sync);
    };

    // What this node is doing with the poses it was sent, gathered from the
    // three places that actually know: the receive path's sequence tracking,
    // the pose channels that answer each frame, and the presenters that draw
    // the plate. Assembled here rather than counted separately, so the report
    // cannot drift from what the node did.
    const auto refresh_tracking = [&]() noexcept {
        net::RuntimeTracking tracking{};
        tracking.pose_sequence = node_last_pose_sequence;
        tracking.poses_missed = node_missed_poses;
        tracking.poses_coalesced = service.tracked_poses_coalesced();
        for (const TrackedCameraChannels::Entry &entry :
             node_pose_channels.entries()) {
            const track::PoseChannelCounters &counters =
                entry.channel.counters();
            tracking.poses_applied += counters.exact_frames;
            tracking.held_frames += counters.held_frames;
        }
        for (const NodeOutputPresenter &slot : output_presenters) {
            const show::PresenterPlateStats plates = slot.presenter->plates();
            tracking.plates_drawn += plates.drawn;
            tracking.plates_skipped += plates.skipped;
            tracking.plate_refusals += plates.refusals;
        }
        node_tracking = tracking;
    };

    const auto publish_tracking = [&](bool force) noexcept {
        refresh_tracking();
        if (!force && node_tracking == published_tracking) {
            return;
        }
        published_tracking = node_tracking;
        service.publish_runtime_tracking(node_tracking);
    };

    const auto publish_renderer_readiness = [&](bool force) noexcept {
        net::RuntimeRendererReadiness aggregate{};
        bool first = true;
        for (const NodeOutputPresenter &slot : output_presenters) {
            const net::RuntimeRendererReadiness observed =
                runtime_renderer_readiness(
                    slot.presenter->renderer_readiness());
            if (first) {
                aggregate = observed;
                first = false;
            } else {
                auto profile = observed;
                profile.assets = aggregate.assets;
                profile.timing = aggregate.timing;
                profile.temporal_restarts = aggregate.temporal_restarts;
                profile.temporal_restart_frame = aggregate.temporal_restart_frame;
                if (profile != aggregate) {
                    aggregate = {};
                    break;
                }
                const bool all_timed = aggregate.timing.sequence != 0U && observed.timing.sequence != 0U;
                const bool all_current = aggregate.timing.profile_digest == aggregate.profile_digest
                    && observed.timing.profile_digest == aggregate.profile_digest;
                const bool all_gpu = aggregate.timing.gpu_valid && observed.timing.gpu_valid;
                const auto cpu_max = std::max(aggregate.timing.cpu_record_ns, observed.timing.cpu_record_ns);
                if (observed.timing.gpu_ns > aggregate.timing.gpu_ns) aggregate.timing = observed.timing;
                aggregate.timing.cpu_record_ns = cpu_max;
                if (!all_timed) aggregate.timing.sequence = 0U;
                if (!all_current) aggregate.timing.profile_digest = 0U;
                aggregate.timing.gpu_valid = all_gpu;
                aggregate.temporal_restarts = std::max(aggregate.temporal_restarts, observed.temporal_restarts);
                aggregate.temporal_restart_frame = std::max(aggregate.temporal_restart_frame, observed.temporal_restart_frame);
                if (aggregate.conformance.adapter != observed.conformance.adapter)
                    aggregate.conformance.adapter = {};
                aggregate.assets.ready = aggregate.assets.ready && observed.assets.ready &&
                    aggregate.assets.digest == observed.assets.digest;
                aggregate.assets.visible_commit = aggregate.assets.visible_commit == observed.assets.visible_commit
                    ? aggregate.assets.visible_commit : 0;
            }
        }
#if defined(MEDIASERVER_HAS_GUI)
        if (aggregate.selected == net::RuntimeRendererBackend::Advanced) {
            const auto adapter = aggregate.conformance.adapter;
            node_conformance.poll(adapter, service.conformance_request(), aggregate.conformance);
        }
#endif
        node_renderer_readiness = aggregate;
        if (!force && node_renderer_readiness ==
                          published_renderer_readiness) {
            return;
        }
        published_renderer_readiness = node_renderer_readiness;
        service.publish_runtime_renderer_readiness(node_renderer_readiness);
    };

    const auto apply_sync_policy = [&]() noexcept {
        node_sync_request = show::PresenterSyncRequest{
            node_sync_policy.requires_physical_sync(), node_sync_policy.group,
            node_sync_policy.barrier};
        for (NodeOutputPresenter &slot : output_presenters) {
            static_cast<void>(slot.presenter->apply_sync(node_sync_request));
        }
        node_sync_request_pending = false;
        publish_output_sync(true);
    };

    show::FrameWaiter frame_waiter;
    show::BoundaryLateness boundary_lateness;
    while (stop_requested == 0 && !closed) {
        const auto now = Clock::now();
        boundary_lateness.observe(now);
        if (driver.config().timing_authority ==
            show::FrameBoundaryDriverConfig::TimingAuthority::VideoReference) {
            latest_video_reference = read_video_reference();
        } else {
            latest_video_reference = {};
        }
        auto ticked =
            driver.config().timing_authority ==
                    show::FrameBoundaryDriverConfig::TimingAuthority::VideoReference
                ? driver.tick_from_video_reference(now, latest_video_reference)
                : driver.tick(now);
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
        if (!service.running()) {
            const auto session_status = service.snapshot();
            if (session_status.state == net::NodeSessionServiceState::Faulted) {
                std::cerr << "node session faulted (reason "
                          << static_cast<unsigned>(session_status.last_failure)
                          << "); stopping the render process so Output Monitor "
                             "can report and restart it\n";
                return 1;
            }
        }
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
        if (report.checkpoint_activated) {
            node_checkpoint_waiting = true;
            // A checkpoint can rewind the stream. If this tick published no
            // new approved frame, the mailbox contains only the old stream.
            // Retire it while the connector keeps its last complete image.
            if (report.published == 0U &&
                driver.mailbox().consume_latest(incoming, skipped, &retired)) {
                static_cast<void>(retired.retire(std::move(incoming.root)));
                incoming = {};
            }
        }
        // Only a cabled Render Server the Hardware Map lists is in the
        // Director's asset cohort. Any other output is never sent a retry, so
        // it holds no activation: it shows a set as soon as its own renderer
        // has it.
        const auto asset_frame =
            !node_checkpoint_waiting &&
                    active_deployment.render_server != doc::NodeId{}
                ? held_asset_activation(node_renderer_readiness.assets,
                                        node_asset_commit,
                                        presented.control_frame.value(),
                                        show::read_transport(presented.root))
                : 0U;
        const bool retain_activation =
            asset_frame != 0 && asset_frame == presented.control_frame.value();
        if (!retain_activation &&
            driver.mailbox().consume_latest(incoming, skipped, &retired,
                                            cmd::FrameNumber{asset_frame})) {
            if (node_checkpoint_waiting ||
                (incoming.version == presented.version &&
                 !incoming.root.same_snapshot(presented.root))) {
                // Versions are local to a document stream. A replacement
                // root can reuse the same number with different content.
                scene_loaded = compositor_loaded = feed_loaded = false;
                node_assets_loaded = node_hardware_loaded = node_sync_loaded = false;
                node_pose_channels.reset();
                node_last_pose_sequence = 0U;
                output_playback_buffer = OutputPlaybackBuffer{
                    options.active_preferences.output_buffer_frames};
                for (NodeOutputPresenter &slot : output_presenters) {
                    slot.submission.reset();
                    slot.presenter->begin_document_stream();
                }
            }
            node_checkpoint_waiting = false;
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
                const auto authority =
                    node_sync_policy.requires_external_reference()
                    ? show::FrameBoundaryDriverConfig::TimingAuthority::
                          VideoReference
                    : show::FrameBoundaryDriverConfig::TimingAuthority::
                          SystemClock;
                auto changed = driver.set_timing_authority(authority, now);
                if (!changed) {
                    std::cerr << "could not switch node timing authority "
                              << "(code "
                              << static_cast<unsigned>(changed.error())
                              << ")\n";
                    return 1;
                }
            }
        }

        const bool hardware_changed =
            !node_hardware_loaded ||
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
                local_inventory != nullptr ? local_inventory->sample_generation
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
                    for (const auto &problem : wanted.issues) {
                        std::cerr << "output deployment for node "
                                  << options.node_index << ": "
                                  << problem.message << '\n';
                    }
                    output_presenters.clear();
                    node_output_deployment =
                        runtime_output_deployment_state(wanted);
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
                    } catch (const std::bad_alloc &) {
                        created_all = false;
                    }
                    for (const DeployedOutput &deployed : wanted.outputs) {
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
                                " feed " + std::to_string(deployed.feed_index),
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
                        // The bindings resolved; the presenter refused them.
                        // That is a different fault from a display that is
                        // missing or mirrored, and it is the one an operator
                        // reaches by checking the saved mode against what the
                        // display actually advertises.
                        node_output_deployment = net::
                            RuntimeOutputDeploymentState::PresenterUnavailable;
                        node_feed_bindings_runtime_ready = false;
                        node_sync_request_pending = true;
                        std::cerr << "could not apply the complete output "
                                     "deployment for node "
                                  << options.node_index << '\n';
                    } else {
                        output_presenters = std::move(replacements);
                        node_output_deployment =
                            runtime_output_deployment_state(wanted);
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
                    auto replacement =
                        make_presenter(presenter_options(options, *wanted),
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
                        output_presenters.front().submission.reset();
                        output_presenters.front().output.window = *wanted;
                        active_presenter_settings = *wanted;
                        node_sync_request_pending = true;
                    }
                }
            }
        }

        // An operator's stop, pause or seek can arrive ahead of the frames
        // this node is still working through. It then decides the position
        // until the committed document carries it, so the wall answers the
        // operator however far behind on frames this node is.
        const auto transport = show::presented_anchor(
            presented.transport, service.priority_transport());
        const bool priority_transport = transport != presented.transport;
        show::TestPatternInput input{};
        // The position was derived from the anchor at the exact control frame
        // that carried it, so the two can never be a mismatched pair.
        input.show_frame =
            !transport ? options.first_frame
            : priority_transport
                ? show::derive_show_frame(*transport, presented.control_frame)
                : presented.show_frame;
        input.control_frame = presented.control_frame;
        input.rate = transport ? transport->rate : options.rate;
        input.state =
            transport ? transport->state : show::TransportState::Stopped;
        input.node_index = options.node_index;
        input.held = report.held;
        input.needs_resync =
            report.last_outcome == show::FrameCommitOutcome::NeedsResync;
        // A Loop cue's crossfade, from the same anchor as the position.
        std::optional<show::TransportBlend> loop_blend =
            transport ? show::derive_transport_blend(*transport,
                                                     presented.control_frame)
                      : std::nullopt;
        if (transport && driver.config().clock.rate != transport->rate) {
            if (!driver.reconfigure_rate(transport->rate, now)) {
                std::cerr << "node frame pacer refused the replicated show "
                             "rate\n";
                return 1;
            }
            interval = status_interval(options, transport->rate);
            since_status = 0U;
        }
        if (options.output && transport.has_value()) {
            const OutputPlaybackBuffer::Selection buffered =
                output_playback_buffer.select(
                    *transport, presented.control_frame, at_boundary,
                    cmd::FrameNumber{asset_frame});
            input.control_frame = buffered.control_frame;
            input.show_frame = buffered.show_frame;
            loop_blend = buffered.blend;
        }

        // The poses the Director stamped for one control frame, taken off the
        // session and handed to each camera's channel. Nothing here decides
        // which frame they belong to: the Director already did, and the frame
        // is in the message. The whole stack lands together, so this node
        // never composites one camera's new pose over another's previous one.
        if (const auto received = service.take_tracked_camera_pose()) {
            // Gaps in the pose stream, before anything is applied. The
            // ordinal is assigned when the Director publishes, so one that
            // never arrived still consumed a number and shows up here as a
            // hole.
            //
            // This is not the only way a pose is lost, and the two must not be
            // confused: a pose this node received and overwrote before
            // draining also leaves a hole here, and that one is this node's
            // own backlog rather than anything the Director or the network
            // did. The session counts those separately - see
            // `poses-coalesced` below - so the hole this counts is what is
            // left after they are accounted for.
            if (received->pose_sequence != 0U) {
                if (node_last_pose_sequence != 0U
                    && received->pose_sequence > node_last_pose_sequence + 1U) {
                    node_missed_poses += received->pose_sequence
                        - node_last_pose_sequence - 1U;
                }
                node_last_pose_sequence =
                    std::max(node_last_pose_sequence, received->pose_sequence);
            }
            for (const net::TrackedCameraSample &sample : received->samples) {
                auto &entry = node_pose_channels.open(
                    doc::NodeId{sample.camera_node});
                entry.lens = server::tracked_camera_lens(sample);
                if (entry.channel.accept(server::stamped_pose(
                        sample, received->control_frame))
                    == track::PoseChannelOutcome::Accepted) {
                    ++entry.accepted;
                }
            }
        }

        // A node draws its slice of the replicated scene through its feed map,
        // or draws the raw slice, or shows the test pattern - decided once at
        // start-up by which renderers the presenter was built with. A node that
        // could not build the path it was asked for never reaches here:
        // make_presenter refuses it. The output path implies the scene path,
        // which is the source it transforms.
        const bool render_output = options.output;
        const bool render_scene = options.scene;
        // Source readiness is checked against the exact authored bytes. A
        // later proxy resolution may replace only the evaluated runtime path;
        // the document path and persistent asset id are never rewritten.
        if (render_scene || render_output) {
            const bool document_changed =
                !node_assets_loaded ||
                presented.version.value() != node_asset_version;
            bool digest_changed = false;
            bool rechecked = false;
            if (document_changed) {
                node_asset_model.reset(presented.root);
                node_asset_registry = node_asset_model.evaluate();
                const core::u64 registry_digest =
                    assets::digest(node_asset_registry);
                if (!node_assets_loaded ||
                    registry_digest != node_asset_registry_digest) {
                    // Proxy policy and requested profile are authored asset
                    // state but intentionally do not change the content
                    // manifest. Clear only session materializations whenever
                    // that complete registry identity changes.
                    node_proxy_playback.reset();
                    node_asset_registry_digest = registry_digest;
                    if (options.output && node_proxy_generator != nullptr) {
                        for (const assets::Asset &entry :
                             node_asset_registry.assets) {
                            const auto *const media =
                                std::get_if<assets::MediaAsset>(&entry);
                            if (media != nullptr) {
                                node_proxy_generator->request(*media);
                            }
                        }
                    }
                    next_node_media_status_refresh = Clock::time_point::min();
                }
                const ProjectAssetManifest manifest =
                    collect_project_asset_manifest(presented.root,
                                                   node_asset_registry);
                digest_changed = !node_assets_loaded ||
                                 manifest.digest != node_asset_manifest_digest;
                if (digest_changed) {
                    node_asset_manifest_digest = manifest.digest;
                    node_asset_availability = scan_project_assets(manifest);
                    node_media_status_count = 0U;
                    if (options.output) {
                        node_decode_support.replace(manifest.digest,
                                                    node_asset_registry);
                    }
                    next_node_media_status_refresh = Clock::time_point::min();
                    rechecked = true;
                }
                node_asset_version = presented.version.value();
                node_assets_loaded = true;
            }
            if (rechecked) {
                const bool ready = node_asset_availability.render_ready();
                const bool readiness_changed =
                    ready != node_asset_runtime_ready;
                node_asset_runtime_ready = ready;
                // The report carries the digest it answers, so a new manifest
                // has to be republished even when this node was already ready:
                // otherwise the Director keeps comparing against the digest
                // this node last named and the Program gate never opens.
                if (digest_changed || readiness_changed) {
                    publish_runtime_adoption();
                }
                // One line per actual change, not per retry. The ongoing state
                // belongs on the Output Monitor, not in a five-second log.
                if (!node_asset_availability.ready() && (digest_changed || readiness_changed)) {
                    for (const ProjectAssetPresence &presence :
                         node_asset_availability.assets) {
                        if (!presence.present) {
                            std::cerr << "node " << options.node_index
                                      << " cannot open asset "
                                      << presence.source << '\n';
                        }
                    }
                }
            }
            if (options.output && node_assets_loaded &&
                now >= next_node_media_status_refresh) {
                std::array<net::RuntimeMediaAssetStatus,
                           net::max_runtime_media_assets>
                    refreshed{};
                std::size_t refreshed_count{};
                if (build_node_media_status(
                        node_asset_registry, node_asset_availability,
                        node_proxy_artifacts.get(), node_proxy_generator.get(),
                        node_decode_support, node_asset_manifest_digest,
                        refreshed, refreshed_count)) {
                    const bool changed =
                        refreshed_count != node_media_status_count ||
                        !std::equal(
                            refreshed.begin(),
                            refreshed.begin() +
                                static_cast<std::ptrdiff_t>(refreshed_count),
                            node_media_status.begin());
                    if (changed) {
                        node_media_status = refreshed;
                        node_media_status_count = refreshed_count;
                        publish_runtime_adoption();
                    }
                }
                next_node_media_status_refresh =
                    now + library_proxy_retry_interval;
            }
        }
        scene::Scene scene_frame;
        core::u64 authored_scene_digest{};
        scene::Scene transition_from_frame;
        scene::Scene transition_to_frame;
        // Compositor graphs are scene-scoped, so a dissolve mixes two pictures
        // that were each composited through their own graph.
        compositor::CompositorGraph compositor_graph;
        compositor::CompositorGraph transition_from_graph;
        compositor::CompositorGraph transition_to_graph;
        show::SwitcherProgress switcher_progress{};
        output::FeedMap feed_map;
        show::PresenterStatus status{show::PresenterStatus::Skipped};
        bool output_frame_attempted{};
        bool output_frame_complete{};
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
            const scene::SceneTime time{input.show_frame.value(),
                                        input.rate.numerator,
                                        input.rate.denominator};
            // The show's repeat, so a clip still on air across the wrap has the
            // frames it lands on decoded before it gets there. The range rides
            // the transport anchor, so this node warms exactly what the
            // Director does, on the same frame.
            const scene::ShowLoop show_loop =
                transport ? scene::ShowLoop{transport->first_frame.value(),
                                            transport->last_frame.value(),
                                            transport->loop}
                          : scene::ShowLoop{};
            // A graph object authored before graphs were scene-scoped carries
            // no owner. It used to apply to whatever Program was showing, so
            // Program is the scene it is adopted into. Both the Director and
            // every node resolve it the same way, from the same document.
            const auto node_scenes = scene_model.scenes();
            const auto switcher = show::read_switcher(presented.root);
            const scene::SceneId legacy_graph_owner =
                switcher.has_value()
                    ? switcher->program_scene
                    : (node_scenes.empty() ? scene::SceneId{}
                                           : node_scenes.front().id);
            if (switcher.has_value()) {
                switcher_progress = show::derive_switcher_progress(
                    *switcher, input.control_frame);
                scene_frame = scene_model.evaluate(switcher->program_scene,
                                                   time, show_loop);
                compositor_graph = compositor_model.evaluate(
                    switcher->program_scene, legacy_graph_owner);
                const auto publication_scene=scene::publication::render_scene(presented.root,switcher->program_scene);
                if (publication_scene != switcher->program_scene) {
                    scene_frame=scene_model.evaluate(publication_scene,time,show_loop);
                    compositor_graph=compositor_model.evaluate(publication_scene,legacy_graph_owner);
                }
                if (publication_scene != switcher->program_scene || !scene_frame.publication_revision.empty()) {
                    const auto keep=publication_scene != switcher->program_scene ? switcher->program_scene
                        : doc::NodeId{scene::publication::integer(presented.root.get(publication_scene),scene::publication::rollback)};
                    if (keep.value()) scene_frame.publication_retained_models=scene_model.evaluate(keep,time,show_loop).models;
                }
                if (switcher_progress.active) {
                    transition_from_frame = scene_model.evaluate(
                        switcher_progress.from, time, show_loop);
                    transition_to_frame = scene_model.evaluate(
                        switcher_progress.to, time, show_loop);
                    transition_from_graph = compositor_model.evaluate(
                        switcher_progress.from, legacy_graph_owner);
                    transition_to_graph = compositor_model.evaluate(
                        switcher_progress.to, legacy_graph_owner);
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
                scene_frame = scene_model.evaluate(time, show_loop);
                compositor_graph = compositor_model.evaluate(
                    scene_frame.id, legacy_graph_owner);
            }
            // A Loop cue's crossfade is a dissolve within one scene: the
            // scene at the show frame the loop is leaving, mixed into the one
            // it went round to. A switcher dissolve owns the mix while one
            // runs.
            if (!switcher_progress.active && loop_blend.has_value()) {
                transition_from_frame = scene_model.evaluate(
                    scene_frame.id,
                    scene::SceneTime{loop_blend->from.value(),
                                     input.rate.numerator,
                                     input.rate.denominator},
                    show_loop);
                transition_to_frame = scene_frame;
                transition_from_graph = compositor_graph;
                transition_to_graph = compositor_graph;
                switcher_progress = show::SwitcherProgress{
                    scene_frame.id, scene_frame.id, loop_blend->elapsed,
                    loop_blend->frames, true};
            }

            for (auto* candidate : {&scene_frame, &transition_from_frame, &transition_to_frame}) {
                // A dissolve publishes one asset set. Both input scenes use
                // the permit authored on the current Program stage.
                candidate->asset_commit = scene_frame.asset_commit;
                candidate->asset_manifest_digest = node_asset_manifest_digest;
                candidate->control_frame = input.control_frame.value();
                candidate->asset_cohort_required =
                    active_deployment.render_server != doc::NodeId{};
            }
            node_asset_commit = scene_frame.asset_commit;
#if defined(MEDIASERVER_HAS_GUI)
            scene_frame.publication_verified=node_publication_verification.observe(presented.root,scene_frame.id);
#endif

            // The pose this node was given for the frame it is drawing.
            //
            // Received, never sampled: there is no tracker on a render node,
            // and a node that read one would get a different pose from its
            // neighbour and open a seam mid-wall. A frame with no pose holds
            // the previous one, which every node does identically.
            //
            // Injected before the digest so this node's agreement check
            // compares the scene it actually draws against the Director's.
            inject_tracked_poses(scene_frame, node_pose_channels,
                                 input.control_frame.value(), true);
            authored_scene_digest = scene::digest(scene_frame);

            std::vector<ProxyPlacementObservation> observations;
            bool proxies_ready = resolve_scene_proxies(
                node_proxy_playback, scene_frame, node_asset_registry,
                node_asset_availability, observations);
            if (switcher_progress.active) {
                proxies_ready = resolve_scene_proxies(
                                    node_proxy_playback, transition_from_frame,
                                    node_asset_registry,
                                    node_asset_availability, observations) &&
                                proxies_ready;
                proxies_ready = resolve_scene_proxies(
                                    node_proxy_playback, transition_to_frame,
                                    node_asset_registry,
                                    node_asset_availability, observations) &&
                                proxies_ready;
            }
            report_proxy_status_changes("node", observations,
                                        node_proxy_observations);
            node_proxy_observations = std::move(observations);
            if (proxies_ready != node_proxy_runtime_ready) {
                node_proxy_runtime_ready = proxies_ready;
                publish_runtime_adoption();
            }
        }

        if (node_checkpoint_waiting) {
            // The checkpoint's root is staged state, not an image permit.
            // Keep the last complete connector image until a frame from the
            // replacement stream has actually been committed and published.
            for (NodeOutputPresenter &slot : output_presenters) {
                if (slot.presenter->hold() == show::PresenterStatus::Closed) {
                    status = show::PresenterStatus::Closed;
                    break;
                }
            }
        } else if (render_output) {
            if (!feed_loaded || presented.version.value() != feed_version) {
                feed_model.reset(presented.root);
                feed_version = presented.version.value();
                feed_loaded = true;
            }
            feed_map = feed_model.evaluate(
                scene::SceneTime{input.show_frame.value(), input.rate.numerator,
                                 input.rate.denominator});
            const bool feeds_ready =
                active_deployment.legacy_single_output ||
                std::all_of(
                    active_deployment.outputs.begin(),
                    active_deployment.outputs.end(),
                    [&](const DeployedOutput &deployed) {
                        return !feed_map.maps_for(deployed.feed_index).empty();
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
                [&](const NodeOutputPresenter &slot) {
                    return !feed_map.maps_for(slot.output.feed_index).empty();
                });
            const bool hold_group =
                !outputs_complete &&
                node_sync_policy.late_frame == sync::LateFramePolicy::HoldGroup;
            status = show::PresenterStatus::Skipped;
            if (hold_group) {
                if (at_boundary) {
                    policy_held_boundaries += report.boundaries;
                    missed_boundaries += report.boundaries;
                }
                for (NodeOutputPresenter &slot : output_presenters) {
                    if (slot.presenter->hold() ==
                        show::PresenterStatus::Closed) {
                        status = show::PresenterStatus::Closed;
                        break;
                    }
                }
            } else {
                output_frame_attempted = !output_presenters.empty();
                const OutputFrameKey frame_key{
                    input.control_frame.value(), input.show_frame.value(),
                    presented.version.value(), scene::digest(scene_frame),
                    switcher_progress.active
                        ? scene::digest(transition_from_frame) : 0U,
                    switcher_progress.active
                        ? scene::digest(transition_to_frame) : 0U,
                    output::digest(feed_map), node_asset_manifest_digest};
                bool output_deferred = false;
                for (NodeOutputPresenter &slot : output_presenters) {
                    const auto maps = feed_map.maps_for(slot.output.feed_index);
                    show::PresenterStatus slot_status =
                        show::PresenterStatus::Skipped;
                    if (!maps.empty()) {
                        slot_status = slot.submission.present(
                            frame_key,
                            [&]() noexcept {
                                if (switcher_progress.active) {
                                    return slot.presenter->present(
                                        transition_from_frame,
                                        transition_to_frame,
                                        static_cast<float>(switcher_progress.mix()),
                                        transition_from_graph,
                                        transition_to_graph, maps);
                                }
                                return slot.presenter->present(
                                    scene_frame, compositor_graph, maps);
                            },
                            [&]() noexcept { return slot.presenter->hold(); });
                    }
                    if (slot_status == show::PresenterStatus::Closed) {
                        status = slot_status;
                        break;
                    }
                    if (slot_status == show::PresenterStatus::Deferred) {
                        output_deferred = true;
                        continue;
                    }
                    if (slot_status == show::PresenterStatus::Failed ||
                        (slot_status == show::PresenterStatus::Presented &&
                         status == show::PresenterStatus::Skipped)) {
                        status = slot_status;
                    }
                }
                if (output_deferred && status != show::PresenterStatus::Closed &&
                    status != show::PresenterStatus::Failed) {
                    status = show::PresenterStatus::Deferred;
                }
                output_frame_complete =
                    output_frame_attempted &&
                    (status == show::PresenterStatus::Presented ||
                     status == show::PresenterStatus::Skipped);
            }
        } else if (render_scene) {
            // The node's own rectangle of the canvas, from the document. A node
            // the feed map does not bind draws the whole canvas rather than
            // guessing.
            const auto *const bound = scene_frame.slice_for(options.node_index);
            const auto slice =
                bound != nullptr ? *bound : scene::whole(scene_frame.canvas);
            show::Presenter &scene_presenter =
                *output_presenters.front().presenter;
            status =
                switcher_progress.active
                    ? scene_presenter.present(
                          transition_from_frame, transition_to_frame,
                          static_cast<float>(switcher_progress.mix()),
                          transition_from_graph, transition_to_graph, slice)
                    : scene_presenter.present(scene_frame, compositor_graph,
                                              slice);
        } else {
            status = output_presenters.front().presenter->present(input);
        }

        switch (status) {
        case show::PresenterStatus::Closed:
            node_deployment_runtime_ready = false;
            // A window that went away is no longer driving its display, so
            // the deployment is no longer in force either.
            node_output_deployment =
                net::RuntimeOutputDeploymentState::PresenterUnavailable;
            publish_runtime_adoption();
            closed = true;
            break;
        case show::PresenterStatus::Failed:
            ++present_failures;
            if (at_boundary) {
                missed_boundaries += report.boundaries;
            }
            break;
        case show::PresenterStatus::Deferred:
            if (at_boundary) {
                missed_boundaries += report.boundaries;
            }
            break;
        case show::PresenterStatus::Presented:
        case show::PresenterStatus::Skipped:
            break;
        }
        if (render_output && transport.has_value()) {
            output_playback_buffer.observe(output_frame_complete);
            // The Director's Timeline draws this node where its wall is,
            // which trails the control stream by the decode headroom. Only
            // a complete frame moves it: a held boundary left the previous
            // picture up, so the previous position still stands.
            if (output_frame_complete) {
                const net::RuntimePlayhead playhead{
                    true, input.show_frame.value(),
                    input.control_frame.value(),
                    output_playback_buffer.latency_frames(
                        presented.control_frame)};
                if (playhead != published_playhead) {
                    published_playhead = playhead;
                    service.publish_runtime_playhead(playhead);
                }
            }
        }

        // Only on a real boundary: a vendor barrier query is a driver call,
        // and this loop wakes far more often than the show rate. Only a
        // changed barrier state then takes the session lock; the counters
        // ride along at the status cadence below.
        if (at_boundary) {
            publish_output_sync(false);
            publish_renderer_readiness(false);
        }

        since_status += report.boundaries;
        if (since_status >= interval) {
            since_status = 0U;
            publish_output_sync(true);
            publish_renderer_readiness(true);
            publish_tracking(true);
            std::cout
                << "node " << options.node_index
                << " show-frame=" << input.show_frame.value()
                << " control-frame=" << presented.control_frame.value()
                << " output-control-frame=" << input.control_frame.value()
                << " output-buffer-lag="
                << (render_output ? output_playback_buffer.latency_frames(
                                        presented.control_frame)
                                  : 0U)
                << " version=" << presented.version.value()
                << " transport=" << transport_label(input.state)
                << " state=" << outcome_label(report.last_outcome)
                << " boundaries=" << boundaries
                << " presented=" << presented_frames
                << " held=" << driver.counters().held_boundaries
                << " skipped=" << skipped << " overflows="
                << driver.counters().mailbox_overflows
                // Tracking, from the node's own side. Until this existed a
                // node reported nothing about the poses it was drawing from,
                // so a plate stuck on a stale sample looked exactly like a
                // show with no tracking.
                << " pose-seq=" << node_last_pose_sequence
                << " poses-missed=" << node_missed_poses
                << " poses-coalesced=" << node_tracking.poses_coalesced
                << " poses-applied=" << node_tracking.poses_applied
                << " pose-holds=" << node_tracking.held_frames
                << " plates=" << node_tracking.plates_drawn
                << " plates-skipped=" << node_tracking.plates_skipped
                << " plate-refusals=" << node_tracking.plate_refusals
                // The scene digest compares the replicated state itself,
                // not a rendering of it, so it agrees across nodes drawing
                // different slices - which the frame-buffer digest cannot.
                << " digest="
                << (render_scene
                        ? authored_scene_digest
                        : show::frame_buffer_digest(
                              output_presenters.front().presenter->surface()));
            // The feed-map digest agrees across nodes for the same reason the
            // scene one does: it is the replicated parameters, not a rendering.
            if (render_output) {
                std::cout << " feed=" << output::digest(feed_map);
            }
            if (!output_presenters.empty()) {
                std::cout << gpu_timing_text(
                    output_presenters.front().presenter->timing());
            }
            std::cout << boundary_wake_text(boundary_lateness.take());
            std::cout << " sync=" << sync_mode_label(node_sync_policy.mode)
                      << " barrier="
                      << runtime_barrier_label(node_output_sync.barrier) << '/'
                      << node_output_sync.joined_outputs << 'x'
                      << node_output_sync.expected_outputs
                      << " missed=" << node_output_sync.missed_frames
                      << " late=" << node_output_sync.late_frames
                      << " policy-held=" << node_output_sync.held_frames
                      << " repeated=" << node_output_sync.repeated_frames
                      << " sync-loss=" << node_output_sync.loss_of_sync_events
                      << " renderer="
                      << static_cast<unsigned>(node_renderer_readiness.state)
                      << '/' << node_renderer_readiness.profile_digest
                      << " assets=" << node_renderer_readiness.assets.digest
                      << '/' << node_renderer_readiness.assets.ready
                      << '/' << node_renderer_readiness.assets.visible_commit
                      << " permit=" << node_asset_commit.digest << '/' << node_asset_commit.control_frame
                      << '/' << node_asset_commit.show_frame;
            std::cout << '\n' << std::flush;
        }

        // Off-boundary housekeeping: this is the only place a superseded trie
        // is actually released.
        static_cast<void>(retired.drain());

        if (options.run_frames != 0U && boundaries >= options.run_frames) {
            break;
        }
        if (driver.config().timing_authority ==
            show::FrameBoundaryDriverConfig::TimingAuthority::VideoReference) {
            // This is merely a responsive poll for the hardware counter, not
            // a pacing source. The counter above remains the only way a frame
            // boundary is created in external-reference mode.
            frame_waiter.sleep_for(std::chrono::milliseconds{1});
        } else {
            boundary_lateness.arm(driver.clock().next_boundary_at());
            sleep_until_boundary(frame_waiter,
                                 driver.clock().next_boundary_at());
        }
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
                          ? "closed"
                          : "headless")
                  << gpu_timing_text(
                         output_presenters.front().presenter->timing());
    }
    std::cout << " last=" << outcome_label(last_outcome) << '\n' << std::flush;
    return 0;
}

int run_director_show(cmd::DirectorScheduler &scheduler,
                      persist::CommandLog &command_log,
                      persist::FrameDurability frame_durability,
                      net::DirectorSessionHub &hub,
                      net::DirectorFrameBarrier &barrier,
                      net::DirectorSessionService *session_service,
                      const PlaybackOptions &options,
                      const volatile std::sig_atomic_t &stop_requested) {
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
    // operator's surface, not an output window. Everything else - the seal
    // loop, the transport, replication - is identical either way.
    std::unique_ptr<show::Presenter> presenter;
    std::string clip_source;
    std::unique_ptr<server::ProxyGenerationService> director_proxy_artifacts;
    if (options.scene || options.output || options.edit) {
        director_proxy_artifacts =
            open_proxy_playback_artifacts(options, "Director");
    }
    std::unique_ptr<ProxyGenerationWorker> director_proxy_generator;
    if (options.edit) {
        auto generation_artifacts =
            open_proxy_playback_artifacts(options, "proxy generator");
        if (generation_artifacts != nullptr) {
            director_proxy_generator = std::make_unique<ProxyGenerationWorker>(
                std::move(generation_artifacts));
        }
    }
    ProxyPlaybackService director_proxy_playback{
        director_proxy_artifacts.get()};
    std::vector<ProxyPlacementObservation> director_proxy_observations;

    // The one tracker socket in the product.
    //
    // Deliberately outside the editor block: tracking is show state, not
    // operator chrome. A Director running headless on a stage machine tracks
    // exactly as one with an editor open does, and closing the editor window
    // does not stop the camera. The sampling happens at the frame boundary
    // below rather than in the UI loop, so the frame a pose is stamped for is
    // the frame it was sampled on.
    server::TrackerListenerPool tracker_listeners;
    // The node-side hold semantics, run here too, once per tracked camera. The
    // Director resolves the same answers a node would, so the Live Data panel
    // reports what the wall is drawing rather than what arrived on the socket.
    TrackedCameraChannels director_pose_channels;
    server::DirectorPosePredictors director_pose_predictors;
    core::u64 director_poses_sent{};
    // The last frame a pose was replicated for, so the stamped stream stays
    // monotonic across a compensation change. See where it is used.
    core::u64 last_replicated_pose_frame{};

#if defined(MEDIASERVER_HAS_GUI)
    std::unique_ptr<editor::EditorShell> editor;
    std::unique_ptr<ClusterLifecycleController> lifecycle_controller;
    // Finds render machines for the Hardware Map's DETECTED ON NETWORK strip.
    std::unique_ptr<net::ClusterDiscoveryBrowser> discovery_browser;
    std::error_code discovery_error;
    std::unique_ptr<server::ThumbnailService> thumbnail_service;
    std::unique_ptr<server::PlatesetMotionService> plateset_motion;
    std::vector<assets::DerivativeKey> retained_thumbnail_keys;
    std::vector<assets::DerivativeKey> retained_filmstrip_keys;
    std::vector<editor::LibraryProxyRuntimeObservation> library_proxy_runtime;
    auto next_output_media_projection = Clock::time_point::min();
    auto next_editor_refresh = Clock::time_point::min();
    std::optional<cmd::FrameNumber> last_editor_control_frame;
    core::u64 editor_frame_calls{};
    std::optional<std::filesystem::path> workspace_layout_file;
    persist::WorkspaceLayoutBytes workspace_layout;
    if (options.edit) {
        const auto located_layout = persist::default_workspace_layout_file();
        if (!located_layout) {
            std::cerr << "could not locate workspace layout settings (code "
                      << static_cast<unsigned>(located_layout.error().code)
                      << ")\n";
        } else {
            workspace_layout_file = located_layout.value();
            auto loaded_layout =
                persist::load_workspace_layout(*workspace_layout_file);
            if (!loaded_layout) {
                std::cerr << "could not load workspace layout (code "
                          << static_cast<unsigned>(loaded_layout.error().code)
                          << ')';
                if (loaded_layout.error().system_error) {
                    std::cerr << ": "
                              << loaded_layout.error().system_error.message();
                }
                std::cerr << '\n';
            } else {
                workspace_layout = std::move(loaded_layout).value();
            }
        }
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
        editor_config.workspace_layout = workspace_layout;
        auto created_editor = editor::EditorShell::create(editor_config);
        if (!created_editor) {
            std::cerr << "could not open the operator editor window\n";
            return 1;
        }
        editor = std::move(created_editor).value();
        // Plateset motion signatures are cached beside the other
        // derivatives; without a cache root they are only kept this session.
        {
            std::error_code motion_path_error;
            std::filesystem::path motion_root =
                derivative_cache_root(options, motion_path_error);
            if (motion_path_error) {
                motion_root.clear();
            } else {
                motion_root /= "motion-v1";
            }
            try {
                plateset_motion = std::make_unique<server::PlatesetMotionService>(
                    std::move(motion_root));
            } catch (const std::system_error &error) {
                std::cerr << "could not start plateset analysis: "
                          << error.what() << '\n';
            } catch (const std::bad_alloc &) {
                std::cerr << "could not start plateset analysis: "
                             "allocation failed\n";
            }
        }
        {
            std::error_code cache_path_error;
            const std::filesystem::path cache_root =
                derivative_cache_root(options, cache_path_error) / "thumbnails";
            constexpr core::u64 thumbnail_cache_bytes = 512U * 1'024U * 1'024U;
            if (cache_path_error) {
                std::cerr << "could not locate the thumbnail cache root: "
                          << cache_path_error.message() << '\n';
            } else {
                auto cache =
                    assets::DerivativeCache::open(assets::DerivativeCacheConfig{
                        cache_root, thumbnail_cache_bytes});
                if (!cache) {
                    std::cerr << "could not open the thumbnail cache (code "
                              << static_cast<unsigned>(cache.error().code)
                              << ")\n";
                } else {
                    try {
                        thumbnail_service =
                            std::make_unique<server::ThumbnailService>(
                                std::move(cache).value());
                    } catch (const std::bad_alloc &) {
                        std::cerr << "could not initialize thumbnail workers: "
                                     "allocation failed\n";
                    } catch (const std::system_error &error) {
                        std::cerr << "could not initialize thumbnail workers: "
                                  << error.what() << '\n';
                    }
                }
            }
        }
        try {
            ClusterLifecycleControllerConfig lifecycle_config{};
            lifecycle_config.shared_secret = options.lifecycle_secret;
            lifecycle_controller = std::make_unique<ClusterLifecycleController>(
                std::move(lifecycle_config));
        } catch (const std::bad_alloc &) {
            std::cerr << "could not initialize cluster lifecycle controller: "
                         "allocation failed\n";
            return 1;
        } catch (const std::system_error &error) {
            std::cerr << "could not initialize cluster lifecycle controller: "
                      << error.what() << '\n';
            return 1;
        }
        if (options.lifecycle_discovery_port != 0U) {
            static_assert(net::cluster_discovery_port == 47'839U,
                          "PlaybackOptions documents the discovery port");
            try {
                net::ClusterDiscoveryBrowserConfig discovery_config{};
                discovery_config.port = options.lifecycle_discovery_port;
                discovery_browser =
                    std::make_unique<net::ClusterDiscoveryBrowser>(
                        std::move(discovery_config));
                discovery_error = discovery_browser->start();
            } catch (const std::bad_alloc &) {
                discovery_error =
                    std::make_error_code(std::errc::not_enough_memory);
            }
            if (discovery_error) {
                std::cerr << "render machine discovery is unavailable: "
                          << discovery_error.message() << '\n';
            }
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
            options.rate, show::ShowFrame{0U},
            show::ShowFrame{std::numeric_limits<core::u64>::max() - 1U}, false},
        start);
    auto created = show::TransportAuthority::create(clock_config(options),
                                                    scheduler.sealed_through());
    if (!paced || !created) {
        std::cerr << "invalid Director show clock configuration\n";
        return 1;
    }
    auto pacer = std::move(paced).value();

    // What the pacer above is actually keyed to.
    //
    // This is the other half of genlock, and the half that is easy to forget:
    // [03] locks every node's *present* to house reference through the vendor
    // barrier, but the control frame is the replication clock the whole cohort
    // derives its position from. If that advances on this machine's
    // steady_clock while the presents are referenced, the two drift and the
    // wall hitches together - with every node reporting itself perfectly
    // locked while it happens.
    sync::DirectorClockStatus director_clock{};
    bool director_reference_seen = false;
    core::u32 director_last_reference_frame = 0U;

    auto transport = std::move(created).value();
    bool restored_recovered_transport = false;

    if (!pacer.play(start)) {
        std::cerr << "could not start the Director frame pacer\n";
        return 1;
    }
#if defined(MEDIASERVER_HAS_GUI)
    if (options.edit && recovered_session) {
        const doc::Node *const raw_transport =
            scheduler.projected_document().get(show::transport_node_id);
        const auto recovered_transport =
            show::read_transport(scheduler.projected_document());
        if (raw_transport != nullptr &&
            std::string_view{raw_transport->type} ==
                show::transport_node_type &&
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
    // cohort, and a playing show adds no further edits because every motion is
    // a rate the nodes integrate against the show frame themselves, and the
    // feed map does not move at all.
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
    // Operator moves from the editor, coalesced to the latest per node,
    // authored at the next frame boundary like the transport commands.
    std::vector<editor::EditIntent> pending_edits;
    std::vector<editor::WorldEdit> pending_world_edits;
    // A pending scene-history step, authored at the boundary after the edits so
    // it navigates from exactly the state those edits just left behind.
    std::optional<editor::HistoryRequest> pending_history;
    // A pending timeline scrub: the show frame the operator dragged the
    // playhead to, applied to the transport (paused) at the boundary.
    std::optional<core::u64> pending_seek;
    // The same, for Preview's local playhead. Kept separate all the way to the
    // boundary so a scrub authored while rehearsing can never reach the show.
    std::optional<core::u64> pending_preview_seek;
    // Transport actions aimed at Preview rather than at the live show.
    std::vector<show::PresenterCommand> preview_commands;
    // The Timeline edit being dragged, which the Stage shows at its edit
    // point. Replaced by every editor frame; evaluated for the Stage alone.
    std::optional<editor::TimelineEditPreview> stage_edit_request;
    // The Timeline's solo, resolved to what the Stage draws. Editor-local,
    // like the edit preview.
    std::optional<scene::SoloFilter> stage_solo_request;
    // A pending show range/loop change from the timeline.
    std::optional<editor::RangeRequest> pending_range;
    // A pending exact project cadence. The transport anchor and the Director
    // pacer change on the same sealed boundary.
    std::optional<editor::ShowRateRequest> pending_show_rate;
    // Pending keyframe requests, authored as scene.channel edits at the
    // boundary.
    std::vector<editor::KeyIntent> pending_keys;
    // Pending keyframe retimes (a diamond dragged to a new time).
    std::vector<editor::KeyMove> pending_key_moves;
    // Alt-drag duplicates are complete key-set operations, planned together at
    // the boundary so every destination shares one transaction and delta.
    std::optional<editor::KeyDuplicateRequest> pending_key_duplicate;
    std::optional<editor::KeyBatchRequest> pending_key_batch;
    std::optional<editor::TimelineKeyScalePreview> key_scale_preview;
    // Clip placements and cuts from the timeline, authored at the boundary
    // like every other edit.
    std::optional<editor::ProjectSettingsEdit> pending_project_settings;
    std::vector<editor::ClipTimingEdit> pending_clip_timings;
    std::vector<editor::ClipSplitIntent> pending_clip_splits;
    std::vector<editor::PlatesetPlaceIntent> pending_plateset_places;
    std::vector<editor::PlatesetSwapIntent> pending_plateset_swaps;
    std::vector<editor::TimelineLaneEditIntent> pending_lane_edits;
    std::vector<editor::TimelineMarkerEditIntent> pending_marker_edits;
    std::optional<bool> pending_cues_armed;
    std::vector<editor::TimelineLayerOrderIntent> pending_layer_orders;
    std::vector<editor::ElementFlagIntent> pending_element_flags;
    std::vector<editor::ElementRenameIntent> pending_renames;
    std::vector<editor::ElementHighlightIntent> pending_highlights;
    // Set by the Library's RESCAN control. Nothing rescans on a timer: on a
    // stage the files move under you constantly and a periodic disk read is a
    // periodic stall, so looking again is something the operator asks for.
    bool pending_asset_rescan = false;
    std::vector<editor::TimelineEditIntent> pending_timeline_edits;
    std::optional<editor::GroupEditIntent> pending_group_edit;
    std::optional<editor::SelectionSetEditIntent> pending_selection_set_edit;
    std::optional<editor::MaskEditIntent> pending_mask_edit;
    ClipClipboard clip_clipboard;
    std::optional<doc::Root> linked_clipboard;
    std::optional<editor::ClipClipboardRequest> pending_linked_paste;
    std::optional<PendingClipPaste> pending_clip_paste;
    std::vector<editor::SceneDuplicateRequest> pending_scene_duplicates;
    KeyClipboard key_clipboard;
    std::optional<PendingKeyPaste> pending_key_paste;
    // Pending Inspector edits, authored as base-property edits at the boundary.
    std::vector<editor::PropertyEdit> pending_properties;
    // Pending Inspector edits of named properties (light fields).
    std::vector<editor::GenericEdit> pending_generics;
    // Selection locks stay authorable during a dissolve because they do not
    // alter either rendered scene.
    std::vector<editor::LockEdit> pending_locks;
    // Pending element creations and deletions from the editor's Add/Edit menus.
    std::vector<editor::AddIntent> pending_adds;
    std::vector<editor::AssetImportIntent> pending_asset_imports;
    std::vector<editor::AssetRevisionIntent> pending_asset_revisions;
    std::vector<editor::AssetDeleteIntent> pending_asset_deletes;
    std::vector<editor::AssetProxyEditIntent> pending_asset_proxy_edits;
    std::vector<editor::AssetLibraryEditIntent> pending_asset_library_edits;
    std::vector<editor::SourceReplacementIntent> pending_source_replacements;
    std::vector<editor::EffectIntent> pending_effects;
    std::vector<editor::FilmGrainEffectEditIntent>
        pending_film_grain_effect_edits;
    std::vector<editor::EffectDeleteIntent> pending_effect_deletes;
    std::optional<editor::SwitcherRequest> pending_switcher;
    std::vector<editor::HardwarePlaceRequest> pending_hardware_places;
    std::vector<editor::HardwareMoveRequest> pending_hardware_moves;
    std::vector<editor::HardwareLinkRequest> pending_hardware_links;
    std::vector<editor::HardwarePropertyEdit> pending_hardware_properties;
    std::optional<editor::HardwareControlPortEdit>
        pending_hardware_control_port;
    std::optional<editor::HardwareCanvasEdit> pending_hardware_canvas;
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
    std::vector<editor::FeedOrganizeRequest> pending_feed_organize;
    std::vector<editor::FeedSurfaceAddRequest> pending_feed_surface_adds;
    std::vector<editor::FeedSurfaceEditRequest> pending_feed_surface_edits;
    std::vector<editor::FeedSurfaceDeleteRequest> pending_feed_surface_deletes;
    std::vector<editor::CompositorAddIntent> pending_compositor_adds;
    std::vector<editor::CompositorMoveIntent> pending_compositor_moves;
    std::optional<editor::CompositorClipboardIntent> pending_compositor_clipboard;
    std::optional<editor::CompositorRenameIntent> pending_compositor_rename;
    std::optional<editor::CompositorCommentIntent> pending_compositor_comment;
    std::optional<editor::CompositorCollapseIntent> pending_compositor_collapse;
    std::optional<editor::CompositorBypassIntent> pending_compositor_bypass;
    std::optional<editor::CompositorReorderIntent> pending_compositor_reorder;
    std::vector<doc::Node> compositor_clipboard;
    std::optional<editor::CompositorConnectIntent> pending_compositor_connect;
    std::optional<editor::CompositorRewireIntent> pending_compositor_rewire;
    std::optional<editor::CompositorWaypointIntent> pending_compositor_waypoints;
    std::optional<editor::CompositorPropertyEditIntent>
        pending_compositor_property;
    std::optional<editor::CompositorDeleteIntent> pending_compositor_delete;
    core::u64 pending_compositor_gesture{};
    std::vector<doc::NodeId> pending_deletes;
    core::u64 pending_delete_gesture{};
    std::optional<editor::ProjectFileRequest> pending_project_save;
    std::optional<editor::ProjectFileRequest> pending_project_load;
    EditCoalescingPolicy editor_coalescing_policy;
    std::map<core::u64, EditCoalescingPlan> pending_editor_coalescing;
    bool pending_editor_coalescing_close{};
    HistoryDescriptions history_descriptions;
    editor::HistoryState history_state;
    // Startup validates the recent project before the frame loop, then hands
    // the already-decoded root to the ordinary frame-boundary staging path.
    // Manual Open requests leave this empty and continue to read at the
    // boundary, so both paths share transport/switcher/WAL validation.
    std::optional<doc::Root> pending_preloaded_project;
    std::optional<std::filesystem::path> recent_project_settings_file;
    std::vector<std::string> recent_project_paths;
    std::string project_path;
    std::string project_message = recovered_session
                                      ? std::string{"RECOVERED UNSAVED CHANGES"}
                                      : std::string{};
    doc::Root saved_project_snapshot = scheduler.projected_document();
    // Without a verified saved-file baseline, a recovered WAL is an unsaved
    // untitled session.
    bool project_explicitly_dirty{recovered_session};
    core::u64 project_generation{};
    render::LinkedSceneIngest linked_ingest;
    // Every Blender link of the project, one per linked scene; switching
    // Preview between linked scenes never re-pairs them.
    BlenderLinks blender_links{path_utf8(link_resource_directory({}))};
    std::string blender_store_project;
    // Transactions of accepted Blender revisions, unique across every link.
    core::u64 blender_link_transactions{};
    // The name an unsaved project's .blend gets, chosen when it first opens.
    std::string unsaved_blender_name;
    // Scenes the artist added in Blender, each created as a Media Server scene
    // through the ordinary Create Scene path; `blender_scene_creating` is the
    // one that path is creating now.
    std::deque<BlenderLinks::Created> blender_scene_requests;
    std::optional<BlenderLinks::Created> blender_scene_creating;
    std::vector<BlenderAssociation> blender_associations;
    doc::Root blender_association_root;
    // A Connect dialog choice waits for a Preview scene to exist. Open in
    // Blender waits for any running dissolve to end.
    std::string deferred_blender_connect, deferred_blender_resync;
    std::optional<scene::SceneId> deferred_blender_open;
    std::string blender_connection_error;
    render::LinkedPublication linked_publication;
    render::PublicationVerification director_publication_verification;
    std::optional<std::string> pending_publication_storage;
    bool pending_publication{}, pending_publication_cancel{}, pending_publication_rollback{};
    bool publication_staging{};
    core::u64 publication_project{}, publication_activation{};
    doc::NodeId publication_program{};
    std::string publication_message{"PUBLISH puts Preview on air as a frozen revision; its files are staged in the folder above."};
    std::string publication_storage;
    // The status bar repeats each new publication outcome, so a refused or
    // staged PUBLISH is visible without opening the Publish card.
    std::string publication_message_shown{publication_message};
    std::string scene_link_details, scene_link_diagnostics;
    std::vector<BlenderLinks::View> blender_views;
    std::vector<scene::SceneInfo> listed_scenes;
    std::vector<editor::BlenderSceneState> blender_scene_states;
    std::vector<std::pair<std::string, std::string>> blender_bound_names;
    std::string publication_default_storage;
    doc::Root linked_ingest_base;
    scene::SceneId linked_ingest_target{};
    core::u64 linked_ingest_project{};
    core::u64 linked_ingest_gesture{};

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
            report_recent_project_error("could not locate",
                                        settings_file.error());
        } else {
            recent_project_settings_file = settings_file.value();
            auto paths = persist::load_recent_project_paths(*recent_project_settings_file);
            if (paths) {
                for (const auto &path : paths.value()) {
                    recent_project_paths.push_back(native_path_to_utf8(path));
                }
            } else {
                report_recent_project_error("could not read", paths.error());
            }
            auto recent =
                persist::open_recent_project(*recent_project_settings_file);
            if (!recent) {
                if (recent.error().settings_error.has_value()) {
                    report_recent_project_error("could not read",
                                                *recent.error().settings_error);
                } else if (recent.error().project_error.has_value()) {
                    const auto &error = *recent.error().project_error;
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
                    pending_preloaded_project = std::move(candidate.project);
                    project_message = "REOPENING PROJECT...";
                } else if (options.associate_recent_project_after_recovery &&
                           recovered_project_matches_saved_baseline(
                               candidate.project,
                               scheduler.projected_document())) {
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
            MediaRateEnrichment enrichment =
                enrich_media_frame_rates(scheduler.projected_document());
            if (enrichment.updated_nodes != 0U) {
                const cmd::FrameNumber at{scheduler.sealed_through().value() +
                                          1U};
                auto scheduled = scheduler.schedule_document_replace(
                    enrichment.project, cmd::UndoContextId{7U},
                    cmd::TransactionId{at.value()}, at);
                if (scheduled) {
                    project_explicitly_dirty = true;
                    project_message =
                        "RECOVERED PROJECT - MEDIA FPS UPDATED (" +
                        std::to_string(enrichment.updated_nodes) + ')';
                    std::cerr << "enriched " << enrichment.updated_nodes
                              << " recovered media node(s) with native FPS; "
                                 "save the project to persist metadata\n";
                } else {
                    std::cerr << "could not schedule recovered media FPS "
                                 "metadata enrichment\n";
                }
            }
        } catch (const std::bad_alloc &) {
            std::cerr << "could not enrich recovered media FPS metadata: "
                         "allocation failed\n";
        } catch (const std::length_error &) {
            std::cerr << "could not enrich recovered media FPS metadata: "
                         "project limits exceeded\n";
        }
    }
    // The next id to try for a created element; advanced past any occupied id
    // so an add never collides with an existing node (content, channel, or
    // demo).
    core::u64 next_new_node{100'000U};
#endif
    bool rehearsal_mode{};
    // Preview's playhead. Program's transport is replicated document state
    // every render node derives its frame from; Preview's is Director-local
    // and authored nowhere, which is the whole point: an operator scrubbing,
    // playing, or stepping through the scene they are rehearsing must not move
    // the show. It shares the live range, loop, and rate, because those are one
    // show-wide setting rather than a property of the scene being edited.
    std::optional<show::TransportAuthority> preview_transport;
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
    DirectorSyncView director_sync;
    // Refreshed on the same Director tick as the sync snapshot, and for the
    // same reason: the panel and the diagnostic stream must not be able to
    // disagree about what a node reported.
    std::vector<editor::NodeTrackingObservation> director_node_tracking;
    sync::SyncPolicyModel director_sync_model;
    core::u64 director_sync_version{};
    bool director_sync_loaded{};
    net::RuntimeRendererReadiness observed_local_renderer{};
    server::RendererLockstep director_renderer_lockstep{};
    core::u64 conformance_request{};
#if defined(MEDIASERVER_HAS_GUI)
    render::RenderConformanceService director_conformance;
#endif
    std::optional<std::pair<doc::NodeId, core::u64>> pending_asset_commit;
    core::u64 last_asset_commit_frame{};
    core::u64 last_asset_commit_digest{};
    doc::NodeId last_asset_commit_stage{};
    core::AssetCohort director_asset_cohort{};
    std::optional<core::u32> asset_waiting_node;
    core::AssetCohort logged_asset_cohort{};
    auto autoplay_pending = options.autoplay && !restored_recovered_transport;
    auto closed = false;

    // The Director previews the whole canvas of the same replicated scene. It
    // evaluates from its own committed document, so it needs no mailbox; the
    // model is reset when that document's version changes.
    scene::SceneModel scene_model;
    core::u64 scene_version = 0U;
    bool scene_loaded = false;
    std::optional<core::u64> cue_version;
    bool cued_preview{};
    show::FrameRate cued_rate{};

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
    assets::AssetRegistry director_asset_registry;
    assets::AssetRegistry director_playback_asset_registry;
    core::u64 asset_version = 0U;
    core::u64 asset_registry_digest = 0U;
    core::u64 asset_manifest_digest = 0U;
    bool assets_loaded = false;
    ProjectAssetAvailability director_asset_availability;
    std::vector<ChangedLibrarySource> changed_library_sources;
    std::vector<doc::NodeId> changed_library_assets;
#if defined(MEDIASERVER_HAS_GUI)
    editor::EditorPreferencesState preferences_view{};
    // The stored render node password, as its derived key. Saved from the
    // Preferences panel and applied to the controller without a restart.
    std::string stored_password_key =
        options.stored_preferences.lifecycle_password_key;
    {
        const auto &stored = options.stored_preferences;
        const auto &active = options.active_preferences;
        const auto editor_priority = [](persist::ProxyTranscodePriority value) {
            switch (value) {
            case persist::ProxyTranscodePriority::Background:
                return editor::EditorPreferences::TranscodePriority::Background;
            case persist::ProxyTranscodePriority::Balanced:
                return editor::EditorPreferences::TranscodePriority::Balanced;
            case persist::ProxyTranscodePriority::Fast:
                return editor::EditorPreferences::TranscodePriority::Fast;
            }
            return editor::EditorPreferences::TranscodePriority::Background;
        };
        preferences_view.stored = editor::EditorPreferences{
            stored.director_listen,
            stored.show_id,
            stored.director_epoch,
            !stored.lifecycle_password_key.empty(),
            stored.wal_path,
            stored.proxy_cache_directory,
            stored.proxy_cache_next_to_source,
            stored.proxy_cache_gibibytes,
            stored.output_buffer_frames,
            stored.output_read_ahead_frames,
            stored.playback_cache_mebibytes,
            stored.playback_trail_frames,
            editor_priority(stored.proxy_transcode_priority)};
        preferences_view.active = editor::EditorPreferences{
            active.director_listen,
            active.show_id,
            active.director_epoch,
            !net::lifecycle_key_is_unsecured(options.lifecycle_secret),
            active.wal_path,
            active.proxy_cache_directory,
            active.proxy_cache_next_to_source,
            active.proxy_cache_gibibytes,
            active.output_buffer_frames,
            active.output_read_ahead_frames,
            active.playback_cache_mebibytes,
            active.playback_trail_frames,
            editor_priority(active.proxy_transcode_priority)};
        // A field is only "overridden" when an argument actually displaced a
        // stored value; an argument matching the setting is not a conflict.
        const auto displaced = [](bool from_command_line,
                                  std::string_view stored_value,
                                  std::string_view active_value) {
            return from_command_line && !stored_value.empty() &&
                   stored_value != active_value;
        };
        preferences_view.director_listen_overridden =
            displaced(options.director_listen_from_command_line,
                      stored.director_listen, active.director_listen);
        preferences_view.show_id_overridden = displaced(
            options.show_id_from_command_line, stored.show_id, active.show_id);
        preferences_view.director_epoch_overridden =
            displaced(options.director_epoch_from_command_line,
                      stored.director_epoch, active.director_epoch);
        preferences_view.render_node_password_overridden =
            options.lifecycle_password_from_command_line;
        preferences_view.wal_path_overridden =
            displaced(options.wal_path_from_command_line, stored.wal_path,
                      active.wal_path);
    }
#endif

    compositor::CompositorModel compositor_model;
    core::u64 compositor_version = 0U;
    bool compositor_loaded = false;

    DirectorShowLoop loop{
        .scheduler = scheduler,
        .command_log = command_log,
        .frame_durability = frame_durability,
        .hub = hub,
        .barrier = barrier,
        .session_service = session_service,
        .options = options,
        .stop_requested = stop_requested,
        .presenter = presenter,
        .director_proxy_generator = director_proxy_generator,
        .director_proxy_playback = director_proxy_playback,
        .director_proxy_observations = director_proxy_observations,
        .tracker_listeners = tracker_listeners,
        .director_pose_channels = director_pose_channels,
        .director_pose_predictors = director_pose_predictors,
        .director_poses_sent = director_poses_sent,
        .last_replicated_pose_frame = last_replicated_pose_frame,
#if defined(MEDIASERVER_HAS_GUI)
        .editor = editor,
        .lifecycle_controller = lifecycle_controller,
        .discovery_browser = discovery_browser,
        .discovery_error = discovery_error,
        .thumbnail_service = thumbnail_service,
        .plateset_motion = plateset_motion,
        .retained_thumbnail_keys = retained_thumbnail_keys,
        .retained_filmstrip_keys = retained_filmstrip_keys,
        .library_proxy_runtime = library_proxy_runtime,
        .next_output_media_projection = next_output_media_projection,
        .next_editor_refresh = next_editor_refresh,
        .last_editor_control_frame = last_editor_control_frame,
        .editor_frame_calls = editor_frame_calls,
        .workspace_layout_file = workspace_layout_file,
#endif
        .pacer = pacer,
        .director_clock = director_clock,
        .director_reference_seen = director_reference_seen,
        .director_last_reference_frame = director_last_reference_frame,
        .transport = transport,
        .tracked = tracked,
        .operator_commands = operator_commands,
#if defined(MEDIASERVER_HAS_GUI)
        .pending_edits = pending_edits,
        .pending_world_edits = pending_world_edits,
        .pending_history = pending_history,
        .pending_seek = pending_seek,
        .pending_preview_seek = pending_preview_seek,
        .preview_commands = preview_commands,
        .stage_edit_request = stage_edit_request,
        .stage_solo_request = stage_solo_request,
        .pending_range = pending_range,
        .pending_show_rate = pending_show_rate,
        .pending_keys = pending_keys,
        .pending_key_moves = pending_key_moves,
        .pending_key_duplicate = pending_key_duplicate,
        .pending_key_batch = pending_key_batch,
        .key_scale_preview = key_scale_preview,
        .pending_project_settings = pending_project_settings,
        .pending_clip_timings = pending_clip_timings,
        .pending_clip_splits = pending_clip_splits,
        .pending_plateset_places = pending_plateset_places,
        .pending_plateset_swaps = pending_plateset_swaps,
        .pending_lane_edits = pending_lane_edits,
        .pending_marker_edits = pending_marker_edits,
        .pending_cues_armed = pending_cues_armed,
        .pending_layer_orders = pending_layer_orders,
        .pending_element_flags = pending_element_flags,
        .pending_renames = pending_renames,
        .pending_highlights = pending_highlights,
        .pending_asset_rescan = pending_asset_rescan,
        .pending_timeline_edits = pending_timeline_edits,
        .pending_group_edit = pending_group_edit,
        .pending_selection_set_edit = pending_selection_set_edit,
        .pending_mask_edit = pending_mask_edit,
        .clip_clipboard = clip_clipboard,
        .linked_clipboard = linked_clipboard,
        .pending_linked_paste = pending_linked_paste,
        .pending_clip_paste = pending_clip_paste,
        .pending_scene_duplicates = pending_scene_duplicates,
        .key_clipboard = key_clipboard,
        .pending_key_paste = pending_key_paste,
        .pending_properties = pending_properties,
        .pending_generics = pending_generics,
        .pending_locks = pending_locks,
        .pending_adds = pending_adds,
        .pending_asset_imports = pending_asset_imports,
        .pending_asset_revisions = pending_asset_revisions,
        .pending_asset_deletes = pending_asset_deletes,
        .pending_asset_proxy_edits = pending_asset_proxy_edits,
        .pending_asset_library_edits = pending_asset_library_edits,
        .pending_source_replacements = pending_source_replacements,
        .pending_effects = pending_effects,
        .pending_film_grain_effect_edits = pending_film_grain_effect_edits,
        .pending_effect_deletes = pending_effect_deletes,
        .pending_switcher = pending_switcher,
        .pending_hardware_places = pending_hardware_places,
        .pending_hardware_moves = pending_hardware_moves,
        .pending_hardware_links = pending_hardware_links,
        .pending_hardware_properties = pending_hardware_properties,
        .pending_hardware_control_port = pending_hardware_control_port,
        .pending_hardware_canvas = pending_hardware_canvas,
        .pending_sync_policy = pending_sync_policy,
        .pending_hardware_output_binding_edits = pending_hardware_output_binding_edits,
        .pending_hardware_output_binding_deletes = pending_hardware_output_binding_deletes,
        .pending_hardware_deletes = pending_hardware_deletes,
        .pending_feed_map_adds = pending_feed_map_adds,
        .pending_feed_map_edits = pending_feed_map_edits,
        .pending_feed_map_deletes = pending_feed_map_deletes,
        .pending_feed_projection_edits = pending_feed_projection_edits,
        .pending_feed_organize = pending_feed_organize,
        .pending_feed_surface_adds = pending_feed_surface_adds,
        .pending_feed_surface_edits = pending_feed_surface_edits,
        .pending_feed_surface_deletes = pending_feed_surface_deletes,
        .pending_compositor_adds = pending_compositor_adds,
        .pending_compositor_moves = pending_compositor_moves,
        .pending_compositor_clipboard = pending_compositor_clipboard,
        .pending_compositor_rename = pending_compositor_rename,
        .pending_compositor_comment = pending_compositor_comment,
        .pending_compositor_collapse = pending_compositor_collapse,
        .pending_compositor_bypass = pending_compositor_bypass,
        .pending_compositor_reorder = pending_compositor_reorder,
        .compositor_clipboard = compositor_clipboard,
        .pending_compositor_connect = pending_compositor_connect,
        .pending_compositor_rewire = pending_compositor_rewire,
        .pending_compositor_waypoints = pending_compositor_waypoints,
        .pending_compositor_property = pending_compositor_property,
        .pending_compositor_delete = pending_compositor_delete,
        .pending_compositor_gesture = pending_compositor_gesture,
        .pending_deletes = pending_deletes,
        .pending_delete_gesture = pending_delete_gesture,
        .pending_project_save = pending_project_save,
        .pending_project_load = pending_project_load,
        .editor_coalescing_policy = editor_coalescing_policy,
        .pending_editor_coalescing = pending_editor_coalescing,
        .pending_editor_coalescing_close = pending_editor_coalescing_close,
        .history_descriptions = history_descriptions,
        .history_state = history_state,
        .pending_preloaded_project = pending_preloaded_project,
        .recent_project_settings_file = recent_project_settings_file,
        .recent_project_paths = recent_project_paths,
        .project_path = project_path,
        .project_message = project_message,
        .saved_project_snapshot = saved_project_snapshot,
        .project_explicitly_dirty = project_explicitly_dirty,
        .project_generation = project_generation,
        .linked_ingest = linked_ingest,
        .blender_links = blender_links,
        .blender_store_project = blender_store_project,
        .blender_link_transactions = blender_link_transactions,
        .unsaved_blender_name = unsaved_blender_name,
        .blender_scene_requests = blender_scene_requests,
        .blender_scene_creating = blender_scene_creating,
        .blender_associations = blender_associations,
        .blender_association_root = blender_association_root,
        .deferred_blender_connect = deferred_blender_connect,
        .deferred_blender_resync = deferred_blender_resync,
        .deferred_blender_open = deferred_blender_open,
        .blender_connection_error = blender_connection_error,
        .linked_publication = linked_publication,
        .director_publication_verification = director_publication_verification,
        .pending_publication_storage = pending_publication_storage,
        .pending_publication = pending_publication,
        .pending_publication_cancel = pending_publication_cancel,
        .pending_publication_rollback = pending_publication_rollback,
        .publication_staging = publication_staging,
        .publication_project = publication_project,
        .publication_activation = publication_activation,
        .publication_program = publication_program,
        .publication_message = publication_message,
        .publication_storage = publication_storage,
        .publication_message_shown = publication_message_shown,
        .scene_link_details = scene_link_details,
        .scene_link_diagnostics = scene_link_diagnostics,
        .blender_views = blender_views,
        .listed_scenes = listed_scenes,
        .blender_scene_states = blender_scene_states,
        .blender_bound_names = blender_bound_names,
        .publication_default_storage = publication_default_storage,
        .linked_ingest_base = linked_ingest_base,
        .linked_ingest_target = linked_ingest_target,
        .linked_ingest_project = linked_ingest_project,
        .linked_ingest_gesture = linked_ingest_gesture,
        .next_new_node = next_new_node,
#endif
        .rehearsal_mode = rehearsal_mode,
        .preview_transport = preview_transport,
        .authored = authored,
        .interval = interval,
        .sealed = sealed,
        .since_status = since_status,
        .anchors = anchors,
        .present_failures = present_failures,
        .configured_cluster_nodes = configured_cluster_nodes,
        .cluster_configuration_authored = cluster_configuration_authored,
        .cluster_diagnostics = cluster_diagnostics,
        .director_sync = director_sync,
        .director_node_tracking = director_node_tracking,
        .director_sync_model = director_sync_model,
        .director_sync_version = director_sync_version,
        .director_sync_loaded = director_sync_loaded,
        .observed_local_renderer = observed_local_renderer,
        .director_renderer_lockstep = director_renderer_lockstep,
        .conformance_request = conformance_request,
#if defined(MEDIASERVER_HAS_GUI)
        .director_conformance = director_conformance,
#endif
        .pending_asset_commit = pending_asset_commit,
        .last_asset_commit_frame = last_asset_commit_frame,
        .last_asset_commit_digest = last_asset_commit_digest,
        .last_asset_commit_stage = last_asset_commit_stage,
        .director_asset_cohort = director_asset_cohort,
        .asset_waiting_node = asset_waiting_node,
        .logged_asset_cohort = logged_asset_cohort,
        .autoplay_pending = autoplay_pending,
        .closed = closed,
        .scene_model = scene_model,
        .scene_version = scene_version,
        .scene_loaded = scene_loaded,
        .cue_version = cue_version,
        .cued_preview = cued_preview,
        .cued_rate = cued_rate,
        .feed_model = feed_model,
        .feed_version = feed_version,
        .feed_loaded = feed_loaded,
        .hardware_model = hardware_model,
        .hardware_version = hardware_version,
        .hardware_loaded = hardware_loaded,
        .asset_model = asset_model,
        .director_asset_registry = director_asset_registry,
        .director_playback_asset_registry = director_playback_asset_registry,
        .asset_version = asset_version,
        .asset_registry_digest = asset_registry_digest,
        .asset_manifest_digest = asset_manifest_digest,
        .assets_loaded = assets_loaded,
        .director_asset_availability = director_asset_availability,
        .changed_library_sources = changed_library_sources,
        .changed_library_assets = changed_library_assets,
#if defined(MEDIASERVER_HAS_GUI)
        .preferences_view = preferences_view,
        .stored_password_key = stored_password_key,
#endif
        .compositor_model = compositor_model,
        .compositor_version = compositor_version,
        .compositor_loaded = compositor_loaded,
    };
    return loop.run();
}

} // namespace mediaserver::app
