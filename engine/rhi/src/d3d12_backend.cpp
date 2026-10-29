#include "d3d12_backend.hpp"

#if defined(_WIN32) && defined(MEDIASERVER_HAS_D3D12_BACKEND)

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#ifdef DeviceCapabilities
#undef DeviceCapabilities
#endif

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <utility>

namespace mediaserver::rhi::detail {
namespace {

using Microsoft::WRL::ComPtr;

constexpr core::u32 max_command_contexts = 4U;
constexpr core::u32 swapchain_buffer_count = 3U;
constexpr core::u32 cpu_view_capacity = 16'384U;
constexpr core::u32 cpu_sampler_capacity = 2'048U;
constexpr core::u32 dynamic_view_capacity = 32'768U;
// D3D12's shader-visible sampler heap limit is 2,048 descriptors.
constexpr core::u32 dynamic_sampler_capacity = 2'048U;
// Resource state is speculated while a command list is recorded. Keep the
// original value in a fixed journal so cancelling that list can restore CPU
// state without allocating on the frame path. The limit is deliberately tied
// to the device's bounded texture-descriptor population.
constexpr core::u32 texture_state_capacity = cpu_view_capacity;
// Five persistently mapped pages give every command context an 80 MiB upload
// budget without making command acquisition allocate or wait. A DCI 4K RGBA8
// plate is 33.75 MiB, including no row-padding overhead at 4,096 pixels wide;
// two such plates therefore fit with 12.5 MiB left for uniforms and smaller
// updates. Texture copies may span pages, so no individual upload is limited to
// one page. Pages belong to a context and are reused only after that context's
// fence has completed.
constexpr core::u64 upload_page_capacity = 16ULL * 1024ULL * 1024ULL;
constexpr core::u32 upload_page_count = 5U;
constexpr core::u64 command_upload_capacity =
    upload_page_capacity * upload_page_count;
// Device-local buffers one recording may touch; submission stamps each one's
// fence so it can be released safely.
constexpr core::u32 buffer_use_capacity = 64U;
constexpr core::u32 invalid_descriptor = std::numeric_limits<core::u32>::max();
// D3D12 allows sixteen sampler slots per shader stage. A fragment stage that
// asks for more is refused at pipeline creation rather than at draw time.
constexpr core::u32 max_sampler_table_size = 16U;
constexpr core::u32 max_sampled_texture_table_size = 64U;
// Read-write texture bindings in one dispatch. Bounded for the same reason the
// sampler table is: the recording heap is fixed, and a shader asking for more
// than this is refused at pipeline creation rather than at the dispatch that
// would have exhausted it.
constexpr core::u32 max_storage_texture_table_size = 16U;

[[nodiscard]] constexpr core::u64 align_up(
    core::u64 value, core::u64 alignment) noexcept {
    return (value + alignment - 1U) & ~(alignment - 1U);
}

// A depth texture that is also sampled cannot be created as D32_FLOAT: a
// depth-stencil format is not a legal shader-resource format. The resource has
// to be typeless, with the depth view and the shader view each naming the
// interpretation they want. This is the only place in the backend where the
// resource format and the view format differ, and it is why they are three
// separate lookups rather than one.
[[nodiscard]] DXGI_FORMAT dxgi_resource_format(
    TextureFormat format, bool depth_and_sampled) noexcept;

[[nodiscard]] DXGI_FORMAT dxgi_format(TextureFormat format) noexcept {
    switch (format) {
    case TextureFormat::Rgb10A2Unorm: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case TextureFormat::Bc7Unorm: return DXGI_FORMAT_BC7_UNORM;
    case TextureFormat::Bc5Unorm: return DXGI_FORMAT_BC5_UNORM;
    case TextureFormat::Rgba8Unorm:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::Bgra8Unorm:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case TextureFormat::Rgba16Float:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case TextureFormat::Rgba32Float:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case TextureFormat::Rg16Float:
        return DXGI_FORMAT_R16G16_FLOAT;
    case TextureFormat::R32Float:
        return DXGI_FORMAT_R32_FLOAT;
    case TextureFormat::R32Uint:
        return DXGI_FORMAT_R32_UINT;
    case TextureFormat::R16Unorm:
        return DXGI_FORMAT_R16_UNORM;
    case TextureFormat::Rg16Unorm:
        return DXGI_FORMAT_R16G16_UNORM;
    case TextureFormat::R8Unorm:
        return DXGI_FORMAT_R8_UNORM;
    case TextureFormat::Rg8Unorm:
        return DXGI_FORMAT_R8G8_UNORM;
    case TextureFormat::Depth32Float:
        return DXGI_FORMAT_D32_FLOAT;
    }
    return DXGI_FORMAT_UNKNOWN;
}

DXGI_FORMAT dxgi_resource_format(
    TextureFormat format, bool depth_and_sampled) noexcept {
    if (depth_and_sampled && format == TextureFormat::Depth32Float) {
        return DXGI_FORMAT_R32_TYPELESS;
    }
    return dxgi_format(format);
}

// How a shader reads it. Only the typeless depth case differs.
[[nodiscard]] DXGI_FORMAT dxgi_shader_format(
    TextureFormat format) noexcept {
    if (format == TextureFormat::Depth32Float) {
        return DXGI_FORMAT_R32_FLOAT;
    }
    return dxgi_format(format);
}

[[nodiscard]] core::u32 bytes_per_pixel(TextureFormat format) noexcept {
    switch (format) {
    case TextureFormat::Rgb10A2Unorm:
    case TextureFormat::Rgba8Unorm:
    case TextureFormat::Bgra8Unorm:
    case TextureFormat::Rg16Float:
    case TextureFormat::R32Float:
    case TextureFormat::R32Uint:
    case TextureFormat::Rg16Unorm:
    case TextureFormat::Depth32Float:
        return 4U;
    case TextureFormat::Rgba16Float:
        return 8U;
    case TextureFormat::Rgba32Float:
        return 16U;
    case TextureFormat::R16Unorm:
    case TextureFormat::Rg8Unorm:
        return 2U;
    case TextureFormat::R8Unorm:
        return 1U;
    case TextureFormat::Bc7Unorm:
    case TextureFormat::Bc5Unorm: return 0U;
    }
    return 0U;
}

[[nodiscard]] D3D12_HEAP_PROPERTIES heap_properties(
    D3D12_HEAP_TYPE type) noexcept {
    D3D12_HEAP_PROPERTIES value{};
    value.Type = type;
    value.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    value.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    value.CreationNodeMask = 1U;
    value.VisibleNodeMask = 1U;
    return value;
}

[[nodiscard]] D3D12_RESOURCE_DESC buffer_desc(core::u64 bytes) noexcept {
    D3D12_RESOURCE_DESC value{};
    value.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    value.Width = bytes;
    value.Height = 1U;
    value.DepthOrArraySize = 1U;
    value.MipLevels = 1U;
    value.Format = DXGI_FORMAT_UNKNOWN;
    value.SampleDesc.Count = 1U;
    value.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return value;
}

struct DescriptorHeap final {
    ComPtr<ID3D12DescriptorHeap> heap;
    core::u32 capacity{};
    core::u32 cursor{};
    core::u32 stride{};
    bool shader_visible{};
    std::unique_ptr<core::u32[]> free_slots;
    core::u32 free_count{};

    [[nodiscard]] bool initialize(
        ID3D12Device* device,
        D3D12_DESCRIPTOR_HEAP_TYPE type,
        core::u32 count,
        bool visible) noexcept {
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = type;
        desc.NumDescriptors = count;
        desc.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                             : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)))) {
            return false;
        }
        free_slots.reset(new (std::nothrow) core::u32[count]);
        if (!free_slots) {
            heap.Reset();
            return false;
        }
        capacity = count;
        stride = device->GetDescriptorHandleIncrementSize(type);
        shader_visible = visible;
        return true;
    }

    [[nodiscard]] core::u32 allocate(core::u32 count = 1U) noexcept {
        if (count == 1U && free_count > 0U) {
            return free_slots[--free_count];
        }
        if (count == 0U || cursor > capacity || count > capacity - cursor) {
            return invalid_descriptor;
        }
        const core::u32 first = cursor;
        cursor += count;
        return first;
    }

    void release(core::u32 index) noexcept {
        if (index != invalid_descriptor && index < capacity &&
            free_count < capacity) {
            free_slots[free_count++] = index;
        }
    }

    [[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE cpu(core::u32 index) const noexcept {
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * stride;
        return handle;
    }

    [[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE gpu(core::u32 index) const noexcept {
        D3D12_GPU_DESCRIPTOR_HANDLE handle =
            heap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(index) * stride;
        return handle;
    }

    void reset() noexcept {
        cursor = 0U;
        free_count = 0U;
    }
};

struct CommandContext;

struct D3d12Texture final {
    ComPtr<ID3D12Resource> resource;
    TextureDesc desc{};
    // A plane view of a hardware decoder's own output. This device never
    // writes it and must never transition it: the decoder leaves it in COMMON,
    // sampling promotes it implicitly, and it decays back when the submission
    // completes, which is what lets the decoder write the surface again.
    bool external{};
    D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
    CommandContext* recording_owner{};
    core::u64 last_submission{};
    core::u32 rtv{invalid_descriptor};
    core::u32 dsv{invalid_descriptor};
    core::u32 srv{invalid_descriptor};
    core::u32 uav{invalid_descriptor};
};

struct D3d12Buffer final {
    struct Snapshot final {
        CommandContext* owner{};
        core::u64 generation{};
        D3D12_GPU_VIRTUAL_ADDRESS address{};
        bool bound{};
    };

    ComPtr<ID3D12Resource> resource;
    core::u8* mapped{};
    core::u32 bytes{};
    // An upload used by this command context lives in that context's fixed
    // pages until its fence completes. Root SRV bindings capture the address
    // at record time, so a later upload of the same logical StorageBuffer can
    // safely select another snapshot without changing an earlier draw.
    std::array<Snapshot, max_command_contexts> snapshots{};

    // Ranged (geometry) buffers live in the default heap and are written with
    // copies. Buffers decay to COMMON after every submission, so their state
    // is tracked per recording rather than across recordings.
    struct RecordingState final {
        core::u64 generation{};
        D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
    };
    bool device_local{};
    // Created with UAV access: a dispatch may bind it read-write.
    bool writable{};
    std::array<RecordingState, max_command_contexts> recording_states{};
    core::u32 recording_uses{};
    core::u64 last_submission{};
};

struct D3d12Sampler final {
    SamplerDesc description{};
    core::u32 references{1U};
    core::u32 descriptor{invalid_descriptor};
};

struct D3d12Readback final {
    ComPtr<ID3D12Resource> resource;
    std::unique_ptr<core::u8[]> packed;
    core::u8* gpu_mapped{};
    core::u64 capacity{};
    core::u64 row_bytes{};
    core::u32 row_pitch{};
    core::u32 rows{};
    core::u32 bytes{};
};

struct PipelineLayout final {
    core::u32 vertex_storage_base{invalid_descriptor};
    core::u32 vertex_storage_count{};
    core::u32 vertex_uniform_base{invalid_descriptor};
    core::u32 vertex_uniform_count{};
    core::u32 fragment_texture_table{invalid_descriptor};
    core::u32 fragment_texture_count{};
    core::u32 fragment_sampler_count{};
    core::u32 fragment_sampler_table{invalid_descriptor};
    core::u32 fragment_storage_base{invalid_descriptor};
    core::u32 fragment_storage_count{};
    core::u32 fragment_uniform_base{invalid_descriptor};
    core::u32 fragment_uniform_count{};
};

struct D3d12Pipeline final {
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> pipeline;
    PipelineLayout layout{};
};

// Root parameter indices for a compute pipeline.
//
// Flat rather than reusing PipelineLayout: compute has one stage, so the
// vertex/fragment split there means nothing here, and a layout carrying eleven
// fields of which six are permanently invalid is a layout that invites reading
// the wrong one.
struct ComputeLayout final {
    // Sampled textures and their samplers, as two tables built together the
    // way the graphics path builds them.
    core::u32 texture_table{invalid_descriptor};
    core::u32 texture_count{};
    core::u32 sampler_table{invalid_descriptor};
    // Read-write textures. Their own table because a UAV descriptor is created
    // per subresource at record time and cannot come from the resource's
    // whole-view descriptor the way an SRV can.
    core::u32 storage_texture_table{invalid_descriptor};
    core::u32 storage_texture_count{};
    core::u32 storage_base{invalid_descriptor};
    core::u32 storage_count{};
    core::u32 uniform_base{invalid_descriptor};
    core::u32 uniform_count{};
    // Read-write buffers, as root UAVs after the storage textures' registers.
    core::u32 writable_base{invalid_descriptor};
    core::u32 writable_count{};
};

struct D3d12ComputePipeline final {
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> pipeline;
    ComputeLayout layout{};
};

struct D3d12Device;
struct D3d12Swapchain;

struct UploadPage final {
    ComPtr<ID3D12Resource> resource;
    core::u8* mapped{};
    core::u64 cursor{};
};

struct TextureStateMutation final {
    D3d12Texture* texture{};
    D3D12_RESOURCE_STATES initial{D3D12_RESOURCE_STATE_COMMON};
};

inline constexpr core::u32 max_timed_passes = 4096U;
inline constexpr core::u32 queries_per_context = (max_timed_passes + 1U) * 2U;
struct CommandContext final {
    D3d12Device* owner{};
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    std::array<UploadPage, upload_page_count> upload_pages{};
    std::array<TextureStateMutation, texture_state_capacity> texture_states{};
    core::u32 texture_state_count{};
    std::array<D3d12Buffer*, buffer_use_capacity> buffer_uses{};
    core::u32 buffer_use_count{};
    DescriptorHeap views;
    DescriptorHeap samplers;
    // Remember every complete table in this recording. Alternating materials
    // must not spend the fixed shader-visible heap on the same table again.
    struct SamplerTable {
        std::array<const void*, max_sampler_table_size> handles{};
        core::u32 size{};
        core::u32 first{invalid_descriptor};
    };
    std::array<SamplerTable, dynamic_sampler_capacity> sampler_tables{};
    core::u32 sampler_table_count{};
    core::u64 fence_value{};
    // Incremented every time this context starts a new recording. Buffer
    // snapshots carry the generation so a recycled context never observes a
    // stale address from the prior submission.
    core::u64 recording_generation{};
    core::u32 query_index{};
    core::u32 timed_passes{};
    std::array<core::RenderPass, max_timed_passes> pass_categories{};
    std::array<bool, max_timed_passes> pass_ended{};
    core::u64 query_overflows{}, upload_bytes{}, present_ns{};
    D3d12Swapchain* pending_swapchain{};
    core::u32 pending_swapchain_index{};
    bool recording{};
    bool submitted{};
    bool ticket_owned{};
    bool quarantined{};
};

struct D3d12Device final {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter4> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> graphics_queue;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12QueryHeap> timestamp_queries;
    ComPtr<ID3D12Resource> timestamp_readback;
    core::u64* timestamp_mapped{};
    core::u64 timestamp_frequency{};
    HANDLE fence_event{};
    core::u64 next_fence{1U};
    core::u32 context_count{2U};
    std::array<CommandContext, max_command_contexts> contexts{};
    DescriptorHeap rtvs;
    DescriptorHeap dsvs;
    DescriptorHeap views;
    DescriptorHeap samplers;
    std::array<D3d12Sampler*, cpu_sampler_capacity> sampler_cache{};
    // Indirect argument layouts, made on first use. None changes root
    // arguments, so one serves every pipeline.
    ComPtr<ID3D12CommandSignature> dispatch_signature;
    ComPtr<ID3D12CommandSignature> draw_signature;
    ComPtr<ID3D12CommandSignature> draw_indexed_signature;
    std::string adapter_name;
    // What the chosen adapter has to itself. Zero on an integrated part, and
    // the measure the selection above uses to find the real GPU.
    core::u64 dedicated_video_memory{};
    core::RenderAdapterIdentity identity{};
};

struct D3d12Swapchain final {
    TextureFormat format{TextureFormat::Rgba8Unorm};
    D3d12Device* owner{};
    HWND window{};
    ComPtr<IDXGISwapChain4> swapchain;
    std::array<D3d12Texture, swapchain_buffer_count> buffers{};
    // Plain resource pointers for the vendor barrier seam. The ComPtrs above
    // own the resources; this cache is refreshed with the swapchain buffers
    // and never extends their lifetime.
    std::array<void*, swapchain_buffer_count> native_images{};
    std::array<core::u64, swapchain_buffer_count> buffer_fences{};
    HANDLE frame_latency_waitable{};
    // A successful wait admits one Present, not one attempt to acquire a
    // backbuffer. Retain it across fence/barrier/resize holds and abandoned
    // recordings: those cannot cause DXGI to signal another presentation.
    bool frame_latency_ready{};
    core::u32 width{};
    core::u32 height{};
    UINT flags{};
    PresentMode mode{PresentMode::Immediate};
};

[[nodiscard]] NativeHandle native_device_handle(
    const D3d12Device& device) noexcept {
    return NativeHandle{BackendKind::D3d12, NativeObjectKind::Device,
                        device.device.Get()};
}

[[nodiscard]] NativeHandle native_swapchain_handle(
    const D3d12Swapchain& swapchain) noexcept {
    return NativeHandle{BackendKind::D3d12, NativeObjectKind::Swapchain,
                        swapchain.swapchain.Get()};
}

[[nodiscard]] NativeHandle native_image_handle(
    const D3d12Texture& image) noexcept {
    return NativeHandle{BackendKind::D3d12, NativeObjectKind::SwapchainImage,
                        image.resource.Get()};
}

[[nodiscard]] D3d12Device* as_device(void* value) noexcept {
    return static_cast<D3d12Device*>(value);
}

[[nodiscard]] D3d12Texture* as_texture(void* value) noexcept {
    return static_cast<D3d12Texture*>(value);
}

[[nodiscard]] D3d12Buffer* as_buffer(void* value) noexcept {
    return static_cast<D3d12Buffer*>(value);
}

[[nodiscard]] D3d12Sampler* as_sampler(void* value) noexcept {
    return static_cast<D3d12Sampler*>(value);
}

[[nodiscard]] D3d12Readback* as_readback(void* value) noexcept {
    return static_cast<D3d12Readback*>(value);
}

[[nodiscard]] D3d12ComputePipeline* as_compute_pipeline(void* value) noexcept {
    return static_cast<D3d12ComputePipeline*>(value);
}

[[nodiscard]] D3d12Pipeline* as_pipeline(void* value) noexcept {
    return static_cast<D3d12Pipeline*>(value);
}

[[nodiscard]] CommandContext* as_commands(void* value) noexcept {
    return static_cast<CommandContext*>(value);
}

[[nodiscard]] D3d12Swapchain* as_swapchain(void* value) noexcept {
    return static_cast<D3d12Swapchain*>(value);
}

[[nodiscard]] bool initialize_context(
    D3d12Device& device,
    CommandContext& context,
    core::u32 index) noexcept {
    context.owner = &device;
    context.query_index = index;
    if (FAILED(device.device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&context.allocator)))) {
        return false;
    }
    if (FAILED(device.device->CreateCommandList(
            0U, D3D12_COMMAND_LIST_TYPE_DIRECT, context.allocator.Get(),
            nullptr, IID_PPV_ARGS(&context.list)))) {
        return false;
    }
    if (FAILED(context.list->Close())) {
        return false;
    }
    const auto upload_heap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
    const auto upload_desc = buffer_desc(upload_page_capacity);
    for (auto& page : context.upload_pages) {
        if (FAILED(device.device->CreateCommittedResource(
                &upload_heap, D3D12_HEAP_FLAG_NONE, &upload_desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&page.resource))) ||
            FAILED(page.resource->Map(
                0U, nullptr, reinterpret_cast<void**>(&page.mapped)))) {
            return false;
        }
    }
    return context.views.initialize(
               device.device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
               dynamic_view_capacity, true)
        && context.samplers.initialize(
               device.device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
               dynamic_sampler_capacity, true);
}

void finish_texture_states(CommandContext& commands, bool commit) noexcept {
    for (core::u32 index = 0U; index < commands.texture_state_count; ++index) {
        auto& mutation = commands.texture_states[index];
        if (mutation.texture == nullptr ||
            mutation.texture->recording_owner != &commands) {
            continue;
        }
        if (!commit) {
            mutation.texture->state = mutation.initial;
        }
        if (commit) mutation.texture->last_submission = commands.fence_value;
        mutation.texture->recording_owner = nullptr;
        mutation = {};
    }
    commands.texture_state_count = 0U;
}

[[nodiscard]] bool transition(
    CommandContext& commands,
    D3d12Texture& texture,
    D3D12_RESOURCE_STATES target) noexcept {
    if (texture.external) {
        // A read needs no barrier at all: the decoder leaves its surface in
        // COMMON, reading promotes it implicitly, and it decays back when the
        // submission completes - which is exactly what lets the decoder write
        // the surface again. An explicit transition would defeat that decay,
        // so a read-only target succeeds without recording one and anything
        // that would write the surface is refused.
        constexpr D3D12_RESOURCE_STATES readable =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
            | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
            | D3D12_RESOURCE_STATE_COPY_SOURCE;
        return target != D3D12_RESOURCE_STATE_COMMON
            && (target & ~readable) == 0;
    }
    if (texture.recording_owner != nullptr &&
        texture.recording_owner != &commands) {
        return false;
    }
    if (texture.recording_owner == nullptr) {
        if (commands.texture_state_count >= texture_state_capacity) {
            return false;
        }
        commands.texture_states[commands.texture_state_count++] =
            TextureStateMutation{&texture, texture.state};
        texture.recording_owner = &commands;
    }
    if (texture.state == target) {
        return true;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.resource.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = texture.state;
    barrier.Transition.StateAfter = target;
    commands.list->ResourceBarrier(1U, &barrier);
    texture.state = target;
    return true;
}

// Index reads are part of the one read state: an arena's index stream is both
// a pulled storage buffer and an input-assembler index buffer, often in the
// same pass, and a combined read state needs no barrier between the two.
constexpr D3D12_RESOURCE_STATES buffer_read_state =
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
    | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
    | D3D12_RESOURCE_STATE_INDEX_BUFFER;

// A ranged buffer's state in this recording. Its first use joins the
// context's journal, so submission can stamp the fence that releases it.
[[nodiscard]] D3d12Buffer::RecordingState* buffer_state(
    CommandContext& commands, D3d12Buffer& buffer) noexcept {
    if (commands.owner == nullptr) {
        return nullptr;
    }
    const auto index = static_cast<std::size_t>(
        &commands - commands.owner->contexts.data());
    if (index >= buffer.recording_states.size()) {
        return nullptr;
    }
    auto& entry = buffer.recording_states[index];
    if (entry.generation != commands.recording_generation) {
        if (commands.buffer_use_count >= buffer_use_capacity) {
            return nullptr;
        }
        commands.buffer_uses[commands.buffer_use_count++] = &buffer;
        ++buffer.recording_uses;
        entry = {commands.recording_generation, D3D12_RESOURCE_STATE_COMMON};
    }
    return &entry;
}

[[nodiscard]] bool transition_buffer(
    CommandContext& commands,
    D3d12Buffer& buffer,
    D3D12_RESOURCE_STATES target) noexcept {
    auto* const entry = buffer_state(commands, buffer);
    if (entry == nullptr) {
        return false;
    }
    if (entry->state == target) {
        return true;
    }
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = buffer.resource.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = entry->state;
    barrier.Transition.StateAfter = target;
    commands.list->ResourceBarrier(1U, &barrier);
    entry->state = target;
    return true;
}

void finish_buffer_uses(CommandContext& commands, bool commit) noexcept {
    for (core::u32 index = 0U; index < commands.buffer_use_count; ++index) {
        auto* const buffer = commands.buffer_uses[index];
        if (buffer == nullptr) {
            continue;
        }
        if (buffer->recording_uses > 0U) {
            --buffer->recording_uses;
        }
        if (commit) {
            buffer->last_submission =
                std::max(buffer->last_submission, commands.fence_value);
        }
        commands.buffer_uses[index] = nullptr;
    }
    commands.buffer_use_count = 0U;
}

struct UploadAllocation final {
    ID3D12Resource* resource{};
    core::u8* mapped{};
    core::u64 offset{};
    core::u64 bytes{};
};

// Allocates up to requested bytes, rounded down to a whole minimum block. The
// first fitting page wins, which makes placement deterministic and lets a large
// texture consume a partially used page before moving to the next one.
[[nodiscard]] UploadAllocation allocate_upload(
    CommandContext& commands,
    core::u64 requested,
    core::u64 minimum,
    core::u64 alignment) noexcept {
    if (requested == 0U || minimum == 0U || requested < minimum) {
        return {};
    }
    for (auto& page : commands.upload_pages) {
        const core::u64 offset = align_up(page.cursor, alignment);
        if (offset > upload_page_capacity) {
            continue;
        }
        const core::u64 available = upload_page_capacity - offset;
        core::u64 held = std::min(requested, available);
        held -= held % minimum;
        if (held < minimum) {
            continue;
        }
        page.cursor = offset + held;
        commands.upload_bytes += held;
        return UploadAllocation{
            page.resource.Get(), page.mapped, offset, held};
    }
    return {};
}

[[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS upload_bytes(
    CommandContext& commands,
    std::span<const core::u8> bytes,
    core::u64 alignment) noexcept {
    if (bytes.empty()) {
        return 0U;
    }
    const core::u64 padded = align_up(bytes.size(), alignment);
    if (padded < bytes.size() || padded > upload_page_capacity) {
        return 0U;
    }
    const UploadAllocation allocation = allocate_upload(
        commands, padded, padded, alignment);
    if (allocation.resource == nullptr) {
        return 0U;
    }
    std::memcpy(allocation.mapped + allocation.offset,
                bytes.data(), bytes.size());
    if (padded > bytes.size()) {
        std::memset(
            allocation.mapped + allocation.offset + bytes.size(), 0,
            static_cast<std::size_t>(padded - bytes.size()));
    }
    return allocation.resource->GetGPUVirtualAddress() + allocation.offset;
}

[[nodiscard]] D3d12Buffer::Snapshot* snapshot_for(
    D3d12Buffer& buffer,
    CommandContext& commands) noexcept {
    for (auto& snapshot : buffer.snapshots) {
        if (snapshot.owner == &commands) {
            return &snapshot;
        }
    }
    for (auto& snapshot : buffer.snapshots) {
        if (snapshot.owner == nullptr) {
            snapshot.owner = &commands;
            return &snapshot;
        }
    }
    // A buffer belongs to one device, and a device owns at most
    // max_command_contexts contexts, so every possible owner has a slot.
    return nullptr;
}

[[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS buffer_address(
    CommandContext& commands,
    D3d12Buffer& buffer) noexcept {
    if (buffer.device_local) {
        // Copies into a ranged buffer only ever target ranges no outstanding
        // draw reads; the barrier orders them against this recording's reads.
        return transition_buffer(commands, buffer, buffer_read_state)
            ? buffer.resource->GetGPUVirtualAddress() : 0U;
    }
    auto* const snapshot = snapshot_for(buffer, commands);
    if (snapshot != nullptr
        && snapshot->generation == commands.recording_generation
        && snapshot->address != 0U) {
        snapshot->bound = true;
        return snapshot->address;
    }
    if (snapshot == nullptr) {
        return 0U;
    }
    // Remember that this recording captured the persistent fallback too. A
    // later upload on the same list must not memcpy over it behind an earlier
    // draw; that case is refused below instead of silently changing the draw.
    snapshot->generation = commands.recording_generation;
    snapshot->address = buffer.resource->GetGPUVirtualAddress();
    snapshot->bound = true;
    return snapshot->address;
}

[[nodiscard]] bool committed_buffer_writable(
    const D3d12Buffer& buffer) noexcept {
    const auto committed = buffer.resource->GetGPUVirtualAddress();
    for (const auto& snapshot : buffer.snapshots) {
        if (!snapshot.bound || snapshot.address != committed
            || snapshot.owner == nullptr
            || snapshot.generation
                != snapshot.owner->recording_generation) {
            continue;
        }
        if (snapshot.owner->recording) {
            return false;
        }
        if (snapshot.owner->submitted
            && snapshot.owner->owner != nullptr
            && snapshot.owner->owner->fence
            && snapshot.owner->owner->fence->GetCompletedValue()
                < snapshot.owner->fence_value) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool copy_texture_views(
    D3d12Device& device,
    CommandContext& commands,
    std::span<const SampledTexture> sources,
    D3d12Pipeline& pipeline) noexcept {
    if (sources.size() != pipeline.layout.fragment_texture_count) {
        return false;
    }
    if (sources.empty()) {
        return true;
    }
    const auto sampler_count = pipeline.layout.fragment_sampler_count;
    if (sources.size() > max_sampled_texture_table_size || sampler_count == 0U) {
        return false;
    }
    CommandContext::SamplerTable candidate{};
    candidate.size = sampler_count;
    for (core::u32 i = 0U; i < sampler_count; ++i) {
        if (sources[i].sampler == nullptr) return false;
        candidate.handles[i] = sources[i].sampler->backend_handle();
    }
    bool same_samplers = false;
    for (core::u32 i = 0U; i < commands.sampler_table_count; ++i) {
        const auto& table = commands.sampler_tables[i];
        if (table.size == candidate.size && table.handles == candidate.handles) {
            candidate.first = table.first;
            same_samplers = true;
            break;
        }
    }
    if (!same_samplers && commands.sampler_table_count == commands.sampler_tables.size()) return false;
    const core::u32 view_first = commands.views.allocate(static_cast<core::u32>(sources.size()));
    const core::u32 sampler_first = same_samplers ? candidate.first : commands.samplers.allocate(sampler_count);
    if (view_first == invalid_descriptor || sampler_first == invalid_descriptor) {
        return false;
    }
    for (core::u32 index = 0U; index < sources.size(); ++index) {
        if (sources[index].texture == nullptr || sources[index].sampler == nullptr) {
            return false;
        }
        auto* texture = as_texture(sources[index].texture->backend_handle());
        auto* sampler = as_sampler(sources[index].sampler->backend_handle());
        if (texture == nullptr || sampler == nullptr ||
            texture->srv == invalid_descriptor ||
            sampler->descriptor == invalid_descriptor) {
            return false;
        }
        if (!transition(
                commands, *texture,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) {
            return false;
        }
        device.device->CopyDescriptorsSimple(
            1U, commands.views.cpu(view_first + index),
            device.views.cpu(texture->srv),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if (!same_samplers && index < sampler_count) {
            device.device->CopyDescriptorsSimple(
                1U, commands.samplers.cpu(sampler_first + index),
                device.samplers.cpu(sampler->descriptor),
                D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

        }
    }
    if (!same_samplers) {
        candidate.first = sampler_first;
        commands.sampler_tables[commands.sampler_table_count++] = candidate;
    }
    commands.list->SetGraphicsRootDescriptorTable(
        pipeline.layout.fragment_texture_table,
        commands.views.gpu(view_first));
    commands.list->SetGraphicsRootDescriptorTable(
        pipeline.layout.fragment_sampler_table,
        commands.samplers.gpu(sampler_first));
    return true;
}

void set_viewport(
    CommandContext& commands,
    const D3d12Texture& target,
    ViewportRect region = {}, RasterViewport raster = {}) noexcept {
    const core::u32 x = region.empty() ? 0U : region.x;
    const core::u32 y = region.empty() ? 0U : region.y;
    const core::u32 width = region.empty() ? target.desc.width : region.width;
    const core::u32 height =
        region.empty() ? target.desc.height : region.height;
    D3D12_VIEWPORT viewport{};
    viewport.TopLeftX = static_cast<float>(x);
    viewport.TopLeftY = static_cast<float>(y);
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    if (!raster.empty()) {
        viewport.TopLeftX = raster.x; viewport.TopLeftY = raster.y;
        viewport.Width = raster.width; viewport.Height = raster.height;
    }
    viewport.MaxDepth = 1.0F;
    commands.list->RSSetViewports(1U, &viewport);
    // The scissor matches the viewport rather than the target. Without it a
    // triangle clipped by the viewport transform would still be rasterized
    // outside it, which is the whole thing the caller asked not to happen.
    D3D12_RECT scissor{};
    scissor.left = static_cast<LONG>(x);
    scissor.top = static_cast<LONG>(y);
    scissor.right = static_cast<LONG>(x + width);
    scissor.bottom = static_cast<LONG>(y + height);
    commands.list->RSSetScissorRects(1U, &scissor);
}

void set_pipeline(CommandContext& commands, D3d12Pipeline& pipeline) noexcept {
    commands.list->SetGraphicsRootSignature(pipeline.root_signature.Get());
    commands.list->SetPipelineState(pipeline.pipeline.Get());
    commands.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12DescriptorHeap* heaps[]{
        commands.views.heap.Get(), commands.samplers.heap.Get()};
    commands.list->SetDescriptorHeaps(2U, heaps);
}

[[nodiscard]] bool bind_vertex_buffers(
    CommandContext& commands,
    D3d12Pipeline& pipeline,
    std::span<const StorageBuffer* const> buffers) noexcept {
    if (buffers.size() != pipeline.layout.vertex_storage_count) {
        return false;
    }
    for (core::u32 index = 0U; index < buffers.size(); ++index) {
        if (buffers[index] == nullptr) {
            return false;
        }
        auto* buffer = as_buffer(buffers[index]->backend_handle());
        if (buffer == nullptr) {
            return false;
        }
        const auto address = buffer_address(commands, *buffer);
        if (address == 0U) {
            return false;
        }
        commands.list->SetGraphicsRootShaderResourceView(
            pipeline.layout.vertex_storage_base + index,
            address);
    }
    return true;
}

[[nodiscard]] bool bind_fragment_buffers(
    CommandContext& commands,
    D3d12Pipeline& pipeline,
    std::span<const StorageBuffer* const> buffers) noexcept {
    if (buffers.size() != pipeline.layout.fragment_storage_count) {
        return false;
    }
    for (core::u32 index = 0U; index < buffers.size(); ++index) {
        if (buffers[index] == nullptr) {
            return false;
        }
        auto* buffer = as_buffer(buffers[index]->backend_handle());
        if (buffer == nullptr) {
            return false;
        }
        const auto address = buffer_address(commands, *buffer);
        if (address == 0U) {
            return false;
        }
        commands.list->SetGraphicsRootShaderResourceView(
            pipeline.layout.fragment_storage_base + index,
            address);
    }
    return true;
}

[[nodiscard]] bool bind_uniform(
    CommandContext& commands,
    core::u32 root_index,
    std::span<const core::u8> bytes) noexcept {
    const auto address = upload_bytes(
        commands, bytes, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
    if (address == 0U) {
        return false;
    }
    commands.list->SetGraphicsRootConstantBufferView(root_index, address);
    return true;
}

[[nodiscard]] D3D12_BLEND_DESC blend_desc(ColorBlend mode) noexcept {
    D3D12_BLEND_DESC result{};
    result.AlphaToCoverageEnable = FALSE;
    result.IndependentBlendEnable = FALSE;
    auto& target = result.RenderTarget[0U];
    target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    if (mode == ColorBlend::Disabled) {
        return result;
    }
    target.BlendEnable = TRUE;
    target.BlendOp = D3D12_BLEND_OP_ADD;
    target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D12_BLEND_ONE;
    target.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    switch (mode) {
    case ColorBlend::Disabled:
        break;
    case ColorBlend::Normal:
        target.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        target.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        break;
    case ColorBlend::Add:
        target.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        target.DestBlend = D3D12_BLEND_ONE;
        target.SrcBlendAlpha = D3D12_BLEND_ZERO;
        target.DestBlendAlpha = D3D12_BLEND_ONE;
        break;
    case ColorBlend::Multiply:
        target.SrcBlend = D3D12_BLEND_DEST_COLOR;
        target.DestBlend = D3D12_BLEND_ZERO;
        target.SrcBlendAlpha = D3D12_BLEND_ZERO;
        target.DestBlendAlpha = D3D12_BLEND_ONE;
        break;
    case ColorBlend::Screen:
        target.SrcBlend = D3D12_BLEND_ONE;
        target.DestBlend = D3D12_BLEND_INV_SRC_COLOR;
        target.SrcBlendAlpha = D3D12_BLEND_ZERO;
        target.DestBlendAlpha = D3D12_BLEND_ONE;
        break;
    case ColorBlend::PremultipliedNormal:
        target.SrcBlend = D3D12_BLEND_ONE;
        target.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        break;
    case ColorBlend::PremultipliedAdd:
        target.SrcBlend = D3D12_BLEND_ONE;
        target.DestBlend = D3D12_BLEND_ONE;
        target.SrcBlendAlpha = D3D12_BLEND_ZERO;
        target.DestBlendAlpha = D3D12_BLEND_ONE;
        break;
    }
    return result;
}

[[nodiscard]] std::string narrow_adapter_name(const wchar_t* source) {
    if (source == nullptr || *source == L'\0') {
        return "D3D12";
    }
    const int count = WideCharToMultiByte(
        CP_UTF8, 0, source, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1) {
        return "D3D12";
    }
    std::string result(static_cast<std::size_t>(count), '\0');
    static_cast<void>(WideCharToMultiByte(
        CP_UTF8, 0, source, -1, result.data(), count, nullptr, nullptr));
    result.pop_back();
    return result;
}

[[nodiscard]] bool swapchain_client_extent(
    HWND window, core::u32& width, core::u32& height) noexcept {
    RECT client{};
    if (window == nullptr || !GetClientRect(window, &client)) {
        return false;
    }
    width = static_cast<core::u32>(std::max(client.right - client.left, 0L));
    height = static_cast<core::u32>(std::max(client.bottom - client.top, 0L));
    return width > 0U && height > 0U;
}

void release_swapchain_buffers(D3d12Swapchain& swapchain) noexcept {
    if (swapchain.owner == nullptr) {
        return;
    }
    for (auto& buffer : swapchain.buffers) {
        swapchain.owner->rtvs.release(buffer.rtv);
        buffer = D3d12Texture{};
    }
    swapchain.native_images.fill(nullptr);
}

void refresh_present_barrier(D3d12Swapchain& swapchain) noexcept {
    if (swapchain.owner == nullptr || !swapchain.swapchain) {
        return;
    }
    const PresentBarrierBackendHandle backend = acquire_present_barrier_backend();
    if (!backend) {
        return;
    }
    std::array<NativeHandle, swapchain_buffer_count> images{};
    for (core::u32 index = 0U; index < swapchain_buffer_count; ++index) {
        images[index] = native_image_handle(swapchain.buffers[index]);
    }
    static_cast<void>(backend->refresh(PresentBarrierTarget{
        native_device_handle(*swapchain.owner),
        native_swapchain_handle(swapchain),
        std::span<const NativeHandle>{images},
    }));
}

[[nodiscard]] bool present_barrier_render_ready(
    const D3d12Swapchain& swapchain,
    const D3d12Texture& image) noexcept {
    const PresentBarrierBackendHandle backend = acquire_present_barrier_backend();
    return !backend || backend->render_ready(
                           native_swapchain_handle(swapchain),
                           native_image_handle(image));
}

[[nodiscard]] bool populate_swapchain_buffers(
    D3d12Swapchain& swapchain) noexcept {
    if (swapchain.owner == nullptr || !swapchain.swapchain) {
        return false;
    }
    for (core::u32 index = 0U; index < swapchain_buffer_count; ++index) {
        auto& buffer = swapchain.buffers[index];
        if (FAILED(swapchain.swapchain->GetBuffer(
                index, IID_PPV_ARGS(&buffer.resource)))) {
            release_swapchain_buffers(swapchain);
            return false;
        }
        swapchain.native_images[index] = buffer.resource.Get();
        buffer.desc.width = swapchain.width;
        buffer.desc.height = swapchain.height;
        buffer.desc.format = swapchain.format;
        buffer.desc.color_target = true;
        buffer.state = D3D12_RESOURCE_STATE_PRESENT;
        buffer.rtv = swapchain.owner->rtvs.allocate();
        if (buffer.rtv == invalid_descriptor) {
            release_swapchain_buffers(swapchain);
            return false;
        }
        swapchain.owner->device->CreateRenderTargetView(
            buffer.resource.Get(), nullptr,
            swapchain.owner->rtvs.cpu(buffer.rtv));
    }
    return true;
}

} // namespace

void* d3d12_native_swapchain(void* value) noexcept {
    const auto* swapchain = as_swapchain(value);
    return swapchain != nullptr ? swapchain->swapchain.Get() : nullptr;
}

std::span<void* const> d3d12_present_barrier_images(void* value) noexcept {
    const auto* swapchain = as_swapchain(value);
    if (swapchain == nullptr) {
        return {};
    }
    return std::span<void* const>{swapchain->native_images};
}

bool d3d12_available() noexcept {
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0U, IID_PPV_ARGS(&factory)))) {
        return false;
    }
    for (UINT index = 0U;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc)) ||
            (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0U) {
            continue;
        }
        if (SUCCEEDED(D3D12CreateDevice(
                adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                __uuidof(ID3D12Device), nullptr))) {
            return true;
        }
    }
    return false;
}

core::Result<D3d12CreatedDevice, RhiError> create_d3d12_device(
    const DeviceConfig& config) noexcept {
    using Result = core::Result<D3d12CreatedDevice, RhiError>;
    if (config.frames_in_flight == 0U ||
        config.frames_in_flight > max_command_contexts) {
        return Result::failure(RhiError::InvalidConfiguration);
    }
    if (config.debug) {
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
        }
    }
    auto* state = new (std::nothrow) D3d12Device{};
    if (state == nullptr) {
        return Result::failure(RhiError::AllocationFailed);
    }
    const UINT factory_flags = config.debug ? DXGI_CREATE_FACTORY_DEBUG : 0U;
    if (FAILED(CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&state->factory)))) {
        if (factory_flags == 0U ||
            FAILED(CreateDXGIFactory2(0U, IID_PPV_ARGS(&state->factory)))) {
            delete state;
            return Result::failure(RhiError::Unavailable);
        }
    }
    // Every adapter is considered and the one with the most dedicated video
    // memory wins, rather than taking DXGI's first "high performance" answer.
    //
    // On at least one laptop in this cluster that answer is the integrated
    // Intel part - 128 MiB of dedicated memory next to a 6 GB RTX 2060 - and
    // a render node that lands there cannot hold even two 4K video surfaces,
    // let alone render a show. Dedicated memory is the honest discriminator:
    // an integrated part shares system memory and reports almost none.
    //
    // Ties keep DXGI's preference order, which is what the enumeration below
    // walks when the amounts are equal.
    for (UINT index = 0U;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        const HRESULT enumerated = state->factory->EnumAdapterByGpuPreference(
            index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
            IID_PPV_ARGS(&candidate));
        if (enumerated == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(enumerated)) {
            continue;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(candidate->GetDesc1(&desc)) ||
            (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0U ||
            desc.DedicatedVideoMemory <= state->dedicated_video_memory) {
            continue;
        }
        ComPtr<ID3D12Device> device;
        if (FAILED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0,
                                     IID_PPV_ARGS(&device)))) {
            continue;
        }
        D3D12_FEATURE_DATA_SHADER_MODEL shader_model{D3D_SHADER_MODEL_6_2};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,
                &shader_model, sizeof(shader_model))) || shader_model.HighestShaderModel < D3D_SHADER_MODEL_6_2) {
            continue;
        }
        state->device = std::move(device);
        state->adapter.Reset();
        static_cast<void>(candidate.As(&state->adapter));
        state->dedicated_video_memory =
            static_cast<core::u64>(desc.DedicatedVideoMemory);
        state->adapter_name = narrow_adapter_name(desc.Description);
        state->identity = core::RenderAdapterIdentity{};
        std::copy_n(state->adapter_name.data(), std::min(state->adapter_name.size(), state->identity.name.size() - 1U), state->identity.name.data());
        state->identity.vendor = desc.VendorId;
        state->identity.device = desc.DeviceId;
        LARGE_INTEGER driver_version{};
        if (SUCCEEDED(candidate->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver_version)))
            state->identity.driver_version = static_cast<core::u64>(driver_version.QuadPart);
    }
    if (!state->device) {
        delete state;
        return Result::failure(RhiError::Unavailable);
    }
    D3D12_COMMAND_QUEUE_DESC queue_desc{};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(state->device->CreateCommandQueue(
            &queue_desc, IID_PPV_ARGS(&state->graphics_queue)))) {
        delete state;
        return Result::failure(RhiError::AllocationFailed);
    }
    if (FAILED(state->device->CreateFence(
            0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&state->fence)))) {
        delete state;
        return Result::failure(RhiError::AllocationFailed);
    }
    state->fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (state->fence_event == nullptr ||
        !state->rtvs.initialize(
            state->device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_RTV,
            cpu_view_capacity, false) ||
        !state->dsvs.initialize(
            state->device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
            cpu_view_capacity, false) ||
        !state->views.initialize(
            state->device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
            cpu_view_capacity, false) ||
        !state->samplers.initialize(
            state->device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
            cpu_sampler_capacity, false)) {
        if (state->fence_event != nullptr) {
            CloseHandle(state->fence_event);
        }
        delete state;
        return Result::failure(RhiError::AllocationFailed);
    }
    state->context_count = config.frames_in_flight;
    D3D12_QUERY_HEAP_DESC query_desc{};
    query_desc.Count = state->context_count * queries_per_context;
    query_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    const auto readback_heap = heap_properties(D3D12_HEAP_TYPE_READBACK);
    const auto readback_desc = buffer_desc(
        static_cast<core::u64>(query_desc.Count) * sizeof(core::u64));
    D3D12_RANGE timestamp_cpu_read{
        0U, static_cast<SIZE_T>(readback_desc.Width)};
    if (FAILED(state->device->CreateQueryHeap(
            &query_desc, IID_PPV_ARGS(&state->timestamp_queries)))
        || FAILED(state->device->CreateCommittedResource(
            &readback_heap, D3D12_HEAP_FLAG_NONE, &readback_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&state->timestamp_readback)))
        || FAILED(state->timestamp_readback->Map(
            0U, &timestamp_cpu_read,
            reinterpret_cast<void**>(&state->timestamp_mapped)))
        || FAILED(state->graphics_queue->GetTimestampFrequency(
            &state->timestamp_frequency))
        || state->timestamp_frequency == 0U) {
        CloseHandle(state->fence_event);
        delete state;
        return Result::failure(RhiError::AllocationFailed);
    }
    for (core::u32 index = 0U; index < state->context_count; ++index) {
        if (!initialize_context(*state, state->contexts[index], index)) {
            CloseHandle(state->fence_event);
            delete state;
            return Result::failure(RhiError::AllocationFailed);
        }
    }

    DeviceCapabilities capabilities{};
    capabilities.backend = BackendKind::D3d12;
    // This gate names the end-to-end implementation in this backend. The
    // format checks below remain independent so an unusual adapter still
    // fails renderer readiness deterministically before resource creation.
    capabilities.advanced_scene_raster = true;
    capabilities.offscreen = true;
    capabilities.rgba16_float =
        d3d12_supports_texture(state, TextureFormat::Rgba16Float);
    capabilities.rgba32_float =
        d3d12_supports_texture(state, TextureFormat::Rgba32Float);
    capabilities.sampled_depth = true;
    capabilities.integer_targets =
        d3d12_supports_texture(state, TextureFormat::R32Uint);
    // Bindable and dispatchable, not merely constructible. This backend
    // creates a per-subresource UAV for a storage binding and a dispatch reads
    // and writes through it, which is what the flag now means; before compute
    // existed here it could only honestly mean "this format and resource can
    // be made", and it said so.
    capabilities.storage_images = capabilities.rgba16_float &&
        capabilities.integer_targets;
    capabilities.timestamp_queries = true;
    // Callable: draw_pulled binds PulledDraw::index_buffer as the input
    // assembler's index buffer and issues indexed draws.
    capabilities.indexed_geometry = true;
    // Callable: a format-list pipeline and PulledDraw::additional_targets.
    capabilities.multiple_render_targets = true;
    // Callable: RasterState::fill selects D3D12_FILL_MODE_WIREFRAME.
    capabilities.wireframe_fill = true;
    // Callable: create_compute_pipeline builds a compute root signature and
    // PSO, and CommandList::dispatch binds sampled textures, per-subresource
    // storage textures, storage buffers and uniform bytes, inserts the UAV
    // barrier a dependent dispatch needs, and routes every transition through
    // the same journal a cancelled list rolls back. Everything runs on the
    // direct queue.
    capabilities.compute = true;
    // Callable: create_writable_buffer makes a UAV-capable device-local
    // buffer, a dispatch binds it read-write as a root UAV with a UAV barrier
    // between dependent dispatches, and indirect arguments drive a dispatch
    // or a single-batch pulled draw through ExecuteIndirect.
    capabilities.writable_buffers = true;
    capabilities.indirect_dispatch = true;
    capabilities.indirect_draw = true;
    // The adapter may support these, and the backend owns internal resource
    // transitions, but this RHI still exposes no callable bindless,
    // acceleration-structure or ray-query operation. Do not advertise raw
    // hardware potential as a renderer contract.
    capabilities.bindless = false;
    // Callable: CommandList::transition puts a texture into a named state
    // through the same journal every implicit transition uses, so a plan
    // compiled ahead of the frame can own its own barriers.
    capabilities.explicit_barriers = true;
    // Async compute needs a second queue, a second fence, and cross-queue
    // transitions. Dispatch on the direct queue is not that, and saying so
    // here is the point of keeping the two flags apart.
    capabilities.async_compute = false;
    capabilities.acceleration_structures = false;
    capabilities.ray_queries = false;
    // Plane views of a decoder's own surfaces, through import_video_planes.
    capabilities.external_memory = true;
    return Result::success(D3d12CreatedDevice{
        state, state->device.Get(), capabilities});
}

void destroy_d3d12_device(void* value) noexcept {
    auto* state = as_device(value);
    if (state == nullptr) {
        return;
    }
    d3d12_wait_idle(state);
    if (state->timestamp_readback && state->timestamp_mapped != nullptr) {
        state->timestamp_readback->Unmap(0U, nullptr);
        state->timestamp_mapped = nullptr;
    }
    for (core::u32 index = 0U; index < state->context_count; ++index) {
        for (auto& page : state->contexts[index].upload_pages) {
            if (page.resource && page.mapped != nullptr) {
                page.resource->Unmap(0U, nullptr);
                page.mapped = nullptr;
            }
        }
    }
    if (state->fence_event != nullptr) {
        CloseHandle(state->fence_event);
    }
    delete state;
}

core::RenderAdapterIdentity d3d12_adapter_identity(void* opaque) noexcept {
    const auto* state = static_cast<const D3d12Device*>(opaque);
    return state ? state->identity : core::RenderAdapterIdentity{};
}

std::string_view d3d12_driver_name(void* value) noexcept {
    const auto* state = as_device(value);
    return state == nullptr ? std::string_view{} : state->adapter_name;
}

core::u64 d3d12_dedicated_video_memory(void* value) noexcept {
    const auto* state = as_device(value);
    return state == nullptr ? 0U : state->dedicated_video_memory;
}

bool d3d12_supports_shader(ShaderFormat format) noexcept {
    return format == ShaderFormat::Dxil;
}

bool d3d12_supports_texture(void* value, TextureFormat format) noexcept {
    auto* state = as_device(value);
    const DXGI_FORMAT native = dxgi_format(format);
    if (state == nullptr || native == DXGI_FORMAT_UNKNOWN) {
        return false;
    }
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support{};
    support.Format = native;
    if (FAILED(state->device->CheckFeatureSupport(
            D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))) {
        return false;
    }
    if (format == TextureFormat::Depth32Float) {
        return (support.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL) != 0U;
    }
    return (support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) != 0U;
}

void d3d12_wait_idle(void* value) noexcept {
    auto* state = as_device(value);
    if (state == nullptr || !state->graphics_queue || !state->fence) {
        return;
    }
    const core::u64 target = state->next_fence++;
    if (FAILED(state->graphics_queue->Signal(state->fence.Get(), target))) {
        return;
    }
    if (state->fence->GetCompletedValue() < target &&
        SUCCEEDED(state->fence->SetEventOnCompletion(
            target, state->fence_event))) {
        static_cast<void>(WaitForSingleObject(state->fence_event, INFINITE));
    }
}

core::Result<D3d12CreatedSwapchain, RhiError> d3d12_create_swapchain(
    void* value, void* native_window, PresentMode preferred, TextureFormat format) noexcept {
    using Result = core::Result<D3d12CreatedSwapchain, RhiError>;
    auto* state = as_device(value);
    auto window = static_cast<HWND>(native_window);
    core::u32 width{};
    core::u32 height{};
    if (state == nullptr || window == nullptr || !IsWindow(window) ||
        !swapchain_client_extent(window, width, height)) {
        return Result::failure(RhiError::InvalidConfiguration);
    }
    auto* created = new (std::nothrow) D3d12Swapchain{};
    if (created == nullptr) {
        return Result::failure(RhiError::AllocationFailed);
    }
    if (format != TextureFormat::Rgba8Unorm && format != TextureFormat::Rgb10A2Unorm
        && format != TextureFormat::Rgba16Float) {
        delete created; return Result::failure(RhiError::InvalidConfiguration);
    }
    created->format = format;
    created->owner = state;
    created->window = window;
    created->width = width;
    created->height = height;
    created->mode = preferred == PresentMode::Vsync
        ? PresentMode::Vsync : PresentMode::Immediate;

    BOOL allow_tearing = FALSE;
    if (SUCCEEDED(state->factory->CheckFeatureSupport(
            DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing,
            sizeof(allow_tearing))) && allow_tearing) {
        created->flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }
    created->flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = dxgi_format(format);
    desc.SampleDesc.Count = 1U;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = swapchain_buffer_count;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = created->flags;
    ComPtr<IDXGISwapChain1> base;
    if (FAILED(state->factory->CreateSwapChainForHwnd(
            state->graphics_queue.Get(), window, &desc, nullptr, nullptr,
            &base)) || FAILED(base.As(&created->swapchain))) {
        delete created;
        return Result::failure(RhiError::BackendFailure);
    }
    const auto color_space = format == TextureFormat::Rgba16Float
        ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    UINT color_support{};
    if (FAILED(created->swapchain->CheckColorSpaceSupport(color_space, &color_support))
        || (color_support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) == 0U
        || FAILED(created->swapchain->SetColorSpace1(color_space))) {
        delete created; return Result::failure(RhiError::BackendFailure);
    }
    static_cast<void>(state->factory->MakeWindowAssociation(
        window, DXGI_MWA_NO_ALT_ENTER));
    if (FAILED(created->swapchain->SetMaximumFrameLatency(
            state->context_count))) {
        delete created;
        return Result::failure(RhiError::BackendFailure);
    }
    created->frame_latency_waitable =
        created->swapchain->GetFrameLatencyWaitableObject();
    if (created->frame_latency_waitable == nullptr ||
        !populate_swapchain_buffers(*created)) {
        if (created->frame_latency_waitable != nullptr) {
            CloseHandle(created->frame_latency_waitable);
        }
        delete created;
        return Result::failure(RhiError::AllocationFailed);
    }
    return Result::success(D3d12CreatedSwapchain{
        created, created->mode});
}

void d3d12_release_swapchain(void* value, void* swapchain_value) noexcept {
    auto* state = as_device(value);
    auto* swapchain = as_swapchain(swapchain_value);
    if (state == nullptr || swapchain == nullptr) {
        return;
    }
    // Destruction is outside the frame path, so releasing the buffers only
    // after the queue is idle is safe and keeps DXGI ownership unambiguous.
    d3d12_wait_idle(state);
    release_swapchain_buffers(*swapchain);
    if (swapchain->frame_latency_waitable != nullptr) {
        CloseHandle(swapchain->frame_latency_waitable);
        swapchain->frame_latency_waitable = nullptr;
    }
    delete swapchain;
}

D3d12CreatedSwapchainImage d3d12_acquire_swapchain(
    void* value, void* swapchain_value, void* command_value) noexcept {
    auto* state = as_device(value);
    auto* swapchain = as_swapchain(swapchain_value);
    auto* commands = as_commands(command_value);
    if (state == nullptr || swapchain == nullptr || commands == nullptr ||
        commands->owner != state || !commands->recording ||
        commands->pending_swapchain != nullptr) {
        return {};
    }
    if (!swapchain->frame_latency_ready) {
        if (swapchain->frame_latency_waitable != nullptr &&
            WaitForSingleObject(swapchain->frame_latency_waitable, 0U) !=
                WAIT_OBJECT_0) {
            return {};
        }
        swapchain->frame_latency_ready = true;
    }
    core::u32 width{};
    core::u32 height{};
    if (!swapchain_client_extent(swapchain->window, width, height)) {
        return {};
    }
    if (width != swapchain->width || height != swapchain->height) {
        const core::u64 completed = state->fence->GetCompletedValue();
        for (const core::u64 fence : swapchain->buffer_fences) {
            if (fence > completed) {
                return {};
            }
        }
        release_swapchain_buffers(*swapchain);
        if (FAILED(swapchain->swapchain->ResizeBuffers(
                swapchain_buffer_count, width, height,
                dxgi_format(swapchain->format), swapchain->flags))) {
            return {};
        }
        swapchain->width = width;
        swapchain->height = height;
        swapchain->buffer_fences.fill(0U);
        if (!populate_swapchain_buffers(*swapchain)) {
            swapchain->width = 0U;
            swapchain->height = 0U;
            return {};
        }
        // A resize replaces every resource the native barrier registered.
        // This is a rare configuration event, never a per-frame rejoin.
        refresh_present_barrier(*swapchain);
    }
    const core::u32 index = swapchain->swapchain->GetCurrentBackBufferIndex();
    if (index >= swapchain_buffer_count ||
        swapchain->buffer_fences[index] > state->fence->GetCompletedValue()) {
        return {};
    }
    auto& buffer = swapchain->buffers[index];
    // The barrier uses its own fence to say when this particular backbuffer
    // is reusable. Polling makes a frame a deliberate hold instead of a
    // driver wait on the render path.
    if (!present_barrier_render_ready(*swapchain, buffer)) {
        return {};
    }
    return D3d12CreatedSwapchainImage{
        &buffer, buffer.resource.Get(), buffer.desc.width, buffer.desc.height};
}

bool d3d12_present_swapchain(
    void* command_value, void* swapchain_value, void* image_value,
    void* source_value, Filter) noexcept {
    auto* commands = as_commands(command_value);
    auto* swapchain = as_swapchain(swapchain_value);
    auto* image = as_texture(image_value);
    auto* source = as_texture(source_value);
    if (commands == nullptr || swapchain == nullptr || image == nullptr ||
        source == nullptr || !commands->recording ||
        commands->owner != swapchain->owner ||
        commands->pending_swapchain != nullptr ||
        source->desc.width != image->desc.width ||
        source->desc.height != image->desc.height ||
        source->desc.format != swapchain->format) {
        return false;
    }
    core::u32 index = swapchain_buffer_count;
    for (core::u32 candidate = 0U; candidate < swapchain_buffer_count;
         ++candidate) {
        if (&swapchain->buffers[candidate] == image) {
            index = candidate;
            break;
        }
    }
    if (index >= swapchain_buffer_count) {
        return false;
    }
    if (!transition(*commands, *source, D3D12_RESOURCE_STATE_COPY_SOURCE) ||
        !transition(*commands, *image, D3D12_RESOURCE_STATE_COPY_DEST)) {
        return false;
    }
    commands->list->CopyResource(
        image->resource.Get(), source->resource.Get());
    if (!transition(*commands, *image, D3D12_RESOURCE_STATE_PRESENT)) {
        return false;
    }
    commands->pending_swapchain = swapchain;
    commands->pending_swapchain_index = index;
    return true;
}

core::Result<D3d12CreatedResource, RhiError> d3d12_create_texture(
    void* value, const TextureDesc& desc) noexcept {
    using Result = core::Result<D3d12CreatedResource, RhiError>;
    auto* state = as_device(value);
    const DXGI_FORMAT format = dxgi_format(desc.format);
    // A depth texture a shader also reads is stored typeless; the depth view
    // and the shader view each name their own interpretation below.
    const bool depth_and_sampled = desc.depth_target && desc.sampled;
    const DXGI_FORMAT storage_format =
        dxgi_resource_format(desc.format, depth_and_sampled);
    if (state == nullptr || desc.width == 0U || desc.height == 0U ||
        desc.array_layers == 0U || desc.mip_levels == 0U ||
        format == DXGI_FORMAT_UNKNOWN ||
        // A cube is six faces. Anything else is a caller that meant an array.
        (desc.cube && desc.array_layers != 6U)) {
        return Result::failure(RhiError::InvalidConfiguration);
    }
    auto* texture = new (std::nothrow) D3d12Texture{};
    if (texture == nullptr) {
        return Result::failure(RhiError::AllocationFailed);
    }
    texture->desc = desc;
    const auto release_descriptors = [state](D3d12Texture& candidate) noexcept {
        state->rtvs.release(candidate.rtv);
        state->dsvs.release(candidate.dsv);
        state->views.release(candidate.srv);
        state->views.release(candidate.uav);
    };
    D3D12_RESOURCE_DESC resource{};
    resource.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resource.Width = desc.width;
    resource.Height = desc.height;
    resource.DepthOrArraySize = static_cast<UINT16>(desc.array_layers);
    resource.MipLevels = static_cast<UINT16>(desc.mip_levels);
    resource.Format = storage_format;
    resource.SampleDesc.Count = 1U;
    resource.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (desc.color_target) {
        resource.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }
    if (desc.depth_target) {
        resource.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    }
    if (desc.storage_write) {
        resource.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    D3D12_CLEAR_VALUE clear{};
    clear.Format = format;
    clear.DepthStencil.Depth = 0.0F;
    const D3D12_CLEAR_VALUE* clear_ptr =
        (desc.color_target || desc.depth_target) ? &clear : nullptr;
    const auto heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(state->device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &resource,
            D3D12_RESOURCE_STATE_COMMON, clear_ptr,
            IID_PPV_ARGS(&texture->resource)))) {
        delete texture;
        return Result::failure(RhiError::AllocationFailed);
    }
    if (desc.color_target) {
        texture->rtv = state->rtvs.allocate();
        if (texture->rtv == invalid_descriptor) {
            release_descriptors(*texture);
            delete texture;
            return Result::failure(RhiError::AllocationFailed);
        }
        state->device->CreateRenderTargetView(
            texture->resource.Get(), nullptr, state->rtvs.cpu(texture->rtv));
    }
    if (desc.depth_target) {
        texture->dsv = state->dsvs.allocate();
        if (texture->dsv == invalid_descriptor) {
            release_descriptors(*texture);
            delete texture;
            return Result::failure(RhiError::AllocationFailed);
        }
        D3D12_DEPTH_STENCIL_VIEW_DESC depth_view{};
        depth_view.Format = format;
        depth_view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        state->device->CreateDepthStencilView(
            texture->resource.Get(),
            depth_and_sampled ? &depth_view : nullptr,
            state->dsvs.cpu(texture->dsv));
    }
    if (desc.sampled) {
        texture->srv = state->views.allocate();
        if (texture->srv == invalid_descriptor) {
            release_descriptors(*texture);
            delete texture;
            return Result::failure(RhiError::AllocationFailed);
        }
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = dxgi_shader_format(desc.format);
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (desc.cube) {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            view.TextureCube.MipLevels = desc.mip_levels;
        } else if (desc.array_layers > 1U) {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            view.Texture2DArray.MipLevels = desc.mip_levels;
            view.Texture2DArray.ArraySize = desc.array_layers;
        } else {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipLevels = desc.mip_levels;
        }
        state->device->CreateShaderResourceView(
            texture->resource.Get(), &view, state->views.cpu(texture->srv));
    }
    if (desc.storage_write) {
        texture->uav = state->views.allocate();
        if (texture->uav == invalid_descriptor) {
            release_descriptors(*texture);
            delete texture;
            return Result::failure(RhiError::AllocationFailed);
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = desc.array_layers > 1U
            ? D3D12_UAV_DIMENSION_TEXTURE2DARRAY
            : D3D12_UAV_DIMENSION_TEXTURE2D;
        if (desc.array_layers > 1U) {
            view.Texture2DArray.ArraySize = desc.array_layers;
        }
        state->device->CreateUnorderedAccessView(
            texture->resource.Get(), nullptr, &view,
            state->views.cpu(texture->uav));
    }
    return Result::success(D3d12CreatedResource{
        texture, texture->resource.Get()});
}

core::Result<D3d12CreatedResource, RhiError> d3d12_import_video_plane(
    void* value, void* resource, core::u32 width, core::u32 height,
    TextureFormat format, core::u32 plane) noexcept {
    using Result = core::Result<D3d12CreatedResource, RhiError>;
    auto* state = as_device(value);
    auto* native = static_cast<ID3D12Resource*>(resource);
    if (state == nullptr || native == nullptr || width == 0U || height == 0U) {
        return Result::failure(RhiError::InvalidConfiguration);
    }
    // The plane view has to name a real subresource of the decoder's texture,
    // and the picture has to fill it exactly.
    //
    // A shader samples a whole mip in normalized coordinates and no view can
    // crop one, so a surface the codec padded out to its macroblock grid -
    // 1920x1088 for 1080p H.264, say - would stretch the picture by whatever
    // the padding is. Refusing it here is what sends such a source back to the
    // path that copies the visible rows out itself.
    const D3D12_RESOURCE_DESC described = native->GetDesc();
    if (described.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        described.MipLevels != 1U || described.DepthOrArraySize != 1U ||
        plane > 1U ||
        static_cast<core::u64>(width) << plane != described.Width ||
        static_cast<core::u64>(height) << plane != described.Height) {
        return Result::failure(RhiError::InvalidConfiguration);
    }
    // The view has to name the plane's real format. D3D12 will create an
    // R16 view over an eight-bit plane without complaint and hand back
    // whatever those bytes happen to spell, so this is checked here rather
    // than left to the runtime: reading a plane at the wrong depth is not a
    // worse picture, it is a different one.
    const DXGI_FORMAT expected = described.Format == DXGI_FORMAT_NV12
        ? (plane == 0U ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8_UNORM)
        : (described.Format == DXGI_FORMAT_P010
               || described.Format == DXGI_FORMAT_P016)
            ? (plane == 0U ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R16G16_UNORM)
            : DXGI_FORMAT_UNKNOWN;
    if (expected == DXGI_FORMAT_UNKNOWN || dxgi_shader_format(format) != expected) {
        return Result::failure(RhiError::InvalidConfiguration);
    }
    auto* texture = new (std::nothrow) D3d12Texture{};
    if (texture == nullptr) {
        return Result::failure(RhiError::AllocationFailed);
    }
    texture->external = true;
    texture->desc.width = width;
    texture->desc.height = height;
    texture->desc.format = format;
    texture->desc.sampled = true;
    texture->resource = native;
    texture->srv = state->views.allocate();
    if (texture->srv == invalid_descriptor) {
        delete texture;
        return Result::failure(RhiError::AllocationFailed);
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = dxgi_shader_format(format);
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1U;
    view.Texture2D.PlaneSlice = plane;
    state->device->CreateShaderResourceView(
        texture->resource.Get(), &view, state->views.cpu(texture->srv));
    return Result::success(D3d12CreatedResource{
        texture, texture->resource.Get()});
}

bool d3d12_texture_safe_to_release(void* device_value, void* value) noexcept {
    const auto* state = as_device(device_value);
    const auto* texture = as_texture(value);
    return state != nullptr && texture != nullptr &&
        texture->recording_owner == nullptr &&
        (texture->last_submission == 0U ||
         (state->fence && state->fence->GetCompletedValue() >= texture->last_submission));
}

void d3d12_release_texture(void* device_value, void* value) noexcept {
    auto* state = as_device(device_value);
    auto* texture = as_texture(value);
    if (state != nullptr && texture != nullptr) {
        state->rtvs.release(texture->rtv);
        state->dsvs.release(texture->dsv);
        state->views.release(texture->srv);
        state->views.release(texture->uav);
    }
    delete texture;
}

core::Result<D3d12CreatedResource, RhiError> d3d12_create_storage_buffer(
    void* value, core::u32 bytes) noexcept {
    using Result = core::Result<D3d12CreatedResource, RhiError>;
    auto* state = as_device(value);
    if (state == nullptr || bytes == 0U) {
        return Result::failure(RhiError::InvalidConfiguration);
    }
    auto* buffer = new (std::nothrow) D3d12Buffer{};
    if (buffer == nullptr) {
        return Result::failure(RhiError::AllocationFailed);
    }
    buffer->bytes = bytes;
    const auto heap = heap_properties(D3D12_HEAP_TYPE_UPLOAD);
    const auto desc = buffer_desc(align_up(bytes, 16U));
    if (FAILED(state->device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&buffer->resource))) ||
        FAILED(buffer->resource->Map(
            0U, nullptr, reinterpret_cast<void**>(&buffer->mapped)))) {
        delete buffer;
        return Result::failure(RhiError::AllocationFailed);
    }
    return Result::success(D3d12CreatedResource{
        buffer, buffer->resource.Get()});
}

core::Result<D3d12CreatedResource, RhiError> d3d12_create_geometry_buffer(
    void* value, core::u32 bytes, bool writable) noexcept {
    using Result = core::Result<D3d12CreatedResource, RhiError>;
    auto* state = as_device(value);
    if (state == nullptr || bytes == 0U) {
        return Result::failure(RhiError::InvalidConfiguration);
    }
    auto* buffer = new (std::nothrow) D3d12Buffer{};
    if (buffer == nullptr) {
        return Result::failure(RhiError::AllocationFailed);
    }
    buffer->bytes = bytes;
    buffer->device_local = true;
    buffer->writable = writable;
    const auto heap = heap_properties(D3D12_HEAP_TYPE_DEFAULT);
    auto desc = buffer_desc(align_up(bytes, 16U));
    if (writable) {
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }
    if (FAILED(state->device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&buffer->resource)))) {
        delete buffer;
        return Result::failure(RhiError::AllocationFailed);
    }
    return Result::success(D3d12CreatedResource{
        buffer, buffer->resource.Get()});
}

bool d3d12_buffer_safe_to_release(void* device_value, void* value) noexcept {
    const auto* state = as_device(device_value);
    const auto* buffer = as_buffer(value);
    return state != nullptr && buffer != nullptr && buffer->recording_uses == 0U
        && (buffer->last_submission == 0U
            || (state->fence && state->fence->GetCompletedValue() >= buffer->last_submission));
}

void d3d12_release_storage_buffer(void*, void* value) noexcept {
    auto* buffer = as_buffer(value);
    if (buffer != nullptr && buffer->resource && buffer->mapped != nullptr) {
        buffer->resource->Unmap(0U, nullptr);
    }
    delete buffer;
}

core::Result<void*, RhiError> d3d12_create_sampler(
    void* value, const SamplerDesc& description) noexcept {
    using Result = core::Result<void*, RhiError>;
    auto* state = as_device(value);
    if (state == nullptr) {
        return Result::failure(RhiError::InvalidConfiguration);
    }
    for (auto* cached : state->sampler_cache) {
        if (cached != nullptr && cached->description == description) {
            ++cached->references;
            return Result::success(cached);
        }
    }
    auto* sampler = new (std::nothrow) D3d12Sampler{};
    if (sampler == nullptr) {
        return Result::failure(RhiError::AllocationFailed);
    }
    sampler->descriptor = state->samplers.allocate();
    if (sampler->descriptor == invalid_descriptor) {
        delete sampler;
        return Result::failure(RhiError::AllocationFailed);
    }
    D3D12_SAMPLER_DESC desc{};
    const auto reduction = description.comparison == SamplerCompare::Disabled
        ? D3D12_FILTER_REDUCTION_TYPE_STANDARD : D3D12_FILTER_REDUCTION_TYPE_COMPARISON;
    desc.Filter = description.max_anisotropy > 1U
        ? D3D12_ENCODE_ANISOTROPIC_FILTER(reduction)
        : D3D12_ENCODE_BASIC_FILTER(
            description.min_filter == Filter::Linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
            description.mag_filter == Filter::Linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
            description.mip_filter == Filter::Linear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT,
            reduction);
    const auto address = [](AddressMode mode) {
        switch (mode) {
        case AddressMode::Repeat: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        case AddressMode::Mirror: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        default: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        }
    };
    desc.AddressU = address(description.address_u);
    desc.AddressV = address(description.address_v);
    desc.AddressW = address(description.address_w);
    desc.MipLODBias = description.lod_bias;
    desc.MinLOD = description.min_lod;
    desc.MaxLOD = description.max_lod;
    desc.MaxAnisotropy = description.max_anisotropy;
    desc.ComparisonFunc = description.comparison == SamplerCompare::Disabled
        ? D3D12_COMPARISON_FUNC_ALWAYS : static_cast<D3D12_COMPARISON_FUNC>(description.comparison);
    sampler->description = description;
    state->sampler_cache[sampler->descriptor] = sampler;
    state->device->CreateSampler(&desc, state->samplers.cpu(sampler->descriptor));
    return Result::success(sampler);
}

void d3d12_release_sampler(void* device_value, void* value) noexcept {
    auto* state = as_device(device_value);
    auto* sampler = as_sampler(value);
    if (state != nullptr && sampler != nullptr) {
        if (--sampler->references != 0U) return;
        state->sampler_cache[sampler->descriptor] = nullptr;
        state->samplers.release(sampler->descriptor);
    }
    delete sampler;
}

core::Result<void*, RhiError> d3d12_create_readback(
    void* value, core::u32 bytes) noexcept {
    using Result = core::Result<void*, RhiError>;
    auto* state = as_device(value);
    if (state == nullptr || bytes == 0U) {
        return Result::failure(RhiError::InvalidConfiguration);
    }
    auto* readback = new (std::nothrow) D3d12Readback{};
    if (readback == nullptr) {
        return Result::failure(RhiError::AllocationFailed);
    }
    readback->packed.reset(new (std::nothrow) core::u8[bytes]);
    if (!readback->packed) {
        delete readback;
        return Result::failure(RhiError::AllocationFailed);
    }
    readback->bytes = bytes;
    readback->capacity = bytes;
    const auto heap = heap_properties(D3D12_HEAP_TYPE_READBACK);
    const auto desc = buffer_desc(bytes);
    if (FAILED(state->device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback->resource)))) {
        delete readback;
        return Result::failure(RhiError::AllocationFailed);
    }
    return Result::success(readback);
}

std::span<const core::u8> d3d12_map_readback(
    void* value, core::u32 bytes) noexcept {
    auto* readback = as_readback(value);
    if (readback == nullptr || bytes > readback->bytes) {
        return {};
    }
    const auto readable = static_cast<SIZE_T>(readback->capacity);
    D3D12_RANGE range{0U, readable};
    if (readback->gpu_mapped == nullptr && FAILED(readback->resource->Map(
            0U, &range, reinterpret_cast<void**>(&readback->gpu_mapped)))) {
        return {};
    }
    if (readback->rows != 0U && readback->row_bytes != 0U) {
        core::u64 packed_offset{};
        for (core::u32 row = 0U; row < readback->rows; ++row) {
            if (packed_offset + readback->row_bytes > bytes) {
                d3d12_unmap_readback(value);
                return {};
            }
            std::copy_n(
                readback->gpu_mapped + static_cast<std::size_t>(row)
                    * readback->row_pitch,
                static_cast<std::size_t>(readback->row_bytes),
                readback->packed.get() + packed_offset);
            packed_offset += readback->row_bytes;
        }
    } else {
        std::copy_n(readback->gpu_mapped, bytes, readback->packed.get());
    }
    return {readback->packed.get(), bytes};
}

void d3d12_unmap_readback(void* value) noexcept {
    auto* readback = as_readback(value);
    if (readback != nullptr && readback->resource
        && readback->gpu_mapped != nullptr) {
        D3D12_RANGE written{0U, 0U};
        readback->resource->Unmap(0U, &written);
        readback->gpu_mapped = nullptr;
    }
}

void d3d12_release_readback(void*, void* value) noexcept {
    d3d12_unmap_readback(value);
    delete as_readback(value);
}

core::Result<void*, RhiError> d3d12_create_pipeline(
    void* value,
    const ShaderDesc& vertex,
    const ShaderDesc& fragment,
    std::span<const TextureFormat> target_formats,
    ColorBlend blend,
    DepthState depth,
    RasterState raster) noexcept {
    using Result = core::Result<void*, RhiError>;
    auto* state = as_device(value);
    if (target_formats.empty() || target_formats.size() > max_color_targets
        || (target_formats.size() > 1U && blend != ColorBlend::Disabled)) {
        return Result::failure(RhiError::InvalidConfiguration);
    }
    if (state == nullptr || vertex.stage != ShaderStage::Vertex ||
        fragment.stage != ShaderStage::Fragment ||
        vertex.format != ShaderFormat::Dxil ||
        fragment.format != ShaderFormat::Dxil ||
        vertex.bytecode.empty() || fragment.bytecode.empty()) {
        return Result::failure(RhiError::ShaderFormatUnsupported);
    }
    const auto texture_count = fragment.sampled_texture_count == 0U
        ? fragment.sampler_count : fragment.sampled_texture_count;
    if (fragment.sampler_count > max_sampler_table_size
        || texture_count > max_sampled_texture_table_size
        || texture_count < fragment.sampler_count
        || (texture_count != 0U && fragment.sampler_count == 0U)) {
        return Result::failure(RhiError::InvalidConfiguration);
    }
    auto* pipeline = new (std::nothrow) D3d12Pipeline{};
    if (pipeline == nullptr) {
        return Result::failure(RhiError::AllocationFailed);
    }
    constexpr core::u32 max_parameters = 32U;
    std::array<D3D12_ROOT_PARAMETER1, max_parameters> parameters{};
    std::array<D3D12_DESCRIPTOR_RANGE1, 2U> ranges{};
    core::u32 parameter_count{};
    pipeline->layout.vertex_storage_count = vertex.storage_buffer_count;
    pipeline->layout.vertex_uniform_count = vertex.uniform_buffer_count;
    pipeline->layout.fragment_texture_count = texture_count;
    pipeline->layout.fragment_sampler_count = fragment.sampler_count;
    pipeline->layout.fragment_storage_count = fragment.storage_buffer_count;
    pipeline->layout.fragment_uniform_count = fragment.uniform_buffer_count;
    if (vertex.storage_buffer_count > 0U) {
        pipeline->layout.vertex_storage_base = parameter_count;
    }
    for (core::u32 index = 0U; index < vertex.storage_buffer_count; ++index) {
        auto& parameter = parameters[parameter_count++];
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameter.Descriptor.ShaderRegister = index;
        parameter.Descriptor.RegisterSpace = 0U;
        parameter.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    if (vertex.uniform_buffer_count > 0U) {
        pipeline->layout.vertex_uniform_base = parameter_count;
    }
    for (core::u32 index = 0U; index < vertex.uniform_buffer_count; ++index) {
        auto& parameter = parameters[parameter_count++];
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameter.Descriptor.ShaderRegister = index;
        parameter.Descriptor.RegisterSpace = 1U;
        parameter.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    if (fragment.sampler_count > 0U) {
        ranges[0U].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0U].NumDescriptors = texture_count;
        ranges[0U].BaseShaderRegister = 0U;
        ranges[0U].RegisterSpace = 2U;
        ranges[0U].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
        ranges[0U].OffsetInDescriptorsFromTableStart = 0U;
        pipeline->layout.fragment_texture_table = parameter_count;
        auto& textures = parameters[parameter_count++];
        textures.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        textures.DescriptorTable.NumDescriptorRanges = 1U;
        textures.DescriptorTable.pDescriptorRanges = &ranges[0U];
        textures.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        ranges[1U].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        ranges[1U].NumDescriptors = fragment.sampler_count;
        ranges[1U].BaseShaderRegister = 0U;
        ranges[1U].RegisterSpace = 2U;
        ranges[1U].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
        ranges[1U].OffsetInDescriptorsFromTableStart = 0U;
        pipeline->layout.fragment_sampler_table = parameter_count;
        auto& samplers = parameters[parameter_count++];
        samplers.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        samplers.DescriptorTable.NumDescriptorRanges = 1U;
        samplers.DescriptorTable.pDescriptorRanges = &ranges[1U];
        samplers.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    if (fragment.storage_buffer_count > 0U) {
        pipeline->layout.fragment_storage_base = parameter_count;
    }
    for (core::u32 index = 0U; index < fragment.storage_buffer_count; ++index) {
        auto& parameter = parameters[parameter_count++];
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameter.Descriptor.ShaderRegister = texture_count + index;
        parameter.Descriptor.RegisterSpace = 2U;
        parameter.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    if (fragment.uniform_buffer_count > 0U) {
        pipeline->layout.fragment_uniform_base = parameter_count;
    }
    for (core::u32 index = 0U; index < fragment.uniform_buffer_count; ++index) {
        auto& parameter = parameters[parameter_count++];
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameter.Descriptor.ShaderRegister = index;
        parameter.Descriptor.RegisterSpace = 3U;
        parameter.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    if (parameter_count > max_parameters) {
        delete pipeline;
        return Result::failure(RhiError::InvalidConfiguration);
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    root_desc.Desc_1_1.NumParameters = parameter_count;
    root_desc.Desc_1_1.pParameters = parameters.data();
    root_desc.Desc_1_1.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    if (FAILED(D3D12SerializeVersionedRootSignature(
            &root_desc, &signature, &error)) ||
        FAILED(state->device->CreateRootSignature(
            0U, signature->GetBufferPointer(), signature->GetBufferSize(),
            IID_PPV_ARGS(&pipeline->root_signature)))) {
        delete pipeline;
        return Result::failure(RhiError::BackendFailure);
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = pipeline->root_signature.Get();
    desc.VS = {vertex.bytecode.data(), vertex.bytecode.size()};
    desc.PS = {fragment.bytecode.data(), fragment.bytecode.size()};
    desc.BlendState = blend_desc(blend);
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = raster.fill == FillMode::Wireframe
        ? D3D12_FILL_MODE_WIREFRAME
        : D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.FrontCounterClockwise = TRUE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = depth.enabled;
    desc.DepthStencilState.DepthWriteMask = depth.write
        ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = depth.greater
        ? D3D12_COMPARISON_FUNC_GREATER_EQUAL
        : D3D12_COMPARISON_FUNC_LESS_EQUAL;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = static_cast<UINT>(target_formats.size());
    bool formats_known = true;
    for (std::size_t index = 0U; index < target_formats.size(); ++index) {
        desc.RTVFormats[index] = dxgi_format(target_formats[index]);
        formats_known = formats_known && desc.RTVFormats[index] != DXGI_FORMAT_UNKNOWN;
    }
    desc.DSVFormat = depth.enabled ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1U;
    if (!formats_known ||
        FAILED(state->device->CreateGraphicsPipelineState(
            &desc, IID_PPV_ARGS(&pipeline->pipeline)))) {
        delete pipeline;
        return Result::failure(RhiError::BackendFailure);
    }
    return Result::success(pipeline);
}

void d3d12_release_pipeline(void*, void* value) noexcept {
    delete as_pipeline(value);
}

core::Result<void*, RhiError> d3d12_create_compute_pipeline(
    void* value, const ShaderDesc& compute) noexcept {
    using Result = core::Result<void*, RhiError>;
    auto* state = as_device(value);
    if (state == nullptr || compute.stage != ShaderStage::Compute ||
        compute.format != ShaderFormat::Dxil || compute.bytecode.empty()) {
        return Result::failure(RhiError::ShaderFormatUnsupported);
    }
    if ((compute.sampled_texture_count != 0U && compute.sampled_texture_count != compute.sampler_count) ||
        compute.sampler_count > max_sampler_table_size ||
        compute.storage_texture_count > max_storage_texture_table_size) {
        return Result::failure(RhiError::InvalidConfiguration);
    }
    auto* pipeline = new (std::nothrow) D3d12ComputePipeline{};
    if (pipeline == nullptr) {
        return Result::failure(RhiError::AllocationFailed);
    }
    constexpr core::u32 max_parameters = 32U;
    std::array<D3D12_ROOT_PARAMETER1, max_parameters> parameters{};
    std::array<D3D12_DESCRIPTOR_RANGE1, 3U> ranges{};
    core::u32 parameter_count{};
    pipeline->layout.texture_count = compute.sampler_count;
    pipeline->layout.storage_texture_count = compute.storage_texture_count;
    pipeline->layout.storage_count = compute.storage_buffer_count;
    pipeline->layout.uniform_count = compute.uniform_buffer_count;

    // Register spaces are separated by kind rather than packed, matching the
    // graphics layout above. A shader author reading either root signature can
    // see which space a binding lives in without counting the ones before it.
    if (compute.sampler_count > 0U) {
        ranges[0U].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0U].NumDescriptors = compute.sampler_count;
        ranges[0U].BaseShaderRegister = 0U;
        ranges[0U].RegisterSpace = 0U;
        ranges[0U].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
        ranges[0U].OffsetInDescriptorsFromTableStart = 0U;
        pipeline->layout.texture_table = parameter_count;
        auto& textures = parameters[parameter_count++];
        textures.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        textures.DescriptorTable.NumDescriptorRanges = 1U;
        textures.DescriptorTable.pDescriptorRanges = &ranges[0U];
        textures.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        ranges[1U].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
        ranges[1U].NumDescriptors = compute.sampler_count;
        ranges[1U].BaseShaderRegister = 0U;
        ranges[1U].RegisterSpace = 0U;
        ranges[1U].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE;
        ranges[1U].OffsetInDescriptorsFromTableStart = 0U;
        pipeline->layout.sampler_table = parameter_count;
        auto& samplers = parameters[parameter_count++];
        samplers.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        samplers.DescriptorTable.NumDescriptorRanges = 1U;
        samplers.DescriptorTable.pDescriptorRanges = &ranges[1U];
        samplers.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    if (compute.storage_texture_count > 0U) {
        ranges[2U].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[2U].NumDescriptors = compute.storage_texture_count;
        ranges[2U].BaseShaderRegister = 0U;
        ranges[2U].RegisterSpace = 0U;
        // The descriptors are written into the recording heap immediately
        // before the dispatch that reads them, so neither the descriptor nor
        // the data behind it may be assumed static.
        ranges[2U].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE |
            D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE;
        ranges[2U].OffsetInDescriptorsFromTableStart = 0U;
        pipeline->layout.storage_texture_table = parameter_count;
        auto& storage = parameters[parameter_count++];
        storage.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        storage.DescriptorTable.NumDescriptorRanges = 1U;
        storage.DescriptorTable.pDescriptorRanges = &ranges[2U];
        storage.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    if (compute.storage_buffer_count > 0U) {
        pipeline->layout.storage_base = parameter_count;
    }
    for (core::u32 index = 0U; index < compute.storage_buffer_count; ++index) {
        auto& parameter = parameters[parameter_count++];
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        parameter.Descriptor.ShaderRegister = compute.sampler_count + index;
        parameter.Descriptor.RegisterSpace = 0U;
        parameter.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    if (compute.uniform_buffer_count > 0U) {
        pipeline->layout.uniform_base = parameter_count;
    }
    for (core::u32 index = 0U; index < compute.uniform_buffer_count; ++index) {
        auto& parameter = parameters[parameter_count++];
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameter.Descriptor.ShaderRegister = index;
        parameter.Descriptor.RegisterSpace = 1U;
        parameter.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    // Read-write buffers continue the UAV registers after the storage
    // textures. Root UAVs need no descriptor, so binding one costs nothing at
    // record time; the shader indexes it as a raw or structured buffer.
    pipeline->layout.writable_count = compute.writable_buffer_count;
    if (compute.writable_buffer_count > 0U) {
        pipeline->layout.writable_base = parameter_count;
    }
    for (core::u32 index = 0U;
         index < compute.writable_buffer_count && parameter_count < max_parameters; ++index) {
        auto& parameter = parameters[parameter_count++];
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameter.Descriptor.ShaderRegister = compute.storage_texture_count + index;
        parameter.Descriptor.RegisterSpace = 0U;
        parameter.Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    if (parameter_count > max_parameters ||
        compute.writable_buffer_count > max_parameters) {
        delete pipeline;
        return Result::failure(RhiError::InvalidConfiguration);
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC root_desc{};
    root_desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    root_desc.Desc_1_1.NumParameters = parameter_count;
    root_desc.Desc_1_1.pParameters = parameters.data();
    // Every graphics stage is denied: a compute root signature that admitted
    // them would reserve root space for stages that can never run against it.
    root_desc.Desc_1_1.Flags =
        D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
        D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;
    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    if (FAILED(D3D12SerializeVersionedRootSignature(
            &root_desc, &signature, &error)) ||
        FAILED(state->device->CreateRootSignature(
            0U, signature->GetBufferPointer(), signature->GetBufferSize(),
            IID_PPV_ARGS(&pipeline->root_signature)))) {
        delete pipeline;
        return Result::failure(RhiError::BackendFailure);
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = pipeline->root_signature.Get();
    desc.CS = {compute.bytecode.data(), compute.bytecode.size()};
    if (FAILED(state->device->CreateComputePipelineState(
            &desc, IID_PPV_ARGS(&pipeline->pipeline)))) {
        delete pipeline;
        return Result::failure(RhiError::BackendFailure);
    }
    return Result::success(pipeline);
}

void d3d12_release_compute_pipeline(void*, void* value) noexcept {
    delete as_compute_pipeline(value);
}

core::Result<D3d12CreatedResource, RhiError> d3d12_acquire_commands(
    void* value) noexcept {
    using Result = core::Result<D3d12CreatedResource, RhiError>;
    auto* state = as_device(value);
    if (state == nullptr) {
        return Result::failure(RhiError::InvalidConfiguration);
    }
    for (core::u32 index = 0U; index < state->context_count; ++index) {
        auto& context = state->contexts[index];
        if (context.quarantined || context.recording || context.ticket_owned ||
            (context.submitted &&
             state->fence->GetCompletedValue() < context.fence_value)) {
            continue;
        }
        context.submitted = false;
        ++context.recording_generation;
        if (context.recording_generation == 0U) {
            // Zero is the value-initialized "no recording" generation held by
            // every newly created buffer snapshot.
            context.recording_generation = 1U;
        }
        for (auto& page : context.upload_pages) {
            page.cursor = 0U;
        }
        context.pending_swapchain = nullptr;
        context.pending_swapchain_index = 0U;
        context.views.reset();
        context.samplers.reset();
        context.sampler_table_count = 0U;
        if (FAILED(context.allocator->Reset()) ||
            FAILED(context.list->Reset(context.allocator.Get(), nullptr))) {
            return Result::failure(RhiError::BackendFailure);
        }
        context.recording = true;
        context.timed_passes = 0U;
        context.query_overflows = context.upload_bytes = context.present_ns = 0U;
        context.list->EndQuery(
            state->timestamp_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
            context.query_index * queries_per_context);
        return Result::success(D3d12CreatedResource{
            &context, context.list.Get()});
    }
    // Never wait for a context on the frame path. The caller can hold or drop
    // according to show policy and try again at the next boundary.
    return Result::failure(RhiError::Unavailable);
}

void d3d12_release_commands(void*, void* value) noexcept {
    auto* context = as_commands(value);
    if (context == nullptr || !context->recording) {
        return;
    }
    static_cast<void>(context->list->Close());
    finish_texture_states(*context, false);
    finish_buffer_uses(*context, false);
    context->recording = false;
    context->pending_swapchain = nullptr;
    context->pending_swapchain_index = 0U;
}

core::u64 d3d12_upload_space(void* command_value) noexcept {
    const auto* commands = as_commands(command_value);
    if (commands == nullptr || !commands->recording) {
        return 0U;
    }
    core::u64 space{};
    for (const auto& page : commands->upload_pages) {
        const core::u64 offset = align_up(page.cursor, 4U);
        if (offset < upload_page_capacity) {
            space += upload_page_capacity - offset;
        }
    }
    return space;
}

core::u32 d3d12_texture_upload_row_capacity(
    void* command_value, void* texture_value, core::u64 reserve_bytes) noexcept {
    const auto* commands = as_commands(command_value);
    const auto* texture = as_texture(texture_value);
    if (commands == nullptr || texture == nullptr || !commands->recording)
        return 0U;
    const core::u32 block = block_compressed(texture->desc.format) ? 4U : 1U;
    const core::u32 pixel_bytes = block == 4U ? 16U : bytes_per_pixel(texture->desc.format);
    const core::u64 row_pitch = align_up(
        static_cast<core::u64>((texture->desc.width + block - 1U) / block) * pixel_bytes,
        D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    if (pixel_bytes == 0U || row_pitch == 0U) return 0U;
    const core::u64 available = d3d12_upload_space(command_value);
    if (available <= reserve_bytes) return 0U;
    core::u64 budget = available - reserve_bytes;
    core::u64 rows{};
    for (const auto& page : commands->upload_pages) {
        const core::u64 offset = align_up(page.cursor, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (offset >= upload_page_capacity) continue;
        const core::u64 count = std::min(upload_page_capacity - offset, budget) / row_pitch;
        rows += count;
        budget -= count * row_pitch;
    }
    return static_cast<core::u32>(std::min<core::u64>(texture->desc.height, rows * block));
}

bool d3d12_upload_buffer_range(
    void* command_value,
    void* buffer_value,
    core::u32 offset,
    std::span<const core::u8> data) noexcept {
    auto* commands = as_commands(command_value);
    auto* buffer = as_buffer(buffer_value);
    if (commands == nullptr || buffer == nullptr || !commands->recording
        || !buffer->device_local
        || static_cast<core::u64>(offset) + data.size() > buffer->bytes) {
        return false;
    }
    if (data.empty()) {
        return true;
    }
    // All or nothing: a refusal never leaves a partly copied range.
    if (d3d12_upload_space(command_value) < data.size()
        || !transition_buffer(*commands, *buffer, D3D12_RESOURCE_STATE_COPY_DEST)) {
        return false;
    }
    for (core::u64 copied = 0U; copied < data.size();) {
        const UploadAllocation allocation = allocate_upload(
            *commands, data.size() - copied, 1U, 4U);
        if (allocation.resource == nullptr) {
            return false;
        }
        std::memcpy(allocation.mapped + allocation.offset,
                    data.data() + copied,
                    static_cast<std::size_t>(allocation.bytes));
        commands->list->CopyBufferRegion(
            buffer->resource.Get(), offset + copied,
            allocation.resource, allocation.offset, allocation.bytes);
        copied += allocation.bytes;
    }
    return true;
}

bool d3d12_copy_buffer(
    void* command_value,
    void* source_value,
    void* destination_value,
    core::u32 bytes) noexcept {
    auto* commands = as_commands(command_value);
    auto* source = as_buffer(source_value);
    auto* destination = as_buffer(destination_value);
    if (commands == nullptr || source == nullptr || destination == nullptr
        || source == destination || !commands->recording
        || !source->device_local || !destination->device_local
        || bytes > source->bytes || bytes > destination->bytes) {
        return false;
    }
    if (bytes == 0U) {
        return true;
    }
    if (!transition_buffer(*commands, *source, D3D12_RESOURCE_STATE_COPY_SOURCE)
        || !transition_buffer(*commands, *destination, D3D12_RESOURCE_STATE_COPY_DEST)) {
        return false;
    }
    commands->list->CopyBufferRegion(
        destination->resource.Get(), 0U, source->resource.Get(), 0U, bytes);
    return true;
}

bool d3d12_upload_buffer(
    void* command_value,
    void* buffer_value,
    std::span<const core::u8> data) noexcept {
    auto* commands = as_commands(command_value);
    auto* buffer = as_buffer(buffer_value);
    if (commands == nullptr || buffer == nullptr || !commands->recording ||
        buffer->device_local || data.size() > buffer->bytes) {
        return false;
    }
    if (data.empty()) {
        return true;
    }

    auto* const snapshot = snapshot_for(*buffer, *commands);
    if (snapshot == nullptr) {
        return false;
    }
    const auto committed_address = buffer->resource->GetGPUVirtualAddress();
    if (!committed_buffer_writable(*buffer)) {
        // An earlier recorded or in-flight draw captured the persistent
        // resource. Updating it would retroactively change that draw, and
        // skipping the update would violate StorageBuffer's persistent
        // contents for a later recording. Refuse without waiting so the owner
        // can retain its prior frame and discard this command list.
        return false;
    }

    // The fixed command pages make ordinary per-frame storage immutable for
    // the lifetime of this submission. In particular, two mapped views may
    // upload the same logical instance/effect buffers and record both draws in
    // one command list without the second upload changing the first draw.
    const auto address = upload_bytes(*commands, data, 16U);
    if (address != 0U) {
        std::memcpy(buffer->mapped, data.data(), data.size());
        snapshot->generation = commands->recording_generation;
        snapshot->address = address;
        snapshot->bound = false;
        return true;
    }

    // A single storage payload larger than one page cannot be represented by
    // one root SRV into the page set. Preserve support for large, infrequently
    // changed arenas through their committed upload resource, but never
    // overwrite that resource after this command list has bound it. The
    // caller gets a bounded failure and can discard the recording rather than
    // submitting a nondeterministic frame.
    if (data.size() > upload_page_capacity) {
        std::memcpy(buffer->mapped, data.data(), data.size());
        snapshot->generation = commands->recording_generation;
        snapshot->address = committed_address;
        snapshot->bound = false;
        return true;
    }
    return false;
}

bool d3d12_upload_texture(
    void* command_value, void* texture_value,
    std::span<const core::u8> data) noexcept {
    const auto* texture = as_texture(texture_value);
    return texture != nullptr && texture->desc.mip_levels == 1U &&
        texture->desc.array_layers == 1U &&
        d3d12_upload_texture_rows(command_value, texture_value, 0U, 0U,
                                 0U, texture->desc.height, data);
}

bool d3d12_upload_texture_rows(
    void* command_value,
    void* texture_value, core::u32 mip, core::u32 layer,
    core::u32 first_row, core::u32 row_count,
    std::span<const core::u8> data) noexcept {
    auto* commands = as_commands(command_value);
    auto* texture = as_texture(texture_value);
    if (commands == nullptr || texture == nullptr || !commands->recording ||
        mip >= texture->desc.mip_levels || mip >= 32U ||
        layer >= texture->desc.array_layers) {
        return false;
    }
    const core::u32 width = std::max(1U, texture->desc.width >> mip);
    const core::u32 height = std::max(1U, texture->desc.height >> mip);
    if (row_count == 0U || first_row >= height || row_count > height - first_row)
        return false;
    const core::u32 subresource = mip + layer * texture->desc.mip_levels;
    const core::u32 block = block_compressed(texture->desc.format) ? 4U : 1U;
    if (first_row % block != 0U || (row_count % block != 0U && first_row + row_count != height)) return false;
    const core::u32 pixel_bytes = block == 4U ? 16U : bytes_per_pixel(texture->desc.format);
    const core::u64 source_row =
        static_cast<core::u64>((width + block - 1U) / block) * pixel_bytes;
    const core::u64 source_bytes =
        source_row * static_cast<core::u64>((row_count + block - 1U) / block);
    if (pixel_bytes == 0U || data.size() != source_bytes) {
        return false;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows{};
    UINT64 row_bytes{};
    UINT64 total_bytes{};
    ComPtr<ID3D12Device> owner;
    if (FAILED(texture->resource->GetDevice(IID_PPV_ARGS(&owner)))) {
        return false;
    }
    const D3D12_RESOURCE_DESC resource = texture->resource->GetDesc();
    owner->GetCopyableFootprints(
        &resource, subresource, 1U, 0U, &footprint, &rows, &row_bytes, &total_bytes);
    const core::u64 row_pitch = footprint.Footprint.RowPitch;
    rows = (row_count + block - 1U) / block;
    if (rows > height || row_bytes < source_row ||
        row_pitch < source_row || row_pitch > upload_page_capacity ||
        row_pitch * rows > command_upload_capacity) {
        return false;
    }

    struct TextureUploadChunk final {
        ID3D12Resource* resource{};
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        core::u32 destination_row{};
    };
    std::array<TextureUploadChunk, upload_page_count> chunks{};
    std::array<core::u64, upload_page_count> saved_cursors{};
    for (core::u32 index = 0U; index < upload_page_count; ++index) {
        saved_cursors[index] = commands->upload_pages[index].cursor;
    }

    core::u32 copied_rows{};
    core::u32 chunk_count{};
    while (copied_rows < rows && chunk_count < upload_page_count) {
        const core::u64 remaining_rows = rows - copied_rows;
        const core::u64 requested = remaining_rows * row_pitch;
        const UploadAllocation allocation = allocate_upload(
            *commands, requested, row_pitch,
            D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (allocation.resource == nullptr) {
            break;
        }
        const auto chunk_rows = static_cast<core::u32>(
            allocation.bytes / row_pitch);
        if (chunk_rows == 0U) {
            break;
        }
        for (core::u32 row = 0U; row < chunk_rows; ++row) {
            std::memcpy(
                allocation.mapped + allocation.offset +
                    static_cast<core::u64>(row) * row_pitch,
                data.data() +
                    static_cast<core::u64>(copied_rows + row) * source_row,
                static_cast<std::size_t>(source_row));
        }
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT chunk_footprint = footprint;
        chunk_footprint.Offset = allocation.offset;
        chunk_footprint.Footprint.Height = std::min(chunk_rows * block, row_count - copied_rows * block);
        chunks[chunk_count] = TextureUploadChunk{
            allocation.resource, chunk_footprint, copied_rows};
        ++chunk_count;
        copied_rows += chunk_rows;
    }
    if (copied_rows != rows) {
        for (core::u32 index = 0U; index < upload_page_count; ++index) {
            commands->upload_pages[index].cursor = saved_cursors[index];
        }
        return false;
    }

    if (!transition(*commands, *texture, D3D12_RESOURCE_STATE_COPY_DEST)) {
        for (core::u32 index = 0U; index < upload_page_count; ++index) {
            commands->upload_pages[index].cursor = saved_cursors[index];
        }
        return false;
    }
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = texture->resource.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = subresource;
    for (core::u32 index = 0U; index < chunk_count; ++index) {
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = chunks[index].resource;
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = chunks[index].footprint;
        commands->list->CopyTextureRegion(
            &destination, 0U, first_row + chunks[index].destination_row * block, 0U,
            &source, nullptr);
    }
    return transition(
        *commands, *texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

bool d3d12_draw_instanced_quads(
    void* command_value,
    void* target_value,
    Color clear,
    void* pipeline_value,
    void* instances_value,
    core::u32 instance_count,
    void* atlas_value,
    void* sampler_value) noexcept {
    auto* commands = as_commands(command_value);
    auto* target = as_texture(target_value);
    auto* pipeline = as_pipeline(pipeline_value);
    auto* instances = as_buffer(instances_value);
    if (commands == nullptr || target == nullptr || pipeline == nullptr ||
        instances == nullptr || !commands->recording || target->rtv == invalid_descriptor ||
        pipeline->layout.vertex_storage_count != 1U) {
        return false;
    }
    auto* state = commands->owner;
    if (state == nullptr) {
        return false;
    }
    auto* atlas = as_texture(atlas_value);
    auto* sampler = as_sampler(sampler_value);
    if (atlas == nullptr || sampler == nullptr ||
        atlas->srv == invalid_descriptor ||
        sampler->descriptor == invalid_descriptor ||
        pipeline->layout.fragment_texture_count != 1U) {
        return false;
    }
    if (!transition(*commands, *target, D3D12_RESOURCE_STATE_RENDER_TARGET) ||
        !transition(
            *commands, *atlas,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) {
        return false;
    }
    const auto rtv = state->rtvs.cpu(target->rtv);
    commands->list->OMSetRenderTargets(1U, &rtv, FALSE, nullptr);
    const float clear_value[]{clear.r, clear.g, clear.b, clear.a};
    commands->list->ClearRenderTargetView(rtv, clear_value, 0U, nullptr);
    set_viewport(*commands, *target);
    set_pipeline(*commands, *pipeline);
    commands->list->SetGraphicsRootShaderResourceView(
        pipeline->layout.vertex_storage_base,
        instances->resource->GetGPUVirtualAddress());
    const core::u32 view_slot = commands->views.allocate();
    const core::u32 sampler_slot = commands->samplers.allocate();
    if (view_slot == invalid_descriptor || sampler_slot == invalid_descriptor) {
        return false;
    }
    state->device->CopyDescriptorsSimple(
        1U, commands->views.cpu(view_slot), state->views.cpu(atlas->srv),
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    state->device->CopyDescriptorsSimple(
        1U, commands->samplers.cpu(sampler_slot),
        state->samplers.cpu(sampler->descriptor),
        D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    commands->list->SetGraphicsRootDescriptorTable(
        pipeline->layout.fragment_texture_table,
        commands->views.gpu(view_slot));
    commands->list->SetGraphicsRootDescriptorTable(
        pipeline->layout.fragment_sampler_table,
        commands->samplers.gpu(sampler_slot));
    commands->list->DrawInstanced(6U, instance_count, 0U, 0U);
    return true;
}

// The command signature for one kind of indirect argument, made the first
// time it is needed. Null if the device refuses it.
[[nodiscard]] ID3D12CommandSignature* indirect_signature(
    D3d12Device& state, D3D12_INDIRECT_ARGUMENT_TYPE type) noexcept {
    ComPtr<ID3D12CommandSignature>* slot = &state.dispatch_signature;
    UINT stride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    if (type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW) {
        slot = &state.draw_signature;
        stride = sizeof(D3D12_DRAW_ARGUMENTS);
    } else if (type == D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED) {
        slot = &state.draw_indexed_signature;
        stride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
    }
    if (!*slot) {
        D3D12_INDIRECT_ARGUMENT_DESC argument{};
        argument.Type = type;
        D3D12_COMMAND_SIGNATURE_DESC desc{};
        desc.ByteStride = stride;
        desc.NumArgumentDescs = 1U;
        desc.pArgumentDescs = &argument;
        if (FAILED(state.device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(slot->GetAddressOf())))) {
            return nullptr;
        }
    }
    return slot->Get();
}

// The device-local buffer behind indirect arguments, if they fit in it.
[[nodiscard]] D3d12Buffer* indirect_buffer(
    const IndirectArguments& arguments, core::u32 argument_bytes) noexcept {
    if (arguments.buffer == nullptr || !*arguments.buffer) {
        return nullptr;
    }
    auto* const buffer = as_buffer(arguments.buffer->backend_handle());
    if (buffer == nullptr || !buffer->device_local || arguments.offset % 4U != 0U ||
        arguments.offset > buffer->bytes || buffer->bytes - arguments.offset < argument_bytes) {
        return nullptr;
    }
    return buffer;
}

// Puts a writable buffer into UAV access for a dispatch. Already there means an
// earlier dispatch in this recording had it, so a UAV barrier orders that
// dispatch's writes before this one's accesses.
[[nodiscard]] bool bind_writable_buffer(CommandContext& commands, D3d12Buffer& buffer) noexcept {
    auto* const entry = buffer_state(commands, buffer);
    if (entry == nullptr) {
        return false;
    }
    if (entry->state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = buffer.resource.Get();
        commands.list->ResourceBarrier(1U, &barrier);
        return true;
    }
    return transition_buffer(commands, buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

bool d3d12_draw_pulled(
    void* command_value,
    void* color_value,
    Color clear,
    void* depth_value,
    float depth_clear,
    const PulledDraw& draw) noexcept {
    auto* commands = as_commands(command_value);
    auto* color = as_texture(color_value);
    auto* depth = as_texture(depth_value);
    auto* pipeline = draw.pipeline == nullptr
        ? nullptr : as_pipeline(draw.pipeline->backend_handle());
    if (commands == nullptr || color == nullptr || depth == nullptr ||
        pipeline == nullptr || !commands->recording || commands->owner == nullptr ||
        color->rtv == invalid_descriptor || depth->dsv == invalid_descriptor ||
        draw.batches.size() > std::numeric_limits<core::u32>::max() ||
        (!draw.viewport.fits(color->desc.width, color->desc.height) || !draw.raster_viewport.valid())) {
        return false;
    }
    auto* state = commands->owner;
    // Indirect counts replace the one batch's.
    D3d12Buffer* draw_arguments = nullptr;
    ID3D12CommandSignature* draw_signature = nullptr;
    if (draw.indirect.buffer != nullptr) {
        const bool indexed = draw.index_buffer != nullptr;
        draw_arguments = draw.batches.size() != 1U ? nullptr
            : indirect_buffer(draw.indirect, indexed ? sizeof(D3D12_DRAW_INDEXED_ARGUMENTS)
                                                     : sizeof(D3D12_DRAW_ARGUMENTS));
        draw_signature = draw_arguments == nullptr ? nullptr
            : indirect_signature(*state, indexed ? D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED
                                                 : D3D12_INDIRECT_ARGUMENT_TYPE_DRAW);
        if (draw_signature == nullptr) {
            return false;
        }
    }
    if (draw.additional_targets.size() + 1U > max_color_targets) {
        return false;
    }
    std::array<D3d12Texture*, max_color_targets> targets{color};
    for (std::size_t index = 0U; index < draw.additional_targets.size(); ++index) {
        auto* const target = draw.additional_targets[index] == nullptr
            ? nullptr : as_texture(draw.additional_targets[index]->backend_handle());
        if (target == nullptr || target->rtv == invalid_descriptor
            || target->desc.width != color->desc.width
            || target->desc.height != color->desc.height) {
            return false;
        }
        targets[index + 1U] = target;
    }
    const auto target_count = static_cast<UINT>(draw.additional_targets.size() + 1U);
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, max_color_targets> rtvs{};
    for (UINT index = 0U; index < target_count; ++index) {
        if (!transition(*commands, *targets[index], D3D12_RESOURCE_STATE_RENDER_TARGET)) {
            return false;
        }
        rtvs[index] = state->rtvs.cpu(targets[index]->rtv);
    }
    if (!transition(*commands, *depth, D3D12_RESOURCE_STATE_DEPTH_WRITE)) {
        return false;
    }
    const auto dsv = state->dsvs.cpu(depth->dsv);
    commands->list->OMSetRenderTargets(target_count, rtvs.data(), FALSE, &dsv);
    if (!draw.load) {
        const float clear_value[]{clear.r, clear.g, clear.b, clear.a};
        for (UINT index = 0U; index < target_count; ++index) {
            commands->list->ClearRenderTargetView(rtvs[index], clear_value, 0U, nullptr);
        }
        commands->list->ClearDepthStencilView(
            dsv, D3D12_CLEAR_FLAG_DEPTH, depth_clear, 0U, 0U, nullptr);
    }
    set_viewport(*commands, *color, draw.viewport, draw.raster_viewport);
    set_pipeline(*commands, *pipeline);
    if (!bind_vertex_buffers(*commands, *pipeline, draw.vertex_buffers) ||
        !bind_fragment_buffers(*commands, *pipeline, draw.fragment_buffers)) {
        return false;
    }
    if (pipeline->layout.vertex_uniform_count > 0U &&
        !bind_uniform(
            *commands, pipeline->layout.vertex_uniform_base,
            draw.vertex_uniform)) {
        return false;
    }
    if (pipeline->layout.fragment_uniform_count > 0U &&
        !bind_uniform(
            *commands, pipeline->layout.fragment_uniform_base,
            draw.fragment_uniform)) {
        return false;
    }

    std::array<SampledTexture, 1U> legacy_sources{};
    std::span<const SampledTexture> global_sources = draw.fragment_textures;
    if (draw.fragment_textures.empty() && draw.texture != nullptr &&
        draw.sampler != nullptr) {
        legacy_sources[0U] = SampledTexture{draw.texture, draw.sampler};
        global_sources = legacy_sources;
    }
    if (draw.fragment_textures_per_batch == 0U &&
        !copy_texture_views(*state, *commands, global_sources, *pipeline)) {
        return false;
    }
    if (draw.fragment_textures_per_batch > 0U &&
        draw.fragment_textures.size() !=
            draw.batches.size() * draw.fragment_textures_per_batch) {
        return false;
    }
    if (pipeline->layout.vertex_uniform_count > 1U &&
        (draw.batch_uniform_stride == 0U ||
         draw.batch_uniforms.size() !=
            draw.batches.size() * draw.batch_uniform_stride)) {
        return false;
    }
    if (draw.index_buffer != nullptr) {
        // Only a device-local (ranged) buffer has one address for its whole
        // capacity; an uploaded buffer's snapshot may be shorter.
        auto* indices = as_buffer(draw.index_buffer->backend_handle());
        if (indices == nullptr || !indices->device_local) {
            return false;
        }
        const core::u64 index_capacity = indices->bytes / sizeof(core::u32);
        for (const DrawBatch& batch : draw.batches) {
            if (static_cast<core::u64>(batch.first_index) + batch.vertex_count
                > index_capacity) {
                return false;
            }
        }
        const auto address = buffer_address(*commands, *indices);
        if (address == 0U) {
            return false;
        }
        D3D12_INDEX_BUFFER_VIEW view{};
        view.BufferLocation = address;
        view.SizeInBytes = indices->bytes;
        view.Format = DXGI_FORMAT_R32_UINT;
        commands->list->IASetIndexBuffer(&view);
    }
    for (core::u32 index = 0U; index < draw.batches.size(); ++index) {
        if (draw.fragment_textures_per_batch > 0U) {
            const auto batch_sources = draw.fragment_textures.subspan(
                static_cast<std::size_t>(index) *
                    draw.fragment_textures_per_batch,
                draw.fragment_textures_per_batch);
            if (!copy_texture_views(
                    *state, *commands, batch_sources, *pipeline)) {
                return false;
            }
        }
        if (pipeline->layout.vertex_uniform_count > 1U) {
            const auto batch_uniform = draw.batch_uniforms.subspan(
                static_cast<std::size_t>(index) * draw.batch_uniform_stride,
                draw.batch_uniform_stride);
            if (!bind_uniform(
                    *commands, pipeline->layout.vertex_uniform_base + 1U,
                    batch_uniform)) {
                return false;
            }
        }
        if (draw_arguments != nullptr) {
            if (!transition_buffer(*commands, *draw_arguments, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)) {
                return false;
            }
            commands->list->ExecuteIndirect(
                draw_signature, 1U, draw_arguments->resource.Get(), draw.indirect.offset, nullptr, 0U);
        } else if (draw.index_buffer != nullptr) {
            commands->list->DrawIndexedInstanced(
                draw.batches[index].vertex_count,
                draw.batches[index].instance_count,
                draw.batches[index].first_index, 0, 0U);
        } else {
            commands->list->DrawInstanced(
                draw.batches[index].vertex_count,
                draw.batches[index].instance_count, 0U, 0U);
        }
    }
    return true;
}

bool d3d12_draw_output_transform(
    void* command_value,
    void* target_value,
    Color clear,
    void* pipeline_value,
    std::span<const SampledTexture> sources,
    std::span<const core::u8> fragment_uniform,
    bool load) noexcept {
    auto* commands = as_commands(command_value);
    auto* target = as_texture(target_value);
    auto* pipeline = as_pipeline(pipeline_value);
    if (commands == nullptr || target == nullptr || pipeline == nullptr ||
        !commands->recording || commands->owner == nullptr ||
        target->rtv == invalid_descriptor) {
        return false;
    }
    auto* state = commands->owner;
    if (!transition(*commands, *target, D3D12_RESOURCE_STATE_RENDER_TARGET)) {
        return false;
    }
    const auto rtv = state->rtvs.cpu(target->rtv);
    commands->list->OMSetRenderTargets(1U, &rtv, FALSE, nullptr);
    if (!load) {
        const float clear_value[]{clear.r, clear.g, clear.b, clear.a};
        commands->list->ClearRenderTargetView(rtv, clear_value, 0U, nullptr);
    }
    set_viewport(*commands, *target);
    set_pipeline(*commands, *pipeline);
    if (!copy_texture_views(*state, *commands, sources, *pipeline)) {
        return false;
    }
    if (pipeline->layout.fragment_uniform_count > 0U &&
        !bind_uniform(
            *commands, pipeline->layout.fragment_uniform_base,
            fragment_uniform)) {
        return false;
    }
    commands->list->DrawInstanced(3U, 1U, 0U, 0U);
    return true;
}

// A UAV barrier for one resource, ordering a prior dispatch's accesses before
// this one's.
//
// This is a distinct barrier type from a transition and the state journal does
// not model it, deliberately: a UAV barrier changes no state, so there is
// nothing for a cancelled list to restore. It is pure ordering.
void unordered_access_barrier(
    CommandContext& commands, D3d12Texture& texture) noexcept {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = texture.resource.Get();
    commands.list->ResourceBarrier(1U, &barrier);
}

// Writes one subresource UAV straight into the recording heap.
//
// Created here rather than taken from the texture's own descriptor because the
// binding names a mip and a layer: the resource-wide UAV made at creation
// covers the whole chain and cannot address one face of one level, which is
// exactly what prefiltering an environment map needs. Creating per dispatch
// costs one descriptor write against a heap that is already per-recording.
// Whether this binding can legally address this texture at all.
//
// Asked before anything is recorded, and that ordering is the point. A
// transition to UNORDERED_ACCESS on a resource created without
// ALLOW_UNORDERED_ACCESS is a validation error the debug layer treats as fatal,
// so a refusal that came after the barrier had been written would take the
// whole command list down with it - the refusal has to happen while the list is
// still clean.
//
// `write` is checked against the texture's own declaration rather than used to
// pick a view: a D3D12 UAV is read-write either way. Its job is to catch a
// caller binding a read-only texture where the shader scatters, which the API
// would otherwise accept and the GPU would silently drop.
[[nodiscard]] bool storage_binding_valid(
    const StorageTexture& binding, const D3d12Texture& texture) noexcept {
    return binding.mip_level < texture.desc.mip_levels &&
        binding.array_layer < texture.desc.array_layers &&
        (!binding.write || texture.desc.storage_write) &&
        (texture.desc.storage_read || texture.desc.storage_write);
}

void write_storage_view(
    D3d12Device& device,
    CommandContext& commands,
    const StorageTexture& binding,
    D3d12Texture& texture,
    core::u32 slot) noexcept {
    D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
    view.Format = dxgi_format(texture.desc.format);
    if (texture.desc.array_layers > 1U) {
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.MipSlice = binding.mip_level;
        view.Texture2DArray.FirstArraySlice = binding.array_layer;
        view.Texture2DArray.ArraySize = 1U;
    } else {
        view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipSlice = binding.mip_level;
    }
    device.device->CreateUnorderedAccessView(
        texture.resource.Get(), nullptr, &view, commands.views.cpu(slot));
}

[[nodiscard]] bool bind_compute_storage_textures(
    D3d12Device& device,
    CommandContext& commands,
    std::span<const StorageTexture> textures,
    D3d12ComputePipeline& pipeline) noexcept {
    if (textures.size() != pipeline.layout.storage_texture_count ||
        textures.size() > max_storage_texture_table_size) {
        return false;
    }
    if (textures.empty()) {
        return true;
    }
    // The whole span is validated first, so a refusal on the last binding
    // leaves no transition recorded for the ones before it. Half a dispatch's
    // barriers with no dispatch is a state journal that has to unwind on a
    // list that was never cancelled.
    std::array<D3d12Texture*, max_storage_texture_table_size> resolved{};
    for (core::u32 index = 0U; index < textures.size(); ++index) {
        if (textures[index].texture == nullptr) {
            return false;
        }
        auto* texture = as_texture(textures[index].texture->backend_handle());
        if (texture == nullptr ||
            !storage_binding_valid(textures[index], *texture)) {
            return false;
        }
        resolved[index] = texture;
    }
    const core::u32 first =
        commands.views.allocate(static_cast<core::u32>(textures.size()));
    if (first == invalid_descriptor) {
        return false;
    }
    for (core::u32 index = 0U; index < textures.size(); ++index) {
        D3d12Texture* texture = resolved[index];
        // Owned by this recording and already in UNORDERED_ACCESS means an
        // earlier dispatch here bound it, so this one must be ordered after
        // that one. The transition below cannot do it: a transition into the
        // state a resource already holds emits nothing.
        //
        // Read before the transition, which is what claims ownership.
        //
        // Emitted for any prior use, not only a prior write. Which of the two
        // it was is not tracked, and only read-after-read would not need the
        // barrier - the cheapest of the four orderings, and the only one this
        // costs anything on. Tracking it would be more state to keep correct
        // than the redundant barrier is worth.
        const bool prior_use_here =
            texture->recording_owner == &commands &&
            texture->state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        if (!transition(
                commands, *texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)) {
            return false;
        }
        if (prior_use_here) {
            unordered_access_barrier(commands, *texture);
        }
        write_storage_view(
            device, commands, textures[index], *texture, first + index);
    }
    commands.list->SetComputeRootDescriptorTable(
        pipeline.layout.storage_texture_table, commands.views.gpu(first));
    return true;
}

// The sampled half of a dispatch's texture bindings.
//
// Deliberately not sharing `copy_texture_views` with the graphics path: that
// helper transitions to PIXEL_SHADER_RESOURCE and sets a *graphics* root
// descriptor table, and both are wrong here. Sharing it would have meant two
// branches inside it on a caller-supplied flag, which is the same code with a
// worse name.
[[nodiscard]] bool bind_compute_textures(
    D3d12Device& device,
    CommandContext& commands,
    std::span<const SampledTexture> sources,
    D3d12ComputePipeline& pipeline) noexcept {
    if (sources.size() != pipeline.layout.texture_count ||
        sources.size() > max_sampler_table_size) {
        return false;
    }
    if (sources.empty()) {
        return true;
    }
    // Resolved and checked before anything is recorded, for the same reason
    // the storage bindings are: a refused dispatch must leave the list exactly
    // as it found it, and a refusal on the last binding would otherwise have
    // already transitioned the ones before it.
    std::array<D3d12Texture*, max_sampler_table_size> resolved_textures{};
    std::array<D3d12Sampler*, max_sampler_table_size> resolved_samplers{};
    for (core::u32 index = 0U; index < sources.size(); ++index) {
        if (sources[index].texture == nullptr ||
            sources[index].sampler == nullptr) {
            return false;
        }
        auto* texture = as_texture(sources[index].texture->backend_handle());
        auto* sampler = as_sampler(sources[index].sampler->backend_handle());
        if (texture == nullptr || sampler == nullptr ||
            texture->srv == invalid_descriptor ||
            sampler->descriptor == invalid_descriptor) {
            return false;
        }
        resolved_textures[index] = texture;
        resolved_samplers[index] = sampler;
    }
    const core::u32 view_first =
        commands.views.allocate(static_cast<core::u32>(sources.size()));
    const core::u32 sampler_first =
        commands.samplers.allocate(static_cast<core::u32>(sources.size()));
    if (view_first == invalid_descriptor ||
        sampler_first == invalid_descriptor) {
        return false;
    }
    // The graphics path's sampler-table reuse is not repeated here. It exists
    // because a pulled draw rebinds per batch with identical samplers; a
    // dispatch binds once, so there is nothing to reuse and a cache would only
    // be state that can go stale.
    for (core::u32 index = 0U; index < sources.size(); ++index) {
        D3d12Texture* texture = resolved_textures[index];
        D3d12Sampler* sampler = resolved_samplers[index];
        // Sampling what an earlier dispatch wrote transitions out of
        // UNORDERED_ACCESS, and that transition is itself the ordering
        // guarantee - a UAV barrier would add nothing.
        if (!transition(
                commands, *texture,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)) {
            return false;
        }
        device.device->CopyDescriptorsSimple(
            1U, commands.views.cpu(view_first + index),
            device.views.cpu(texture->srv),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        device.device->CopyDescriptorsSimple(
            1U, commands.samplers.cpu(sampler_first + index),
            device.samplers.cpu(sampler->descriptor),
            D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    }
    commands.list->SetComputeRootDescriptorTable(
        pipeline.layout.texture_table, commands.views.gpu(view_first));
    commands.list->SetComputeRootDescriptorTable(
        pipeline.layout.sampler_table, commands.samplers.gpu(sampler_first));
    return true;
}

bool d3d12_dispatch(
    void* command_value, const ComputeDispatch& work) noexcept {
    auto* commands = as_commands(command_value);
    if (commands == nullptr || !commands->recording ||
        commands->owner == nullptr || work.pipeline == nullptr) {
        return false;
    }
    auto* pipeline = as_compute_pipeline(work.pipeline->backend_handle());
    if (pipeline == nullptr) {
        return false;
    }
    // A zero-group dispatch is refused rather than recorded as a no-op. It is
    // always a caller that computed its group count wrong, and silently doing
    // nothing is the hardest version of that to find. Indirect counts are the
    // GPU's to choose and may be zero.
    const bool indirect = work.indirect.buffer != nullptr;
    if (!indirect && (work.groups_x == 0U || work.groups_y == 0U || work.groups_z == 0U)) {
        return false;
    }
    auto* state = commands->owner;
    // Everything that can refuse is checked before the first barrier, so a
    // refused dispatch leaves the list as it found it.
    D3d12Buffer* arguments = nullptr;
    ID3D12CommandSignature* signature = nullptr;
    if (indirect) {
        arguments = indirect_buffer(work.indirect, sizeof(D3D12_DISPATCH_ARGUMENTS));
        signature = arguments == nullptr
            ? nullptr : indirect_signature(*state, D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH);
        if (signature == nullptr) {
            return false;
        }
    }
    if (work.writable_buffers.size() != pipeline->layout.writable_count) {
        return false;
    }
    std::array<D3d12Buffer*, 32U> writable{};
    for (std::size_t index = 0U; index < work.writable_buffers.size(); ++index) {
        auto* const buffer = work.writable_buffers[index] == nullptr
            ? nullptr : as_buffer(work.writable_buffers[index]->backend_handle());
        if (buffer == nullptr || !buffer->writable || buffer == arguments ||
            std::find(writable.begin(), writable.begin() + static_cast<std::ptrdiff_t>(index), buffer)
                != writable.begin() + static_cast<std::ptrdiff_t>(index)) {
            return false;
        }
        writable[index] = buffer;
    }
    commands->list->SetComputeRootSignature(pipeline->root_signature.Get());
    commands->list->SetPipelineState(pipeline->pipeline.Get());
    ID3D12DescriptorHeap* heaps[]{
        commands->views.heap.Get(), commands->samplers.heap.Get()};
    commands->list->SetDescriptorHeaps(2U, heaps);

    if (!bind_compute_textures(
            *state, *commands, work.sampled_textures, *pipeline) ||
        !bind_compute_storage_textures(
            *state, *commands, work.storage_textures, *pipeline)) {
        return false;
    }
    if (work.storage_buffers.size() != pipeline->layout.storage_count) {
        return false;
    }
    for (core::u32 index = 0U; index < work.storage_buffers.size(); ++index) {
        if (work.storage_buffers[index] == nullptr) {
            return false;
        }
        auto* buffer = as_buffer(work.storage_buffers[index]->backend_handle());
        if (buffer == nullptr) {
            return false;
        }
        const auto address = buffer_address(*commands, *buffer);
        if (address == 0U) {
            return false;
        }
        commands->list->SetComputeRootShaderResourceView(
            pipeline->layout.storage_base + index, address);
    }
    if (pipeline->layout.uniform_count > 0U) {
        const auto address = upload_bytes(
            *commands, work.uniform,
            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT);
        if (address == 0U) {
            return false;
        }
        commands->list->SetComputeRootConstantBufferView(
            pipeline->layout.uniform_base, address);
    }
    for (core::u32 index = 0U; index < pipeline->layout.writable_count; ++index) {
        if (!bind_writable_buffer(*commands, *writable[index])) {
            return false;
        }
        commands->list->SetComputeRootUnorderedAccessView(
            pipeline->layout.writable_base + index,
            writable[index]->resource->GetGPUVirtualAddress());
    }
    if (indirect) {
        if (!transition_buffer(*commands, *arguments, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)) {
            return false;
        }
        commands->list->ExecuteIndirect(
            signature, 1U, arguments->resource.Get(), work.indirect.offset, nullptr, 0U);
    } else {
        commands->list->Dispatch(work.groups_x, work.groups_y, work.groups_z);
    }
    return true;
}

[[nodiscard]] D3D12_RESOURCE_STATES native_state(
    ResourceState state) noexcept {
    switch (state) {
    case ResourceState::ColorTarget:
        return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case ResourceState::DepthTarget:
        return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    case ResourceState::ShaderRead:
        // Both read states at once. A planner declares that a texture is read
        // by a shader, not which stage reads it, and this is the state that is
        // true either way. A later implicit transition inside a draw narrows it
        // to the pixel state, which is one redundant barrier and no hazard.
        return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
            | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case ResourceState::StorageRead:
    case ResourceState::StorageWrite:
        return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case ResourceState::CopySource:
        return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case ResourceState::CopyDestination:
        return D3D12_RESOURCE_STATE_COPY_DEST;
    case ResourceState::Present:
        return D3D12_RESOURCE_STATE_PRESENT;
    }
    return D3D12_RESOURCE_STATE_COMMON;
}

bool d3d12_transition(
    void* command_value, void* target_value, ResourceState state) noexcept {
    auto* commands = as_commands(command_value);
    auto* target = as_texture(target_value);
    if (commands == nullptr || target == nullptr || !commands->recording) {
        return false;
    }
    // The one helper, so the journal stays complete. A second path to resource
    // state is how a cancelled list stops rolling back.
    return transition(*commands, *target, native_state(state));
}

bool d3d12_clear(
    void* command_value, void* target_value, Color clear) noexcept {
    auto* commands = as_commands(command_value);
    auto* target = as_texture(target_value);
    if (commands == nullptr || target == nullptr || !commands->recording ||
        commands->owner == nullptr || target->rtv == invalid_descriptor) {
        return false;
    }
    if (!transition(*commands, *target, D3D12_RESOURCE_STATE_RENDER_TARGET)) {
        return false;
    }
    const auto rtv = commands->owner->rtvs.cpu(target->rtv);
    const float value[]{clear.r, clear.g, clear.b, clear.a};
    commands->list->ClearRenderTargetView(rtv, value, 0U, nullptr);
    return true;
}

bool d3d12_blit(
    void* command_value,
    void* source_value,
    void* destination_value,
    Filter) noexcept {
    auto* commands = as_commands(command_value);
    auto* source = as_texture(source_value);
    auto* destination = as_texture(destination_value);
    if (commands == nullptr || source == nullptr || destination == nullptr ||
        !commands->recording || source->desc.width != destination->desc.width ||
        source->desc.height != destination->desc.height ||
        source->desc.format != destination->desc.format) {
        return false;
    }
    if (!transition(*commands, *source, D3D12_RESOURCE_STATE_COPY_SOURCE) ||
        !transition(*commands, *destination, D3D12_RESOURCE_STATE_COPY_DEST)) {
        return false;
    }
    commands->list->CopyResource(
        destination->resource.Get(), source->resource.Get());
    return true;
}

bool d3d12_copy_region(
    void* command_value,
    void* source_value,
    ViewportRect region,
    void* destination_value) noexcept {
    auto* commands = as_commands(command_value);
    auto* source = as_texture(source_value);
    auto* destination = as_texture(destination_value);
    if (commands == nullptr || source == nullptr || destination == nullptr ||
        !commands->recording || region.empty() ||
        source->desc.format != destination->desc.format ||
        !region.fits(source->desc.width, source->desc.height) ||
        region.width > destination->desc.width ||
        region.height > destination->desc.height) {
        return false;
    }
    if (!transition(*commands, *source, D3D12_RESOURCE_STATE_COPY_SOURCE) ||
        !transition(*commands, *destination, D3D12_RESOURCE_STATE_COPY_DEST)) {
        return false;
    }
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = source->resource.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex = 0U;
    D3D12_TEXTURE_COPY_LOCATION to{};
    to.pResource = destination->resource.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.SubresourceIndex = 0U;
    D3D12_BOX box{};
    box.left = region.x;
    box.top = region.y;
    box.front = 0U;
    box.right = region.x + region.width;
    box.bottom = region.y + region.height;
    box.back = 1U;
    commands->list->CopyTextureRegion(&to, 0U, 0U, 0U, &from, &box);
    return true;
}

bool d3d12_download(
    void* command_value, void* source_value, void* readback_value, core::u32 mip, core::u32 layer) noexcept {
    auto* commands = as_commands(command_value);
    auto* source = as_texture(source_value);
    auto* readback = as_readback(readback_value);
    if (commands == nullptr || source == nullptr || readback == nullptr ||
        !commands->recording || commands->owner == nullptr) {
        return false;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows{};
    UINT64 row_bytes{};
    UINT64 total_bytes{};
    const auto desc = source->resource->GetDesc();
    commands->owner->device->GetCopyableFootprints(
        &desc, mip + layer * source->desc.mip_levels, 1U, 0U, &footprint, &rows, &row_bytes, &total_bytes);
    if (row_bytes * rows > readback->bytes) {
        return false;
    }
    if (total_bytes > readback->capacity) {
        const auto heap = heap_properties(D3D12_HEAP_TYPE_READBACK);
        const auto buffer = buffer_desc(total_bytes);
        ComPtr<ID3D12Resource> replacement;
        if (FAILED(commands->owner->device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&replacement)))) {
            return false;
        }
        readback->resource = std::move(replacement);
        readback->capacity = total_bytes;
    }
    readback->row_bytes = row_bytes;
    readback->row_pitch = footprint.Footprint.RowPitch;
    readback->rows = rows;
    if (!transition(*commands, *source, D3D12_RESOURCE_STATE_COPY_SOURCE)) {
        return false;
    }
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = source->resource.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION to{};
    from.SubresourceIndex = mip + layer * source->desc.mip_levels;
    to.pResource = readback->resource.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = footprint;
    commands->list->CopyTextureRegion(&to, 0U, 0U, 0U, &from, nullptr);
    return true;
}

bool d3d12_download_buffer(
    void* command_value, void* source_value, core::u32 offset, core::u32 bytes,
    void* readback_value) noexcept {
    auto* commands = as_commands(command_value);
    auto* source = as_buffer(source_value);
    auto* readback = as_readback(readback_value);
    if (commands == nullptr || source == nullptr || readback == nullptr ||
        !commands->recording || commands->owner == nullptr || !source->device_local ||
        bytes == 0U || offset > source->bytes || source->bytes - offset < bytes ||
        bytes > readback->bytes) {
        return false;
    }
    if (bytes > readback->capacity) {
        const auto heap = heap_properties(D3D12_HEAP_TYPE_READBACK);
        const auto buffer = buffer_desc(bytes);
        ComPtr<ID3D12Resource> replacement;
        if (FAILED(commands->owner->device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &buffer,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&replacement)))) {
            return false;
        }
        readback->resource = std::move(replacement);
        readback->capacity = bytes;
    }
    // One row: the bytes, unpadded.
    readback->row_bytes = bytes;
    readback->row_pitch = bytes;
    readback->rows = 1U;
    if (!transition_buffer(*commands, *source, D3D12_RESOURCE_STATE_COPY_SOURCE)) {
        return false;
    }
    commands->list->CopyBufferRegion(readback->resource.Get(), 0U, source->resource.Get(), offset, bytes);
    return true;
}

core::Result<void*, RhiError> d3d12_submit(
    void* value, void* command_value) noexcept {
    using Result = core::Result<void*, RhiError>;
    auto* state = as_device(value);
    auto* commands = as_commands(command_value);
    if (state == nullptr || commands == nullptr || commands->owner != state ||
        !commands->recording) {
        return Result::failure(RhiError::BackendFailure);
    }
    for (core::u32 i = 0U; i < commands->timed_passes; ++i) {
        if (!commands->pass_ended[i]) {
            ++commands->query_overflows;
            d3d12_end_gpu_pass(commands, i + 1U);
        }
    }
    const core::u32 first_query = commands->query_index * queries_per_context;
    commands->list->EndQuery(
        state->timestamp_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
        first_query + 1U);
    commands->list->ResolveQueryData(
        state->timestamp_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
        first_query, (commands->timed_passes + 1U) * 2U, state->timestamp_readback.Get(),
        static_cast<core::u64>(first_query) * sizeof(core::u64));
    if (FAILED(commands->list->Close())) {
        finish_texture_states(*commands, false);
        finish_buffer_uses(*commands, false);
        commands->recording = false;
        commands->pending_swapchain = nullptr;
        commands->pending_swapchain_index = 0U;
        return Result::failure(RhiError::BackendFailure);
    }
    ID3D12CommandList* lists[]{commands->list.Get()};
    state->graphics_queue->ExecuteCommandLists(1U, lists);
    // ExecuteCommandLists has no failure return. From this point onward the GPU
    // may reference every resource captured by the recording, even if Present
    // or Signal subsequently reports a failure.
    commands->submitted = true;
    commands->fence_value = state->next_fence++;
    finish_texture_states(*commands, true);
    finish_buffer_uses(*commands, true);
    bool present_failed = false;
    if (commands->pending_swapchain != nullptr) {
        const auto present_begin = std::chrono::steady_clock::now();
        auto& swapchain = *commands->pending_swapchain;
        const UINT interval = swapchain.mode == PresentMode::Vsync ? 1U : 0U;
        UINT flags = DXGI_PRESENT_DO_NOT_WAIT;
        if (interval == 0U &&
            (swapchain.flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0U) {
            flags |= DXGI_PRESENT_ALLOW_TEARING;
        }
        PresentBarrierPresentResult barrier_present =
            PresentBarrierPresentResult::PassThrough;
        const PresentBarrierBackendHandle backend =
            acquire_present_barrier_backend();
        if (backend && commands->pending_swapchain_index <
                           swapchain_buffer_count) {
            // Once a native present barrier owns this swapchain, hardware
            // reference is the cadence authority. Do not add DXGI vsync as a
            // second pacer; the backend returns PassThrough for an ordinary
            // output, whose configured interval is still used below.
            barrier_present = backend->present(
                native_device_handle(*state),
                native_swapchain_handle(swapchain),
                native_image_handle(swapchain.buffers[
                    commands->pending_swapchain_index]),
                0U, flags);
        }
        if (barrier_present == PresentBarrierPresentResult::PassThrough) {
            const HRESULT presented =
                swapchain.swapchain->Present(interval, flags);
            // Neither an occluded nor a busy Present queued a displayed
            // frame. Retry with the admission already obtained from DXGI.
            if (presented == S_OK) swapchain.frame_latency_ready = false;
            present_failed = FAILED(presented) &&
                presented != DXGI_ERROR_WAS_STILL_DRAWING &&
                presented != DXGI_STATUS_OCCLUDED;
        } else {
            present_failed =
                barrier_present == PresentBarrierPresentResult::Failed;
            if (barrier_present == PresentBarrierPresentResult::Presented)
                swapchain.frame_latency_ready = false;
        }
        commands->present_ns = static_cast<core::u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - present_begin).count());
    }
    if (FAILED(state->graphics_queue->Signal(
            state->fence.Get(), commands->fence_value))) {
        commands->recording = false;
        commands->quarantined = true;
        commands->pending_swapchain = nullptr;
        commands->pending_swapchain_index = 0U;
        return Result::failure(RhiError::BackendFailure);
    }
    if (commands->pending_swapchain != nullptr &&
        commands->pending_swapchain_index < swapchain_buffer_count) {
        commands->pending_swapchain
            ->buffer_fences[commands->pending_swapchain_index] =
            commands->fence_value;
    }
    commands->pending_swapchain = nullptr;
    commands->recording = false;
    commands->ticket_owned = !present_failed;
    if (present_failed) {
        return Result::failure(RhiError::BackendFailure);
    }
    commands->ticket_owned = true;
    return Result::success(commands);
}

bool d3d12_submission_accepted(void* value) noexcept {
    const auto* commands = as_commands(value);
    return commands != nullptr && commands->submitted;
}

bool d3d12_work_complete(void* value, void* work) noexcept {
    auto* state = as_device(value);
    auto* commands = as_commands(work);
    return state != nullptr && commands != nullptr && commands->submitted &&
        state->fence->GetCompletedValue() >= commands->fence_value;
}

bool d3d12_wait_work(void* value, void* work) noexcept {
    auto* state = as_device(value);
    auto* commands = as_commands(work);
    if (state == nullptr || commands == nullptr || !commands->submitted) {
        return false;
    }
    if (state->fence->GetCompletedValue() >= commands->fence_value) {
        return true;
    }
    if (FAILED(state->fence->SetEventOnCompletion(
            commands->fence_value, state->fence_event))) {
        return false;
    }
    return WaitForSingleObject(state->fence_event, INFINITE) == WAIT_OBJECT_0;
}

core::u64 d3d12_work_gpu_nanoseconds(void* value, void* work) noexcept {
    auto* state = as_device(value);
    auto* commands = as_commands(work);
    if (state == nullptr || commands == nullptr || state->timestamp_mapped == nullptr
        || state->timestamp_frequency == 0U
        || !d3d12_work_complete(state, commands)) {
        return 0U;
    }
    const core::u32 first = commands->query_index * queries_per_context;
    const core::u64 begin = state->timestamp_mapped[first];
    const core::u64 end = state->timestamp_mapped[first + 1U];
    if (end < begin) {
        return 0U;
    }
    const core::u64 ticks = end - begin;
    const core::u64 seconds = ticks / state->timestamp_frequency;
    const core::u64 remainder = ticks % state->timestamp_frequency;
    return seconds * 1'000'000'000ULL
        + remainder * 1'000'000'000ULL / state->timestamp_frequency;
}

core::u32 d3d12_begin_gpu_pass(void* value, core::RenderPass pass) noexcept {
    auto* commands = as_commands(value);
    if (!commands || !commands->recording || pass >= core::RenderPass::Count) return 0U;
    if (commands->timed_passes == max_timed_passes) { ++commands->query_overflows; return 0U; }
    const core::u32 token = ++commands->timed_passes;
    commands->pass_categories[token - 1U] = pass;
    commands->pass_ended[token - 1U] = false;
    commands->list->EndQuery(commands->owner->timestamp_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
        commands->query_index * queries_per_context + token * 2U);
    return token;
}
void d3d12_end_gpu_pass(void* value, core::u32 token) noexcept {
    auto* commands = as_commands(value);
    if (!commands || !commands->recording || token == 0U || token > commands->timed_passes
        || commands->pass_ended[token - 1U]) return;
    commands->list->EndQuery(commands->owner->timestamp_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
        commands->query_index * queries_per_context + token * 2U + 1U);
    commands->pass_ended[token - 1U] = true;
}
core::u64 d3d12_upload_bytes(void* value) noexcept {
    const auto* commands = as_commands(value);
    return commands ? commands->upload_bytes : 0U;
}
core::FrameTiming d3d12_work_frame_timing(void* value, void* work) noexcept {
    core::FrameTiming result{};
    auto* state = as_device(value);
    auto* commands = as_commands(work);
    if (!d3d12_work_complete(state, commands)) return result;
    result.sequence = commands->fence_value;
    result.gpu_ns = d3d12_work_gpu_nanoseconds(value, work);
    result.upload_bytes = commands->upload_bytes;
    result.present_ns = commands->present_ns;
    result.query_overflows = commands->query_overflows;
    result.gpu_valid = commands->query_overflows == 0U;
    const auto* stamps = state->timestamp_mapped + commands->query_index * queries_per_context;
    for (core::u32 i = 0U; i < commands->timed_passes; ++i) {
        const core::u64 begin = stamps[(i + 1U) * 2U], end = stamps[(i + 1U) * 2U + 1U];
        if (!commands->pass_ended[i] || end < begin) { result.gpu_valid = false; continue; }
        const auto ticks = end - begin;
        result.pass_ns[static_cast<std::size_t>(commands->pass_categories[i])] +=
            ticks / state->timestamp_frequency * 1'000'000'000ULL
            + ticks % state->timestamp_frequency * 1'000'000'000ULL / state->timestamp_frequency;
    }
    return result;
}

void d3d12_release_work(void*, void* work) noexcept {
    auto* commands = as_commands(work);
    if (commands != nullptr) {
        commands->ticket_owned = false;
    }
}

} // namespace mediaserver::rhi::detail

#else

namespace mediaserver::rhi::detail {

core::u32 d3d12_begin_gpu_pass(void*, core::RenderPass) noexcept { return 0U; }
void d3d12_end_gpu_pass(void*, core::u32) noexcept {}
core::u64 d3d12_upload_bytes(void*) noexcept { return 0U; }
core::FrameTiming d3d12_work_frame_timing(void*, void*) noexcept { return {}; }
bool d3d12_available() noexcept { return false; }
core::Result<D3d12CreatedDevice, RhiError> create_d3d12_device(
    const DeviceConfig&) noexcept {
    return core::Result<D3d12CreatedDevice, RhiError>::failure(
        RhiError::Unavailable);
}
void destroy_d3d12_device(void*) noexcept {}
core::RenderAdapterIdentity d3d12_adapter_identity(void*) noexcept { return {}; }
std::string_view d3d12_driver_name(void*) noexcept { return {}; }
core::u64 d3d12_dedicated_video_memory(void*) noexcept { return 0U; }
bool d3d12_supports_shader(ShaderFormat) noexcept { return false; }
bool d3d12_supports_texture(void*, TextureFormat) noexcept { return false; }
void d3d12_wait_idle(void*) noexcept {}
core::Result<D3d12CreatedSwapchain, RhiError> d3d12_create_swapchain(
    void*, void*, PresentMode, TextureFormat) noexcept {
    return core::Result<D3d12CreatedSwapchain, RhiError>::failure(
        RhiError::Unavailable);
}
void d3d12_release_swapchain(void*, void*) noexcept {}
void* d3d12_native_swapchain(void*) noexcept { return nullptr; }
std::span<void* const> d3d12_present_barrier_images(void*) noexcept {
    return {};
}
D3d12CreatedSwapchainImage d3d12_acquire_swapchain(
    void*, void*, void*) noexcept {
    return {};
}
bool d3d12_present_swapchain(
    void*, void*, void*, void*, Filter) noexcept {
    return false;
}
core::Result<D3d12CreatedResource, RhiError> d3d12_create_texture(
    void*, const TextureDesc&) noexcept {
    return core::Result<D3d12CreatedResource, RhiError>::failure(
        RhiError::Unavailable);
}
core::Result<D3d12CreatedResource, RhiError> d3d12_import_video_plane(
    void*, void*, core::u32, core::u32, TextureFormat, core::u32) noexcept {
    return core::Result<D3d12CreatedResource, RhiError>::failure(
        RhiError::Unavailable);
}
void d3d12_release_texture(void*, void*) noexcept {}
core::Result<D3d12CreatedResource, RhiError> d3d12_create_storage_buffer(
    void*, core::u32) noexcept {
    return core::Result<D3d12CreatedResource, RhiError>::failure(
        RhiError::Unavailable);
}
void d3d12_release_storage_buffer(void*, void*) noexcept {}
core::Result<D3d12CreatedResource, RhiError> d3d12_create_geometry_buffer(
    void*, core::u32, bool) noexcept {
    return core::Result<D3d12CreatedResource, RhiError>::failure(
        RhiError::Unavailable);
}
bool d3d12_buffer_safe_to_release(void*, void*) noexcept { return false; }
bool d3d12_upload_buffer_range(void*, void*, core::u32, std::span<const core::u8>) noexcept { return false; }
bool d3d12_copy_buffer(void*, void*, void*, core::u32) noexcept { return false; }
core::u64 d3d12_upload_space(void*) noexcept { return 0U; }
core::u32 d3d12_texture_upload_row_capacity(void*, void*, core::u64) noexcept { return 0U; }
core::Result<void*, RhiError> d3d12_create_sampler(void*, const SamplerDesc&) noexcept {
    return core::Result<void*, RhiError>::failure(RhiError::Unavailable);
}
void d3d12_release_sampler(void*, void*) noexcept {}
core::Result<void*, RhiError> d3d12_create_readback(void*, core::u32) noexcept {
    return core::Result<void*, RhiError>::failure(RhiError::Unavailable);
}
std::span<const core::u8> d3d12_map_readback(void*, core::u32) noexcept { return {}; }
void d3d12_unmap_readback(void*) noexcept {}
void d3d12_release_readback(void*, void*) noexcept {}
core::Result<void*, RhiError> d3d12_create_pipeline(
    void*, const ShaderDesc&, const ShaderDesc&, std::span<const TextureFormat>, ColorBlend,
    DepthState, RasterState) noexcept {
    return core::Result<void*, RhiError>::failure(RhiError::Unavailable);
}
void d3d12_release_pipeline(void*, void*) noexcept {}
core::Result<void*, RhiError> d3d12_create_compute_pipeline(
    void*, const ShaderDesc&) noexcept {
    return core::Result<void*, RhiError>::failure(RhiError::Unavailable);
}
void d3d12_release_compute_pipeline(void*, void*) noexcept {}
bool d3d12_dispatch(void*, const ComputeDispatch&) noexcept { return false; }
core::Result<D3d12CreatedResource, RhiError> d3d12_acquire_commands(
    void*) noexcept {
    return core::Result<D3d12CreatedResource, RhiError>::failure(
        RhiError::Unavailable);
}
void d3d12_release_commands(void*, void*) noexcept {}
bool d3d12_upload_buffer(void*, void*, std::span<const core::u8>) noexcept { return false; }
bool d3d12_texture_safe_to_release(void*, void*) noexcept { return false; }
bool d3d12_upload_texture_rows(void*, void*, core::u32, core::u32, core::u32, core::u32, std::span<const core::u8>) noexcept { return false; }
bool d3d12_upload_texture(void*, void*, std::span<const core::u8>) noexcept { return false; }
bool d3d12_draw_instanced_quads(
    void*, void*, Color, void*, void*, core::u32, void*, void*) noexcept { return false; }
bool d3d12_draw_pulled(void*, void*, Color, void*, float, const PulledDraw&) noexcept { return false; }
bool d3d12_draw_output_transform(
    void*, void*, Color, void*, std::span<const SampledTexture>,
    std::span<const core::u8>, bool) noexcept { return false; }
bool d3d12_clear(void*, void*, Color) noexcept { return false; }
bool d3d12_transition(void*, void*, ResourceState) noexcept { return false; }
bool d3d12_blit(void*, void*, void*, Filter) noexcept { return false; }
bool d3d12_download(void*, void*, void*, core::u32, core::u32) noexcept { return false; }
bool d3d12_download_buffer(void*, void*, core::u32, core::u32, void*) noexcept { return false; }
bool d3d12_copy_region(void*, void*, ViewportRect, void*) noexcept {
    return false;
}
core::Result<void*, RhiError> d3d12_submit(void*, void*) noexcept {
    return core::Result<void*, RhiError>::failure(RhiError::Unavailable);
}
bool d3d12_submission_accepted(void*) noexcept { return false; }
bool d3d12_work_complete(void*, void*) noexcept { return false; }
bool d3d12_wait_work(void*, void*) noexcept { return false; }
core::u64 d3d12_work_gpu_nanoseconds(void*, void*) noexcept { return 0U; }
void d3d12_release_work(void*, void*) noexcept {}

} // namespace mediaserver::rhi::detail

#endif
