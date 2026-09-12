#include <mediaserver/render/model_loader.hpp>
#include "model_parallel.hpp"
#include <mediaserver/assets/content_fingerprint.hpp>
#include <deque>
#include <atomic>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <bc7enc.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>

namespace mediaserver::render {
namespace {
std::atomic<core::u64> compression_count{};
struct TextureCacheEntry { std::string key; ModelTexture texture; std::size_t bytes{}; };
std::mutex texture_cache_mutex;
std::deque<TextureCacheEntry> texture_cache;
std::size_t texture_cache_bytes{};
constexpr std::size_t texture_cache_limit = 64U*1024U*1024U;
std::string texture_key(const ModelTexture& texture, bool normal) {
    assets::Sha256Hasher hash;
    const auto number = [&](core::u32 n) {
        const std::array<core::u8,4> bytes{static_cast<core::u8>(n),static_cast<core::u8>(n>>8),
            static_cast<core::u8>(n>>16),static_cast<core::u8>(n>>24)};
        hash.update(bytes);
    };
    number(normal ? 1U : 0U); number(texture.requires_base_level ? 1U : 0U);
    number(texture.width); number(texture.height); hash.update(texture.rgba);
    for (const auto& mip : texture.lower_mips) { number(mip.width); number(mip.height); hash.update(mip.rgba); }
    return assets::sha256_hex(hash.finish());
}
void encode_bc4(const std::array<core::u8, 64U>& block, std::size_t channel,
                core::u8* output) {
    core::u8 low = 255U, high = 0U;
    for (std::size_t i = 0U; i < 16U; ++i) {
        low = std::min(low, block[i * 4U + channel]);
        high = std::max(high, block[i * 4U + channel]);
    }
    output[0U] = high;
    output[1U] = low;
    std::array<unsigned, 8U> palette{high, low};
    for (unsigned i = 1U; i <= 6U; ++i)
        palette[i + 1U] = ((7U - i) * high + i * low) / 7U;
    core::u64 indices{};
    for (std::size_t i = 0U; i < 16U; ++i) {
        unsigned best{}, error = std::numeric_limits<unsigned>::max();
        for (unsigned j = 0U; j < 8U; ++j) {
            const int distance = static_cast<int>(block[i * 4U + channel]) - static_cast<int>(palette[j]);
            const auto candidate = static_cast<unsigned>(distance * distance);
            if (candidate < error) { best = j; error = candidate; }
        }
        indices |= static_cast<core::u64>(best) << (i * 3U);
    }
    for (std::size_t i = 0U; i < 6U; ++i) output[i + 2U] = static_cast<core::u8>(indices >> (8U * i));
}

void compress(core::u32 width, core::u32 height, std::vector<core::u8>& pixels,
               bool normal, const bc7enc_compress_block_params& parameters) {
    const auto columns = (width + 3U) / 4U;
    const auto rows = (height + 3U) / 4U;
    std::vector<core::u8> encoded(static_cast<std::size_t>(columns) * rows * 16U);
    model_parallel_rows(rows, [&](std::size_t row) {
      const auto y = static_cast<core::u32>(row);
      for (core::u32 x = 0U; x < columns; ++x) {
        std::array<core::u8, 64U> block{};
        for (core::u32 by = 0U; by < 4U; ++by) for (core::u32 bx = 0U; bx < 4U; ++bx) {
            const auto source = (static_cast<std::size_t>(std::min(y * 4U + by, height - 1U)) * width
                + std::min(x * 4U + bx, width - 1U)) * 4U;
            std::copy_n(pixels.data() + source, 4U, block.data() + (by * 4U + bx) * 4U);
        }
        auto* output = encoded.data() + (static_cast<std::size_t>(y) * columns + x) * 16U;
        if (normal) {
            encode_bc4(block, 0U, output);
            encode_bc4(block, 1U, output + 8U);
        } else {
            static_cast<void>(bc7enc_compress_block(output, block.data(), &parameters));
        }
      }
    });
    pixels = std::move(encoded);
}
} // namespace

bool compress_model_textures(ModelAsset& asset) {
    static std::once_flag initialised;
    std::call_once(initialised, bc7enc_compress_block_init);
    // Unused images (including missing files) are not package dependencies.
    std::vector<std::optional<core::u32>> remap(asset.textures.size());
    std::vector<ModelTexture> used;
    for (auto& part : asset.parts) {
        for (auto* index : {&part.base_color_texture, &part.metallic_roughness_texture, &part.normal_texture, &part.occlusion_texture, &part.emissive_texture, &part.clearcoat_texture, &part.clearcoat_roughness_texture, &part.clearcoat_normal_texture, &part.sheen_color_texture, &part.sheen_roughness_texture, &part.transmission_texture, &part.specular_texture, &part.specular_color_texture, &part.anisotropy_texture}) {
            if (!*index) continue;
            if (**index >= asset.textures.size()) return false;
            auto& translated = remap[**index];
            if (!translated) {
                if (!asset.textures[**index].valid()) return false;
                translated = static_cast<core::u32>(used.size());
                used.push_back(std::move(asset.textures[**index]));
            }
            *index = translated;
        }
    }
    asset.textures = std::move(used);
    if (!prepare_model_mips(asset)) return false;
    bc7enc_compress_block_params parameters{};
    bc7enc_compress_block_params_init(&parameters);
    bc7enc_compress_block_params_init_linear_weights(&parameters);
    parameters.m_uber_level = 1U;
    for (std::size_t i = 0U; i < asset.textures.size(); ++i) {
        auto& texture = asset.textures[i];
        if (!texture.valid() || texture.encoding != ModelTextureEncoding::Rgba8) continue;
        // D3D12 requires block-aligned base dimensions. Preserve arbitrary
        // authored dimensions as RGBA8 rather than rescaling or padding UVs.
        if (texture.width % 4U != 0U || texture.height % 4U != 0U) continue;
        const bool normal = std::any_of(asset.parts.begin(), asset.parts.end(), [&](const auto& part) {
            return part.normal_texture == i || part.clearcoat_normal_texture == i;
        });
        const auto key = texture_key(texture, normal);
        {
            std::lock_guard lock{texture_cache_mutex};
            const auto cached = std::find_if(texture_cache.begin(), texture_cache.end(),
                [&](const auto& entry) { return entry.key == key; });
            if (cached != texture_cache.end()) { texture = cached->texture; continue; }
        }
        ++compression_count;
        compress(texture.width, texture.height, texture.rgba, normal, parameters);
        for (auto& mip : texture.lower_mips) compress(mip.width, mip.height, mip.rgba, normal, parameters);
        texture.encoding = normal ? ModelTextureEncoding::Bc5 : ModelTextureEncoding::Bc7;
        std::size_t bytes = texture.rgba.size();
        for (const auto& mip : texture.lower_mips) bytes += mip.rgba.size();
        if (bytes <= texture_cache_limit) {
            std::lock_guard lock{texture_cache_mutex};
            while (!texture_cache.empty() && (texture_cache_bytes + bytes > texture_cache_limit || texture_cache.size() >= 256)) {
                texture_cache_bytes -= texture_cache.front().bytes; texture_cache.pop_front();
            }
            texture_cache.push_back({key, texture, bytes}); texture_cache_bytes += bytes;
        }
    }
    return true;
}
core::u64 model_texture_compression_count() noexcept { return compression_count.load(); }
} // namespace mediaserver::render
