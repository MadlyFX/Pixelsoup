#pragma once

#include <mediaserver/render/model_loader.hpp>
#include <mediaserver/rhi/device.hpp>

namespace mediaserver::render {

// 9 UV address pairs, 12 authored filter combinations, and four anisotropic
// overrides of trilinear filtering. Constructed once; frame selection is pure.
inline constexpr std::size_t material_sampler_count = 9U * 16U;

[[nodiscard]] inline core::u32 material_sampler_index(
    const ModelSampler& sampler, core::u32 anisotropy) noexcept {
    core::u32 filter = static_cast<core::u32>(sampler.min) * 6U
        + static_cast<core::u32>(sampler.mag) * 3U + static_cast<core::u32>(sampler.mip);
    if (filter == 11U && anisotropy > 1U) {
        filter = anisotropy >= 16U ? 15U : anisotropy >= 8U ? 14U
            : anisotropy >= 4U ? 13U : 12U;
    }
    return (static_cast<core::u32>(sampler.u) * 3U
        + static_cast<core::u32>(sampler.v)) * 16U + filter;
}

[[nodiscard]] inline rhi::SamplerDesc material_sampler_desc(std::size_t index) noexcept {
    rhi::SamplerDesc desc{};
    desc.address_u = static_cast<rhi::AddressMode>(index / 48U);
    desc.address_v = static_cast<rhi::AddressMode>((index / 16U) % 3U);
    const auto filter = index % 16U;
    if (filter >= 12U) {
        desc.max_anisotropy = 2U << (filter - 12U);
    } else {
        desc.min_filter = static_cast<rhi::Filter>(filter / 6U);
        desc.mag_filter = static_cast<rhi::Filter>((filter / 3U) % 2U);
        desc.mip_filter = filter % 3U == 2U ? rhi::Filter::Linear : rhi::Filter::Nearest;
        if (filter % 3U == 0U) desc.max_lod = 0.0F;
    }
    return desc;
}

} // namespace mediaserver::render
