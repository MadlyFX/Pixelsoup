#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// ajfx Studio's window: the scene tree on the left, the viewport and its
// transport in the middle, the inspector on the right, and the few actions a
// whole scene has across the top.
namespace ajfx::studio {

struct StudioOptions final {
    // A scene to open at start; empty takes up where Studio was left, or
    // starts a new scene.
    std::filesystem::path open;
    // Take up where Studio was left: the scene, the window and panels. A
    // scene given at start replaces it; --fresh forgets the lot.
    bool restore_page{true};
    bool fresh{false};
    // Start paused this many seconds in; negative plays from the start.
    float at{-1.0F};
    // Play once and stop instead of looping.
    bool once{false};
    // The preview's overlays, when given at start: guides and handles, and
    // particle paths. Otherwise they are as they were left.
    std::optional<bool> guides;
    std::optional<bool> paths;
    // The graphics API's validation layer, for chasing a lost GPU device.
    bool gpu_debug{false};
    // Seconds after start to have the GPU device removed, as a fault or a
    // driver reset would, to check what Studio does then; negative never.
    float lose_device{-1.0F};
};

int run_studio(const StudioOptions& options);

} // namespace ajfx::studio
