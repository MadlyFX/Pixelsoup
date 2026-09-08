#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/rhi/device.hpp>

// Full-scene Program transition compositor.
//
// Both scenes are rendered completely before this pass. Mixing completed
// textures keeps a dissolve independent of object ordering/effects and gives
// every output node one deterministic scalar to apply at a control frame.
namespace mediaserver::render {

class SceneMixer final {
public:
    [[nodiscard]] static bool shaders_available() noexcept;

    [[nodiscard]] static core::Result<SceneMixer, RenderError>
    create(rhi::Device& device, core::u32 width, core::u32 height,
           rhi::TextureFormat format = rhi::TextureFormat::Rgba8Unorm);

    SceneMixer(const SceneMixer&) = delete;
    SceneMixer& operator=(const SceneMixer&) = delete;
    SceneMixer(SceneMixer&&) = default;
    SceneMixer& operator=(SceneMixer&&) = default;
    ~SceneMixer() = default;

    [[nodiscard]] rhi::Texture& target() noexcept { return color_; }
    [[nodiscard]] const rhi::Texture& target() const noexcept { return color_; }

    [[nodiscard]] bool resize(rhi::Device& device, core::u32 width,
                              core::u32 height) noexcept;

    // `amount` is clamped to [0,1]. Zero is entirely `from`, one entirely `to`.
    [[nodiscard]] bool render(rhi::CommandList& commands,
                              const rhi::Texture& from,
                              const rhi::Texture& to,
                              float amount) noexcept;

private:
    SceneMixer(rhi::Texture color, rhi::GraphicsPipeline pipeline,
               rhi::Sampler sampler) noexcept;

    rhi::Texture color_;
    rhi::GraphicsPipeline pipeline_;
    rhi::Sampler sampler_;
};

} // namespace mediaserver::render
