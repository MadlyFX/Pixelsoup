#include "nvapi_sync_device.hpp"

#include "system_hardware_probe.hpp"

#if !defined(_WIN32)
#error "The NVAPI sync-status provider is Windows-only."
#endif

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mediaserver::app {
namespace {

namespace core = mediaserver::core;
namespace net = mediaserver::net;

// Keep this unit independent of the NVAPI SDK, just like the present-barrier
// backend. These ABI declarations are the small G-Sync status subset from
// NVIDIA's public NVAPI headers; functions are resolved at runtime.
using NvStatus = std::int32_t;
using NvU32 = std::uint32_t;
using NvGSyncDeviceHandle = void*;
using NvPhysicalGpuHandle = void*;

constexpr NvStatus nv_ok = 0;
constexpr NvStatus nv_nvidia_device_not_found = -6;
constexpr NvU32 maximum_gsync_devices = 4U;
constexpr NvU32 maximum_gsync_gpus = 16U;
constexpr NvU32 maximum_gsync_displays = 32U;
constexpr NvU32 gsync_display_unsynced = 0U;
constexpr NvU32 gsync_sync_source_vsync = 0U;
constexpr NvU32 gsync_sync_source_house = 1U;

using NvQueryInterface = void* (__cdecl *)(NvU32);
using NvInitialize = NvStatus (__cdecl *)();
using NvUnload = NvStatus (__cdecl *)();
using NvGSyncEnumSyncDevices = NvStatus (__cdecl *)(
    NvGSyncDeviceHandle*, NvU32*);
using NvGSyncGetTopology = NvStatus (__cdecl *)(
    NvGSyncDeviceHandle, NvU32*, void*, NvU32*, void*);
using NvGSyncGetSyncStatus = NvStatus (__cdecl *)(
    NvGSyncDeviceHandle, NvPhysicalGpuHandle, void*);
using NvGSyncGetStatusParameters = NvStatus (__cdecl *)(
    NvGSyncDeviceHandle, void*);
using NvGSyncGetControlParameters = NvStatus (__cdecl *)(
    NvGSyncDeviceHandle, void*);
using NvDispGetDisplayIdByDisplayName = NvStatus (__cdecl *)(
    const char*, NvU32*);
using NvSysGetPhysicalGpuFromDisplayId = NvStatus (__cdecl *)(
    NvU32, NvPhysicalGpuHandle*);

template <typename Value>
[[nodiscard]] constexpr NvU32 nv_version() noexcept {
    return static_cast<NvU32>(sizeof(Value)) | (1U << 16U);
}

// Interface identifiers are published in NVIDIA's nvapi_interface.h.
constexpr NvU32 nvapi_initialize_id = 0x0150e828U;
constexpr NvU32 nvapi_unload_id = 0xd22bdd7eU;
constexpr NvU32 nvapi_disp_get_display_id_by_name_id = 0xae457190U;
constexpr NvU32 nvapi_gsync_enum_sync_devices_id = 0xd9639601U;
constexpr NvU32 nvapi_gsync_get_topology_id = 0x4562bc38U;
constexpr NvU32 nvapi_gsync_get_sync_status_id = 0xf1f5b434U;
constexpr NvU32 nvapi_gsync_get_status_parameters_id = 0x70d404ecU;
constexpr NvU32 nvapi_gsync_get_control_parameters_id = 0x16de1c6aU;
constexpr NvU32 nvapi_sys_get_physical_gpu_from_display_id_id = 0x9ea74659U;

struct NvGSyncGpu final {
    NvU32 version{};
    NvPhysicalGpuHandle physical_gpu{};
    NvU32 connector{};
    NvPhysicalGpuHandle proxy_physical_gpu{};
    NvU32 flags{};
};

struct NvGSyncDisplay final {
    NvU32 version{};
    NvU32 display_id{};
    NvU32 flags{};
    NvU32 sync_state{};
    std::array<NvU32, 20U> reserved{};
};

struct NvGSyncStatus final {
    NvU32 version{};
    NvU32 synchronized{};
    NvU32 stereo_synchronized{};
    NvU32 signal_available{};
};

struct NvGSyncStatusParameters final {
    NvU32 version{};
    NvU32 refresh_rate{};
    std::array<NvU32, 2U> rj45_io{};
    std::array<NvU32, 2U> rj45_ethernet{};
    NvU32 house_sync_incoming{};
    NvU32 house_sync{};
    NvU32 flags{};
};

struct NvGSyncDelay final {
    NvU32 version{};
    NvU32 lines{};
    NvU32 pixels{};
    NvU32 maximum_lines{};
    NvU32 minimum_pixels{};
};

struct NvGSyncControlParameters final {
    NvU32 version{};
    NvU32 polarity{};
    NvU32 video_mode{};
    NvU32 interval{};
    NvU32 source{};
    NvU32 flags{};
    NvGSyncDelay skew{};
    NvGSyncDelay startup_delay{};
    NvU32 multiply_divide_mode{};
    std::uint8_t multiply_divide_value{};
};

static_assert(sizeof(NvGSyncDisplay) == 96U);
static_assert(sizeof(NvGSyncStatus) == 16U);
static_assert(sizeof(NvGSyncStatusParameters) == 36U);
static_assert(sizeof(NvGSyncControlParameters) == 72U);

struct Api final {
    NvInitialize initialize{};
    NvUnload unload{};
    NvGSyncEnumSyncDevices enum_sync_devices{};
    NvGSyncGetTopology get_topology{};
    NvGSyncGetSyncStatus get_sync_status{};
    NvGSyncGetStatusParameters get_status_parameters{};
    NvGSyncGetControlParameters get_control_parameters{};
    NvDispGetDisplayIdByDisplayName get_display_id_by_name{};
    NvSysGetPhysicalGpuFromDisplayId get_physical_gpu_from_display_id{};
};

template <typename Function>
[[nodiscard]] Function load_interface(NvQueryInterface query,
                                      NvU32 id) noexcept {
    return reinterpret_cast<Function>(query(id));
}

[[nodiscard]] core::u64 adapter_id(const LUID& luid) noexcept {
    const auto high = static_cast<std::uint32_t>(luid.HighPart);
    return static_cast<core::u64>(luid.LowPart) |
           (static_cast<core::u64>(high) << 32U);
}

template <std::size_t Extent>
[[nodiscard]] std::wstring_view
terminated_view(const wchar_t (&value)[Extent]) noexcept {
    std::size_t length{};
    while (length < Extent && value[length] != L'\0') {
        ++length;
    }
    return {value, length};
}

[[nodiscard]] std::string ascii(std::wstring_view value) {
    std::string result;
    result.reserve(value.size());
    for (const wchar_t character : value) {
        if (character > 0x7f) {
            return {};
        }
        result.push_back(static_cast<char>(character));
    }
    return result;
}

[[nodiscard]] bool query_display_paths(
    std::vector<DISPLAYCONFIG_PATH_INFO>& paths,
    std::vector<DISPLAYCONFIG_MODE_INFO>& modes) {
    constexpr UINT32 flags = QDC_ONLY_ACTIVE_PATHS | QDC_VIRTUAL_MODE_AWARE;
    for (unsigned attempt = 0U; attempt < 3U; ++attempt) {
        UINT32 path_count{};
        UINT32 mode_count{};
        LONG queried = GetDisplayConfigBufferSizes(flags, &path_count, &mode_count);
        if (queried != ERROR_SUCCESS) {
            return false;
        }
        paths.resize(path_count);
        modes.resize(mode_count);
        queried = QueryDisplayConfig(flags, &path_count, paths.data(),
                                     &mode_count, modes.data(), nullptr);
        if (queried == ERROR_INSUFFICIENT_BUFFER) {
            continue;
        }
        if (queried != ERROR_SUCCESS) {
            return false;
        }
        paths.resize(path_count);
        modes.resize(mode_count);
        return true;
    }
    return false;
}

struct DisplayBinding final {
    NvU32 display_id{};
    net::ClusterDisplayTargetInfo target{};
};

[[nodiscard]] std::vector<DisplayBinding> collect_display_bindings(
    const Api& api) {
    const net::ClusterMachineInfo machine = collect_local_machine_info();
    if (machine.display_state != net::ClusterDisplayEnumerationState::Available) {
        return {};
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    if (!query_display_paths(paths, modes)) {
        return {};
    }

    std::vector<DisplayBinding> bindings;
    bindings.reserve(paths.size());
    for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
        if ((path.flags & DISPLAYCONFIG_PATH_ACTIVE) == 0U) {
            continue;
        }
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) {
            continue;
        }
        const std::string gdi_name =
            ascii(terminated_view(source.viewGdiDeviceName));
        NvU32 display_id{};
        if (gdi_name.empty() ||
            api.get_display_id_by_name(gdi_name.c_str(), &display_id) != nv_ok) {
            continue;
        }
        const core::u64 path_adapter = adapter_id(path.targetInfo.adapterId);
        const core::u32 path_target =
            static_cast<core::u32>(path.targetInfo.id);
        const auto target = std::find_if(
            machine.display_targets.begin(), machine.display_targets.end(),
            [path_adapter, path_target](
                const net::ClusterDisplayTargetInfo& observed) {
                return observed.adapter_id == path_adapter &&
                       observed.target_id == path_target;
            });
        if (target == machine.display_targets.end()) {
            continue;
        }
        const auto duplicate = std::find_if(
            bindings.begin(), bindings.end(),
            [display_id](const DisplayBinding& observed) {
                return observed.display_id == display_id;
            });
        if (duplicate == bindings.end()) {
            bindings.push_back(DisplayBinding{display_id, *target});
        }
    }
    return bindings;
}

[[nodiscard]] const DisplayBinding* find_binding(
    std::span<const DisplayBinding> bindings, NvU32 display_id) noexcept {
    const auto found = std::find_if(
        bindings.begin(), bindings.end(),
        [display_id](const DisplayBinding& observed) {
            return observed.display_id == display_id;
        });
    return found == bindings.end() ? nullptr : &*found;
}

[[nodiscard]] net::ClusterSyncStatus failed_status(
    core::u64 generation, std::string detail) {
    net::ClusterSyncStatus result{};
    result.availability = net::ClusterSyncStatusAvailability::Failed;
    result.board = net::ClusterSyncDeviceAvailability::Failed;
    result.reference = net::ClusterSyncReferenceState::Unknown;
    result.sample_generation = generation;
    result.detail = std::move(detail);
    return result;
}

class NvapiSyncStatusProviderImpl final : public NvapiSyncStatusProvider {
public:
    NvapiSyncStatusProviderImpl() noexcept {
        module_ = ::LoadLibraryW(L"nvapi64.dll");
        if (module_ == nullptr) {
            detail_ = "NVAPI runtime is unavailable";
            return;
        }
        const auto query = reinterpret_cast<NvQueryInterface>(
            ::GetProcAddress(module_, "nvapi_QueryInterface"));
        if (query == nullptr || !load_interfaces(query) ||
            api_.initialize() != nv_ok) {
            detail_ = "NVAPI G-Sync topology interfaces are unavailable";
            unload_module();
            return;
        }
        initialized_ = true;
        detail_ = "NVAPI Quadro Sync topology/status provider is active";
    }

    ~NvapiSyncStatusProviderImpl() override {
        if (initialized_ && api_.unload != nullptr) {
            static_cast<void>(api_.unload());
        }
        unload_module();
    }

    [[nodiscard]] bool ready() const noexcept override { return initialized_; }

    [[nodiscard]] std::string_view detail() const noexcept override {
        return detail_;
    }

    [[nodiscard]] net::ClusterSyncStatus collect(
        core::u64 sample_generation) noexcept override {
        if (!initialized_) {
            net::ClusterSyncStatus result{};
            result.availability = net::ClusterSyncStatusAvailability::Available;
            result.board = net::ClusterSyncDeviceAvailability::Unsupported;
            result.reference = net::ClusterSyncReferenceState::Unsupported;
            result.sample_generation = sample_generation;
            result.detail = detail_;
            return result;
        }
        try {
            return collect_impl(sample_generation);
        } catch (const std::bad_alloc&) {
            return failed_status(sample_generation,
                                 "NVAPI Sync status allocation failed");
        } catch (...) {
            return failed_status(sample_generation,
                                 "NVAPI Sync status collection failed");
        }
    }

private:
    [[nodiscard]] bool load_interfaces(NvQueryInterface query) noexcept {
        api_.initialize = load_interface<NvInitialize>(query, nvapi_initialize_id);
        api_.unload = load_interface<NvUnload>(query, nvapi_unload_id);
        api_.enum_sync_devices = load_interface<NvGSyncEnumSyncDevices>(
            query, nvapi_gsync_enum_sync_devices_id);
        api_.get_topology = load_interface<NvGSyncGetTopology>(
            query, nvapi_gsync_get_topology_id);
        api_.get_sync_status = load_interface<NvGSyncGetSyncStatus>(
            query, nvapi_gsync_get_sync_status_id);
        api_.get_status_parameters = load_interface<NvGSyncGetStatusParameters>(
            query, nvapi_gsync_get_status_parameters_id);
        api_.get_control_parameters =
            load_interface<NvGSyncGetControlParameters>(
                query, nvapi_gsync_get_control_parameters_id);
        api_.get_display_id_by_name =
            load_interface<NvDispGetDisplayIdByDisplayName>(
                query, nvapi_disp_get_display_id_by_name_id);
        api_.get_physical_gpu_from_display_id =
            load_interface<NvSysGetPhysicalGpuFromDisplayId>(
                query, nvapi_sys_get_physical_gpu_from_display_id_id);
        return api_.initialize != nullptr && api_.unload != nullptr &&
               api_.enum_sync_devices != nullptr && api_.get_topology != nullptr &&
               api_.get_sync_status != nullptr &&
               api_.get_status_parameters != nullptr &&
               api_.get_control_parameters != nullptr &&
               api_.get_display_id_by_name != nullptr &&
               api_.get_physical_gpu_from_display_id != nullptr;
    }

    [[nodiscard]] net::ClusterSyncStatus collect_impl(
        core::u64 sample_generation) {
        net::ClusterSyncStatus result{};
        result.availability = net::ClusterSyncStatusAvailability::Available;
        result.board = net::ClusterSyncDeviceAvailability::Present;
        result.reference = net::ClusterSyncReferenceState::Unknown;
        result.sample_generation = sample_generation;

        std::array<NvGSyncDeviceHandle, maximum_gsync_devices> devices{};
        NvU32 device_count{};
        const NvStatus enumerated =
            api_.enum_sync_devices(devices.data(), &device_count);
        if (enumerated == nv_nvidia_device_not_found || device_count == 0U) {
            result.board = net::ClusterSyncDeviceAvailability::Absent;
            result.reference = net::ClusterSyncReferenceState::Absent;
            result.detail = "NVAPI reports no Quadro Sync device";
            return result;
        }
        if (enumerated != nv_ok || device_count > devices.size()) {
            return failed_status(sample_generation,
                                 "NVAPI could not enumerate Quadro Sync devices");
        }

        const std::vector<DisplayBinding> bindings = collect_display_bindings(api_);
        bool any_house_source = false;
        bool any_missing_house_signal = false;
        bool any_unstable = false;
        bool any_non_house_source = false;
        bool mapped_all_displays = true;
        std::set<std::pair<core::u64, core::u32>> emitted_outputs;

        for (NvU32 device_index = 0U; device_index < device_count;
             ++device_index) {
            NvU32 gpu_count{};
            NvU32 display_count{};
            if (api_.get_topology(devices[device_index], &gpu_count, nullptr,
                                  &display_count, nullptr) != nv_ok) {
                return failed_status(sample_generation,
                                     "NVAPI could not query Quadro Sync topology");
            }

            const bool truncated_gpus = gpu_count > maximum_gsync_gpus;
            const bool truncated_displays = display_count > maximum_gsync_displays;
            gpu_count = std::min(gpu_count, maximum_gsync_gpus);
            display_count = std::min(display_count, maximum_gsync_displays);
            result.truncated = result.truncated || truncated_gpus ||
                               truncated_displays;

            std::array<NvGSyncGpu, maximum_gsync_gpus> gpus{};
            std::array<NvGSyncDisplay, maximum_gsync_displays> displays{};
            for (NvU32 index = 0U; index < gpu_count; ++index) {
                gpus[index].version = nv_version<NvGSyncGpu>();
            }
            for (NvU32 index = 0U; index < display_count; ++index) {
                displays[index].version = nv_version<NvGSyncDisplay>();
            }
            if (api_.get_topology(devices[device_index], &gpu_count, gpus.data(),
                                  &display_count, displays.data()) != nv_ok) {
                return failed_status(sample_generation,
                                     "NVAPI could not read Quadro Sync topology");
            }

            NvGSyncStatusParameters status_parameters{};
            status_parameters.version = nv_version<NvGSyncStatusParameters>();
            NvGSyncControlParameters controls{};
            controls.version = nv_version<NvGSyncControlParameters>();
            if (api_.get_status_parameters(devices[device_index],
                                           &status_parameters) != nv_ok ||
                api_.get_control_parameters(devices[device_index], &controls) !=
                    nv_ok) {
                return failed_status(
                    sample_generation,
                    "NVAPI could not query Quadro Sync reference state");
            }

            bool board_has_gpu = false;
            bool board_all_gpus_locked = true;
            for (NvU32 index = 0U; index < gpu_count; ++index) {
                if (gpus[index].physical_gpu == nullptr) {
                    board_all_gpus_locked = false;
                    continue;
                }
                board_has_gpu = true;
                NvGSyncStatus status{};
                status.version = nv_version<NvGSyncStatus>();
                if (api_.get_sync_status(devices[device_index],
                                         gpus[index].physical_gpu, &status) !=
                        nv_ok ||
                    status.synchronized == 0U || status.signal_available == 0U) {
                    board_all_gpus_locked = false;
                }
            }

            const bool house_source = controls.source == gsync_sync_source_house;
            const bool house_signal = status_parameters.house_sync != 0U;
            if (house_source) {
                any_house_source = true;
                if (!house_signal) {
                    any_missing_house_signal = true;
                } else if (!board_has_gpu || !board_all_gpus_locked) {
                    any_unstable = true;
                }
            } else if (controls.source == gsync_sync_source_vsync) {
                any_non_house_source = true;
            } else {
                any_unstable = true;
            }

            for (NvU32 index = 0U; index < display_count; ++index) {
                const DisplayBinding* const binding = find_binding(
                    bindings, displays[index].display_id);
                if (binding == nullptr) {
                    mapped_all_displays = false;
                    continue;
                }
                const auto identity = std::pair{binding->target.adapter_id,
                                                binding->target.target_id};
                if (!emitted_outputs.insert(identity).second) {
                    continue;
                }

                NvPhysicalGpuHandle display_gpu{};
                NvGSyncStatus display_status{};
                display_status.version = nv_version<NvGSyncStatus>();
                const bool status_available =
                    api_.get_physical_gpu_from_display_id(
                        displays[index].display_id, &display_gpu) == nv_ok &&
                    display_gpu != nullptr &&
                    api_.get_sync_status(devices[device_index], display_gpu,
                                         &display_status) == nv_ok;
                const bool locked = displays[index].sync_state !=
                                        gsync_display_unsynced &&
                                    status_available &&
                                    display_status.synchronized != 0U &&
                                    display_status.signal_available != 0U;
                net::ClusterSyncOutputStatus output{};
                output.adapter_id = binding->target.adapter_id;
                output.target_id = binding->target.target_id;
                output.connector_instance = binding->target.connector_instance;
                output.width = binding->target.width;
                output.height = binding->target.height;
                output.refresh_numerator = binding->target.refresh_numerator;
                output.refresh_denominator = binding->target.refresh_denominator;
                output.timing_reported = output.width != 0U &&
                                         output.height != 0U &&
                                         output.refresh_numerator != 0U &&
                                         output.refresh_denominator != 0U;
                output.lock = status_available
                    ? (locked ? net::ClusterSyncLockState::Locked
                              : net::ClusterSyncLockState::Unlocked)
                    : net::ClusterSyncLockState::Unknown;
                if (result.outputs.size() >= net::max_cluster_sync_outputs) {
                    result.truncated = true;
                    break;
                }
                result.outputs.push_back(output);
            }
        }

        if (any_missing_house_signal) {
            result.reference = net::ClusterSyncReferenceState::Absent;
        } else if (any_unstable) {
            result.reference = net::ClusterSyncReferenceState::Unstable;
        } else if (any_house_source && !any_non_house_source) {
            result.reference = net::ClusterSyncReferenceState::InUse;
        } else if (any_non_house_source) {
            // A board can frame-lock to a GPU master without consuming house
            // sync. That is a real lock, but it is not an external camera
            // reference and must not satisfy an external-reference policy.
            result.reference = net::ClusterSyncReferenceState::Present;
        } else {
            result.reference = net::ClusterSyncReferenceState::Unknown;
        }
        result.detail = mapped_all_displays
            ? "NVAPI Quadro Sync topology and reference sampled"
            : "NVAPI Quadro Sync sampled; one or more displays could not be mapped to Windows targets";
        return result;
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
    std::string detail_;
};

} // namespace

std::shared_ptr<NvapiSyncStatusProvider>
create_nvapi_sync_status_provider() noexcept {
    try {
        return std::make_shared<NvapiSyncStatusProviderImpl>();
    } catch (...) {
        return {};
    }
}

} // namespace mediaserver::app
