#include <mediaserver/render/spatial_blur.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/box_blur_frag_dxil.h>
#include <mediaserver/shaders/directional_blur_frag_dxil.h>
#include <mediaserver/shaders/output_vert_dxil.h>
#endif

#include <array>
#include <cmath>
#include <span>
#include <utility>

namespace mediaserver::render {
namespace {

using CreateResult = core::Result<SpatialBlur, RenderError>;

struct ShaderBlobs final {
  std::span<const core::u8> vertex;
  std::span<const core::u8> box;
  std::span<const core::u8> directional;
  rhi::ShaderFormat format{rhi::ShaderFormat::Dxil};
  bool present{};
};

[[nodiscard]] ShaderBlobs shader_blobs() noexcept {
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
  return ShaderBlobs{
      std::span<const core::u8>{
          reinterpret_cast<const core::u8 *>(mediaserver_output_vert_dxil),
          sizeof(mediaserver_output_vert_dxil)},
      std::span<const core::u8>{
          reinterpret_cast<const core::u8 *>(mediaserver_box_blur_frag_dxil),
          sizeof(mediaserver_box_blur_frag_dxil)},
      std::span<const core::u8>{reinterpret_cast<const core::u8 *>(
                                    mediaserver_directional_blur_frag_dxil),
                                sizeof(mediaserver_directional_blur_frag_dxil)},
      rhi::ShaderFormat::Dxil,
      true,
  };
#else
  return ShaderBlobs{};
#endif
}

[[nodiscard]] rhi::TextureDesc color_desc(core::u32 width,
                                          core::u32 height,
                                          rhi::TextureFormat format) noexcept {
  rhi::TextureDesc desc{};
  desc.width = width;
  desc.height = height;
  desc.format = format;
  desc.color_target = true;
  desc.sampled = true;
  return desc;
}

[[nodiscard]] core::Result<rhi::GraphicsPipeline, RenderError>
create_pipeline(rhi::Device &device, const ShaderBlobs &blobs,
                std::span<const core::u8> fragment_bytecode,
                rhi::TextureFormat format) {
  rhi::ShaderDesc vertex{};
  vertex.stage = rhi::ShaderStage::Vertex;
  vertex.format = blobs.format;
  vertex.bytecode = blobs.vertex;

  rhi::ShaderDesc fragment{};
  fragment.stage = rhi::ShaderStage::Fragment;
  fragment.format = blobs.format;
  fragment.bytecode = fragment_bytecode;
  fragment.sampler_count = 1U;
  fragment.uniform_buffer_count = 1U;

  auto pipeline =
      device.create_pipeline(vertex, fragment, format, false,
                             rhi::DepthState{});
  if (!pipeline) {
    return core::Result<rhi::GraphicsPipeline, RenderError>::failure(
        pipeline.error() == rhi::RhiError::ShaderFormatUnsupported
            ? RenderError::ShaderFormatUnsupported
            : RenderError::BackendFailure);
  }
  return core::Result<rhi::GraphicsPipeline, RenderError>::success(
      std::move(pipeline).value());
}

struct alignas(16) BoxUniform final {
  float texel_x{};
  float texel_y{};
  core::u32 radius_px{};
  core::u32 padding{};
};

static_assert(sizeof(BoxUniform) == 16U);

struct alignas(16) DirectionalUniform final {
  float step_x{};
  float step_y{};
  float start_x{};
  float start_y{};
  core::u32 taps{};
  std::array<core::u32, 3U> padding{};
};

static_assert(sizeof(DirectionalUniform) == 32U);

template <typename Value>
[[nodiscard]] std::span<const core::u8> bytes(const Value &value) noexcept {
  return std::span<const core::u8>{reinterpret_cast<const core::u8 *>(&value),
                                   sizeof(value)};
}

[[nodiscard]] bool valid_source(const rhi::Texture &source, core::u32 width,
                                core::u32 height) noexcept {
  return source && source.desc().sampled &&
         source.desc().format != rhi::TextureFormat::Depth32Float &&
         source.width() == width && source.height() == height;
}

} // namespace

bool SpatialBlur::shaders_available() noexcept {
  return shader_blobs().present;
}

SpatialBlur::SpatialBlur(rhi::Texture first, rhi::Texture second,
                         rhi::GraphicsPipeline box_pipeline,
                         rhi::GraphicsPipeline directional_pipeline,
                         rhi::Sampler sampler) noexcept
    : first_(std::move(first)), second_(std::move(second)),
      box_pipeline_(std::move(box_pipeline)),
      directional_pipeline_(std::move(directional_pipeline)),
      sampler_(std::move(sampler)) {}

CreateResult SpatialBlur::create(rhi::Device &device, core::u32 width,
                                 core::u32 height,
                                 rhi::TextureFormat format) {
  const ShaderBlobs blobs = shader_blobs();
  if (!blobs.present || !device.supports(blobs.format)) {
    return CreateResult::failure(RenderError::ShaderFormatUnsupported);
  }
  if (width == 0U || height == 0U) {
    return CreateResult::failure(RenderError::InvalidConfiguration);
  }

  auto first = device.create_texture(color_desc(width, height, format));
  if (!first) {
    return CreateResult::failure(RenderError::AllocationFailed);
  }
  auto second = device.create_texture(color_desc(width, height, format));
  if (!second) {
    return CreateResult::failure(RenderError::AllocationFailed);
  }
  auto box_pipeline = create_pipeline(device, blobs, blobs.box, format);
  if (!box_pipeline) {
    return CreateResult::failure(box_pipeline.error());
  }
  auto directional_pipeline =
      create_pipeline(device, blobs, blobs.directional, format);
  if (!directional_pipeline) {
    return CreateResult::failure(directional_pipeline.error());
  }
  auto sampler = device.create_sampler(rhi::Filter::Linear);
  if (!sampler) {
    return CreateResult::failure(RenderError::AllocationFailed);
  }

  return CreateResult::success(SpatialBlur{
      std::move(first).value(), std::move(second).value(),
      std::move(box_pipeline).value(), std::move(directional_pipeline).value(),
      std::move(sampler).value()});
}

bool SpatialBlur::resize(rhi::Device &device, core::u32 width,
                         core::u32 height) noexcept {
  if (width == 0U || height == 0U) {
    return false;
  }
  if (first_.width() == width && first_.height() == height &&
      second_.width() == width && second_.height() == height) {
    return true;
  }
  const auto format = first_.desc().format;
  auto first = device.create_texture(color_desc(width, height, format));
  if (!first) {
    return false;
  }
  auto second = device.create_texture(color_desc(width, height, format));
  if (!second) {
    return false;
  }
  first_ = std::move(first).value();
  second_ = std::move(second).value();
  return true;
}

rhi::Texture &
SpatialBlur::destination_for(const rhi::Texture &source) noexcept {
  return &source == &first_ ? second_ : first_;
}

SpatialBlurResult SpatialBlur::apply_box(rhi::CommandList &commands,
                                         const rhi::Texture &source,
                                         BoxBlurKernel kernel) noexcept {
  if (!valid_source(source, width(), height())) {
    return SpatialBlurResult::failure(RenderError::InvalidConfiguration);
  }
  if (kernel.radius_px == 0U) {
    return SpatialBlurResult::success(&source);
  }
  if (kernel.radius_px > max_box_blur_radius_px || kernel.passes == 0U ||
      kernel.passes > max_box_blur_passes) {
    return SpatialBlurResult::failure(RenderError::InvalidConfiguration);
  }

  const rhi::Texture *input = &source;
  for (core::u32 pass = 0U; pass < kernel.passes; ++pass) {
    rhi::Texture &horizontal = destination_for(*input);
    const BoxUniform horizontal_uniform{1.0F / static_cast<float>(width()),
                                        0.0F, kernel.radius_px, 0U};
    if (!commands.draw_output_transform(
            horizontal, rhi::Color{0.0F, 0.0F, 0.0F, 0.0F}, box_pipeline_,
            *input, sampler_, bytes(horizontal_uniform))) {
      return SpatialBlurResult::failure(RenderError::BackendFailure);
    }

    rhi::Texture &vertical = destination_for(horizontal);
    const BoxUniform vertical_uniform{0.0F, 1.0F / static_cast<float>(height()),
                                      kernel.radius_px, 0U};
    if (!commands.draw_output_transform(
            vertical, rhi::Color{0.0F, 0.0F, 0.0F, 0.0F}, box_pipeline_,
            horizontal, sampler_, bytes(vertical_uniform))) {
      return SpatialBlurResult::failure(RenderError::BackendFailure);
    }
    input = &vertical;
  }
  return SpatialBlurResult::success(input);
}

SpatialBlurResult
SpatialBlur::apply_directional(rhi::CommandList &commands,
                               const rhi::Texture &source,
                               DirectionalBlurKernel kernel) noexcept {
  if (!valid_source(source, width(), height()) ||
      !std::isfinite(kernel.length_px) || !std::isfinite(kernel.angle_turns)) {
    return SpatialBlurResult::failure(RenderError::InvalidConfiguration);
  }
  if (kernel.length_px == 0.0F) {
    return SpatialBlurResult::success(&source);
  }
  if (kernel.length_px < 0.0F ||
      kernel.length_px > max_directional_blur_length_px ||
      kernel.taps < min_directional_blur_taps ||
      kernel.taps > max_directional_blur_taps || (kernel.taps & 1U) == 0U) {
    return SpatialBlurResult::failure(RenderError::InvalidConfiguration);
  }

  const scene::Turns angle = scene::wrap_turns(kernel.angle_turns);
  const float direction_x = scene::deterministic::cos_turns(angle);
  const float direction_y = scene::deterministic::sin_turns(angle);
  const float intervals = static_cast<float>(kernel.taps - 1U);
  const float step_px = kernel.length_px / intervals;
  const float start_px = -kernel.length_px * 0.5F;
  const DirectionalUniform uniform{
      direction_x * step_px / static_cast<float>(width()),
      direction_y * step_px / static_cast<float>(height()),
      direction_x * start_px / static_cast<float>(width()),
      direction_y * start_px / static_cast<float>(height()),
      kernel.taps,
      {}};
  rhi::Texture &destination = destination_for(source);
  if (!commands.draw_output_transform(
          destination, rhi::Color{0.0F, 0.0F, 0.0F, 0.0F},
          directional_pipeline_, source, sampler_, bytes(uniform))) {
    return SpatialBlurResult::failure(RenderError::BackendFailure);
  }
  return SpatialBlurResult::success(&destination);
}

} // namespace mediaserver::render
