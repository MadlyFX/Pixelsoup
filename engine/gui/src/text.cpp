#include <mediaserver/gui/text.hpp>

// FreeType is not clean under this project's warnings-as-errors; its headers are
// included with warnings silenced. The code below stays fully checked.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif
#include <ft2build.h>
#include FT_FREETYPE_H
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>

namespace mediaserver::gui {
namespace {

// A fixed atlas is enough for ASCII at operator sizes; on-demand growth and a
// bigger character set are later steps.
constexpr core::u32 atlas_size = 512U;

} // namespace

void FontAtlas::init_base() {
    width_ = atlas_size;
    height_ = atlas_size;

    // rgb = 255 everywhere, coverage in alpha (0 to start). Solid fills sample
    // the reserved white block at the top-left, whose alpha is 255.
    pixels_.assign(
        static_cast<std::size_t>(atlas_size) * atlas_size * 4U, 0U);
    for (std::size_t i = 0U; i < pixels_.size(); i += 4U) {
        pixels_[i] = 255U;
        pixels_[i + 1U] = 255U;
        pixels_[i + 2U] = 255U;
    }
    for (core::u32 y = 0U; y < 2U; ++y) {
        for (core::u32 x = 0U; x < 2U; ++x) {
            pixels_[(static_cast<std::size_t>(y) * atlas_size + x) * 4U + 3U] =
                255U;
        }
    }
    white_u_ = 0.5F / static_cast<float>(atlas_size);
    white_v_ = 0.5F / static_cast<float>(atlas_size);
}

FontAtlas FontAtlas::empty(float line_height, float ascent) {
    FontAtlas atlas;
    atlas.init_base();
    atlas.line_height_ = line_height;
    atlas.ascent_ = ascent;
    return atlas;
}

std::optional<FontAtlas> FontAtlas::create(
    std::string_view font_path, core::u32 pixel_height) {
    FT_Library library{};
    if (FT_Init_FreeType(&library) != 0) {
        return std::nullopt;
    }
    FT_Face face{};
    if (FT_New_Face(library, std::string{font_path}.c_str(), 0, &face) != 0) {
        FT_Done_FreeType(library);
        return std::nullopt;
    }
    FT_Set_Pixel_Sizes(face, 0U, pixel_height);

    FontAtlas atlas;
    atlas.init_base();
    atlas.line_height_ =
        static_cast<float>(face->size->metrics.height) / 64.0F;
    atlas.ascent_ = static_cast<float>(face->size->metrics.ascender) / 64.0F;

    const auto set_alpha = [&atlas](core::u32 x, core::u32 y, core::u8 a) {
        atlas.pixels_[(static_cast<std::size_t>(y) * atlas_size + x) * 4U + 3U] = a;
    };

    // Shelf packing, starting past the white block.
    core::u32 pen_x = 4U;
    core::u32 pen_y = 0U;
    core::u32 row_height = 0U;
    for (char character = 32; character < 127; ++character) {
        if (FT_Load_Char(face, static_cast<FT_ULong>(character), FT_LOAD_RENDER)
            != 0) {
            continue;
        }
        const FT_Bitmap& bitmap = face->glyph->bitmap;
        const core::u32 glyph_w = bitmap.width;
        const core::u32 glyph_h = bitmap.rows;
        if (pen_x + glyph_w + 1U > atlas_size) {
            pen_x = 0U;
            pen_y += row_height + 1U;
            row_height = 0U;
        }
        if (pen_y + glyph_h > atlas_size) {
            break; // atlas full; remaining glyphs are simply absent
        }
        for (core::u32 row = 0U; row < glyph_h; ++row) {
            for (core::u32 col = 0U; col < glyph_w; ++col) {
                const auto coverage = bitmap.buffer[
                    static_cast<std::size_t>(row) * static_cast<std::size_t>(bitmap.pitch)
                    + col];
                set_alpha(pen_x + col, pen_y + row, coverage);
            }
        }
        Glyph glyph{};
        glyph.u0 = static_cast<float>(pen_x) / static_cast<float>(atlas_size);
        glyph.v0 = static_cast<float>(pen_y) / static_cast<float>(atlas_size);
        glyph.u1 =
            static_cast<float>(pen_x + glyph_w) / static_cast<float>(atlas_size);
        glyph.v1 =
            static_cast<float>(pen_y + glyph_h) / static_cast<float>(atlas_size);
        glyph.width = static_cast<float>(glyph_w);
        glyph.height = static_cast<float>(glyph_h);
        glyph.bearing_x = static_cast<float>(face->glyph->bitmap_left);
        glyph.bearing_y = static_cast<float>(face->glyph->bitmap_top);
        glyph.advance = static_cast<float>(face->glyph->advance.x) / 64.0F;
        atlas.glyphs_.emplace(character, glyph);

        pen_x += glyph_w + 1U;
        row_height = std::max(row_height, glyph_h);
    }

    FT_Done_Face(face);
    FT_Done_FreeType(library);
    return atlas;
}

const Glyph* FontAtlas::glyph(char character) const noexcept {
    const auto found = glyphs_.find(character);
    return found == glyphs_.end() ? nullptr : &found->second;
}

float FontAtlas::measure(std::string_view text) const {
    float width = 0.0F;
    for (const char character : text) {
        const Glyph* const g = glyph(character);
        if (g != nullptr) {
            width += g->advance;
        }
    }
    return width;
}

float draw_text(
    UiDrawList& list,
    const FontAtlas& atlas,
    float x,
    float y,
    std::string_view text,
    UiColor color) {
    float pen_x = x;
    // FreeType has already rasterized and hinted these small glyphs for the
    // physical pixel grid. Putting that bitmap at a fractional screen position
    // makes the linear sampler filter it a second time, which is especially
    // visible in odd-height rows such as collapsible section headers. Snap each
    // bitmap quad (including the baseline) to whole pixels while retaining the
    // atlas's grayscale edge coverage for antialiasing.
    const float baseline = std::round(y + atlas.ascent());
    for (const char character : text) {
        const Glyph* const g = atlas.glyph(character);
        if (g == nullptr) {
            continue;
        }
        if (g->width > 0.0F && g->height > 0.0F) {
            list.add_textured(
                std::round(pen_x + g->bearing_x),
                std::round(baseline - g->bearing_y),
                g->width,
                g->height,
                g->u0,
                g->v0,
                g->u1,
                g->v1,
                color);
        }
        pen_x += g->advance;
    }
    return pen_x - x;
}

} // namespace mediaserver::gui
