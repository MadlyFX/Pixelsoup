#pragma once

#include <array>
#include <memory>

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/show/presenter.hpp>
#include <mediaserver/show/test_pattern.hpp>

namespace mediaserver::ui {

// One offscreen colour target and everything needed to draw a frame into it.
//
// This is the unit the plan means by "one offscreen target per output": a
// surface that content is composited into, owned separately from whatever
// eventually displays it. A window blits it to a swapchain today; output
// hardware will take it directly later, and neither case is the renderer's
// business.
//
// Rendering is deliberately independent of any window, which is also what
// makes it testable on a build machine with a GPU and no display.
class GpuOutputTarget final {
public:
    [[nodiscard]] static core::Result<GpuOutputTarget, rhi::RhiError> create(
        rhi::Device& device,
        core::u32 width,
        core::u32 height);

    // True when this build carries shader bytecode for some format. False
    // means no device, however capable, can be given a pipeline.
    [[nodiscard]] static bool shaders_available() noexcept;

    GpuOutputTarget(const GpuOutputTarget&) = delete;
    GpuOutputTarget& operator=(const GpuOutputTarget&) = delete;
    GpuOutputTarget(GpuOutputTarget&&) noexcept = default;
    GpuOutputTarget& operator=(GpuOutputTarget&&) noexcept = default;
    ~GpuOutputTarget() = default;

    // Records the frame into `commands`: one instance upload and one draw. The
    // caller submits, because the same submission usually carries the present.
    [[nodiscard]] bool record(
        rhi::CommandList& commands,
        const show::TestPatternInput& frame) noexcept;

    [[nodiscard]] const rhi::Texture& texture() const noexcept { return target_; }
    [[nodiscard]] rhi::Texture& texture() noexcept { return target_; }
    [[nodiscard]] core::u32 width() const noexcept { return target_.width(); }
    [[nodiscard]] core::u32 height() const noexcept { return target_.height(); }

    // Instances the last recorded frame needed. Reported because it is the
    // measure of what the frame path actually uploads.
    [[nodiscard]] std::size_t last_instance_count() const noexcept {
        return last_instances_;
    }

    // Reads the target back and digests it. A self-check, not a frame path.
    [[nodiscard]] show::PresenterReadback readback(rhi::Device& device) noexcept;

private:
    GpuOutputTarget(
        rhi::Texture target,
        rhi::Texture atlas,
        rhi::Sampler sampler,
        rhi::GraphicsPipeline pipeline,
        rhi::StorageBuffer instances) noexcept;

    rhi::Texture target_;
    rhi::Texture atlas_;
    rhi::Sampler sampler_;
    rhi::GraphicsPipeline pipeline_;
    rhi::StorageBuffer instances_;
    // Heap-owned so the object stays cheap to move; sized once, never grown.
    std::unique_ptr<std::array<show::PatternQuad, show::max_pattern_quads>> quads_;
    std::size_t last_instances_{};
};

} // namespace mediaserver::ui
