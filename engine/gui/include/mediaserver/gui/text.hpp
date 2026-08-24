#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/gui/ui_renderer.hpp>

#include <map>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// Text for the bespoke UI: a FreeType-rasterized font baked into the same RGBA
// atlas the UiRenderer samples (coverage in alpha, rgb = 1), plus a reserved
// white texel for solid fills. ASCII 32..126 is baked once at construction,
// which covers the operator UI's labels and numbers; complex shaping (HarfBuzz)
// and on-demand glyphs are a later step.
//
// Small UI numbers use FreeType's hinted grayscale rasterization rather than an
// SDF, which at these sizes reads as blur - the dark-room readability point from
// the UI plan.
namespace mediaserver::gui {

struct Glyph final {
    float u0{};
    float v0{};
    float u1{};
    float v1{};
    float width{};
    float height{};
    float bearing_x{};
    float bearing_y{};
    float advance{};
};

class FontAtlas final {
public:
    // Bakes `font_path` at `pixel_height`. Returns nullopt if FreeType cannot
    // load the file, so the caller can fall back or skip text.
    [[nodiscard]] static std::optional<FontAtlas> create(
        std::string_view font_path, core::u32 pixel_height);

    [[nodiscard]] const Glyph* glyph(char character) const noexcept;
    [[nodiscard]] float line_height() const noexcept { return line_height_; }
    [[nodiscard]] float ascent() const noexcept { return ascent_; }
    [[nodiscard]] float measure(std::string_view text) const;

    [[nodiscard]] float white_u() const noexcept { return white_u_; }
    [[nodiscard]] float white_v() const noexcept { return white_v_; }
    [[nodiscard]] std::span<const core::u8> pixels() const noexcept {
        return pixels_;
    }
    [[nodiscard]] core::u32 width() const noexcept { return width_; }
    [[nodiscard]] core::u32 height() const noexcept { return height_; }

private:
    std::map<char, Glyph> glyphs_;
    std::vector<core::u8> pixels_;
    core::u32 width_{};
    core::u32 height_{};
    float line_height_{};
    float ascent_{};
    float white_u_{};
    float white_v_{};
};

// Appends `text` to `list` with its top-left at (x, y), in `color`. Returns the
// advance width, so a caller can lay out following content.
float draw_text(
    UiDrawList& list,
    const FontAtlas& atlas,
    float x,
    float y,
    std::string_view text,
    UiColor color);

} // namespace mediaserver::gui
