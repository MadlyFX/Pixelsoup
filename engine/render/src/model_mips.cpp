#include <mediaserver/render/model_loader.hpp>
#include "model_parallel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace mediaserver::render {
namespace {
enum class Role { Data, Color, Normal };
float linear(float v) {
    return v <= 0.04045F ? v / 12.92F : std::pow((v + 0.055F) / 1.055F, 2.4F);
}
float srgb(float v) {
    return v <= 0.0031308F ? v * 12.92F : 1.055F * std::pow(v, 1.0F / 2.4F) - 0.055F;
}
core::u8 byte(float v) {
    return static_cast<core::u8>(std::clamp(std::lround(v * 255.0F), 0L, 255L));
}

void pyramid(ModelTexture &texture, Role role) {
    static const auto linear_bytes = [] {
        std::array<float, 256U> table{};
        for (std::size_t i = 0; i < table.size(); ++i)
            table[i] = linear(static_cast<float>(i) / 255.0F);
        return table;
    }();
    texture.lower_mips.clear();
    auto width = texture.width, height = texture.height;
    const std::vector<core::u8> *source = &texture.rgba;
    // Prevent vector growth invalidating the source of the previous level.
    texture.lower_mips.reserve(32U);
    while (width > 1U || height > 1U) {
        ModelTextureMip mip{std::max(1U, width / 2U), std::max(1U, height / 2U), {}};
        mip.rgba.resize(static_cast<std::size_t>(mip.width) * mip.height * 4U);
        model_parallel_rows(mip.height, [&](std::size_t row) {
            const auto y = static_cast<core::u32>(row);
            const auto y0 =
                static_cast<core::u32>(static_cast<core::u64>(y) * height / mip.height);
            const auto y1 = static_cast<core::u32>(static_cast<core::u64>(y + 1U) *
                                                   height / mip.height);
            for (core::u32 x = 0U; x < mip.width; ++x) {
                const auto x0 = static_cast<core::u32>(static_cast<core::u64>(x) *
                                                       width / mip.width);
                const auto x1 = static_cast<core::u32>(static_cast<core::u64>(x + 1U) *
                                                       width / mip.width);
                std::array<float, 4U> sum{};
                float samples = 0.0F;
                for (auto sy = y0; sy < y1; ++sy)
                    for (auto sx = x0; sx < x1; ++sx) {
                        const auto offset =
                            (static_cast<std::size_t>(sy) * width + sx) * 4U;
                        const float alpha = (*source)[offset + 3U] / 255.0F;
                        for (std::size_t c = 0U; c < 3U; ++c) {
                            const float v = (*source)[offset + c] / 255.0F;
                            sum[c] += role == Role::Color    ? linear_bytes[(*source)[offset + c]] * alpha
                                      : role == Role::Normal ? v * 2.0F - 1.0F
                                                             : v;
                        }
                        sum[3U] += alpha;
                        samples += 1.0F;
                    }
                const float length = std::sqrt(sum[0U] * sum[0U] + sum[1U] * sum[1U] +
                                               sum[2U] * sum[2U]);
                const auto offset = (static_cast<std::size_t>(y) * mip.width + x) * 4U;
                for (std::size_t c = 0U; c < 3U; ++c) {
                    float v = sum[c] / samples;
                    if (role == Role::Color)
                        v = sum[3U] > 0.0F ? srgb(sum[c] / sum[3U]) : 0.0F;
                    if (role == Role::Normal)
                        v = 0.5F + 0.5F * (length > 0.00001F ? sum[c] / length
                                                             : (c == 2U ? 1.0F : 0.0F));
                    mip.rgba[offset + c] = byte(v);
                }
                mip.rgba[offset + 3U] = byte(sum[3U] / samples);
            }
        });
        width = mip.width;
        height = mip.height;
        texture.lower_mips.push_back(std::move(mip));
        source = &texture.lower_mips.back().rgba;
    }
}
} // namespace

bool prepare_model_mips(ModelAsset &asset) {
    const auto original = asset.textures.size();
    if (original > std::numeric_limits<core::u32>::max() / 3U)
        return false;
    std::vector<std::array<std::optional<core::u32>, 3U>> views(original);
    std::vector<Role> roles(original, Role::Data);
    const auto assign = [&](std::optional<core::u32> &view, Role role) {
        if (!view || *view >= original)
            return;
        auto &entry = views[*view];
        auto &index = entry[static_cast<std::size_t>(role)];
        if (!index) {
            if (std::none_of(entry.begin(), entry.end(),
                             [](auto v) { return v.has_value(); })) {
                index = *view;
                roles[*view] = role;
            } else {
                ModelTexture copy = asset.textures[*view];
                copy.lower_mips.clear();
                index = static_cast<core::u32>(asset.textures.size());
                asset.textures.push_back(std::move(copy));
                roles.push_back(role);
            }
        }
        view = index;
    };
    for (auto &part : asset.parts) {
        assign(part.base_color_texture, Role::Color);
        assign(part.emissive_texture, Role::Color);
        assign(part.metallic_roughness_texture, Role::Data);
        assign(part.occlusion_texture, Role::Data);
        assign(part.normal_texture, Role::Normal);
        assign(part.clearcoat_texture, Role::Data);
        assign(part.clearcoat_roughness_texture, Role::Data);
        assign(part.clearcoat_normal_texture, Role::Normal);
        assign(part.sheen_color_texture, Role::Color);
        assign(part.sheen_roughness_texture, Role::Data);
        assign(part.transmission_texture, Role::Data);
        assign(part.specular_texture, Role::Data);
        assign(part.specular_color_texture, Role::Color);
        assign(part.anisotropy_texture, Role::Data);

    }
    for (auto& texture : asset.textures) texture.requires_base_level = false;
    for (const auto& part : asset.parts) {
        const std::array indices{part.base_color_texture, part.metallic_roughness_texture,
            part.normal_texture, part.occlusion_texture, part.emissive_texture,
            part.clearcoat_texture, part.clearcoat_roughness_texture, part.clearcoat_normal_texture,
            part.sheen_color_texture, part.sheen_roughness_texture, part.transmission_texture,
            part.specular_texture, part.specular_color_texture, part.anisotropy_texture};
        for (std::size_t i = 0U; i < indices.size(); ++i)
            if (indices[i] && *indices[i] < asset.textures.size()
                && part.samplers[i].mip == ModelMipFilter::None)
                asset.textures[*indices[i]].requires_base_level = true;
    }
    for (std::size_t i = 0U; i < asset.textures.size(); ++i)
        if (asset.textures[i].valid() && asset.textures[i].encoding == ModelTextureEncoding::Rgba8)
            pyramid(asset.textures[i], roles[i]);
    return true;
}
} // namespace mediaserver::render
