#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/rhi/device.hpp>

#include <span>
#include <vector>

namespace mediaserver::render {

// Morphology is deliberately bounded: it is a pair of exact separable
// min/max gathers and the shader loop is compiled to this limit. Positive
// values dilate foreground coverage; negative values erode it.
inline constexpr core::i32 max_key_morphology_radius_px = 64;

// Key colours are straight Rec.709 RGB. Sources and results are premultiplied.
// Tolerance and softness measure Euclidean distance in Rec.709 Cb/Cr space.
struct ChromaKeyParameters final {
    rhi::Color key_color{0.0F, 1.0F, 0.0F, 1.0F};
    float tolerance{0.1F};
    float softness{0.1F};
    core::i32 morphology_px{};
    float spill_suppression{0.0F};
    float mix{1.0F};

    bool operator==(const ChromaKeyParameters&) const = default;
};

// Coverage is one through the ordered [low_threshold, high_threshold] band.
// Each softness extends its threshold outward: low fades over
// [low-low_softness, low], high over [high, high+high_softness].
struct LumaKeyParameters final {
    float low_threshold{0.0F};
    float high_threshold{1.0F};
    float low_softness{0.0F};
    float high_softness{0.0F};
    bool invert{};
    float mix{1.0F};

    bool operator==(const LumaKeyParameters&) const = default;
};

struct KeyBoundsExpansion final {
    float x{};
    float y{};

    bool operator==(const KeyBoundsExpansion&) const = default;
};

[[nodiscard]] bool valid_chroma_key(
    const ChromaKeyParameters& parameters) noexcept;
[[nodiscard]] bool valid_luma_key(
    const LumaKeyParameters& parameters) noexcept;
[[nodiscard]] bool chroma_key_is_identity(
    const ChromaKeyParameters& parameters) noexcept;
[[nodiscard]] bool luma_key_is_identity(
    const LumaKeyParameters& parameters) noexcept;
[[nodiscard]] KeyBoundsExpansion chroma_key_bounds(
    const ChromaKeyParameters& parameters) noexcept;
[[nodiscard]] constexpr KeyBoundsExpansion luma_key_bounds(
    const LumaKeyParameters&) noexcept {
    return {};
}

// Deterministic CPU references for model/processor tests. Pixels are row-major
// premultiplied RGBA. These implement the same matte, square morphology, spill,
// and final-coverage equations as the GPU shaders. Morphology reshapes the
// matte only inside source coverage: final alpha is always gated by source
// alpha, so neither keyer expands the source bounds or creates black fringes
// in transparent padding.
using KeyReferenceResult =
    core::Result<std::vector<rhi::Color>, RenderError>;

[[nodiscard]] KeyReferenceResult chroma_key_reference(
    std::span<const rhi::Color> source, core::u32 width, core::u32 height,
    const ChromaKeyParameters& parameters);
[[nodiscard]] KeyReferenceResult luma_key_reference(
    std::span<const rhi::Color> source, core::u32 width, core::u32 height,
    const LumaKeyParameters& parameters);

// An active key changes colour coverage but not the tested source depth. The
// later isolated-layer compositor consumes this pair together; retaining the
// exact depth pointer here prevents a key from turning intersecting geometry
// into an average-depth card.
struct KeyedLayer final {
    const rhi::Texture* color{};
    const rhi::Texture* depth{};

    bool operator==(const KeyedLayer&) const = default;
};

struct KeyerStats final {
    core::u64 recorded_passes{};
    core::u64 processed_pixels{};

    bool operator==(const KeyerStats&) const = default;
};

using LayerKeyerResult = core::Result<KeyedLayer, RenderError>;

class LayerKeyer final {
public:
    [[nodiscard]] static bool shaders_available() noexcept;

    [[nodiscard]] static core::Result<LayerKeyer, RenderError>
    create(rhi::Device& device, core::u32 width, core::u32 height,
           rhi::TextureFormat format = rhi::TextureFormat::Rgba8Unorm);

    LayerKeyer(const LayerKeyer&) = delete;
    LayerKeyer& operator=(const LayerKeyer&) = delete;
    LayerKeyer(LayerKeyer&&) = default;
    LayerKeyer& operator=(LayerKeyer&&) = default;
    ~LayerKeyer() = default;

    [[nodiscard]] core::u32 width() const noexcept { return matte_a_.width(); }
    [[nodiscard]] core::u32 height() const noexcept {
        return matte_a_.height();
    }

    [[nodiscard]] bool resize(rhi::Device& device, core::u32 width,
                              core::u32 height) noexcept;

    // Mix zero returns the exact borrowed source colour/depth pair and records
    // no command or statistic. Active calls produce a new premultiplied colour
    // texture and retain `source_depth` unchanged.
    [[nodiscard]] LayerKeyerResult apply_chroma(
        rhi::CommandList& commands, const rhi::Texture& source_color,
        const rhi::Texture& source_depth,
        const ChromaKeyParameters& parameters) noexcept;
    [[nodiscard]] LayerKeyerResult apply_luma(
        rhi::CommandList& commands, const rhi::Texture& source_color,
        const rhi::Texture& source_depth,
        const LumaKeyParameters& parameters) noexcept;

    [[nodiscard]] const KeyerStats& stats() const noexcept { return stats_; }
    void reset_stats() noexcept { stats_ = {}; }

private:
    LayerKeyer(rhi::Texture matte_a, rhi::Texture matte_b,
               rhi::Texture output_a, rhi::Texture output_b,
               rhi::GraphicsPipeline chroma_matte_pipeline,
               rhi::GraphicsPipeline luma_matte_pipeline,
               rhi::GraphicsPipeline morphology_pipeline,
               rhi::GraphicsPipeline chroma_compose_pipeline,
               rhi::GraphicsPipeline luma_compose_pipeline,
               rhi::Sampler sampler) noexcept;

    [[nodiscard]] rhi::Texture& output_for(
        const rhi::Texture& source) noexcept;
    void record_pass() noexcept;

    rhi::Texture matte_a_;
    rhi::Texture matte_b_;
    rhi::Texture output_a_;
    rhi::Texture output_b_;
    rhi::GraphicsPipeline chroma_matte_pipeline_;
    rhi::GraphicsPipeline luma_matte_pipeline_;
    rhi::GraphicsPipeline morphology_pipeline_;
    rhi::GraphicsPipeline chroma_compose_pipeline_;
    rhi::GraphicsPipeline luma_compose_pipeline_;
    rhi::Sampler sampler_;
    KeyerStats stats_{};
};

} // namespace mediaserver::render
