#pragma once

#include <mediaserver/rhi/device.hpp>

#include <string_view>

namespace mediaserver::rhi::detail {
[[nodiscard]] bool d3d12_texture_safe_to_release(void* device, void* texture) noexcept;
[[nodiscard]] bool d3d12_upload_texture_rows(
    void* commands, void* texture, core::u32 mip, core::u32 layer,
    core::u32 first_row, core::u32 row_count,
    std::span<const core::u8> pixels) noexcept;

struct D3d12CreatedDevice final {
    void* state{};
    void* native{};
    DeviceCapabilities capabilities{};
};

struct D3d12CreatedResource final {
    void* state{};
    void* native{};
};

struct D3d12CreatedSwapchain final {
    void* state{};
    PresentMode mode{PresentMode::Immediate};
};

struct D3d12CreatedSwapchainImage final {
    void* state{};
    void* native{};
    core::u32 width{};
    core::u32 height{};
};

[[nodiscard]] bool d3d12_available() noexcept;
[[nodiscard]] core::Result<D3d12CreatedDevice, RhiError>
create_d3d12_device(const DeviceConfig& config) noexcept;
void destroy_d3d12_device(void* state) noexcept;
[[nodiscard]] core::RenderAdapterIdentity d3d12_adapter_identity(void* state) noexcept;
[[nodiscard]] std::string_view d3d12_driver_name(void* state) noexcept;
[[nodiscard]] core::u64 d3d12_dedicated_video_memory(void* state) noexcept;
[[nodiscard]] bool d3d12_supports_shader(ShaderFormat format) noexcept;
[[nodiscard]] bool d3d12_supports_texture(
    void* state, TextureFormat format) noexcept;
void d3d12_wait_idle(void* state) noexcept;

[[nodiscard]] core::Result<D3d12CreatedSwapchain, RhiError>
d3d12_create_swapchain(
    void* state, void* native_window, PresentMode preferred, TextureFormat format) noexcept;
void d3d12_release_swapchain(void* state, void* swapchain) noexcept;
// Native objects borrowed by the installed vendor present-barrier backend.
// Backbuffer pointers stay valid until the next resize or release.
[[nodiscard]] void* d3d12_native_swapchain(void* swapchain) noexcept;
[[nodiscard]] std::span<void* const>
d3d12_present_barrier_images(void* swapchain) noexcept;
[[nodiscard]] D3d12CreatedSwapchainImage d3d12_acquire_swapchain(
    void* state, void* swapchain, void* commands) noexcept;
[[nodiscard]] bool d3d12_present_swapchain(
    void* commands, void* swapchain, void* image, void* source,
    Filter filter) noexcept;

[[nodiscard]] core::Result<D3d12CreatedResource, RhiError>
d3d12_create_texture(void* state, const TextureDesc& desc) noexcept;
[[nodiscard]] core::Result<D3d12CreatedResource, RhiError>
d3d12_import_video_plane(
    void* state, void* resource, core::u32 width, core::u32 height,
    TextureFormat format, core::u32 plane) noexcept;
void d3d12_release_texture(void* state, void* texture) noexcept;

[[nodiscard]] core::Result<D3d12CreatedResource, RhiError>
d3d12_create_storage_buffer(void* state, core::u32 bytes) noexcept;
[[nodiscard]] core::Result<D3d12CreatedResource, RhiError>
d3d12_create_geometry_buffer(void* state, core::u32 bytes, bool writable = false) noexcept;
void d3d12_release_storage_buffer(void* state, void* buffer) noexcept;
[[nodiscard]] bool d3d12_buffer_safe_to_release(void* state, void* buffer) noexcept;

[[nodiscard]] core::Result<void*, RhiError>
d3d12_create_sampler(void* state, const SamplerDesc& desc) noexcept;
void d3d12_release_sampler(void* state, void* sampler) noexcept;

[[nodiscard]] core::Result<void*, RhiError>
d3d12_create_readback(void* state, core::u32 bytes) noexcept;
[[nodiscard]] std::span<const core::u8>
d3d12_map_readback(void* readback, core::u32 bytes) noexcept;
void d3d12_unmap_readback(void* readback) noexcept;
void d3d12_release_readback(void* state, void* readback) noexcept;

[[nodiscard]] core::Result<void*, RhiError> d3d12_create_pipeline(
    void* state,
    const ShaderDesc& vertex,
    const ShaderDesc& fragment,
    std::span<const TextureFormat> target_formats,
    ColorBlend blend,
    DepthState depth,
    RasterState raster) noexcept;
void d3d12_release_pipeline(void* state, void* pipeline) noexcept;

[[nodiscard]] core::Result<void*, RhiError> d3d12_create_compute_pipeline(
    void* state, const ShaderDesc& compute) noexcept;
void d3d12_release_compute_pipeline(void* state, void* pipeline) noexcept;
[[nodiscard]] bool d3d12_dispatch(
    void* commands, const ComputeDispatch& work) noexcept;

[[nodiscard]] core::Result<D3d12CreatedResource, RhiError>
d3d12_acquire_commands(void* state) noexcept;
void d3d12_release_commands(void* state, void* commands) noexcept;
[[nodiscard]] bool d3d12_upload_buffer(
    void* commands, void* buffer, std::span<const core::u8> data) noexcept;
[[nodiscard]] bool d3d12_upload_buffer_range(
    void* commands, void* buffer, core::u32 offset, std::span<const core::u8> data) noexcept;
[[nodiscard]] bool d3d12_copy_buffer(
    void* commands, void* source, core::u32 source_offset, void* destination, core::u32 destination_offset,
    core::u32 bytes) noexcept;
[[nodiscard]] core::u64 d3d12_upload_space(void* commands) noexcept;
[[nodiscard]] core::u32 d3d12_texture_upload_row_capacity(
    void* commands, void* texture, core::u64 reserve_bytes) noexcept;
[[nodiscard]] bool d3d12_upload_texture(
    void* commands, void* texture, std::span<const core::u8> data) noexcept;
[[nodiscard]] bool d3d12_draw_instanced_quads(
    void* commands,
    void* target,
    Color clear,
    void* pipeline,
    void* instances,
    core::u32 instance_count,
    void* atlas,
    void* sampler) noexcept;
[[nodiscard]] bool d3d12_draw_pulled(
    void* commands,
    void* color,
    Color clear,
    void* depth,
    float depth_clear,
    const PulledDraw& draw) noexcept;
[[nodiscard]] bool d3d12_draw_output_transform(
    void* commands,
    void* target,
    Color clear,
    void* pipeline,
    std::span<const SampledTexture> sources,
    std::span<const core::u8> fragment_uniform,
    bool load) noexcept;
[[nodiscard]] bool d3d12_transition(
    void* commands, void* target, ResourceState state) noexcept;
[[nodiscard]] bool d3d12_clear(
    void* commands, void* target, Color clear) noexcept;
[[nodiscard]] bool d3d12_blit(
    void* commands, void* source, void* destination, Filter filter) noexcept;
[[nodiscard]] bool d3d12_copy_region(
    void* commands,
    void* source,
    ViewportRect region,
    void* destination) noexcept;
[[nodiscard]] bool d3d12_download(
    void* commands, void* source, void* readback, core::u32 mip = 0U, core::u32 layer = 0U) noexcept;
[[nodiscard]] bool d3d12_download_buffer(
    void* commands, void* source, core::u32 offset, core::u32 bytes, void* readback) noexcept;

[[nodiscard]] core::Result<void*, RhiError>
d3d12_submit(void* state, void* commands) noexcept;
// True once ExecuteCommandLists has accepted this recording, including a
// submission whose later fence signal or presentation reporting failed. The
// command receipt must never call that case Canceled: GPU resources captured
// by the list may already be in use.
[[nodiscard]] bool d3d12_submission_accepted(void* commands) noexcept;
[[nodiscard]] bool d3d12_work_complete(void* state, void* work) noexcept;
[[nodiscard]] bool d3d12_wait_work(void* state, void* work) noexcept;
[[nodiscard]] core::u32 d3d12_begin_gpu_pass(void*, core::RenderPass) noexcept;
void d3d12_end_gpu_pass(void*, core::u32) noexcept;
core::u64 d3d12_upload_bytes(void*) noexcept;
core::FrameTiming d3d12_work_frame_timing(void*, void*) noexcept;
core::u64 d3d12_work_gpu_nanoseconds(
    void* state, void* work) noexcept;
void d3d12_release_work(void* state, void* work) noexcept;

} // namespace mediaserver::rhi::detail
