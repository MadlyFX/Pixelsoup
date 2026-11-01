#pragma once

#include <ajfx/effect.hpp>
#include <ajfx/package.hpp>
#include <ajfx/result.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

// Sprite textures. A package carries each one as an entry "textures/<name>"
// holding a complete RGBA8 mip chain, built when the package is authored, so
// readers need no image decoder and every node samples identical texels.
// Entry layout, all integers little-endian:
//
//     "AJTX"                 4 bytes
//     texture version        u32
//     width, height          u32, u32 (1 to max_texture_size)
//     mip count              u32 (1 to the full chain)
//     colour space           u32 (0 sRGB, 1 linear)
//     the mips, largest first, each width x height x 4 bytes, rows packed
//
// Pixels are straight (not premultiplied) alpha.
namespace ajfx {

inline constexpr std::uint32_t texture_format_version = 1U;
inline constexpr std::uint32_t max_texture_size = 4096U;

enum class ColorSpace : std::uint32_t {
    Srgb,   // colour images: sampled through sRGB decoding
    Linear, // data, masks
};

struct TextureData final {
    std::uint32_t width{0U};
    std::uint32_t height{0U};
    ColorSpace color_space{ColorSpace::Srgb};
    // RGBA8 levels, largest first.
    std::vector<std::vector<std::uint8_t>> mips;

    friend bool operator==(const TextureData&, const TextureData&) = default;
};

// The size of mip `level` along an edge of `size` texels.
[[nodiscard]] constexpr std::uint32_t mip_size(std::uint32_t size, std::uint32_t level) noexcept {
    const std::uint32_t shifted = level < 32U ? size >> level : 0U;
    return shifted > 0U ? shifted : 1U;
}

// A texture with its full mip chain from `rgba` (width x height x 4 bytes,
// straight alpha). Each level is a 2x2 box filter of the one above, averaged
// in linear light for sRGB textures, with alpha-weighted colour so edges do
// not darken. Deterministic: the same pixels always give the same bytes.
[[nodiscard]] Result<TextureData> make_texture(std::uint32_t width, std::uint32_t height,
                                               std::span<const std::uint8_t> rgba,
                                               ColorSpace color_space = ColorSpace::Srgb);

[[nodiscard]] std::vector<std::uint8_t> write_texture(const TextureData& texture);
// Fails with MalformedPackage for a bad header or size, UnsupportedVersion
// for a newer layout.
[[nodiscard]] Result<TextureData> read_texture(std::span<const std::uint8_t> bytes);

// Textures by the name emitters use (`SpriteRenderer::texture`).
using TextureSet = std::map<std::string, TextureData, std::less<>>;

// "textures/<name>" entries for write_package's `extra`.
[[nodiscard]] std::vector<PackageEntry> texture_package_entries(const TextureSet& textures);

// Checks that `textures` holds every texture the effect's emitters name, and
// that each emitter's flipbook has at least one texel per cell. Fails with
// MissingTexture or InvalidEffect, naming the emitter.
[[nodiscard]] Result<bool> check_textures(const Effect& effect, const TextureSet& textures);

// Every texture the effect names, read from its package and checked as above.
[[nodiscard]] Result<TextureSet> read_package_textures(std::span<const std::uint8_t> package, const Effect& effect);

} // namespace ajfx
