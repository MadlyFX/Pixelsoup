#include "nvapi_present_barrier.hpp"

#if !defined(_WIN32)
#error "The NVAPI present-barrier backend is Windows-only."
#endif

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <string_view>

namespace mediaserver::app {
namespace {

namespace core = mediaserver::core;
namespace rhi = mediaserver::rhi;

// Keep this translation unit independent of the NVAPI SDK. nvapi64.dll
// exposes nvapi_QueryInterface specifically for this purpose, so a normal
// developer build never needs NVIDIA headers or an import library. The ABI
// declarations below mirror the public MIT-licensed NVAPI headers.
using NvStatus = std::int32_t;
using NvU32 = std::uint32_t;
constexpr NvStatus nv_ok = 0;

using NvQueryInterface = void* (__cdecl *)(NvU32);
using NvInitialize = NvStatus (__cdecl *)();
using NvUnload = NvStatus (__cdecl *)();
using NvD3dPresent = NvStatus (__cdecl *)(
    IUnknown*, IDXGISwapChain*, UINT, UINT);
using NvQueryFrameCount = NvStatus (__cdecl *)(IUnknown*, NvU32*);
using NvQueryMaxSwapGroup = NvStatus (__cdecl *)(
    IUnknown*, NvU32*, NvU32*);
using NvJoinSwapGroup = NvStatus (__cdecl *)(
    IUnknown*, IDXGISwapChain*, NvU32, BOOL);
using NvBindSwapBarrier = NvStatus (__cdecl *)(IUnknown*, NvU32, NvU32);
using NvQueryPresentBarrierSupport = NvStatus (__cdecl *)(
    ID3D12Device*, bool*);
using NvCreatePresentBarrierClient = NvStatus (__cdecl *)(
    ID3D12Device*, IDXGISwapChain*, void**);
using NvRegisterPresentBarrierResources = NvStatus (__cdecl *)(
    void*, ID3D12Fence*, ID3D12Resource**, NvU32);

struct NvJoinPresentBarrierParams final {
    NvU32 dwVersion{};
};

using NvJoinPresentBarrier = NvStatus (__cdecl *)(
    void*, NvJoinPresentBarrierParams*);
using NvLeavePresentBarrier = NvStatus (__cdecl *)(void*);
using NvDestroyPresentBarrierClient = NvStatus (__cdecl *)(void*);

struct NvPresentBarrierFrameStatistics final {
    NvU32 dwVersion{};
    NvU32 sync_mode{};
    NvU32 present_count{};
    NvU32 present_in_sync_count{};
    NvU32 flip_in_sync_count{};
    NvU32 refresh_count{};
};

using NvQueryPresentBarrierFrameStatistics = NvStatus (__cdecl *)(
    void*, NvPresentBarrierFrameStatistics*);

template <typename Value>
[[nodiscard]] constexpr NvU32 nv_version() noexcept {
    return static_cast<NvU32>(sizeof(Value)) | (1U << 16U);
}

// Interface IDs are published with the NVAPI SDK in nvapi_interface.h.
constexpr NvU32 nvapi_initialize_id = 0x0150e828U;
constexpr NvU32 nvapi_unload_id = 0xd22bdd7eU;
constexpr NvU32 nvapi_d3d1x_present_id = 0x03b845a1U;
constexpr NvU32 nvapi_d3d1x_query_frame_count_id = 0x9152e055U;
constexpr NvU32 nvapi_d3d1x_query_max_swap_group_id = 0x9bb9d68fU;
constexpr NvU32 nvapi_d3d1x_join_swap_group_id = 0x14610cd7U;
constexpr NvU32 nvapi_d3d1x_bind_swap_barrier_id = 0x9de8c729U;
constexpr NvU32 nvapi_d3d12_query_present_barrier_support_id = 0xa15faef7U;
constexpr NvU32 nvapi_d3d12_create_present_barrier_client_id = 0x4d815de9U;
constexpr NvU32 nvapi_d3d12_register_present_barrier_resources_id =
    0xd53c9ef0U;
constexpr NvU32 nvapi_destroy_present_barrier_client_id = 0x3c5c351bU;
constexpr NvU32 nvapi_join_present_barrier_id = 0x17f6bf82U;
constexpr NvU32 nvapi_leave_present_barrier_id = 0xc3ec5a7fU;
constexpr NvU32 nvapi_query_present_barrier_frame_statistics_id = 0x61b844a1U;

constexpr NvU32 present_barrier_not_joined = 0U;
constexpr NvU32 present_barrier_sync_system = 2U;
constexpr NvU32 present_barrier_sync_cluster = 3U;
constexpr std::size_t maximum_barrier_clients = 16U;
constexpr std::size_t maximum_backbuffers = 4U;

[[nodiscard]] bool is_d3d12_device(const rhi::NativeHandle& handle) noexcept {
    return handle.backend == rhi::BackendKind::D3d12 &&
           handle.object == rhi::NativeObjectKind::Device && handle.value != nullptr;
}

[[nodiscard]] bool is_d3d12_swapchain(
    const rhi::NativeHandle& handle) noexcept {
    return handle.backend == rhi::BackendKind::D3d12 &&
           handle.object == rhi::NativeObjectKind::Swapchain &&
           handle.value != nullptr;
}

[[nodiscard]] bool is_d3d12_image(const rhi::NativeHandle& handle) noexcept {
    return handle.backend == rhi::BackendKind::D3d12 &&
           handle.object == rhi::NativeObjectKind::SwapchainImage &&
           handle.value != nullptr;
}

class NvapiPresentBarrierBackend final : public rhi::PresentBarrierBackend {
public:
    NvapiPresentBarrierBackend() noexcept {
        module_ = ::LoadLibraryW(L"nvapi64.dll");
        if (module_ == nullptr) {
            return;
        }
        const auto query = reinterpret_cast<NvQueryInterface>(
            ::GetProcAddress(module_, "nvapi_QueryInterface"));
        if (query == nullptr || !load_interfaces(query) ||
            api_.initialize() != nv_ok) {
            unload_module();
            return;
        }
        initialized_ = true;
    }

    ~NvapiPresentBarrierBackend() override {
        for (Client& client : clients_) {
            destroy_client(client);
        }
        if (initialized_ && api_.unload != nullptr) {
            static_cast<void>(api_.unload());
        }
        unload_module();
    }

    [[nodiscard]] bool ready() const noexcept { return initialized_; }

    [[nodiscard]] std::string_view name() const noexcept override {
        return "nvapi-d3d12";
    }

    [[nodiscard]] std::string_view detail() const noexcept override {
        return initialized_
            ? "NVAPI D3D12 present barrier is installed; Quadro Sync "
              "support is verified per output device"
            : "NVAPI present-barrier interfaces are unavailable";
    }

    [[nodiscard]] rhi::PresentBarrierSupport support(
        const rhi::NativeHandle& device) const noexcept override {
        if (!initialized_ || !is_d3d12_device(device)) {
            return rhi::PresentBarrierSupport::Unsupported;
        }
        bool supported = false;
        const NvStatus status = api_.query_present_barrier_support(
            static_cast<ID3D12Device*>(device.value), &supported);
        return status == nv_ok && supported
            ? rhi::PresentBarrierSupport::Available
            : rhi::PresentBarrierSupport::Unsupported;
    }

    [[nodiscard]] rhi::PresentBarrierStatus join(
        const rhi::PresentBarrierTarget& target,
        const rhi::PresentBarrierDesc& desc) noexcept override {
        if (!initialized_ || !is_d3d12_device(target.device) ||
            !is_d3d12_swapchain(target.swapchain)) {
            return rhi::PresentBarrierStatus::Unsupported;
        }
        if (!target.valid() || desc.group == 0U || desc.barrier == 0U ||
            target.images.empty() || target.images.size() > maximum_backbuffers) {
            return rhi::PresentBarrierStatus::Failed;
        }
        for (const rhi::NativeHandle& image : target.images) {
            if (!is_d3d12_image(image)) {
                return rhi::PresentBarrierStatus::Failed;
            }
        }

        if (Client* const existing = find(target.swapchain.value)) {
            if (existing->status.load(std::memory_order_acquire) ==
                    rhi::PresentBarrierStatus::Joined &&
                existing->desc == desc) {
                return rhi::PresentBarrierStatus::Joined;
            }
            destroy_client(*existing);
        }

        Client* const client = allocate_client();
        if (client == nullptr) {
            return rhi::PresentBarrierStatus::Failed;
        }
        client->status.store(rhi::PresentBarrierStatus::Joining,
                             std::memory_order_release);
        // Publish the key before native setup. A setup failure deliberately
        // remains addressable as Failed, so the RHI will hold instead of
        // treating a failed native join as a harmless NotJoined state.
        client->presents.store(0U, std::memory_order_relaxed);
        client->presents_in_sync.store(0U, std::memory_order_relaxed);
        client->flips_in_sync.store(0U, std::memory_order_relaxed);
        client->leave_events.store(0U, std::memory_order_relaxed);
        client->key.store(target.swapchain.value, std::memory_order_release);
        client->device = static_cast<ID3D12Device*>(target.device.value);
        client->swapchain = static_cast<IDXGISwapChain*>(target.swapchain.value);
        client->desc = desc;

        bool supported = false;
        NvU32 max_groups{};
        NvU32 max_barriers{};
        if (api_.query_present_barrier_support(client->device, &supported) !=
            nv_ok) {
            fail_client(*client);
            return rhi::PresentBarrierStatus::Failed;
        }
        if (!supported) {
            // A supported NVAPI runtime on a non-Quadro adapter is not a
            // failed membership; this output simply cannot offer a barrier.
            client->status.store(rhi::PresentBarrierStatus::Unsupported,
                                 std::memory_order_release);
            return rhi::PresentBarrierStatus::Unsupported;
        }
        if (api_.query_max_swap_group(client->device, &max_groups,
                                      &max_barriers) != nv_ok ||
            desc.group > max_groups || desc.barrier > max_barriers ||
            api_.join_swap_group(client->device, client->swapchain, desc.group,
                                 FALSE) != nv_ok) {
            fail_client(*client);
            return rhi::PresentBarrierStatus::Failed;
        }
        client->legacy_joined = true;
        if (api_.bind_swap_barrier(client->device, desc.group, desc.barrier) !=
                nv_ok ||
            api_.create_present_barrier_client(
                client->device, client->swapchain, &client->barrier_client) !=
                nv_ok ||
            FAILED(client->device->CreateFence(
                0U, D3D12_FENCE_FLAG_NONE,
                IID_PPV_ARGS(&client->fence)))) {
            fail_client(*client);
            return rhi::PresentBarrierStatus::Failed;
        }
        client->image_count = static_cast<NvU32>(target.images.size());
        for (NvU32 index = 0U; index < client->image_count; ++index) {
            client->images[index] = static_cast<ID3D12Resource*>(
                target.images[index].value);
        }
        if (api_.register_present_barrier_resources(
                client->barrier_client, client->fence, client->images.data(),
                client->image_count) != nv_ok) {
            fail_client(*client);
            return rhi::PresentBarrierStatus::Failed;
        }
        NvJoinPresentBarrierParams parameters{};
        parameters.dwVersion = nv_version<NvJoinPresentBarrierParams>();
        if (api_.join_present_barrier(client->barrier_client, &parameters) !=
                nv_ok) {
            fail_client(*client);
            return rhi::PresentBarrierStatus::Failed;
        }
        client->modern_joined = true;
        client->status.store(rhi::PresentBarrierStatus::Joined,
                             std::memory_order_release);
        return rhi::PresentBarrierStatus::Joined;
    }

    void leave(const rhi::NativeHandle& swapchain) noexcept override {
        if (!is_d3d12_swapchain(swapchain)) {
            return;
        }
        if (Client* const client = find(swapchain.value)) {
            destroy_client(*client);
        }
    }

    [[nodiscard]] rhi::PresentBarrierStatus status(
        const rhi::NativeHandle& swapchain) const noexcept override {
        if (!is_d3d12_swapchain(swapchain)) {
            return rhi::PresentBarrierStatus::Unsupported;
        }
        const Client* const client = find(swapchain.value);
        return client == nullptr
            ? rhi::PresentBarrierStatus::NotJoined
            : client->status.load(std::memory_order_acquire);
    }

    [[nodiscard]] rhi::PresentBarrierStatistics statistics(
        const rhi::NativeHandle& swapchain) const noexcept override {
        const Client* const client = is_d3d12_swapchain(swapchain)
            ? find(swapchain.value)
            : nullptr;
        if (client == nullptr) {
            return {};
        }
        return rhi::PresentBarrierStatistics{
            client->presents.load(std::memory_order_relaxed),
            client->presents_in_sync.load(std::memory_order_relaxed),
            client->flips_in_sync.load(std::memory_order_relaxed),
            client->leave_events.load(std::memory_order_relaxed),
        };
    }

    [[nodiscard]] bool render_ready(
        const rhi::NativeHandle& swapchain,
        const rhi::NativeHandle& image) const noexcept override {
        // DXGI backbuffer reuse is already fenced by the D3D12 RHI. The
        // NVAPI registration fence belongs to the driver; there is no caller
        // supplied target value to wait for. Crucially, do not block here.
        static_cast<void>(image);
        const auto state = status(swapchain);
        return state != rhi::PresentBarrierStatus::Failed &&
               state != rhi::PresentBarrierStatus::Left;
    }

    [[nodiscard]] rhi::PresentBarrierPresentResult present(
        const rhi::NativeHandle& device,
        const rhi::NativeHandle& swapchain,
        const rhi::NativeHandle& image,
        core::u32 sync_interval,
        core::u32 flags) noexcept override {
        if (!is_d3d12_device(device) || !is_d3d12_swapchain(swapchain) ||
            !is_d3d12_image(image)) {
            return rhi::PresentBarrierPresentResult::PassThrough;
        }
        Client* const client = find(swapchain.value);
        if (client == nullptr) {
            return rhi::PresentBarrierPresentResult::PassThrough;
        }
        if (client->status.load(std::memory_order_acquire) !=
            rhi::PresentBarrierStatus::Joined) {
            return rhi::PresentBarrierPresentResult::Failed;
        }
        if (api_.present(static_cast<IUnknown*>(device.value), client->swapchain,
                         static_cast<UINT>(sync_interval),
                         static_cast<UINT>(flags)) != nv_ok) {
            client->status.store(rhi::PresentBarrierStatus::Failed,
                                 std::memory_order_release);
            ++client->leave_events;
            return rhi::PresentBarrierPresentResult::Failed;
        }
        update_statistics(*client);
        return client->status.load(std::memory_order_acquire) ==
                rhi::PresentBarrierStatus::Joined
            ? rhi::PresentBarrierPresentResult::Presented
            : rhi::PresentBarrierPresentResult::Failed;
    }

    [[nodiscard]] rhi::PresentBarrierStatus refresh(
        const rhi::PresentBarrierTarget& target) noexcept override {
        if (!target.valid()) {
            return rhi::PresentBarrierStatus::Failed;
        }
        Client* const client = find(target.swapchain.value);
        if (client == nullptr) {
            return rhi::PresentBarrierStatus::NotJoined;
        }
        const rhi::PresentBarrierDesc desc = client->desc;
        // DXGI replaces all resource pointers on resize. Rebuilding this one
        // client is a configuration event, never a per-frame operation.
        destroy_client(*client);
        return join(target, desc);
    }

    [[nodiscard]] rhi::VideoReferenceSample video_reference(
        const rhi::NativeHandle& device) const noexcept override {
        if (!initialized_ || !is_d3d12_device(device)) {
            return {};
        }
        NvU32 frame_counter{};
        if (api_.query_frame_count(static_cast<IUnknown*>(device.value),
                                   &frame_counter) != nv_ok) {
            return rhi::VideoReferenceSample{rhi::VideoReferenceState::Lost,
                                             0U};
        }
        return rhi::VideoReferenceSample{rhi::VideoReferenceState::Locked,
                                         frame_counter};
    }

private:
    struct Api final {
        NvInitialize initialize{};
        NvUnload unload{};
        NvD3dPresent present{};
        NvQueryFrameCount query_frame_count{};
        NvQueryMaxSwapGroup query_max_swap_group{};
        NvJoinSwapGroup join_swap_group{};
        NvBindSwapBarrier bind_swap_barrier{};
        NvQueryPresentBarrierSupport query_present_barrier_support{};
        NvCreatePresentBarrierClient create_present_barrier_client{};
        NvRegisterPresentBarrierResources register_present_barrier_resources{};
        NvJoinPresentBarrier join_present_barrier{};
        NvLeavePresentBarrier leave_present_barrier{};
        NvDestroyPresentBarrierClient destroy_present_barrier_client{};
        NvQueryPresentBarrierFrameStatistics query_statistics{};
    };

    struct Client final {
        std::atomic<void*> key{};
        ID3D12Device* device{};
        IDXGISwapChain* swapchain{};
        ID3D12Fence* fence{};
        void* barrier_client{};
        std::array<ID3D12Resource*, maximum_backbuffers> images{};
        NvU32 image_count{};
        rhi::PresentBarrierDesc desc{};
        bool legacy_joined{};
        bool modern_joined{};
        std::atomic<rhi::PresentBarrierStatus> status{
            rhi::PresentBarrierStatus::NotJoined};
        std::atomic<core::u64> presents{};
        std::atomic<core::u64> presents_in_sync{};
        std::atomic<core::u64> flips_in_sync{};
        std::atomic<core::u64> leave_events{};
    };

    template <typename Function>
    // `interface` is a macro in the Windows COM headers, so keep this helper's
    // name deliberately distinct from that SDK spelling.
    [[nodiscard]] static Function load_interface(
        NvQueryInterface query, NvU32 id) noexcept {
        return reinterpret_cast<Function>(query(id));
    }

    [[nodiscard]] bool load_interfaces(NvQueryInterface query) noexcept {
        api_.initialize = load_interface<NvInitialize>(query, nvapi_initialize_id);
        api_.unload = load_interface<NvUnload>(query, nvapi_unload_id);
        api_.present = load_interface<NvD3dPresent>(query, nvapi_d3d1x_present_id);
        api_.query_frame_count = load_interface<NvQueryFrameCount>(
            query, nvapi_d3d1x_query_frame_count_id);
        api_.query_max_swap_group = load_interface<NvQueryMaxSwapGroup>(
            query, nvapi_d3d1x_query_max_swap_group_id);
        api_.join_swap_group = load_interface<NvJoinSwapGroup>(
            query, nvapi_d3d1x_join_swap_group_id);
        api_.bind_swap_barrier = load_interface<NvBindSwapBarrier>(
            query, nvapi_d3d1x_bind_swap_barrier_id);
        api_.query_present_barrier_support =
            load_interface<NvQueryPresentBarrierSupport>(
                query, nvapi_d3d12_query_present_barrier_support_id);
        api_.create_present_barrier_client =
            load_interface<NvCreatePresentBarrierClient>(
                query, nvapi_d3d12_create_present_barrier_client_id);
        api_.register_present_barrier_resources =
            load_interface<NvRegisterPresentBarrierResources>(
                query, nvapi_d3d12_register_present_barrier_resources_id);
        api_.destroy_present_barrier_client =
            load_interface<NvDestroyPresentBarrierClient>(
                query, nvapi_destroy_present_barrier_client_id);
        api_.join_present_barrier = load_interface<NvJoinPresentBarrier>(
            query, nvapi_join_present_barrier_id);
        api_.leave_present_barrier = load_interface<NvLeavePresentBarrier>(
            query, nvapi_leave_present_barrier_id);
        api_.query_statistics = load_interface<NvQueryPresentBarrierFrameStatistics>(
            query, nvapi_query_present_barrier_frame_statistics_id);
        return api_.initialize != nullptr && api_.unload != nullptr &&
               api_.present != nullptr && api_.query_frame_count != nullptr &&
               api_.query_max_swap_group != nullptr &&
               api_.join_swap_group != nullptr && api_.bind_swap_barrier != nullptr &&
               api_.query_present_barrier_support != nullptr &&
               api_.create_present_barrier_client != nullptr &&
               api_.register_present_barrier_resources != nullptr &&
               api_.destroy_present_barrier_client != nullptr &&
               api_.join_present_barrier != nullptr &&
               api_.leave_present_barrier != nullptr &&
               api_.query_statistics != nullptr;
    }

    [[nodiscard]] Client* find(void* key) noexcept {
        for (Client& client : clients_) {
            if (client.key.load(std::memory_order_acquire) == key) {
                return &client;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const Client* find(void* key) const noexcept {
        for (const Client& client : clients_) {
            if (client.key.load(std::memory_order_acquire) == key) {
                return &client;
            }
        }
        return nullptr;
    }

    [[nodiscard]] Client* allocate_client() noexcept {
        for (Client& client : clients_) {
            if (client.key.load(std::memory_order_acquire) == nullptr &&
                client.status.load(std::memory_order_acquire) ==
                    rhi::PresentBarrierStatus::NotJoined) {
                return &client;
            }
        }
        return nullptr;
    }

    void update_statistics(Client& client) noexcept {
        NvPresentBarrierFrameStatistics statistics{};
        statistics.dwVersion = nv_version<NvPresentBarrierFrameStatistics>();
        if (api_.query_statistics(client.barrier_client, &statistics) != nv_ok) {
            client.status.store(rhi::PresentBarrierStatus::Failed,
                                std::memory_order_release);
            ++client.leave_events;
            return;
        }
        client.presents.store(statistics.present_count, std::memory_order_relaxed);
        client.presents_in_sync.store(statistics.present_in_sync_count,
                                      std::memory_order_relaxed);
        client.flips_in_sync.store(statistics.flip_in_sync_count,
                                   std::memory_order_relaxed);
        const bool synchronized = statistics.sync_mode ==
                                      present_barrier_sync_system ||
                                  statistics.sync_mode ==
                                      present_barrier_sync_cluster;
        // A joined client that is no longer counted as synchronized is a real
        // loss, even before a later API call formally removes it.
        if (statistics.sync_mode == present_barrier_not_joined || !synchronized ||
            (statistics.present_count != 0U &&
             statistics.present_in_sync_count < statistics.present_count)) {
            client.status.store(rhi::PresentBarrierStatus::Left,
                                std::memory_order_release);
            ++client.leave_events;
        }
    }

    void release_native_client(Client& client) noexcept {
        if (client.modern_joined && client.barrier_client != nullptr) {
            static_cast<void>(api_.leave_present_barrier(client.barrier_client));
        }
        if (client.barrier_client != nullptr) {
            static_cast<void>(api_.destroy_present_barrier_client(
                client.barrier_client));
        }
        if (client.legacy_joined && client.device != nullptr &&
            client.swapchain != nullptr) {
            static_cast<void>(api_.join_swap_group(
                client.device, client.swapchain, 0U, FALSE));
        }
        if (client.fence != nullptr) {
            client.fence->Release();
        }
        client.device = nullptr;
        client.swapchain = nullptr;
        client.fence = nullptr;
        client.barrier_client = nullptr;
        client.images.fill(nullptr);
        client.image_count = 0U;
        client.legacy_joined = false;
        client.modern_joined = false;
    }

    void fail_client(Client& client) noexcept {
        const bool existed = client.key.load(std::memory_order_acquire) != nullptr ||
                             client.barrier_client != nullptr ||
                             client.legacy_joined;
        release_native_client(client);
        if (existed) {
            ++client.leave_events;
        }
        // Keep `key` and `desc` published so status(), acquire(), health, and
        // eventual swapchain destruction all see and clean up the failure.
        client.status.store(rhi::PresentBarrierStatus::Failed,
                            std::memory_order_release);
    }

    void destroy_client(Client& client) noexcept {
        const bool existed = client.key.load(std::memory_order_acquire) != nullptr ||
                             client.barrier_client != nullptr ||
                             client.legacy_joined;
        // Make a concurrent observer see NotJoined before raw native objects
        // are released. Presentation calls are serialized by the RHI thread.
        client.key.store(nullptr, std::memory_order_release);
        release_native_client(client);
        client.desc = {};
        if (existed) {
            ++client.leave_events;
        }
        client.status.store(rhi::PresentBarrierStatus::NotJoined,
                            std::memory_order_release);
    }

    void unload_module() noexcept {
        if (module_ != nullptr) {
            ::FreeLibrary(module_);
            module_ = nullptr;
        }
    }

    HMODULE module_{};
    Api api_{};
    bool initialized_{};
    std::array<Client, maximum_barrier_clients> clients_{};
};

} // namespace

rhi::PresentBarrierBackendHandle
create_nvapi_present_barrier_backend() noexcept {
    try {
        auto backend = std::make_shared<NvapiPresentBarrierBackend>();
        return backend->ready() ? backend : rhi::PresentBarrierBackendHandle{};
    } catch (...) {
        return {};
    }
}

} // namespace mediaserver::app
