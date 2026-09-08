#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/scene.hpp>

namespace mediaserver::render {

// These are processor limits, not editor suggestions. The document models
// use the same bounds so an authored project cannot request a shader loop
// larger than the compiled kernel.
inline constexpr core::u32 max_box_blur_radius_px =
    scene::max_box_blur_radius_px;
inline constexpr core::u32 max_box_blur_passes = scene::max_box_blur_passes;
inline constexpr float max_directional_blur_length_px =
    scene::max_directional_blur_length_px;
inline constexpr core::u32 min_directional_blur_taps =
    scene::min_directional_blur_taps;
inline constexpr core::u32 max_directional_blur_taps =
    scene::max_directional_blur_taps;

struct BoxBlurKernel final {
  core::u32 radius_px{};
  core::u32 passes{1U};

  bool operator==(const BoxBlurKernel &) const = default;
};

// `length_px` is the full canvas-space line segment sampled, centred on the
// output pixel. Angle zero runs left/right and one quarter turn runs up/down.
// `taps` includes both endpoints and is forced odd by the scene model.
struct DirectionalBlurKernel final {
  float length_px{};
  scene::Turns angle_turns{};
  core::u32 taps{17U};

  bool operator==(const DirectionalBlurKernel &) const = default;
};

using SpatialBlurResult = core::Result<const rhi::Texture *, RenderError>;

// Two real, distinct gather processors sharing only their transient targets.
// Neither route aliases or delegates to GaussianBlur. Sharing the targets is
// safe because a recorded operation always chooses a destination different
// from its source and returns the texture containing the completed result.
class SpatialBlur final {
public:
  [[nodiscard]] static bool shaders_available() noexcept;

  [[nodiscard]] static core::Result<SpatialBlur, RenderError>
  create(rhi::Device &device, core::u32 width, core::u32 height,
         rhi::TextureFormat format = rhi::TextureFormat::Rgba8Unorm);

  SpatialBlur(const SpatialBlur &) = delete;
  SpatialBlur &operator=(const SpatialBlur &) = delete;
  SpatialBlur(SpatialBlur &&) = default;
  SpatialBlur &operator=(SpatialBlur &&) = default;
  ~SpatialBlur() = default;

  [[nodiscard]] core::u32 width() const noexcept { return first_.width(); }
  [[nodiscard]] core::u32 height() const noexcept { return first_.height(); }

  [[nodiscard]] bool resize(rhi::Device &device, core::u32 width,
                            core::u32 height) noexcept;

  // Radius zero is a bit-exact identity and records no work. An active box
  // blur records horizontal then vertical passes, repeated `passes` times.
  [[nodiscard]] SpatialBlurResult apply_box(rhi::CommandList &commands,
                                            const rhi::Texture &source,
                                            BoxBlurKernel kernel) noexcept;

  // Length zero is a bit-exact identity and records no work. An active
  // directional blur records one bounded gather along the centred line.
  [[nodiscard]] SpatialBlurResult
  apply_directional(rhi::CommandList &commands, const rhi::Texture &source,
                    DirectionalBlurKernel kernel) noexcept;

private:
  SpatialBlur(rhi::Texture first, rhi::Texture second,
              rhi::GraphicsPipeline box_pipeline,
              rhi::GraphicsPipeline directional_pipeline,
              rhi::Sampler sampler) noexcept;

  [[nodiscard]] rhi::Texture &
  destination_for(const rhi::Texture &source) noexcept;

  rhi::Texture first_;
  rhi::Texture second_;
  rhi::GraphicsPipeline box_pipeline_;
  rhi::GraphicsPipeline directional_pipeline_;
  rhi::Sampler sampler_;
};

} // namespace mediaserver::render
