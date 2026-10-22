#pragma once

// Pacing constants, console labels, presenter creation and transport
// commands shared by the Director and node playback loops.

#include "edit_coalescing_policy.hpp"
#include "output_window_config.hpp"
#include "playback.hpp"

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/net/session_protocol.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/show/frame_boundary_driver.hpp>
#include <mediaserver/show/frame_waiter.hpp>
#include <mediaserver/show/presenter.hpp>
#include <mediaserver/show/show_clock.hpp>
#include <mediaserver/show/transport_authority.hpp>
#include <mediaserver/sync/sync_policy.hpp>
#include <mediaserver/sync/sync_status.hpp>

#if defined(MEDIASERVER_HAS_GUI)
#include <mediaserver/render/hybrid_scene_renderer.hpp>
#endif

#include <algorithm>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mediaserver::app {

using Clock = show::ShowClock::Clock;

// Shutdown latency ceiling. A boundary further away than this is slept to in
// steps so an interrupt is still observed promptly.
constexpr std::chrono::milliseconds max_sleep{10};

// Bound on the frames one Director iteration will seal after a late wake-up.
constexpr core::u64 max_director_catch_up = 8U;

// How often a process re-stats required paths while any of them is missing.
// A file can appear after start-up - a share mounts, or the media is copied in
// late - and the report has to follow. Nothing is re-checked once everything
// is present: only a manifest change asks again.
constexpr std::chrono::milliseconds library_proxy_retry_interval{500};

[[nodiscard]] bool parse_u64(std::string_view value,
                             core::u64 &parsed) noexcept;

void sleep_until_boundary(show::FrameWaiter &waiter,
                          Clock::time_point boundary) noexcept;

[[nodiscard]] const char *transport_label(show::TransportState state) noexcept;

[[nodiscard]] const char *
outcome_label(show::FrameCommitOutcome outcome) noexcept;

[[nodiscard]] const char *sync_mode_label(sync::SyncMode mode) noexcept;

[[nodiscard]] const char *
runtime_barrier_label(net::RuntimeBarrierState state) noexcept;

[[nodiscard]] const char *
presenter_error_text(show::PresenterError error) noexcept;

[[nodiscard]] show::ShowClockConfig
clock_config(const PlaybackOptions &options) noexcept;

// How late this loop woke for the boundaries since the last status line.
[[nodiscard]] std::string
boundary_wake_text(const show::BoundaryLateness::Window &window);

[[nodiscard]] std::string gpu_timing_text(const show::PresenterTiming &timing);

[[nodiscard]] net::RuntimeRendererBackend runtime_renderer_backend(
    scene::RendererBackend backend) noexcept;

[[nodiscard]] net::RuntimeRendererReadinessState runtime_renderer_state(
    show::PresenterRendererReadinessState state) noexcept;

[[nodiscard]] net::RuntimeRendererReadinessState runtime_renderer_state(
    render::RendererReadinessCode state) noexcept;

[[nodiscard]] net::RuntimeRendererReadiness runtime_renderer_readiness(
    const show::PresenterRendererReadiness &observed) noexcept;

[[nodiscard]] sync::ShowClockReferenceState show_clock_reference_state(
    show::VideoReferenceState state) noexcept;

[[nodiscard]] core::u64 pacer_lag(const show::ShowClock &pacer,
                                  core::u64 sealed) noexcept;

[[nodiscard]] core::u64 status_interval(const PlaybackOptions &options,
                                        show::FrameRate rate) noexcept;

[[nodiscard]] std::unique_ptr<show::Presenter>
make_presenter(const PlaybackOptions &options, const std::string &title,
               bool accepts_commands,
               const PresenterWindowSettings *applied_window = nullptr);

[[nodiscard]] std::vector<show::CuePoint>
scene_cue_sheet(const scene::SceneModel &model, scene::SceneId scene_id,
                show::FrameRate rate);

void apply_command(show::TransportAuthority &transport,
                   show::PresenterCommand command,
                   cmd::FrameNumber at) noexcept;

} // namespace mediaserver::app
