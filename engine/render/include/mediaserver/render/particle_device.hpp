#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/rhi/device.hpp>

#include <ajfx/gpu/device.hpp>

#include <array>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

// ajfx's GPU interface on this RHI, so particle simulations record into the
// renderer's own command lists and run the same precompiled kernels a package
// carries. Compute only: the renderer draws particles with its own passes,
// reading ajfx::gpu::EmitterBuffers through `buffer`.
//
// Every ajfx buffer is an RHI writable buffer, every kernel a compute pipeline
// with ajfx's fixed layout (uniform bytes at b0 in space 1, eight writable
// buffers at u0..u7), and an indirect dispatch an RHI indirect dispatch. The
// RHI orders dependent dispatches itself, so ajfx's barriers record nothing.
namespace mediaserver::render {

class ParticleDevice final : public ajfx::gpu::Device {
public:
    // Refused where the device cannot run compute with writable buffers and
    // indirect dispatch; the caller reports particles as unsupported then.
    [[nodiscard]] static core::Result<std::unique_ptr<ParticleDevice>, rhi::RhiError> create(rhi::Device& device);

    ~ParticleDevice() override;

    // Work ajfx records goes into `commands` until end_recording; the caller
    // submits it with the rest of its frame. With no list open, the device
    // records into one of its own, which flush submits and waits for - the
    // path tools and tests use.
    void begin_recording(rhi::CommandList& commands) noexcept;
    void end_recording() noexcept;

    // The RHI buffer behind an ajfx buffer, or null.
    [[nodiscard]] const rhi::StorageBuffer* buffer(ajfx::gpu::Buffer handle) const noexcept;
    // The first failure recorded since the last call, if any. Recording calls
    // cannot return one, so a renderer checks this once per frame.
    [[nodiscard]] std::optional<ajfx::Error> take_error() noexcept;
    // Buffers and kernels ajfx has released that submitted work may still be
    // using. They are freed as the RHI reports them safe; see collect.
    [[nodiscard]] std::size_t retired() const noexcept {
        return retired_buffers_.size() + retired_pipelines_.size();
    }

    [[nodiscard]] ajfx::gpu::ShaderFormat shader_format() const noexcept override;
    [[nodiscard]] std::string_view adapter_name() const noexcept override;

    [[nodiscard]] ajfx::Result<ajfx::gpu::Buffer> create_buffer(std::uint64_t bytes, std::string_view name) override;
    void destroy_buffer(ajfx::gpu::Buffer buffer) override;
    [[nodiscard]] ajfx::Result<ajfx::gpu::Pipeline> create_compute_pipeline(std::span<const std::uint8_t> bytecode,
                                                                            std::string_view name) override;
    [[nodiscard]] ajfx::Result<ajfx::gpu::Pipeline> create_graphics_pipeline(const ajfx::gpu::GraphicsPipelineDesc& desc,
                                                                             std::string_view name) override;
    void destroy_pipeline(ajfx::gpu::Pipeline pipeline) override;
    [[nodiscard]] ajfx::Result<ajfx::gpu::Texture> create_render_target(std::uint32_t width, std::uint32_t height,
                                                                        ajfx::gpu::TextureFormat format,
                                                                        std::string_view name) override;
    void destroy_texture(ajfx::gpu::Texture texture) override;

    void write_buffer(ajfx::gpu::Buffer buffer, std::uint64_t offset, std::span<const std::uint8_t> bytes) override;
    void copy_buffer(ajfx::gpu::Buffer destination, std::uint64_t destination_offset, ajfx::gpu::Buffer source,
                     std::uint64_t source_offset, std::uint64_t bytes) override;
    void dispatch(const ajfx::gpu::Dispatch& dispatch) override;
    void begin_rendering(ajfx::gpu::Texture target, const std::array<float, 4>* clear_color) override;
    void draw(const ajfx::gpu::Draw& draw) override;
    void end_rendering() override;
    void barrier() override;

    [[nodiscard]] ajfx::Result<bool> flush() override;
    [[nodiscard]] ajfx::Result<std::vector<std::uint8_t>> read_buffer(ajfx::gpu::Buffer buffer, std::uint64_t offset,
                                                                     std::uint64_t bytes) override;
    [[nodiscard]] ajfx::Result<std::vector<std::uint8_t>> read_texture(ajfx::gpu::Texture texture) override;

private:
    explicit ParticleDevice(rhi::Device& device) noexcept;

    // The list to record into: the borrowed one, or this device's own,
    // acquired on first use. Null (with an error noted) if none can be had.
    [[nodiscard]] rhi::CommandList* commands() noexcept;
    void note(std::string message) noexcept;
    // Frees retired buffers the RHI reports safe, and retired kernels whose
    // witness is: a buffer their last dispatch bound, so its last use is no
    // earlier than theirs.
    void collect() noexcept;
    [[nodiscard]] bool witness_safe(std::uint32_t witness) const noexcept;

    rhi::Device* device_;
    rhi::CommandList* borrowed_{};
    std::optional<rhi::CommandList> own_;
    std::unordered_map<std::uint32_t, rhi::StorageBuffer> buffers_;
    std::unordered_map<std::uint32_t, rhi::ComputePipeline> pipelines_;
    // The RHI frees a released buffer or pipeline at once, so ones submitted
    // work may still read wait here. The RHI tracks buffer use but not
    // pipeline use, so each kernel keeps a witness buffer (an ajfx buffer id,
    // or a placeholder slot with placeholder_witness set; 0 if never used).
    static constexpr std::uint32_t placeholder_witness = 0x80000000U;
    struct RetiredPipeline final {
        rhi::ComputePipeline pipeline;
        std::uint32_t witness{};
    };
    std::vector<std::pair<std::uint32_t, rhi::StorageBuffer>> retired_buffers_;
    std::vector<RetiredPipeline> retired_pipelines_;
    std::unordered_map<std::uint32_t, std::uint32_t> witnesses_;
    // One per slot: a buffer may be bound once per dispatch, so unbound
    // slots each need their own placeholder.
    std::array<rhi::StorageBuffer, ajfx::gpu::max_bound_buffers> placeholders_;
    std::uint32_t next_id_{1U};
    std::optional<ajfx::Error> error_;
};

} // namespace mediaserver::render
