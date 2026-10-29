#include "d3d12_video_halve.hpp"

#if defined(_WIN32) && defined(MEDIASERVER_HAS_DXIL_SHADERS)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <mediaserver/shaders/video_halve_comp_dxil.h>

#include <array>
#include <mutex>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#endif

namespace mediaserver::render::detail {

#if defined(_WIN32) && defined(MEDIASERVER_HAS_DXIL_SHADERS)
namespace {

using Microsoft::WRL::ComPtr;


struct PlaneFormats final {
    DXGI_FORMAT surface;
    DXGI_FORMAT luma;
    DXGI_FORMAT chroma;
    UINT max_code;
};

[[nodiscard]] PlaneFormats plane_formats(bool ten_bit) noexcept {
    return ten_bit
        ? PlaneFormats{DXGI_FORMAT_P010, DXGI_FORMAT_R16_UNORM,
                       DXGI_FORMAT_R16G16_UNORM, 65535U}
        : PlaneFormats{DXGI_FORMAT_NV12, DXGI_FORMAT_R8_UNORM,
                       DXGI_FORMAT_R8G8_UNORM, 255U};
}

[[nodiscard]] ComPtr<ID3D12Resource> create_texture(
    ID3D12Device *device, UINT width, UINT height, DXGI_FORMAT format,
    D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) noexcept {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC described{};
    described.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    described.Width = width;
    described.Height = height;
    described.DepthOrArraySize = 1U;
    described.MipLevels = 1U;
    described.Format = format;
    described.SampleDesc.Count = 1U;
    described.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    described.Flags = flags;
    ComPtr<ID3D12Resource> texture;
    if (FAILED(device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &described, state, nullptr,
            IID_PPV_ARGS(&texture)))) {
        return {};
    }
    return texture;
}

// Half-size surfaces handed to the renderer. A playing clip at 24 fps would
// otherwise allocate and free a committed resource per frame; returned ones
// wait here for the next frame of the same size. Bounded, so a clip that has
// stopped playing gives back all but a few.
class SurfacePool final : public std::enable_shared_from_this<SurfacePool> {
  public:
    explicit SurfacePool(ComPtr<ID3D12Device> device)
        : device_(std::move(device)) {}

    [[nodiscard]] std::optional<VideoSurfaceCopy>
    acquire(UINT width, UINT height, DXGI_FORMAT format) noexcept {
        ComPtr<ID3D12Resource> surface;
        {
            const std::lock_guard lock{mutex_};
            for (auto found = idle_.begin(); found != idle_.end(); ++found) {
                const D3D12_RESOURCE_DESC described = (*found)->GetDesc();
                if (described.Width == width && described.Height == height &&
                    described.Format == format) {
                    surface = std::move(*found);
                    idle_.erase(found);
                    break;
                }
            }
        }
        if (surface == nullptr) {
            surface = create_texture(device_.Get(), width, height, format,
                                     D3D12_RESOURCE_FLAG_NONE,
                                     D3D12_RESOURCE_STATE_COMMON);
            if (surface == nullptr) {
                return std::nullopt;
            }
        }
        std::shared_ptr<SurfacePool> self;
        try {
            self = shared_from_this();
        } catch (...) {
            return std::nullopt;
        }
        ID3D12Resource *const raw = surface.Detach();
        try {
            VideoSurfaceCopy result{};
            result.resource = raw;
            result.width = width;
            result.height = height;
            result.owner = std::shared_ptr<void>(
                raw, [pool = std::move(self)](void *value) noexcept {
                    pool->release(static_cast<ID3D12Resource *>(value));
                });
            return result;
        } catch (...) {
            // A shared_ptr that fails to construct has already run the
            // deleter, which took the surface back.
            return std::nullopt;
        }
    }

  private:
    void release(ID3D12Resource *surface) noexcept {
        ComPtr<ID3D12Resource> owned;
        owned.Attach(surface);
        try {
            const std::lock_guard lock{mutex_};
            if (idle_.size() < max_idle_surfaces) {
                idle_.push_back(std::move(owned));
            }
        } catch (...) {
        }
    }

    static constexpr std::size_t max_idle_surfaces = 16U;
    ComPtr<ID3D12Device> device_;
    std::mutex mutex_;
    std::vector<ComPtr<ID3D12Resource>> idle_;
};

// One worker's recording state. A context is used by one decode worker at a
// time and waited to completion before it is returned, so its allocator,
// descriptors and intermediate planes are never shared with work in flight.
struct Context final {
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    HANDLE event{};
    UINT64 fence_value{};
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT descriptor_size{};
    // The halved planes are written here and then copied into the surface:
    // a planar NV12/P010 surface cannot be relied on to accept a typed UAV,
    // but every D3D12 device copies an R8/R8G8 (R16/R16G16) texture into its
    // planes.
    ComPtr<ID3D12Resource> luma;
    ComPtr<ID3D12Resource> chroma;
    UINT width{};
    UINT height{};
    bool ten_bit{};

    Context() = default;
    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;
    ~Context() {
        if (event != nullptr) {
            CloseHandle(event);
        }
    }
};

class Halver final {
  public:
    [[nodiscard]] static std::shared_ptr<Halver>
    create(ID3D12Device *device) noexcept {
        try {
            auto halver = std::make_shared<Halver>();
            halver->device_ = device;
            std::array<D3D12_DESCRIPTOR_RANGE, 2U> ranges{};
            ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            ranges[0].NumDescriptors = 1U;
            ranges[0].OffsetInDescriptorsFromTableStart =
                D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            ranges[1].NumDescriptors = 1U;
            ranges[1].OffsetInDescriptorsFromTableStart =
                D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            std::array<D3D12_ROOT_PARAMETER, 2U> parameters{};
            parameters[0].ParameterType =
                D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            parameters[0].Constants.ShaderRegister = 0U;
            parameters[0].Constants.Num32BitValues = 8U;
            parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            parameters[1].ParameterType =
                D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[1].DescriptorTable.NumDescriptorRanges =
                static_cast<UINT>(ranges.size());
            parameters[1].DescriptorTable.pDescriptorRanges = ranges.data();
            parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            D3D12_ROOT_SIGNATURE_DESC signature{};
            signature.NumParameters = static_cast<UINT>(parameters.size());
            signature.pParameters = parameters.data();
            ComPtr<ID3DBlob> serialized;
            ComPtr<ID3DBlob> error;
            if (FAILED(D3D12SerializeRootSignature(
                    &signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized,
                    &error)) ||
                FAILED(device->CreateRootSignature(
                    0U, serialized->GetBufferPointer(),
                    serialized->GetBufferSize(),
                    IID_PPV_ARGS(&halver->root_)))) {
                return nullptr;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
            pipeline.pRootSignature = halver->root_.Get();
            pipeline.CS.pShaderBytecode = mediaserver_video_halve_comp_dxil;
            pipeline.CS.BytecodeLength =
                sizeof(mediaserver_video_halve_comp_dxil);
            if (FAILED(device->CreateComputePipelineState(
                    &pipeline, IID_PPV_ARGS(&halver->pipeline_)))) {
                return nullptr;
            }
            halver->pool_ = std::make_shared<SurfacePool>(halver->device_);
            return halver;
        } catch (...) {
            return nullptr;
        }
    }

    [[nodiscard]] ID3D12Device *device() const noexcept {
        return device_.Get();
    }

    [[nodiscard]] std::optional<VideoSurfaceCopy>
    run(ID3D12Resource *source, UINT width, UINT height,
        bool ten_bit) noexcept {
        // Both the half-size surface and its 4:2:0 chroma must be whole.
        if (source == nullptr || width < 4U || height < 4U ||
            width % 4U != 0U || height % 4U != 0U) {
            return std::nullopt;
        }
        std::unique_ptr<Context> context = acquire_context();
        if (context == nullptr) {
            return std::nullopt;
        }
        auto halved = record_and_wait(*context, source, width, height, ten_bit);
        // A context that failed part way may hold an open list or a
        // half-recorded allocator; it is dropped rather than reused.
        if (halved.has_value()) {
            return_context(std::move(context));
        }
        return halved;
    }

    [[nodiscard]] std::optional<VideoSurfaceCopy>
    crop(ID3D12Resource *source, UINT width, UINT height,
         bool ten_bit) noexcept {
        // The picture and its 4:2:0 chroma must be whole, and lie inside the
        // surface it is cut from.
        if (source == nullptr || width < 2U || height < 2U ||
            width % 2U != 0U || height % 2U != 0U) {
            return std::nullopt;
        }
        const D3D12_RESOURCE_DESC described = source->GetDesc();
        if (described.Width < width || described.Height < height ||
            described.Format != plane_formats(ten_bit).surface) {
            return std::nullopt;
        }
        std::unique_ptr<Context> context = acquire_context();
        if (context == nullptr) {
            return std::nullopt;
        }
        auto cropped =
            record_crop_and_wait(*context, source, width, height, ten_bit);
        if (cropped.has_value()) {
            return_context(std::move(context));
        }
        return cropped;
    }

  private:
    [[nodiscard]] std::unique_ptr<Context> acquire_context() noexcept {
        try {
            {
                const std::lock_guard lock{mutex_};
                if (!idle_.empty()) {
                    auto context = std::move(idle_.back());
                    idle_.pop_back();
                    return context;
                }
            }
            auto context = std::make_unique<Context>();
            D3D12_COMMAND_QUEUE_DESC queue{};
            queue.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
            if (FAILED(device_->CreateCommandQueue(
                    &queue, IID_PPV_ARGS(&context->queue))) ||
                FAILED(device_->CreateCommandAllocator(
                    D3D12_COMMAND_LIST_TYPE_COMPUTE,
                    IID_PPV_ARGS(&context->allocator))) ||
                FAILED(device_->CreateCommandList(
                    0U, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                    context->allocator.Get(), pipeline_.Get(),
                    IID_PPV_ARGS(&context->list))) ||
                FAILED(context->list->Close()) ||
                FAILED(device_->CreateFence(0U, D3D12_FENCE_FLAG_NONE,
                                            IID_PPV_ARGS(&context->fence)))) {
                return nullptr;
            }
            context->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            D3D12_DESCRIPTOR_HEAP_DESC heap{};
            heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heap.NumDescriptors = 4U;
            heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (context->event == nullptr ||
                FAILED(device_->CreateDescriptorHeap(
                    &heap, IID_PPV_ARGS(&context->heap)))) {
                return nullptr;
            }
            context->descriptor_size =
                device_->GetDescriptorHandleIncrementSize(
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            return context;
        } catch (...) {
            return nullptr;
        }
    }

    void return_context(std::unique_ptr<Context> context) noexcept {
        try {
            const std::lock_guard lock{mutex_};
            idle_.push_back(std::move(context));
        } catch (...) {
        }
    }

    [[nodiscard]] std::optional<VideoSurfaceCopy>
    record_and_wait(Context &context, ID3D12Resource *source, UINT width,
                    UINT height, bool ten_bit) noexcept {
        const PlaneFormats formats = plane_formats(ten_bit);
        const UINT half_width = width / 2U;
        const UINT half_height = height / 2U;
        if (context.width != half_width || context.height != half_height ||
            context.ten_bit != ten_bit || context.luma == nullptr) {
            context.luma = create_texture(
                device_.Get(), half_width, half_height, formats.luma,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            context.chroma = create_texture(
                device_.Get(), half_width / 2U, half_height / 2U,
                formats.chroma, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (context.luma == nullptr || context.chroma == nullptr) {
                context.luma.Reset();
                return std::nullopt;
            }
            context.width = half_width;
            context.height = half_height;
            context.ten_bit = ten_bit;
        }
        auto surface = pool_->acquire(half_width, half_height, formats.surface);
        if (!surface.has_value()) {
            return std::nullopt;
        }

        const D3D12_CPU_DESCRIPTOR_HANDLE cpu_base =
            context.heap->GetCPUDescriptorHandleForHeapStart();
        const auto cpu_slot = [&](UINT slot) {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = cpu_base;
            handle.ptr += static_cast<SIZE_T>(slot) * context.descriptor_size;
            return handle;
        };
        const auto plane_view = [&](DXGI_FORMAT format, UINT plane,
                                    UINT slot) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = format;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1U;
            view.Texture2D.PlaneSlice = plane;
            device_->CreateShaderResourceView(source, &view, cpu_slot(slot));
        };
        const auto storage_view = [&](ID3D12Resource *target,
                                      DXGI_FORMAT format, UINT slot) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
            view.Format = format;
            view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device_->CreateUnorderedAccessView(target, nullptr, &view,
                                               cpu_slot(slot));
        };
        plane_view(formats.luma, 0U, 0U);
        storage_view(context.luma.Get(), formats.luma, 1U);
        plane_view(formats.chroma, 1U, 2U);
        storage_view(context.chroma.Get(), formats.chroma, 3U);

        if (FAILED(context.allocator->Reset()) ||
            FAILED(context.list->Reset(context.allocator.Get(),
                                       pipeline_.Get()))) {
            return std::nullopt;
        }
        ID3D12GraphicsCommandList &list = *context.list.Get();
        list.SetComputeRootSignature(root_.Get());
        ID3D12DescriptorHeap *const heaps[] = {context.heap.Get()};
        list.SetDescriptorHeaps(1U, heaps);
        const D3D12_GPU_DESCRIPTOR_HANDLE gpu_base =
            context.heap->GetGPUDescriptorHandleForHeapStart();
        const auto halve_plane = [&](UINT out_width, UINT out_height,
                                     UINT in_width, UINT in_height,
                                     UINT first_slot) {
            const std::array<UINT, 8U> constants{
                out_width, out_height, in_width, in_height,
                formats.max_code, 0U, 0U, 0U};
            list.SetComputeRoot32BitConstants(
                0U, static_cast<UINT>(constants.size()), constants.data(),
                0U);
            D3D12_GPU_DESCRIPTOR_HANDLE table = gpu_base;
            table.ptr += static_cast<UINT64>(first_slot) *
                context.descriptor_size;
            list.SetComputeRootDescriptorTable(1U, table);
            list.Dispatch((out_width + 7U) / 8U, (out_height + 7U) / 8U, 1U);
        };
        halve_plane(half_width, half_height, width, height, 0U);
        halve_plane(half_width / 2U, half_height / 2U, width / 2U,
                    height / 2U, 2U);

        const auto transition = [](ID3D12Resource *resource,
                                   D3D12_RESOURCE_STATES before,
                                   D3D12_RESOURCE_STATES after) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource;
            barrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            return barrier;
        };
        const std::array<D3D12_RESOURCE_BARRIER, 3U> into_copy{
            transition(context.luma.Get(),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(context.chroma.Get(),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_COPY_SOURCE),
            transition(surface->resource, D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_COPY_DEST)};
        list.ResourceBarrier(static_cast<UINT>(into_copy.size()),
                             into_copy.data());
        const auto copy_plane = [&](ID3D12Resource *plane, UINT subresource) {
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = surface->resource;
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = subresource;
            D3D12_TEXTURE_COPY_LOCATION from{};
            from.pResource = plane;
            from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            from.SubresourceIndex = 0U;
            list.CopyTextureRegion(&destination, 0U, 0U, 0U, &from, nullptr);
        };
        copy_plane(context.luma.Get(), 0U);
        copy_plane(context.chroma.Get(), 1U);
        // COMMON, as the decoder's own surfaces are left: the renderer reads
        // it through implicit promotion and never transitions it.
        const std::array<D3D12_RESOURCE_BARRIER, 3U> out_of_copy{
            transition(surface->resource, D3D12_RESOURCE_STATE_COPY_DEST,
                       D3D12_RESOURCE_STATE_COMMON),
            transition(context.luma.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            transition(context.chroma.Get(),
                       D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
        list.ResourceBarrier(static_cast<UINT>(out_of_copy.size()),
                             out_of_copy.data());
        if (!submit_and_wait(context)) {
            return std::nullopt;
        }
        return surface;
    }

    [[nodiscard]] std::optional<VideoSurfaceCopy>
    record_crop_and_wait(Context &context, ID3D12Resource *source, UINT width,
                         UINT height, bool ten_bit) noexcept {
        auto surface =
            pool_->acquire(width, height, plane_formats(ten_bit).surface);
        if (!surface.has_value()) {
            return std::nullopt;
        }
        if (FAILED(context.allocator->Reset()) ||
            FAILED(context.list->Reset(context.allocator.Get(), nullptr))) {
            return std::nullopt;
        }
        ID3D12GraphicsCommandList &list = *context.list.Get();
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = surface->resource;
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        list.ResourceBarrier(1U, &barrier);
        // The source is read through implicit promotion to COPY_SOURCE and
        // decays back to COMMON, where the decoder can write it again.
        const auto copy_plane = [&](UINT subresource, UINT plane_width,
                                    UINT plane_height) {
            D3D12_TEXTURE_COPY_LOCATION destination{};
            destination.pResource = surface->resource;
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            destination.SubresourceIndex = subresource;
            D3D12_TEXTURE_COPY_LOCATION from{};
            from.pResource = source;
            from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            from.SubresourceIndex = subresource;
            const D3D12_BOX picture{0U, 0U, 0U, plane_width, plane_height, 1U};
            list.CopyTextureRegion(&destination, 0U, 0U, 0U, &from, &picture);
        };
        copy_plane(0U, width, height);
        copy_plane(1U, width / 2U, height / 2U);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        list.ResourceBarrier(1U, &barrier);
        if (!submit_and_wait(context)) {
            return std::nullopt;
        }
        return surface;
    }

    [[nodiscard]] static bool submit_and_wait(Context &context) noexcept {
        if (FAILED(context.list->Close())) {
            return false;
        }
        ID3D12CommandList *const lists[] = {context.list.Get()};
        context.queue->ExecuteCommandLists(1U, lists);
        const UINT64 target = ++context.fence_value;
        if (FAILED(context.queue->Signal(context.fence.Get(), target))) {
            return false;
        }
        if (context.fence->GetCompletedValue() < target) {
            if (FAILED(context.fence->SetEventOnCompletion(target,
                                                           context.event))) {
                return false;
            }
            WaitForSingleObject(context.event, INFINITE);
        }
        return true;
    }

    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12RootSignature> root_;
    ComPtr<ID3D12PipelineState> pipeline_;
    std::shared_ptr<SurfacePool> pool_;
    std::mutex mutex_;
    std::vector<std::unique_ptr<Context>> idle_;
};

// One halver for the renderer's device. A device that failed to build one is
// remembered so a clip does not retry pipeline creation every frame.
[[nodiscard]] std::shared_ptr<Halver> halver_for(ID3D12Device *device) {
    static std::mutex mutex;
    static std::shared_ptr<Halver> current;
    static ID3D12Device *failed{};
    const std::lock_guard lock{mutex};
    if (current != nullptr && current->device() == device) {
        return current;
    }
    if (failed == device) {
        return nullptr;
    }
    current = Halver::create(device);
    if (current == nullptr) {
        failed = device;
    }
    return current;
}

} // namespace

std::optional<VideoSurfaceCopy> halve_video_surface(
    ID3D12Device *device, ID3D12Resource *source, core::u32 width,
    core::u32 height, bool ten_bit) noexcept {
    if (device == nullptr) {
        return std::nullopt;
    }
    try {
        const auto halver = halver_for(device);
        return halver == nullptr
            ? std::nullopt
            : halver->run(source, width, height, ten_bit);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<VideoSurfaceCopy> crop_video_surface(
    ID3D12Device *device, ID3D12Resource *source, core::u32 width,
    core::u32 height, bool ten_bit) noexcept {
    if (device == nullptr) {
        return std::nullopt;
    }
    try {
        const auto halver = halver_for(device);
        return halver == nullptr
            ? std::nullopt
            : halver->crop(source, width, height, ten_bit);
    } catch (...) {
        return std::nullopt;
    }
}

#else

std::optional<VideoSurfaceCopy> halve_video_surface(
    ID3D12Device *, ID3D12Resource *, core::u32, core::u32, bool) noexcept {
    return std::nullopt;
}

std::optional<VideoSurfaceCopy> crop_video_surface(
    ID3D12Device *, ID3D12Resource *, core::u32, core::u32, bool) noexcept {
    return std::nullopt;
}

#endif

} // namespace mediaserver::render::detail
