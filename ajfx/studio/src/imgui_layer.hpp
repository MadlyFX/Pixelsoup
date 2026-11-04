#pragma once

#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/ui_renderer.hpp>
#include <mediaserver/gui/window.hpp>

#include <imgui.h>

#include <string>
#include <vector>

// Dear ImGui (docking) as Studio's interface framework, drawn through the
// media server's own renderer so the platform stays behind the RHI: the SDL
// events reach it through the window's event tap, its triangles are handed to
// the UI draw list as they are, and its font page is one more texture the
// window uploads. Everything is in physical pixels; the font and the style are
// sized for the window's scale.
//
// Everything Studio shows is drawn with it; the window's own renderer only
// draws the triangles it makes.
namespace ajfx::studio {

class ImguiLayer final {
public:
    // `ini` is where the arrangement of the windows is kept; empty keeps none.
    ImguiLayer(mediaserver::gui::UiWindow& window, std::string ini);
    ~ImguiLayer();
    ImguiLayer(const ImguiLayer&) = delete;
    ImguiLayer& operator=(const ImguiLayer&) = delete;

    // Starts ImGui's frame. Call straight after the window pumped its events.
    void begin_frame();
    // The workspace dimmed for night.
    void set_night(bool night);
    // Ends the frame and adds what it drew to `list`.
    void end_frame(mediaserver::gui::UiDrawList& list);

    // The texture an image names.
    [[nodiscard]] static ImTextureID texture_id(const mediaserver::rhi::Texture& texture) noexcept {
        return static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(&texture));
    }

private:
    void build_fonts(float scale);
    void apply_style();

    mediaserver::gui::UiWindow& window_;
    std::string ini_;
    const mediaserver::rhi::Texture* font_page_{nullptr};
    float scale_{0.0F};
    bool night_{false};
    bool styled_night_{false};
};

// Adds `data` (ImGui's frame) to `list` as triangles in physical pixels,
// each batch sampling the texture its command names.
void append_draw_data(const ImDrawData& data, mediaserver::gui::UiDrawList& list);

} // namespace ajfx::studio
