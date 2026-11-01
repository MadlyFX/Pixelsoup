#pragma once

#include "document.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Where Studio was left: the page that was open (and its changes, if they
// were not saved), how the page was laid out - folded layers, open blocks,
// the scroll position, the block being worked on - and the window, panels,
// camera, playhead and overlays. Studio writes it when it closes and reads it
// when it opens, so it comes back as it was.
//
// It is the artist's workspace, not the effect: nothing here is exported or
// saved in a page.
namespace ajfx::studio {

// How one page was laid out.
struct PageViewState final {
    std::vector<Uid> folded;
    std::vector<Uid> open;
    // Blocks with "More settings" showing.
    std::vector<Uid> details;
    Uid focus{};
    Uid layer{};
    float scroll{0.0F};

    friend bool operator==(const PageViewState&, const PageViewState&) = default;
};

struct SessionState final {
    // The page that was open; empty for one never saved.
    std::string page;
    // The page itself when it had changes not on disk (or was never saved),
    // as page text; empty when the file on disk is the page.
    std::string unsaved;
    PageViewState view;
    // How other pages were laid out when last open, by path, newest first.
    std::vector<std::pair<std::string, PageViewState>> recent;

    // The window, in logical units; 0 leaves Studio's own size.
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    // The page's share of the space beside the preview.
    float split{0.46F};
    bool palette{true};
    // Palette sections folded shut, by heading.
    std::vector<std::string> palette_folded;

    bool guides{true};
    bool handles{true};
    bool paths{false};
    bool figure{true};
    bool expert{false};
    bool looping{true};

    // The preview camera (yaw, pitch, distance, target x, y, z), when kept.
    std::optional<std::array<float, 6>> camera;
    double seconds{0.0};
    bool playing{true};

    friend bool operator==(const SessionState&, const SessionState&) = default;
};

// Pages remembered in `recent`.
inline constexpr std::size_t remembered_pages = 16U;

[[nodiscard]] std::string save_session(const SessionState& session);
// What can be read of `text`; anything missing or malformed keeps its default.
[[nodiscard]] SessionState load_session(std::string_view text);

// Puts `view` first among the remembered pages, under `page`.
void remember_view(SessionState& session, const std::string& page, const PageViewState& view);
// How `page` was laid out when last open, if it was.
[[nodiscard]] std::optional<PageViewState> remembered_view(const SessionState& session, std::string_view page);

// %APPDATA%/ajfx Studio/session.json on Windows; elsewhere under
// $XDG_CONFIG_HOME or ~/.config. Empty when there is nowhere to keep it.
[[nodiscard]] std::filesystem::path session_path();

} // namespace ajfx::studio
