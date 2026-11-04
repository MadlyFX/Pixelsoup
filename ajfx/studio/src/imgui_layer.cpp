#include "imgui_layer.hpp"

#include <imgui_impl_sdl3.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

namespace ajfx::studio {
namespace {

namespace gui = mediaserver::gui;

constexpr float base_font_size = 15.0F;

// Straight colour from ImGui's packed ABGR.
gui::UiVertex vertex_of(const ImDrawVert& v, float offset_x, float offset_y, float scale_x, float scale_y) {
    constexpr float byte = 1.0F / 255.0F;
    return gui::UiVertex{(v.pos.x - offset_x) * scale_x,
                         (v.pos.y - offset_y) * scale_y,
                         v.uv.x,
                         v.uv.y,
                         static_cast<float>((v.col >> IM_COL32_R_SHIFT) & 0xFFU) * byte,
                         static_cast<float>((v.col >> IM_COL32_G_SHIFT) & 0xFFU) * byte,
                         static_cast<float>((v.col >> IM_COL32_B_SHIFT) & 0xFFU) * byte,
                         static_cast<float>((v.col >> IM_COL32_A_SHIFT) & 0xFFU) * byte};
}

} // namespace

void append_draw_data(const ImDrawData& data, gui::UiDrawList& list) {
    if (data.CmdListsCount == 0) {
        return;
    }
    const float scale_x = data.FramebufferScale.x;
    const float scale_y = data.FramebufferScale.y;
    std::vector<gui::UiVertex> triangles;
    for (int n = 0; n < data.CmdListsCount; ++n) {
        const ImDrawList& commands = *data.CmdLists[n];
        for (const ImDrawCmd& cmd : commands.CmdBuffer) {
            if (cmd.UserCallback != nullptr || cmd.ElemCount == 0U) {
                continue;
            }
            const float min_x = (cmd.ClipRect.x - data.DisplayPos.x) * scale_x;
            const float min_y = (cmd.ClipRect.y - data.DisplayPos.y) * scale_y;
            const float max_x = (cmd.ClipRect.z - data.DisplayPos.x) * scale_x;
            const float max_y = (cmd.ClipRect.w - data.DisplayPos.y) * scale_y;
            triangles.clear();
            triangles.reserve(cmd.ElemCount);
            for (unsigned int i = 0U; i < cmd.ElemCount; ++i) {
                const ImDrawIdx index = commands.IdxBuffer[static_cast<int>(cmd.IdxOffset + i)];
                const ImDrawVert& v = commands.VtxBuffer[static_cast<int>(cmd.VtxOffset + index)];
                triangles.push_back(vertex_of(v, data.DisplayPos.x, data.DisplayPos.y, scale_x, scale_y));
            }
            const auto* texture = reinterpret_cast<const mediaserver::rhi::Texture*>(
                static_cast<std::uintptr_t>(cmd.GetTexID()));
            list.add_triangles(triangles, texture, min_x, min_y, max_x, max_y);
        }
    }
}

ImguiLayer::ImguiLayer(gui::UiWindow& window, std::string ini) : window_{window}, ini_{std::move(ini)} {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.IniFilename = ini_.empty() ? nullptr : ini_.c_str();
    ImGui_ImplSDL3_InitForOther(static_cast<SDL_Window*>(window.native_window()));
    window.set_event_tap([](const void* event) {
        ImGui_ImplSDL3_ProcessEvent(static_cast<const SDL_Event*>(event));
    });
}

ImguiLayer::~ImguiLayer() {
    window_.set_event_tap({});
    if (font_page_ != nullptr) {
        window_.release_texture(font_page_);
    }
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void ImguiLayer::build_fonts(float scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    // What the interface says: Latin with its accents, Greek and Cyrillic, and
    // the marks it draws (degrees, squares, times, minus and the arrows).
    static const std::array<ImWchar, 5U> marks{0x00B0, 0x00B3, 0x2190, 0x2193, 0};
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
    builder.AddRanges(io.Fonts->GetGlyphRangesGreek());
    builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
    builder.AddRanges(marks.data());
    for (const ImWchar c : std::array<ImWchar, 8U>{0x00D7, 0x2013, 0x2014, 0x2022, 0x2026, 0x2212, 0x25CF, 0x25CB}) {
        builder.AddChar(c);
    }
    static ImVector<ImWchar> ranges;
    ranges.clear();
    builder.BuildRanges(&ranges);
    ImFont* font = nullptr;
    for (const char* path : {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf", "C:/Windows/Fonts/tahoma.ttf"}) {
        font = io.Fonts->AddFontFromFileTTF(path, base_font_size * scale, nullptr, ranges.Data);
        if (font != nullptr) {
            break;
        }
    }
    if (font == nullptr) {
        io.Fonts->AddFontDefault();
    }
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    if (font_page_ != nullptr) {
        window_.release_texture(font_page_);
    }
    font_page_ = window_.upload_texture(
        std::span<const std::uint8_t>{pixels, static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U},
        static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height));
    io.Fonts->SetTexID(font_page_ != nullptr ? texture_id(*font_page_) : ImTextureID{});

    ImGuiStyle style;
    ImGui::StyleColorsDark(&style);
    style.WindowRounding = 8.0F;
    style.ChildRounding = 6.0F;
    style.FrameRounding = 6.0F;
    style.PopupRounding = 8.0F;
    style.GrabRounding = 6.0F;
    style.TabRounding = 6.0F;
    style.ScrollbarRounding = 8.0F;
    style.WindowBorderSize = 0.0F;
    style.FrameBorderSize = 0.0F;
    style.WindowPadding = ImVec2{10.0F, 10.0F};
    style.FramePadding = ImVec2{8.0F, 4.0F};
    style.ItemSpacing = ImVec2{8.0F, 6.0F};
    style.ScaleAllSizes(scale);
    ImGui::GetStyle() = style;
}

void ImguiLayer::begin_frame(gui::InputState& input) {
    const float scale = window_.ui_scale();
    if (std::abs(scale - scale_) > 0.001F) {
        scale_ = scale;
        build_fonts(scale);
    }
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    const ImGuiIO& io = ImGui::GetIO();
    pointer_ = io.WantCaptureMouse;
    keyboard_ = io.WantCaptureKeyboard || io.WantTextInput;
    if (pointer_) {
        input.withhold_pointer();
    }
    if (keyboard_) {
        input.withhold_keyboard();
    }
}

void ImguiLayer::end_frame(gui::UiDrawList& list) {
    ImGui::Render();
    if (const ImDrawData* data = ImGui::GetDrawData()) {
        append_draw_data(*data, list);
    }
}

} // namespace ajfx::studio
