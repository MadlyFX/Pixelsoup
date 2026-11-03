#include <ajfx/texture.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <string_view>

namespace ajfx {
namespace {

constexpr std::size_t header_bytes = 24U;
constexpr char texture_magic[4] = {'A', 'J', 'T', 'X'};
constexpr std::string_view texture_prefix = "textures/";

std::uint32_t full_chain(std::uint32_t width, std::uint32_t height) {
    std::uint32_t levels = 1U;
    while (mip_size(width, levels - 1U) > 1U || mip_size(height, levels - 1U) > 1U) {
        ++levels;
    }
    return levels;
}

std::uint64_t level_bytes(std::uint32_t width, std::uint32_t height, std::uint32_t level) {
    return std::uint64_t{mip_size(width, level)} * mip_size(height, level) * 4U;
}

float srgb_to_linear(float c) {
    return c <= 0.04045F ? c / 12.92F : std::pow((c + 0.055F) / 1.055F, 2.4F);
}

float linear_to_srgb(float c) {
    return c <= 0.0031308F ? c * 12.92F : 1.055F * std::pow(c, 1.0F / 2.4F) - 0.055F;
}

std::uint8_t to_byte(float unit) {
    return static_cast<std::uint8_t>(std::clamp(std::lround(unit * 255.0F), 0L, 255L));
}

// One level from the one above: 2x2 box, clamped at odd edges.
std::vector<std::uint8_t> downsample(const std::vector<std::uint8_t>& above, std::uint32_t width,
                                     std::uint32_t height, ColorSpace color_space) {
    static const std::array<float, 256U> decoded = [] {
        std::array<float, 256U> table{};
        for (std::size_t i = 0U; i < table.size(); ++i) {
            table[i] = srgb_to_linear(static_cast<float>(i) / 255.0F);
        }
        return table;
    }();
    const auto light = [&](std::uint8_t byte) {
        return color_space == ColorSpace::Srgb ? decoded[byte] : static_cast<float>(byte) / 255.0F;
    };
    const std::uint32_t out_width = mip_size(width, 1U);
    const std::uint32_t out_height = mip_size(height, 1U);
    std::vector<std::uint8_t> out(std::size_t{out_width} * out_height * 4U);
    for (std::uint32_t y = 0U; y < out_height; ++y) {
        for (std::uint32_t x = 0U; x < out_width; ++x) {
            const std::uint32_t xs[2] = {std::min(2U * x, width - 1U), std::min(2U * x + 1U, width - 1U)};
            const std::uint32_t ys[2] = {std::min(2U * y, height - 1U), std::min(2U * y + 1U, height - 1U)};
            float colour[3]{};
            float plain[3]{};
            float alpha = 0.0F;
            for (const std::uint32_t sy : ys) {
                for (const std::uint32_t sx : xs) {
                    const std::uint8_t* texel = above.data() + (std::size_t{sy} * width + sx) * 4U;
                    const float a = static_cast<float>(texel[3]) / 255.0F;
                    for (std::size_t c = 0U; c < 3U; ++c) {
                        colour[c] += light(texel[c]) * a;
                        plain[c] += light(texel[c]);
                    }
                    alpha += a;
                }
            }
            std::uint8_t* target = out.data() + (std::size_t{y} * out_width + x) * 4U;
            for (std::size_t c = 0U; c < 3U; ++c) {
                // Colour weighted by alpha, so transparent texels do not
                // darken an edge; plain average where all four are clear.
                const float value = alpha > 0.0F ? colour[c] / alpha : plain[c] * 0.25F;
                target[c] = to_byte(color_space == ColorSpace::Srgb ? linear_to_srgb(value) : value);
            }
            target[3] = to_byte(alpha * 0.25F);
        }
    }
    return out;
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (std::uint32_t i = 0U; i < 4U; ++i) {
        out.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
    }
}

std::uint32_t get_u32(std::span<const std::uint8_t> bytes, std::size_t at) {
    std::uint32_t value = 0U;
    for (std::uint32_t i = 0U; i < 4U; ++i) {
        value |= static_cast<std::uint32_t>(bytes[at + i]) << (8U * i);
    }
    return value;
}

Error malformed(const std::string& problem) {
    return Error{ErrorCode::MalformedPackage, "texture " + problem};
}

std::string quoted(std::string_view text) {
    return "'" + std::string{text} + "'";
}

} // namespace

Result<TextureData> make_texture(std::uint32_t width, std::uint32_t height, std::span<const std::uint8_t> rgba,
                                 ColorSpace color_space) {
    if (width == 0U || height == 0U || width > max_texture_size || height > max_texture_size) {
        return Error{ErrorCode::InvalidEffect, "a texture must be 1 to " + std::to_string(max_texture_size) +
                                                   " texels on each side"};
    }
    if (rgba.size() != std::size_t{width} * height * 4U) {
        return Error{ErrorCode::InvalidEffect, "texture pixels do not match its size"};
    }
    TextureData texture;
    texture.width = width;
    texture.height = height;
    texture.color_space = color_space;
    texture.mips.emplace_back(rgba.begin(), rgba.end());
    const std::uint32_t levels = full_chain(width, height);
    for (std::uint32_t level = 1U; level < levels; ++level) {
        texture.mips.push_back(downsample(texture.mips.back(), mip_size(width, level - 1U),
                                          mip_size(height, level - 1U), color_space));
    }
    return texture;
}

std::vector<std::uint8_t> write_texture(const TextureData& texture) {
    std::vector<std::uint8_t> out(std::begin(texture_magic), std::end(texture_magic));
    put_u32(out, texture_format_version);
    put_u32(out, texture.width);
    put_u32(out, texture.height);
    put_u32(out, static_cast<std::uint32_t>(texture.mips.size()));
    put_u32(out, static_cast<std::uint32_t>(texture.color_space));
    for (const std::vector<std::uint8_t>& mip : texture.mips) {
        out.insert(out.end(), mip.begin(), mip.end());
    }
    return out;
}

Result<TextureData> read_texture(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < header_bytes || std::memcmp(bytes.data(), texture_magic, 4U) != 0) {
        return malformed("has no AJTX header");
    }
    const std::uint32_t version = get_u32(bytes, 4U);
    if (version > texture_format_version) {
        return Error{ErrorCode::UnsupportedVersion,
                     "texture version " + std::to_string(version) + " is newer than this library reads"};
    }
    TextureData texture;
    texture.width = get_u32(bytes, 8U);
    texture.height = get_u32(bytes, 12U);
    const std::uint32_t levels = get_u32(bytes, 16U);
    const std::uint32_t space = get_u32(bytes, 20U);
    if (texture.width == 0U || texture.height == 0U || texture.width > max_texture_size ||
        texture.height > max_texture_size) {
        return malformed("size " + std::to_string(texture.width) + "x" + std::to_string(texture.height) +
                         " is out of range");
    }
    if (levels == 0U || levels > full_chain(texture.width, texture.height)) {
        return malformed("has " + std::to_string(levels) + " mip levels");
    }
    if (space > static_cast<std::uint32_t>(ColorSpace::Linear)) {
        return malformed("has an unknown colour space");
    }
    texture.color_space = static_cast<ColorSpace>(space);
    std::size_t at = header_bytes;
    for (std::uint32_t level = 0U; level < levels; ++level) {
        const std::uint64_t size = level_bytes(texture.width, texture.height, level);
        if (bytes.size() - at < size) {
            return malformed("is shorter than its mips");
        }
        texture.mips.emplace_back(bytes.begin() + static_cast<std::ptrdiff_t>(at),
                                  bytes.begin() + static_cast<std::ptrdiff_t>(at + size));
        at += static_cast<std::size_t>(size);
    }
    if (at != bytes.size()) {
        return malformed("has bytes after its mips");
    }
    return texture;
}

std::vector<PackageEntry> texture_package_entries(const TextureSet& textures) {
    std::vector<PackageEntry> entries;
    for (const auto& [name, texture] : textures) {
        entries.push_back({std::string{texture_prefix} + name, write_texture(texture)});
    }
    return entries;
}

Result<bool> check_textures(const Effect& effect, const TextureSet& textures) {
    for (const Emitter& emitter : effect.emitters) {
        const SpriteRenderer& sprite = emitter.renderer;
        if (sprite.texture.empty()) {
            continue;
        }
        const std::string where = "emitter " + quoted(emitter.name);
        const auto found = textures.find(sprite.texture);
        if (found == textures.end()) {
            return Error{ErrorCode::MissingTexture,
                         where + " uses texture " + quoted(sprite.texture) + ", which the package does not hold"};
        }
        if (sprite.flipbook_columns > found->second.width || sprite.flipbook_rows > found->second.height) {
            return Error{ErrorCode::InvalidEffect, where + " has a " + std::to_string(sprite.flipbook_columns) + "x" +
                                                       std::to_string(sprite.flipbook_rows) +
                                                       " flipbook, more cells than texture " +
                                                       quoted(sprite.texture) + " has texels"};
        }
        if (sprite.lighting != Lighting::SixWay) {
            continue;
        }
        // The six-way pair share one grid, so the second map must be the
        // first's size: a cell then reads the same texel from both.
        const auto lightmap = textures.find(sprite.lightmap);
        if (lightmap == textures.end()) {
            return Error{ErrorCode::MissingTexture,
                         where + " uses lightmap " + quoted(sprite.lightmap) + ", which the package does not hold"};
        }
        if (lightmap->second.width != found->second.width || lightmap->second.height != found->second.height) {
            return Error{ErrorCode::InvalidEffect, where + " has lightmap " + quoted(sprite.lightmap) + " of " +
                                                       std::to_string(lightmap->second.width) + "x" +
                                                       std::to_string(lightmap->second.height) +
                                                       ", not the size of texture " + quoted(sprite.texture)};
        }
    }
    return true;
}

Result<TextureSet> read_package_textures(std::span<const std::uint8_t> package, const Effect& effect) {
    Result<std::vector<PackageEntry>> entries = read_container(package);
    if (!entries) {
        return entries.error();
    }
    TextureSet textures;
    for (const Emitter& emitter : effect.emitters) {
        for (const std::string& name : {emitter.renderer.texture, emitter.renderer.lightmap}) {
            if (name.empty() || textures.contains(name)) {
                continue;
            }
            const std::string entry_name = std::string{texture_prefix} + name;
            const auto entry = std::find_if(entries.value().begin(), entries.value().end(),
                                            [&](const PackageEntry& e) { return e.name == entry_name; });
            if (entry == entries.value().end()) {
                continue; // check_textures names the emitter
            }
            Result<TextureData> texture = read_texture(entry->data);
            if (!texture) {
                return Error{texture.error().code, quoted(entry_name) + ": " + texture.error().message};
            }
            textures.emplace(name, std::move(texture).value());
        }
    }
    if (Result<bool> checked = check_textures(effect, textures); !checked) {
        return checked.error();
    }
    return textures;
}

} // namespace ajfx
