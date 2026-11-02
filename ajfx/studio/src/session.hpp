#pragma once

#include "document.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Where Studio was left: the scene that was open (and its changes, if they
// were not saved), the selection and folded groups, and the window, panels,
// camera, playhead and overlays. Studio writes it when it closes and reads it
// when it opens, so it comes back as it was.
//
// It is the artist's workspace, not the scene: nothing here is exported or
// saved in a scene.
namespace ajfx::studio {

struct SessionState final {
    // The scene file that was open; empty for one never saved.
    std::string page;
    // The scene itself when it had changes not on disk (or was never saved),
    // as scene text; empty when the file on disk is the scene.
    std::string unsaved;

    // The window, in logical units; 0 leaves Studio's own size.
    std::uint32_t width{0U};
    std::uint32_t height{0U};

    bool guides{true};
    bool handles{true};
    bool paths{false};
    bool figure{true};
    bool night{false};
    float zoom{1.0F};
    bool looping{true};
    float scene_tree_split{0.19F};
    float scene_inspector_split{0.66F};
    std::vector<Uid> scene_selection;
    std::vector<Uid> scene_folded;

    // The preview camera (yaw, pitch, distance, target x, y, z), when kept.
    std::optional<std::array<float, 6>> camera;
    double seconds{0.0};
    bool playing{true};

    friend bool operator==(const SessionState&, const SessionState&) = default;
};

[[nodiscard]] std::string save_session(const SessionState& session);
// What can be read of `text`; anything missing or malformed keeps its default.
[[nodiscard]] SessionState load_session(std::string_view text);

// %APPDATA%/ajfx Studio/session.json on Windows; elsewhere under
// $XDG_CONFIG_HOME or ~/.config. Empty when there is nowhere to keep it.
[[nodiscard]] std::filesystem::path session_path();

} // namespace ajfx::studio
