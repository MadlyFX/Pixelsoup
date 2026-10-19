#pragma once

#include <csignal>
#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/cmd/node_command_bus.hpp>
#include <mediaserver/net/director_frame_barrier.hpp>
#include <mediaserver/net/director_session_service.hpp>
#include <mediaserver/net/director_session_hub.hpp>
#include <mediaserver/net/node_session_service.hpp>
#include <mediaserver/persist/command_log.hpp>
#include <mediaserver/persist/durable_director.hpp>
#include <mediaserver/persist/settings.hpp>
#include <mediaserver/show/demo_edit_scene.hpp>
#include <mediaserver/show/show_clock.hpp>

namespace mediaserver::app {

struct PlaybackOptions final {
    bool enabled{};
    show::FrameRate rate{show::FrameRate{24U, 1U}};
    show::ShowFrame first_frame{};
    // Five minutes at the default 24 fps; the last frame is inclusive.
    show::ShowFrame last_frame{show::ShowFrame{7'199U}};
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
    // Put each node's slice through its feed map before presenting. The OUTPUT
    // stage keeps the application window raster fixed and places the rendered
    // image in the map's independent destination viewport, leaving the rest
    // black. The Director authors the map alongside the scene; each node reads
    // its own binding from the committed document. Implies `scene`, which is
    // the source it transforms. See FeedMapModel and OutputTransform.
    bool output{};
    // Open the bespoke docked operator editor instead of a preview window.
    // Program, Stage, Inspector, and Timeline all author ordinary document
    // commands. Director-only; authors a built-in edit scene. Requires a build
    // with the editor and a GPU.
    bool edit{};
    core::i32 window_x{-1};
    core::i32 window_y{-1};
    // Separate from the signed coordinates: negative desktop positions are
    // valid when a monitor is arranged left/above the primary display.
    bool window_position_explicit{};
    bool fullscreen{};
    bool borderless{};
    // The key Output Monitor authenticates with render-machine agents,
    // derived from the render node password (command line, else
    // Preferences); with no password it is the unsecured key every Media
    // Server knows. Process configuration, never project data.
    std::string lifecycle_secret;
    // The UDP port render machines answer discovery on, which the Hardware
    // Map asks; 0 turns discovery off. Matches net::cluster_discovery_port.
    std::uint16_t lifecycle_discovery_port{47'839U};
    // The application's default WAL may use an identical recent project as a
    // saved baseline after recovery. Explicit/custom WALs remain untitled.
    bool associate_recent_project_after_recovery{};

    // Machine-local preferences, resolved at start-up. `stored` is what the
    // settings file holds and what the Preferences panel edits; `active` is
    // what this run is really using, which differs wherever a command-line
    // argument was given. Keeping both means the panel can say which is which
    // instead of appearing to ignore the operator.
    std::filesystem::path preferences_file;
    persist::Preferences stored_preferences;
    persist::Preferences active_preferences;
    bool director_listen_from_command_line{};
    bool show_id_from_command_line{};
    bool director_epoch_from_command_line{};
    bool lifecycle_password_from_command_line{};
    bool wal_path_from_command_line{};
};

enum class InitialFeedMapKind : core::u8 {
    None,
    OutputDemo,
    EditorCanvas,
};

struct InitialFeedMapPlan final {
    InitialFeedMapKind kind{InitialFeedMapKind::None};
    core::u32 source_width{};
    core::u32 source_height{};

    bool operator==(const InitialFeedMapPlan&) const = default;
};

// Output nodes refuse to guess a feed binding. A fresh editor therefore seeds
// one node-0 map spanning its full canvas so the supervised output child can
// mirror it. Recovered editor sessions already own their project map, while an
// explicit --output retains the established two-node wall demo. The priority
// also makes --edit --output one authoring operation rather than two batches.
[[nodiscard]] constexpr InitialFeedMapPlan initial_feed_map_plan(
    const PlaybackOptions& options,
    bool recovered_session) noexcept {
    if (options.output) {
        return InitialFeedMapPlan{InitialFeedMapKind::OutputDemo, 0U, 0U};
    }
    if (options.edit && !recovered_session) {
        return InitialFeedMapPlan{
            InitialFeedMapKind::EditorCanvas,
            show::demo_edit_canvas_width,
            show::demo_edit_canvas_height};
    }
    return {};
}

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
// (UI + scene shaders) and a GPU.
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
    persist::CommandLog& command_log,
    persist::FrameDurability frame_durability,
    net::DirectorSessionHub& hub,
    net::DirectorFrameBarrier& barrier,
    const net::DirectorSessionService* session_service,
    const PlaybackOptions& options,
    const volatile std::sig_atomic_t& stop_requested);

} // namespace mediaserver::app
