#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/rhi/device.hpp>

// A reusable two-pass spatial blur.
//
// The pass owns one horizontal intermediate and one final sampled target. It
// records no copies and performs no CPU pixel work: a nonzero blur is one
// fullscreen horizontal draw followed by one fullscreen vertical draw. A zero
// radius returns the source texture itself, preserving the input exactly and
// avoiding two unnecessary passes.
namespace mediaserver::render {

inline constexpr float max_gaussian_blur_radius_px = 256.0F;

using GaussianBlurResult =
    core::Result<const rhi::Texture*, RenderError>;

class GaussianBlur final {
public:
    [[nodiscard]] static bool shaders_available() noexcept;

    [[nodiscard]] static core::Result<GaussianBlur, RenderError> create(
        rhi::Device& device,
        core::u32 width,
        core::u32 height);

    GaussianBlur(const GaussianBlur&) = delete;
    GaussianBlur& operator=(const GaussianBlur&) = delete;
    GaussianBlur(GaussianBlur&&) = default;
    GaussianBlur& operator=(GaussianBlur&&) = default;
    ~GaussianBlur() = default;

    [[nodiscard]] core::u32 width() const noexcept {
        return vertical_.width();
    }
    [[nodiscard]] core::u32 height() const noexcept {
        return vertical_.height();
    }

    // Replaces both targets transactionally. If either allocation fails, the
    // previous pair remains usable. A matching size is a no-op.
    [[nodiscard]] bool resize(rhi::Device& device, core::u32 width,
                              core::u32 height) noexcept;

    // Records the blur and returns the texture a downstream pass should use.
    //
    // `radius_px <= 0` succeeds with `&source` and records no GPU work.
    // Positive radii are clamped to max_gaussian_blur_radius_px and require a
    // sampled source matching this pass's current dimensions. A null result
    // value is never returned; failure is carried as RenderError.
    [[nodiscard]] GaussianBlurResult apply(rhi::CommandList& commands,
                                           const rhi::Texture& source,
                                           float radius_px) noexcept;

private:
    GaussianBlur(rhi::Texture horizontal, rhi::Texture vertical,
                 rhi::GraphicsPipeline pipeline,
                 rhi::Sampler sampler) noexcept;

    rhi::Texture horizontal_;
    rhi::Texture vertical_;
    rhi::GraphicsPipeline pipeline_;
    rhi::Sampler sampler_;
};

} // namespace mediaserver::render
