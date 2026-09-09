#include <mediaserver/render/output_transform.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/output_frag_dxil.h>
#include <mediaserver/shaders/output_linear_frag_dxil.h>
#include <mediaserver/shaders/output_vert_dxil.h>
#endif

#include <array>
#include <span>
#include <utility>

namespace mediaserver::render {
namespace {

using Result = core::Result<OutputTransform, RenderError>;

struct ShaderBlobs final {
    std::span<const core::u8> vertex;
    std::span<const core::u8> fragment;
    std::span<const core::u8> linear_fragment;
    rhi::ShaderFormat format{rhi::ShaderFormat::Dxil};
    bool present{};
};

[[nodiscard]] ShaderBlobs shader_blobs() noexcept {
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    return ShaderBlobs{
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_output_vert_dxil),
            sizeof(mediaserver_output_vert_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_output_frag_dxil),
            sizeof(mediaserver_output_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_output_linear_frag_dxil),
            sizeof(mediaserver_output_linear_frag_dxil)},
        rhi::ShaderFormat::Dxil,
        true,
    };
#else
    // No shader compiler was available when this was built, so there is no
    // output path to offer.
    return ShaderBlobs{};
#endif
}

[[nodiscard]] rhi::TextureDesc color_desc(core::u32 width, core::u32 height, rhi::TextureFormat format) noexcept {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.color_target = true;
    // Sampled as well as drawn into, because presentation blits from it and a
    // readback test reads it.
    desc.sampled = true;
    return desc;
}

// The compiled default: what an output shows with no authored colour at all,
// and what the residency worker bakes identically once asked. It is the same
// request `adopt_color` builds from a default `ColorSettings`, so the two
// cannot disagree about what "nothing authored" looks like.
[[nodiscard]] DisplayLutRequest default_request() noexcept {
    return DisplayLutRequest{scene::ColorSettings{}.display_transform,
                             std::string{}, default_display_lut_size};
}

// The quantisation steps of the colour target's format. The dither is to this
// depth and no other; see `output_bit_depth`.
[[nodiscard]] core::u32 levels_for(rhi::TextureFormat format) noexcept {
    switch (format) {
    case rhi::TextureFormat::Rgb10A2Unorm: return 1023U;
    case rhi::TextureFormat::Rgba8Unorm:
    case rhi::TextureFormat::Bgra8Unorm:
        return 255U;
    default:
        return 0U;
    }
}

struct alignas(16) OutputUniform final {
    float viewport_x{};
    float viewport_y{};
    float viewport_width{1.0F};
    float viewport_height{1.0F};
    // The table's per-axis size and the shaper's pivot, so the shader reads
    // the table it was actually handed rather than a size compiled into it.
    float lut_size{};
    float lut_pivot{};
    float reserved[2U]{};
    // The dither. xy: what to add to an output pixel coordinate to get its
    // absolute canvas coordinate - the canvas region's origin less the
    // viewport's own - so the pattern is keyed to the wall. z: the number of
    // quantisation steps, or zero for no dither. w: unused.
    float dither_x{};
    float dither_y{};
    float dither_levels{};
    float dither_reserved{};
};

static_assert(sizeof(OutputUniform) == 48U);

[[nodiscard]] std::span<const core::u8>
bytes(const OutputUniform& uniform) noexcept {
    return std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(&uniform), sizeof(uniform)};
}

} // namespace

bool OutputTransform::shaders_available() noexcept {
    return shader_blobs().present;
}

OutputTransform::OutputTransform(
    rhi::Texture color,
    rhi::GraphicsPipeline display_pipeline,
    rhi::GraphicsPipeline linear_pipeline,
    rhi::Sampler sampler,
    bool default_scene_linear) noexcept
    : color_(std::move(color)),
      display_pipeline_(std::move(display_pipeline)),
      linear_pipeline_(std::move(linear_pipeline)),
      sampler_(std::move(sampler)),
      default_scene_linear_(default_scene_linear) {}

Result OutputTransform::create(
    rhi::Device& device,
    core::u32 width,
    core::u32 height,
    bool scene_linear_input, rhi::TextureFormat target_format) {
    if (target_format != rhi::TextureFormat::Rgba8Unorm
        && target_format != rhi::TextureFormat::Rgb10A2Unorm
        && target_format != rhi::TextureFormat::Rgba16Float)
        return Result::failure(RenderError::InvalidConfiguration);
    const auto blobs = shader_blobs();
    if (!blobs.present) {
        return Result::failure(RenderError::ShaderFormatUnsupported);
    }
    if (width == 0U || height == 0U) {
        return Result::failure(RenderError::InvalidConfiguration);
    }
    if (!device.supports(blobs.format)) {
        return Result::failure(RenderError::ShaderFormatUnsupported);
    }

    auto color = device.create_texture(color_desc(width, height, target_format));
    if (!color) {
        return Result::failure(RenderError::AllocationFailed);
    }

    rhi::ShaderDesc vertex{};
    vertex.stage = rhi::ShaderStage::Vertex;
    vertex.format = blobs.format;
    vertex.bytecode = blobs.vertex;
    // The fullscreen triangle reads nothing: its three corners come from
    // SV_VertexID, so there are no storage buffers and no uniforms here.

    rhi::ShaderDesc fragment{};
    fragment.stage = rhi::ShaderStage::Fragment;
    fragment.format = blobs.format;
    fragment.bytecode = blobs.fragment;
    // Two on the scene-linear path: the source and the baked table. The
    // display-referred pipeline binds one, because a source that is already
    // display-referred has nothing left to transform.
    fragment.sampler_count = 1U;
    fragment.uniform_buffer_count = 1U;

    auto display_pipeline = device.create_pipeline(
        vertex,
        fragment,
        target_format,
        // Opaque: the transform writes premultiplied colour and owns the whole
        // target, so there is nothing here to blend against.
        false,
        rhi::DepthState{});
    if (!display_pipeline) {
        return Result::failure(
            display_pipeline.error() == rhi::RhiError::ShaderFormatUnsupported
                ? RenderError::ShaderFormatUnsupported
                : RenderError::BackendFailure);
    }
    fragment.bytecode = blobs.linear_fragment;
    // The scene-linear pipeline binds the source and the baked table.
    fragment.sampler_count = 2U;
    auto linear_pipeline = device.create_pipeline(
        vertex, fragment, target_format, false,
        rhi::DepthState{});
    if (!linear_pipeline) {
        return Result::failure(
            linear_pipeline.error() == rhi::RhiError::ShaderFormatUnsupported
                ? RenderError::ShaderFormatUnsupported
                : RenderError::BackendFailure);
    }

    // Linear, so a source rendered at one resolution resamples cleanly onto an
    // output raster of another. The seam's samplers are clamp-to-edge, which
    // keeps the fade from wrapping the far edge's pixels in at the near one.
    auto sampler = device.create_sampler(rhi::Filter::Linear);
    if (!sampler) {
        return Result::failure(RenderError::AllocationFailed);
    }

    // The table the shader samples, and somewhere to put it.
    //
    // Baked here rather than loaded, so an output works with no authored
    // colour configuration at all and from its very first frame. It is the
    // same bake the residency worker performs for a default request, bit for
    // bit, so adopting that later changes nothing on screen.
    const DisplayLutRequest initial = default_request();
    DisplayLut lut = bake_display_lut(initial.transform, initial.size);
    if (!lut.valid()) {
        return Result::failure(RenderError::AllocationFailed);
    }
    rhi::TextureDesc lut_desc{};
    lut_desc.width = display_lut_atlas_width(lut.size);
    lut_desc.height = display_lut_atlas_height(lut.size);
    lut_desc.format = rhi::TextureFormat::Rgba16Float;
    lut_desc.sampled = true;
    auto lut_texture = device.create_texture(lut_desc);
    if (!lut_texture) {
        return Result::failure(RenderError::AllocationFailed);
    }

    OutputTransform transform{
        std::move(color).value(),
        std::move(display_pipeline).value(),
        std::move(linear_pipeline).value(),
        std::move(sampler).value(), scene_linear_input};
    transform.lut_texture_ = std::move(lut_texture).value();
    transform.atlas_ = std::make_shared<const std::vector<core::u8>>(pack_display_lut_atlas(lut));
    transform.lut_ = std::move(lut);
    for (auto& profile : transform.wall_profiles_) {
        auto texture = device.create_texture(lut_desc);
        if (!texture) return Result::failure(RenderError::AllocationFailed);
        profile.texture = std::move(texture).value();
    }
    return Result::success(std::move(transform));
}

void OutputTransform::set_display_lut(DisplayLut lut) {
    if (!lut.valid()) {
        return;
    }
    if (lut.size != lut_.size) {
        // The atlas is sized for the table it was created with. A different
        // size needs a different texture, which is a reallocation this seam
        // deliberately does not do mid-show.
        return;
    }
    atlas_ = std::make_shared<const std::vector<core::u8>>(pack_display_lut_atlas(lut));
    shared_lut_.reset();
    lut_ = std::move(lut);
    lut_uploaded_ = false;
}

void OutputTransform::adopt_color(const scene::ColorSettings& source, core::u64 authored_generation) noexcept {
    color_settings_ = scene::clamped(source);
    authored_generation_ = authored_generation;
    for (auto& profile : wall_profiles_) profile.active = false;
    const DisplayLutRequest request{color_settings_.display_transform,
                                    color_settings_.show_lut_source,
                                    lut_.size, authored_generation};
    const DisplayLutObservation observed = lut_cache_.observe(request);
    if (observed.state == DisplayLutState::Busy) {
        color_state_ = DisplayLutState::Busy;
        // Another thread held the state for this one observation. The resident
        // table and the last known state stand; the next frame asks again.
        return;
    }
    color_state_ = observed.state;
    // Adopt on a generation the frame has not seen, whether Ready or Failed:
    // a failed look publishes the transform alone, which is a more predictable
    // wall than whatever was resident before, and the state says which it is.
    if (observed.ready() && observed.generation != adopted_generation_
        && observed.table->size == lut_.size) {
        shared_lut_ = observed.table;
        atlas_ = observed.atlas;
        lut_uploaded_ = false;
        adopted_generation_ = observed.generation;
    }
}

bool OutputTransform::adopt_wall_color(doc::NodeId wall,
    const scene::WallColorSettings& settings) noexcept {
    if (!settings.enabled) return true;
    if (!wall) { color_state_ = DisplayLutState::Failed; return false; }
    WallProfile* slot = nullptr;
    for (auto& profile : wall_profiles_) if (profile.wall == wall) { slot = &profile; break; }
    if (slot == nullptr) for (auto& profile : wall_profiles_) if (!profile.active) { slot = &profile; break; }
    if (slot == nullptr) { color_state_ = DisplayLutState::Failed; return false; }
    if (slot->wall != wall) {
        slot->wall = wall; slot->atlas.reset(); slot->generation = 0U; slot->uploaded = false;
    }
    slot->active = true;
    DisplayLutRequest request{color_settings_.display_transform, color_settings_.show_lut_source,
        lut_.size, authored_generation_, scene::clamped(settings)};
    const auto observed = lut_cache_.observe(request);
    if (observed.state != DisplayLutState::Ready) color_state_ = observed.state;
    if (observed.ready() && observed.generation != slot->generation) {
        slot->atlas = observed.atlas; slot->generation = observed.generation; slot->uploaded = false;
    }
    return true;
}

core::u32 OutputTransform::output_bit_depth() const noexcept {
    return color_.desc().format == rhi::TextureFormat::Rgb10A2Unorm ? 10U
        : color_.desc().format == rhi::TextureFormat::Rgba16Float ? 16U : 8U;
}

bool OutputTransform::resize(
    rhi::Device& device,
    core::u32 width,
    core::u32 height) noexcept {
    if (width == 0U || height == 0U) {
        return false;
    }
    if (color_.width() == width && color_.height() == height) {
        return true;
    }
    auto color = device.create_texture(color_desc(width, height, color_.desc().format));
    if (!color) {
        return false;
    }
    color_ = std::move(color).value();
    return true;
}

bool OutputTransform::render(
    rhi::CommandList& commands,
    const rhi::Texture& source,
    scene::CanvasRect viewport) {
    return render(
        commands, source, viewport, default_scene_linear_);
}

bool OutputTransform::render(
    rhi::CommandList& commands,
    const rhi::Texture& source,
    scene::CanvasRect viewport,
    bool scene_linear_input) {
    return render(commands, source, viewport, scene_linear_input,
                  scene::CanvasRect{0U, 0U, viewport.width, viewport.height});
}

bool OutputTransform::render(
    rhi::CommandList& commands,
    const rhi::Texture& source,
    scene::CanvasRect viewport,
    bool scene_linear_input,
    scene::CanvasRect canvas_region) {
    return clear(commands) &&
        composite(commands, source, viewport, scene_linear_input,
                  canvas_region);
}

bool OutputTransform::clear(rhi::CommandList& commands) noexcept {
    graph_.begin();
    return graph_.clear(commands, core::RenderPass::Output, color_, rhi::Color{0.0F, 0.0F, 0.0F, 1.0F});
}

bool OutputTransform::composite(
    rhi::CommandList& commands,
    const rhi::Texture& source,
    scene::CanvasRect viewport) {
    return composite(
        commands, source, viewport, default_scene_linear_);
}

bool OutputTransform::composite(
    rhi::CommandList& commands,
    const rhi::Texture& source,
    scene::CanvasRect viewport,
    bool scene_linear_input) {
    return composite(commands, source, viewport, scene_linear_input,
                     scene::CanvasRect{0U, 0U, viewport.width,
                                       viewport.height});
}

bool OutputTransform::composite(
    rhi::CommandList& commands,
    const rhi::Texture& source,
    scene::CanvasRect viewport,
    bool scene_linear_input,
    scene::CanvasRect canvas_region, doc::NodeId wall) {
    const scene::Canvas window{width(), height()};
    if (!scene::contains(window, viewport)) {
        return false;
    }
    OutputUniform uniform{};
    uniform.viewport_x =
        static_cast<float>(viewport.x) / static_cast<float>(width());
    uniform.viewport_y =
        static_cast<float>(viewport.y) / static_cast<float>(height());
    uniform.viewport_width =
        static_cast<float>(viewport.width) / static_cast<float>(width());
    uniform.viewport_height =
        static_cast<float>(viewport.height) / static_cast<float>(height());
    uniform.lut_size = static_cast<float>(lut_.size);
    uniform.lut_pivot = display_shaper_pivot;
    uniform.dither_reserved = color_.desc().format == rhi::TextureFormat::Rgba16Float ? 1.0F : 0.0F;
    // The offset from an output pixel to its wall pixel. Exact in a float for
    // any raster this product drives, and it is the whole of what makes the
    // dither node-independent: two nodes showing adjacent regions add
    // different offsets to their own pixel coordinates and land on the same
    // absolute coordinate for the pixel they share.
    uniform.dither_x = static_cast<float>(
        static_cast<core::i64>(canvas_region.x)
        - static_cast<core::i64>(viewport.x));
    uniform.dither_y = static_cast<float>(
        static_cast<core::i64>(canvas_region.y)
        - static_cast<core::i64>(viewport.y));
    // Only the scene-linear path quantises anything: a display-referred source
    // is already at the target's depth and has no precision left to dither.
    uniform.dither_levels = scene_linear_input && color_settings_.dither
        ? static_cast<float>(levels_for(color_.desc().format))
        : 0.0F;

    if (scene_linear_input) {
        rhi::Texture* selected_texture = &lut_texture_;
        auto selected_atlas = atlas_;
        bool* uploaded = &lut_uploaded_;
        auto* recording = &lut_recording_;
        for (auto& profile : wall_profiles_) if (profile.active && profile.wall == wall && profile.atlas) {
            selected_texture = &profile.texture; selected_atlas = profile.atlas;
            uploaded = &profile.uploaded; recording = &profile.recording; break;
        }
        if (!lut_.valid() || !*selected_texture) {
            return false;
        }
        if (*recording) {
            const auto outcome = recording->outcome();
            if (outcome == rhi::CommandRecordingOutcome::Canceled) {
                *uploaded = false;
                *recording = {};
            } else if (outcome == rhi::CommandRecordingOutcome::Submitted) {
                *recording = {};
            } else if (!recording->same_recording(commands.recording_token())) {
                return false;
            }
        }
        if (!*uploaded) {
            // Once, on the first frame that needs it, and again only when
            // `adopt_color` or `set_display_lut` replaced the table. The bake
            // itself happened off the frame path.
            const auto receipt = commands.recording_token();
            if (!receipt || !selected_atlas || selected_atlas->empty()
                || !commands.upload(*selected_texture, *selected_atlas)) {
                return false;
            }
            *uploaded = true;
            *recording = receipt;
        }
        const std::array<rhi::SampledTexture, 2U> sources{{
            rhi::SampledTexture{&source, &sampler_},
            rhi::SampledTexture{selected_texture, &sampler_},
        }};
        return graph_.fullscreen(commands, core::RenderPass::Output,
            color_, rhi::Color{0.0F, 0.0F, 0.0F, 1.0F}, linear_pipeline_,
            sources, bytes(uniform), true);
    }
    // The target is loaded so other viewports in this window survive. The
    // fragment shader discards outside this viewport.
    return graph_.fullscreen(commands, core::RenderPass::Output,
        color_,
        rhi::Color{0.0F, 0.0F, 0.0F, 1.0F},
        display_pipeline_,
        source,
        sampler_,
        bytes(uniform),
        true);
}

} // namespace mediaserver::render
