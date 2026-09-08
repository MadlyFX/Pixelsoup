#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/scene.hpp>

#include <span>
#include <vector>

namespace mediaserver::render {

// The closed cubic path is converted to this fixed number of chords per
// authored segment by both the CPU reference and the shader. A fixed binary32
// construction is intentionally used instead of an adaptive tolerance whose
// branch decisions could disagree between nodes or GPU vendors. The closed
// chords use non-zero winding. Rectangle distance is exact Euclidean SDF;
// ellipse distance is the documented implicit-gradient approximation
// `k0*(k0-1)/k1`, also shared byte-for-byte in construction order.
inline constexpr core::u32 mask_bezier_subdivisions = 8U;

// Maps the processor's output UV into the MaskProgram's normalized coordinate
// space. `coordinate_extent_canvas_px` converts normalized distances to the
// authored canvas-pixel unit used by feather and expansion. A complete source
// layer uses origin (0,0), extent (1,1); a sliced raster may select a stable
// normalized subrectangle without changing the authored mask geometry.
struct MaskRasterMapping final {
    float origin_x{};
    float origin_y{};
    float extent_x{1.0F};
    float extent_y{1.0F};
    float coordinate_width_canvas_px{1.0F};
    float coordinate_height_canvas_px{1.0F};

    bool operator==(const MaskRasterMapping&) const = default;
};

[[nodiscard]] bool valid_mask_raster_mapping(
    const MaskRasterMapping& mapping) noexcept;

// Renderer-side validation is deliberately repeated even though SceneModel
// only emits normalized programs. Scene is public carrier data and tests,
// plugins, or a corrupt replication boundary can hand-build it. An active
// malformed program fails closed rather than reaching a shader with an invalid
// enum, unordered fold, or unterminated path.
[[nodiscard]] bool valid_mask_program(
    const scene::MaskProgram& program) noexcept;
[[nodiscard]] bool mask_program_is_identity(
    const scene::MaskProgram& program) noexcept;

struct MaskBoundsExpansion final {
    float x{};
    float y{};

    bool operator==(const MaskBoundsExpansion&) const = default;
};

// Final coverage is source RGBA multiplied by the folded field. Feather,
// inversion, and positive expansion can reveal or suppress existing source
// pixels but cannot create edge colour or alpha outside source coverage.
[[nodiscard]] constexpr MaskBoundsExpansion mask_program_bounds(
    const scene::MaskProgram&) noexcept {
    return {};
}

using MaskCoverageReferenceResult =
    core::Result<std::vector<float>, RenderError>;
using MaskedReferenceResult =
    core::Result<std::vector<rhi::Color>, RenderError>;

// Deterministic CPU references for the exact pixel-centre raster contract.
// Expansion subtracts from signed distance. Nonzero feather uses symmetric
// smooth coverage across [-feather,+feather], inversion follows shaping, and
// opacity multiplies last. Coverage is evaluated in row-major order, then the
// premultiplied source is multiplied by it. Positive expansion can reshape
// coverage but cannot manufacture alpha or colour in transparent padding.
[[nodiscard]] MaskCoverageReferenceResult mask_coverage_reference(
    core::u32 width, core::u32 height,
    const scene::MaskProgram& program,
    const MaskRasterMapping& mapping);
[[nodiscard]] MaskedReferenceResult mask_layer_reference(
    std::span<const rhi::Color> source, core::u32 width, core::u32 height,
    const scene::MaskProgram& program,
    const MaskRasterMapping& mapping);

// A mask changes coverage but never substitutes a card depth. The isolated
// compositor consumes this pair and validates that `depth` is the exact tested
// depth texture returned by SceneRenderer.
struct MaskedLayer final {
    const rhi::Texture* color{};
    const rhi::Texture* depth{};

    bool operator==(const MaskedLayer&) const = default;
};

struct MaskRasterStats final {
    core::u64 recorded_passes{};
    core::u64 processed_pixels{};
    core::u64 rasterized_operations{};

    bool operator==(const MaskRasterStats&) const = default;
};

using MaskRasterResult = core::Result<MaskedLayer, RenderError>;

class MaskRaster final {
public:
    [[nodiscard]] static bool shaders_available() noexcept;

    [[nodiscard]] static core::Result<MaskRaster, RenderError>
    create(rhi::Device& device, core::u32 width, core::u32 height,
           rhi::TextureFormat format = rhi::TextureFormat::Rgba8Unorm);

    MaskRaster(const MaskRaster&) = delete;
    MaskRaster& operator=(const MaskRaster&) = delete;
    MaskRaster(MaskRaster&&) = default;
    MaskRaster& operator=(MaskRaster&&) = default;
    ~MaskRaster() = default;

    [[nodiscard]] core::u32 width() const noexcept { return matte_a_.width(); }
    [[nodiscard]] core::u32 height() const noexcept {
        return matte_a_.height();
    }

    [[nodiscard]] bool resize(rhi::Device& device, core::u32 width,
                              core::u32 height) noexcept;

    // An empty program is exact identity: the borrowed source colour/depth
    // pointers are returned before program or mapping payloads are inspected,
    // and no command, texture write, or statistic is produced. Active programs
    // rasterize one ordered pass per operation and one final premultiplied
    // coverage pass.
    [[nodiscard]] MaskRasterResult apply(
        rhi::CommandList& commands, const rhi::Texture& source_color,
        const rhi::Texture& source_depth,
        const scene::MaskProgram& program,
        const MaskRasterMapping& mapping) noexcept;

    [[nodiscard]] const MaskRasterStats& stats() const noexcept {
        return stats_;
    }
    void reset_stats() noexcept { stats_ = {}; }

private:
    MaskRaster(rhi::Texture matte_a, rhi::Texture matte_b,
               rhi::Texture output_a, rhi::Texture output_b,
               rhi::GraphicsPipeline raster_pipeline,
               rhi::GraphicsPipeline compose_pipeline,
               rhi::Sampler sampler) noexcept;

    [[nodiscard]] rhi::Texture& output_for(
        const rhi::Texture& source) noexcept;
    void record_pass(bool raster_operation) noexcept;

    rhi::Texture matte_a_;
    rhi::Texture matte_b_;
    rhi::Texture output_a_;
    rhi::Texture output_b_;
    rhi::GraphicsPipeline raster_pipeline_;
    rhi::GraphicsPipeline compose_pipeline_;
    rhi::Sampler sampler_;
    MaskRasterStats stats_{};
};

} // namespace mediaserver::render
