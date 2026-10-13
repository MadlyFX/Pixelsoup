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
// Small UI text uses FreeType's hinted grayscale rasterization rather than an
// SDF. Glyph bitmaps are kept on the physical pixel grid, preserving the atlas's
// antialiased edge coverage without adding blur from fractional resampling.
//
// A dense display is served by baking the glyphs at the display's size rather
// than by magnifying small ones: an atlas made for 150% holds 18-pixel glyphs
// for a 12-unit font. Its metrics and measurements are reported in logical
// units, so layout never learns the scale; only the glyph bitmaps, and the
// pixel grid draw_text snaps them to, are physical.
namespace mediaserver::gui {

// Physical pixels: the bitmap's own size, bearings and advance.
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
    // Bakes `font_path` at `pixel_height` logical units for a display of
    // `scale` physical pixels per unit. Returns nullopt if FreeType cannot
    // load the file, so the caller can fall back or skip text.
    [[nodiscard]] static std::optional<FontAtlas> create(
        std::string_view font_path, core::u32 pixel_height, float scale = 1.0F);

    // An atlas with the reserved white texel but no glyphs, needing neither a
    // font file nor FreeType. It is the fallback when a font fails to load - the
    // UI still draws its solid fills - and it lets the widget layer be tested
    // headlessly, where interaction, not glyph shape, is under test. The metrics
    // are nominal so layout that centres against them stays sane.
    [[nodiscard]] static FontAtlas empty(
        float line_height = 14.0F, float ascent = 11.0F);

    [[nodiscard]] const Glyph* glyph(char character) const noexcept;
    // Logical units, like everything a layout reads.
    [[nodiscard]] float line_height() const noexcept { return line_height_; }
    [[nodiscard]] float ascent() const noexcept { return ascent_; }
    [[nodiscard]] float measure(std::string_view text) const;
    // Physical pixels per logical unit the glyphs were baked for.
    [[nodiscard]] float scale() const noexcept { return scale_; }

    [[nodiscard]] float white_u() const noexcept { return white_u_; }
    [[nodiscard]] float white_v() const noexcept { return white_v_; }
    [[nodiscard]] std::span<const core::u8> pixels() const noexcept {
        return pixels_;
    }
    [[nodiscard]] core::u32 width() const noexcept { return width_; }
    [[nodiscard]] core::u32 height() const noexcept { return height_; }

private:
    // Fills the RGBA atlas (rgb = 255, alpha = 0) and reserves the opaque white
    // texel, recording its uv. Shared by create() and empty().
    void init_base(core::u32 size);

    std::map<char, Glyph> glyphs_;
    std::vector<core::u8> pixels_;
    core::u32 width_{};
    core::u32 height_{};
    float line_height_{};
    float ascent_{};
    float scale_{1.0F};
    float white_u_{};
    float white_v_{};
};

// Appends `text` to `list` with its top-left at (x, y), in `color`. Returns the
// advance width, so a caller can lay out following content. Both are logical
// units; the list must carry the scale the atlas was baked for.
float draw_text(
    UiDrawList& list,
    const FontAtlas& atlas,
    float x,
    float y,
    std::string_view text,
    UiColor color);

} // namespace mediaserver::gui
