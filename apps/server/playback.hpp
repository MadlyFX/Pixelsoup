#pragma once

#include <csignal>
#include <cstddef>
#include <string_view>

#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/cmd/node_command_bus.hpp>
#include <mediaserver/net/director_frame_barrier.hpp>
#include <mediaserver/net/director_session_hub.hpp>
#include <mediaserver/net/node_session_service.hpp>
#include <mediaserver/show/show_clock.hpp>

namespace mediaserver::app {

struct PlaybackOptions final {
    bool enabled{};
    show::FrameRate rate{show::FrameRate{50U, 1U}};
    show::ShowFrame first_frame{};
    show::ShowFrame last_frame{show::ShowFrame{999'999U}};
    bool loop{};
    // The Director starts its transport immediately. A node always paces
    // itself; what it presents still comes from replicated transport state.
    bool autoplay{true};
    // Show frames to run before exiting cleanly. Zero runs until stopped.
    core::u64 run_frames{};
    // Boundaries between status lines. Zero derives one line per second.
    core::u64 status_interval{};
    // Presentation surface for the numbered test frames.
    core::u32 surface_width{640U};
    core::u32 surface_height{360U};
    core::u32 node_index{};
    // Open a real output window instead of rasterising to memory. Requires a
    // build with the SDL3 shell enabled.
    bool windowed{};
    core::i32 window_x{-1};
    core::i32 window_y{-1};
    bool fullscreen{};
    bool borderless{};
};

// True when this build can open an output window at all.
[[nodiscard]] bool window_output_available() noexcept;

// Parses "NUM/DEN" or "NUM". Returns false without changing `rate` on refusal.
[[nodiscard]] bool parse_frame_rate(
    std::string_view value,
    show::FrameRate& rate) noexcept;

// Parses "FIRST:LAST".
[[nodiscard]] bool parse_show_range(
    std::string_view value,
    show::ShowFrame& first,
    show::ShowFrame& last) noexcept;

[[nodiscard]] bool valid_playback_options(
    const PlaybackOptions& options) noexcept;

// Node role. Paces its own frame boundary, publishes only Director-approved
// frames, and presents the show position carried by the committed document.
// Returns a process exit code.
[[nodiscard]] int run_node_playback(
    net::NodeSessionService& service,
    cmd::NodeCommandBus& node,
    const PlaybackOptions& options,
    const volatile std::sig_atomic_t& stop_requested);

// Director role. Owns the master transport and seals exactly one control frame
// per show frame, so every node derives the same position from the same
// committed revision. Returns a process exit code.
[[nodiscard]] int run_director_show(
    cmd::DirectorScheduler& scheduler,
    net::DirectorSessionHub& hub,
    net::DirectorFrameBarrier& barrier,
    const PlaybackOptions& options,
    const volatile std::sig_atomic_t& stop_requested);

} // namespace mediaserver::app
