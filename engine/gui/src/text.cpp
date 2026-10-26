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
#include <utility>
#include <vector>

namespace mediaserver::gui {
namespace {

// A fixed atlas is enough for ASCII at operator sizes; on-demand growth and a
// bigger character set are later steps. Glyphs baked for a dense display need
// the larger page: ninety-five 30-pixel glyphs do not fit in 512 squared.
constexpr core::u32 default_atlas_size = 512U;
constexpr core::u32 large_atlas_size = 1'024U;
constexpr core::u32 large_atlas_threshold_px = 22U;
// Extra glyph sizes for a zooming surface grow the page. This caps it at
// 64 MiB of RGBA, inside every supported GPU's 2D texture limit.
constexpr core::u32 max_atlas_size = 4'096U;

struct GlyphPen final {
    core::u32 x{4U}; // past the white block
    core::u32 y{0U};
    core::u32 row_height{0U};
};

// Shelf-packs one glyph set at `height` physical pixels into a page of
// `atlas_size` squared, continuing from `pen`. False when the page fills
// before the set is complete; the caller decides what gives.
bool bake_glyphs(FT_Face face, std::vector<core::u8>& pixels,
                 core::u32 atlas_size, float scale, GlyphPen& pen,
                 core::u32 height, std::map<char, Glyph>& glyphs,
                 float& line_height, float& ascent) {
    FT_Set_Pixel_Sizes(face, 0U, height);
    line_height =
        static_cast<float>(face->size->metrics.height) / 64.0F / scale;
    ascent =
        static_cast<float>(face->size->metrics.ascender) / 64.0F / scale;

    for (char character = 32; character < 127; ++character) {
        if (FT_Load_Char(face, static_cast<FT_ULong>(character), FT_LOAD_RENDER)
            != 0) {
            continue;
        }
        const FT_Bitmap& bitmap = face->glyph->bitmap;
        const core::u32 glyph_w = bitmap.width;
        const core::u32 glyph_h = bitmap.rows;
        if (pen.x + glyph_w + 1U > atlas_size) {
            pen.x = 0U;
            pen.y += pen.row_height + 1U;
            pen.row_height = 0U;
        }
        if (pen.y + glyph_h > atlas_size) {
            return false;
        }
        for (core::u32 row = 0U; row < glyph_h; ++row) {
            for (core::u32 col = 0U; col < glyph_w; ++col) {
                const auto coverage = bitmap.buffer[
                    static_cast<std::size_t>(row) * static_cast<std::size_t>(bitmap.pitch)
                    + col];
                pixels[(static_cast<std::size_t>(pen.y + row) * atlas_size
                        + pen.x + col) * 4U + 3U] = coverage;
            }
        }
        Glyph glyph{};
        glyph.u0 = static_cast<float>(pen.x) / static_cast<float>(atlas_size);
        glyph.v0 = static_cast<float>(pen.y) / static_cast<float>(atlas_size);
        glyph.u1 =
            static_cast<float>(pen.x + glyph_w) / static_cast<float>(atlas_size);
        glyph.v1 =
            static_cast<float>(pen.y + glyph_h) / static_cast<float>(atlas_size);
        glyph.width = static_cast<float>(glyph_w);
        glyph.height = static_cast<float>(glyph_h);
        glyph.bearing_x = static_cast<float>(face->glyph->bitmap_left);
        glyph.bearing_y = static_cast<float>(face->glyph->bitmap_top);
        glyph.advance = static_cast<float>(face->glyph->advance.x) / 64.0F;
        glyphs.emplace(character, glyph);

        pen.x += glyph_w + 1U;
        pen.row_height = std::max(pen.row_height, glyph_h);
    }
    return true;
}

} // namespace

void FontAtlas::init_base(core::u32 atlas_size) {
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
    atlas.init_base(default_atlas_size);
    atlas.line_height_ = line_height;
    atlas.ascent_ = ascent;
    return atlas;
}

FontAtlas FontAtlas::placeholder(float line_height, float ascent) {
    FontAtlas atlas = empty(line_height, ascent);
    for (int code = 32; code < 127; ++code) {
        const float u = static_cast<float>(code) / 128.0F;
        atlas.glyphs_.emplace(static_cast<char>(code),
                              Glyph{u, 0.0F, u, 0.0F, 1.0F, 1.0F, 0.0F, 0.0F, 0.0F});
    }
    return atlas;
}

std::optional<FontAtlas> FontAtlas::create(
    std::string_view font_path, core::u32 pixel_height, float scale,
    std::span<const float> extra_sizes) {
    if (!std::isfinite(scale) || scale <= 0.0F) {
        scale = 1.0F;
    }
    FT_Library library{};
    if (FT_Init_FreeType(&library) != 0) {
        return std::nullopt;
    }
    FT_Face face{};
    if (FT_New_Face(library, std::string{font_path}.c_str(), 0, &face) != 0) {
        FT_Done_FreeType(library);
        return std::nullopt;
    }
    const auto physical_height_for = [pixel_height, scale](float size) {
        return std::max<core::u32>(
            1U, static_cast<core::u32>(std::lround(
                    static_cast<float>(pixel_height) * scale * size)));
    };

    // Distinct, finite, positive sizes other than the base, smallest first.
    std::vector<float> sizes;
    for (const float size : extra_sizes) {
        if (std::isfinite(size) && size > 0.0F && size != 1.0F &&
            std::find(sizes.begin(), sizes.end(), size) == sizes.end()) {
            sizes.push_back(size);
        }
    }
    std::sort(sizes.begin(), sizes.end());

    // Extra sets need a bigger page. Rather than estimate how the shelves
    // will pack, start at the size the base alone needs and double it until
    // every set fits. Past the largest page, sets are dropped largest first
    // and their text falls back to the next smaller one.
    const core::u32 physical_height = physical_height_for(1.0F);
    core::u32 atlas_size = physical_height > large_atlas_threshold_px
        ? large_atlas_size : default_atlas_size;
    FontAtlas atlas;
    for (;;) {
        atlas = FontAtlas{};
        atlas.init_base(atlas_size);
        atlas.scale_ = scale;
        GlyphPen pen{};
        bool complete = bake_glyphs(face, atlas.pixels_, atlas_size, scale,
                                    pen, physical_height, atlas.glyphs_,
                                    atlas.line_height_, atlas.ascent_);
        for (std::size_t index = 0U; complete && index < sizes.size(); ++index) {
            SizedSet set{};
            set.size = sizes[index];
            complete = bake_glyphs(face, atlas.pixels_, atlas_size, scale, pen,
                                   physical_height_for(set.size), set.glyphs,
                                   set.line_height, set.ascent);
            if (complete) {
                atlas.sized_.push_back(std::move(set));
            }
        }
        if (complete) {
            break;
        }
        if (atlas_size < max_atlas_size) {
            atlas_size *= 2U;
        } else if (!sizes.empty()) {
            sizes.pop_back();
        } else {
            break; // the base alone overflows: keep the glyphs that fit
        }
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
    return width / scale_;
}

const FontAtlas::SizedSet* FontAtlas::sized(float size) const noexcept {
    const auto found = std::find_if(
        sized_.begin(), sized_.end(),
        [size](const SizedSet& set) noexcept { return set.size == size; });
    return found == sized_.end() ? nullptr : &*found;
}

std::optional<float> FontAtlas::size_at_most(float size) const noexcept {
    if (!std::isfinite(size)) {
        return std::nullopt;
    }
    std::optional<float> best;
    if (size >= 1.0F) {
        best = 1.0F;
    }
    for (const SizedSet& set : sized_) {
        if (set.size <= size && (!best.has_value() || set.size > *best)) {
            best = set.size;
        }
    }
    return best;
}

const Glyph* FontAtlas::glyph(char character, float size) const noexcept {
    const SizedSet* const set = sized(size);
    if (set == nullptr) {
        return glyph(character);
    }
    const auto found = set->glyphs.find(character);
    return found == set->glyphs.end() ? nullptr : &found->second;
}

float FontAtlas::line_height(float size) const noexcept {
    const SizedSet* const set = sized(size);
    return set == nullptr ? line_height_ : set->line_height;
}

float FontAtlas::ascent(float size) const noexcept {
    const SizedSet* const set = sized(size);
    return set == nullptr ? ascent_ : set->ascent;
}

float FontAtlas::measure(std::string_view text, float size) const {
    float width = 0.0F;
    for (const char character : text) {
        const Glyph* const g = glyph(character, size);
        if (g != nullptr) {
            width += g->advance;
        }
    }
    return width / scale_;
}

float draw_text(
    UiDrawList& list,
    const FontAtlas& atlas,
    float x,
    float y,
    std::string_view text,
    UiColor color) {
    return draw_text(list, atlas, x, y, text, color, 1.0F);
}

float draw_text(
    UiDrawList& list,
    const FontAtlas& atlas,
    float x,
    float y,
    std::string_view text,
    UiColor color,
    float size) {
    // FreeType has already rasterized and hinted these small glyphs for the
    // physical pixel grid. Putting that bitmap at a fractional screen position
    // makes the linear sampler filter it a second time, which is especially
    // visible in odd-height rows such as collapsible section headers. Snap each
    // bitmap quad (including the baseline) to whole pixels while retaining the
    // atlas's grayscale edge coverage for antialiasing.
    //
    // The walk happens in physical pixels, where the bitmaps live, and each
    // quad is handed back in logical units that land exactly on that grid once
    // the list scales them.
    const float scale = atlas.scale();
    const float origin = std::round(x * scale);
    float pen_x = origin;
    const float baseline = std::round((y + atlas.ascent(size)) * scale);
    for (const char character : text) {
        const Glyph* const g = atlas.glyph(character, size);
        if (g == nullptr) {
            continue;
        }
        if (g->width > 0.0F && g->height > 0.0F) {
            list.add_textured(
                std::round(pen_x + g->bearing_x) / scale,
                std::round(baseline - g->bearing_y) / scale,
                g->width / scale,
                g->height / scale,
                g->u0,
                g->v0,
                g->u1,
                g->v1,
                color);
        }
        pen_x += g->advance;
    }
    return (pen_x - origin) / scale;
}

} // namespace mediaserver::gui
