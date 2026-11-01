#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// ajfx Studio's window: the page on the left, the preview and its transport
// on the right, and the few actions a whole effect has across the top.
namespace ajfx::studio {

struct StudioOptions final {
    // A page to open at start; empty takes up where Studio was left, or
    // starts from a recipe.
    std::filesystem::path open;
    // Take up where Studio was left: the page, its layout, the window and
    // panels. A page or recipe given at start replaces the page; --fresh
    // forgets the lot.
    bool restore_page{true};
    bool fresh{false};
    // The recipe to start from when no page is given.
    std::string recipe{"campfire"};
    // Starting states, for checking the interface: the expert view, blocks
    // of one kind open ("all" for every block of the first layer), the
    // block picker open.
    bool expert{false};
    std::string open_blocks;
    bool picker{false};
    // Start paused this many seconds in; negative plays from the start.
    float at{-1.0F};
    // Play once and stop instead of looping.
    bool once{false};
    // Values to try for published parameters ("name=v" or "name=r,g,b,a").
    std::vector<std::string> tries;
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
