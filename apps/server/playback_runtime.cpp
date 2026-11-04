#include "playback_runtime.hpp"

#include <mediaserver/core/render_conformance.hpp>

#if defined(MEDIASERVER_HAS_GUI)
#include <mediaserver/ui/sdl_presenter.hpp>
#endif

#include <charconv>
#include <iostream>
#include <system_error>
#include <thread>
#include <utility>

namespace mediaserver::app {

[[nodiscard]] bool parse_u64(std::string_view value,
                             core::u64 &parsed) noexcept {
    if (value.empty()) {
        return false;
    }
    const auto *const begin = value.data();
    const auto *const end = begin + value.size();
    const auto result = std::from_chars(begin, end, parsed, 10);
    return result.ec == std::errc{} && result.ptr == end;
}

void sleep_until_boundary(show::FrameWaiter &waiter,
                          Clock::time_point boundary) noexcept {
    waiter.wait_until(boundary, max_sleep);
}

[[nodiscard]] const char *transport_label(show::TransportState state) noexcept {
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

[[nodiscard]] const char *
outcome_label(show::FrameCommitOutcome outcome) noexcept {
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

[[nodiscard]] const char *sync_mode_label(sync::SyncMode mode) noexcept {
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

[[nodiscard]] const char *
runtime_barrier_label(net::RuntimeBarrierState state) noexcept {
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

[[nodiscard]] const char *
presenter_error_text(show::PresenterError error) noexcept {
    switch (error) {
    case show::PresenterError::InvalidConfiguration:
        return "invalid surface configuration";
    case show::PresenterError::AllocationFailed:
        return "allocation failed";
    case show::PresenterError::ExactDisplayModeUnavailable:
        return "the selected display does not support the exact saved "
               "resolution and refresh rate";
    case show::PresenterError::DisplayTargetUnavailable:
        return "the display this output is bound to is not one this machine "
               "presents as its own desktop area";
    case show::PresenterError::DisplayUnavailable:
        break;
    }
    return "the platform refused a window";
}

[[nodiscard]] show::ShowClockConfig
clock_config(const PlaybackOptions &options) noexcept {
    return show::ShowClockConfig{options.rate, options.first_frame,
                                 options.last_frame, options.loop};
}

// Per-frame presentation cost, reported only where there is one to report.
// The CPU path has no submission to time and says nothing rather than zero.
[[nodiscard]] std::string
boundary_wake_text(const show::BoundaryLateness::Window &window) {
    return " wake-max-us=" + std::to_string(window.max.count()) +
           " wake-mean-us=" + std::to_string(window.mean.count()) +
           " wake-late=" + std::to_string(window.late) + '/' +
           std::to_string(window.boundaries);
}

[[nodiscard]] std::string gpu_timing_text(const show::PresenterTiming &timing) {
    if (!timing.gpu) {
        return {};
    }
    return " present-us=" + std::to_string(timing.last_present_ns / 1'000U) +
           " present-peak-us=" +
           std::to_string(timing.peak_present_ns / 1'000U) +
           " gpu-observed-us=" +
           std::to_string(timing.last_submit_to_observed_ns / 1'000U) +
           " gpu-skipped=" + std::to_string(timing.skipped_presents) +
           " media-buffered=" + std::to_string(timing.media_deferred_frames) +
           " gpu-present=" + (timing.vsync_limited ? "vsync" : "unthrottled");
}

[[nodiscard]] net::RuntimeRendererBackend runtime_renderer_backend(
    scene::RendererBackend backend) noexcept {
    return backend == scene::RendererBackend::Advanced
        ? net::RuntimeRendererBackend::Advanced
        : net::RuntimeRendererBackend::Basic;
}

[[nodiscard]] net::RuntimeRendererReadinessState runtime_renderer_state(
    show::PresenterRendererReadinessState state) noexcept {
    switch (state) {
    case show::PresenterRendererReadinessState::Ready:
        return net::RuntimeRendererReadinessState::Ready;
    case show::PresenterRendererReadinessState::BasicFallback:
        return net::RuntimeRendererReadinessState::BasicFallback;
    case show::PresenterRendererReadinessState::MissingCapabilities:
        return net::RuntimeRendererReadinessState::MissingCapabilities;
    case show::PresenterRendererReadinessState::
        AdvancedImplementationUnavailable:
        return net::RuntimeRendererReadinessState::
            AdvancedImplementationUnavailable;
    case show::PresenterRendererReadinessState::InvalidPolicy:
        return net::RuntimeRendererReadinessState::InvalidPolicy;
    case show::PresenterRendererReadinessState::VideoMemoryShort:
        return net::RuntimeRendererReadinessState::VideoMemoryShort;
    case show::PresenterRendererReadinessState::Unknown:
        return net::RuntimeRendererReadinessState::Unknown;
    }
    return net::RuntimeRendererReadinessState::Unknown;
}

[[nodiscard]] net::RuntimeRendererReadinessState runtime_renderer_state(
    render::RendererReadinessCode state) noexcept {
    switch (state) {
    case render::RendererReadinessCode::Ready:
        return net::RuntimeRendererReadinessState::Ready;
    case render::RendererReadinessCode::BasicFallback:
        return net::RuntimeRendererReadinessState::BasicFallback;
    case render::RendererReadinessCode::MissingCapabilities:
        return net::RuntimeRendererReadinessState::MissingCapabilities;
    case render::RendererReadinessCode::RequestedFeaturesUnavailable:
        return net::RuntimeRendererReadinessState::
            AdvancedImplementationUnavailable;
    case render::RendererReadinessCode::AdvancedImplementationUnavailable:
        return net::RuntimeRendererReadinessState::
            AdvancedImplementationUnavailable;
    case render::RendererReadinessCode::InvalidPolicy:
    case render::RendererReadinessCode::OrthographicCamera:
        return net::RuntimeRendererReadinessState::InvalidPolicy;
    case render::RendererReadinessCode::VideoMemoryShort:
        return net::RuntimeRendererReadinessState::VideoMemoryShort;
    }
    return net::RuntimeRendererReadinessState::Unknown;
}

[[nodiscard]] net::RuntimeRendererReadiness runtime_renderer_readiness(
    const show::PresenterRendererReadiness &observed) noexcept {
    if (observed.profile_digest == 0U ||
        observed.state == show::PresenterRendererReadinessState::Unknown) {
        return {};
    }
    return net::RuntimeRendererReadiness{
        observed.profile_digest,
        runtime_renderer_state(observed.state),
        runtime_renderer_backend(observed.requested),
        runtime_renderer_backend(observed.selected),
        observed.missing_capabilities,
        observed.satisfied,
        observed.fallback,
        observed.assets,
        observed.temporal_restarts,
        observed.temporal_restart_frame,
        core::RenderConformance{.adapter = observed.adapter},
        observed.timing,
        observed.video_memory_needed,
        observed.video_memory_free,
    };
}

// Control frames the wall clock says are owed but the seal loop has not
// produced. Under an anchored transport a Director that cannot keep up makes
// the show run slow rather than skip positions, so this is the number that
// says so instead of the old skipped-position count.
// The show-clock reference vocabulary, from the presenter's.
//
// Two enumerations that happen to line up numerically, kept apart by a switch:
// one is what a presentation backend observed, the other is what an operator
// surface reports, and a cast between them would survive right up until someone
// inserted a value.
[[nodiscard]] sync::ShowClockReferenceState show_clock_reference_state(
    show::VideoReferenceState state) noexcept {
    switch (state) {
    case show::VideoReferenceState::Unsupported:
        return sync::ShowClockReferenceState::Unsupported;
    case show::VideoReferenceState::Waiting:
        return sync::ShowClockReferenceState::Waiting;
    case show::VideoReferenceState::Locked:
        return sync::ShowClockReferenceState::Locked;
    case show::VideoReferenceState::Lost:
        return sync::ShowClockReferenceState::Lost;
    case show::VideoReferenceState::Failed:
        return sync::ShowClockReferenceState::Failed;
    case show::VideoReferenceState::Unknown:
        return sync::ShowClockReferenceState::Unknown;
    }
    return sync::ShowClockReferenceState::Unknown;
}

[[nodiscard]] core::u64 pacer_lag(const show::ShowClock &pacer,
                                  core::u64 sealed) noexcept {
    const auto due = pacer.frame().value();
    return due > sealed ? due - sealed : 0U;
}

[[nodiscard]] core::u64 status_interval(const PlaybackOptions &options,
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
[[nodiscard]] std::unique_ptr<show::Presenter>
make_presenter(const PlaybackOptions &options, const std::string &title,
               bool accepts_commands,
               const PresenterWindowSettings *applied_window) {
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
            config.output_bit_depth = applied_window->output_bit_depth;
            config.require_exact_display_mode =
                applied_window->require_exact_display_mode;
            config.display_bounds_x = applied_window->display_bounds_x;
            config.display_bounds_y = applied_window->display_bounds_y;
            config.display_bounds_width = applied_window->display_bounds_width;
            config.display_bounds_height =
                applied_window->display_bounds_height;
        }
        config.accepts_commands = accepts_commands;
        config.gpu = options.gpu;
        config.scene = options.scene;
        config.output = options.output;
        config.buffered_media = options.output && !accepts_commands;
        config.media_read_ahead_frames =
            options.active_preferences.output_read_ahead_frames;
        config.media_cache_mebibytes =
            options.active_preferences.playback_cache_mebibytes;
        config.media_trail_frames =
            options.active_preferences.playback_trail_frames;

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

    auto created = show::HeadlessPresenter::create(options.surface_width,
                                                   options.surface_height);
    if (!created) {
        std::cerr << "could not allocate the presentation surface: "
                  << presenter_error_text(created.error()) << '\n';
        return nullptr;
    }
    return std::move(created).value();
}

// The cues of one scene as the transport acts on them. Plain markers ask
// nothing of the show and are left out.
[[nodiscard]] std::vector<show::CuePoint>
scene_cue_sheet(const scene::SceneModel &model, scene::SceneId scene_id,
                show::FrameRate rate) {
    // A Loop's wait and crossfade are authored in milliseconds and act in
    // control frames, which are show frames at the show's rate.
    const auto frames = [rate](core::u32 milliseconds) noexcept {
        const core::u64 scaled =
            static_cast<core::u64>(milliseconds) * rate.numerator;
        const core::u64 per = 1'000ULL * std::max(rate.denominator, 1U);
        return (scaled + per / 2U) / per;
    };
    std::vector<show::CuePoint> sheet;
    for (const scene::TimelineMarker &marker : model.markers(scene_id)) {
        if (!marker.is_cue() || marker.frame < 0) {
            continue;
        }
        show::CuePoint cue{
            show::ShowFrame{static_cast<core::u64>(marker.frame)},
            marker.cue == scene::CueAction::Loop ? show::CueAction::Loop
                                                 : show::CueAction::Hold,
            marker.id.value()};
        if (marker.cue == scene::CueAction::Loop) {
            cue.wait_frames = frames(marker.loop_wait_ms);
            cue.fade_frames = frames(marker.loop_fade_ms);
        }
        sheet.push_back(cue);
    }
    return sheet;
}

} // namespace mediaserver::app
