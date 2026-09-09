#pragma once
#include <mediaserver/render/frame_graph.hpp>

#include <mediaserver/render/scene_renderer.hpp>

namespace mediaserver::render {

// Exposure duration as a fraction of one replicated show interval. Explicit
// tracked views carry their physical shutter; ordinary views use scene.camera.
[[nodiscard]] float object_shutter_fraction(
    const scene::Scene& frame, const SceneRenderView* view = nullptr) noexcept;

inline constexpr core::u32 object_motion_samples = 33U;

// Stateless, centred shutter reconstruction of the completed scene. The input
// motion contains object motion through the CURRENT view only, never camera
// motion. Depth remains the instantaneous picking/visibility product; filtered
// coverage is not published as a new hard occluder. No accumulated blur history.
class ObjectMotionBlur final {
public:
    [[nodiscard]] static core::Result<ObjectMotionBlur, RenderError> create(
        rhi::Device& device, core::u32 width, core::u32 height,
        rhi::TextureFormat format = rhi::TextureFormat::Rgba16Float,
        bool merge_layers = false) noexcept;
    ObjectMotionBlur() = default;
    ObjectMotionBlur(const ObjectMotionBlur&) = delete;
    ObjectMotionBlur& operator=(const ObjectMotionBlur&) = delete;
    ObjectMotionBlur(ObjectMotionBlur&&) = default;
    ObjectMotionBlur& operator=(ObjectMotionBlur&&) = default;

    [[nodiscard]] bool resize(rhi::Device& device, core::u32 width, core::u32 height) noexcept;
    [[nodiscard]] bool begin(rhi::CommandList& commands) noexcept;
    [[nodiscard]] bool merge(rhi::CommandList& commands, const rhi::Texture& motion) noexcept;
    [[nodiscard]] const rhi::Texture& merged_motion() const noexcept { return merged_; }
    [[nodiscard]] core::Result<const rhi::Texture*, RenderError> apply(
        rhi::CommandList& commands, const rhi::Texture& color,
        const rhi::Texture& motion, float shutter_fraction,
        core::u32 radius, float motion_width, float motion_height) noexcept;

private:
    FrameGraph graph_;
    rhi::Texture horizontal_;
    rhi::Texture neighborhood_;
    rhi::Texture output_;
    rhi::Texture merged_;
    rhi::GraphicsPipeline dilation_pipeline_;
    rhi::GraphicsPipeline resolve_pipeline_;
    rhi::ComputePipeline merge_pipeline_;
    rhi::Sampler sampler_;
};

} // namespace mediaserver::render
