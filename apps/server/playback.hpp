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
    // Composite into a GPU target and blit it to the swapchain instead of
    // uploading a CPU surface every frame. Requires a window.
    bool gpu{};
    // Present a replicated 3D scene instead of the numbered test pattern. The
    // Director authors a built-in scene into the document; every node evaluates
    // it from the committed document at the show frame and draws its own slice
    // of the shared canvas. Implies a GPU window: there is no CPU path for a
    // depth-tested, shaded scene. See scene::SceneModel and render::SceneRenderer.
    bool scene{};
    // Put each node's slice through its feed map before presenting: the OUTPUT
    // stage that crops and soft-edge blends the rendered region so two nodes
    // covering an overlap cross-fade into one seamless wall. The Director
    // authors a built-in feed map alongside the scene; each node reads its own
    // binding from the committed document. Implies `scene`, which is the source
    // it transforms. See output::FeedMapModel and render::OutputTransform.
    bool output{};
    // Open the ImGui operator editor instead of a preview window: place a clip
    // in a 2D wall view (and later a 3D view) and drag it, each move authored as
    // an ordinary document command. Director-only; authors a built-in edit
    // scene. Requires a build with the editor and a GPU.
    bool edit{};
    core::i32 window_x{-1};
    core::i32 window_y{-1};
    bool fullscreen{};
    bool borderless{};
};

// True when this build can open an output window at all.
[[nodiscard]] bool window_output_available() noexcept;

// True when this build can composite on the GPU: it has both shader bytecode
// and a device to run it on.
[[nodiscard]] bool gpu_output_available() noexcept;

// True when this build can draw a 3D scene: it has the scene shader bytecode
// and a device. A build can have the 2D GPU path and not this one only if the
// shader build was partially applied.
[[nodiscard]] bool scene_output_available() noexcept;

// True when this build can drive the output stage: it has the output shader
// bytecode, the scene it transforms, and a device.
[[nodiscard]] bool output_output_available() noexcept;

// True when this build can open the operator editor: it has the editor module
// (ImGui + scene shaders) and a GPU.
[[nodiscard]] bool editor_available() noexcept;

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
