#include "system_hardware_probe.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#endif

namespace mediaserver::app {
namespace {

using net::ClusterDisplayEnumerationState;
using net::ClusterDisplayTargetInfo;
using net::ClusterGpuAdapterInfo;
using net::ClusterMachineInfo;
using net::ClusterMachineInfoAvailability;
using net::ClusterVideoOutputTechnology;

constexpr auto minimum_refresh_interval = std::chrono::seconds{1};
constexpr auto maximum_refresh_interval = std::chrono::minutes{10};

[[nodiscard]] std::chrono::milliseconds
clamped_refresh_interval(std::chrono::milliseconds value) noexcept {
  return std::clamp(value,
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        minimum_refresh_interval),
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        maximum_refresh_interval));
}

[[nodiscard]] ClusterMachineInfo failed_sample(std::string message) {
  ClusterMachineInfo result{};
  result.availability = ClusterMachineInfoAvailability::Failed;
  result.display_state = ClusterDisplayEnumerationState::Failed;
  result.message = std::move(message);
  return result;
}

#if defined(_WIN32)

using Microsoft::WRL::ComPtr;

[[nodiscard]] core::u64 adapter_id(const LUID &luid) noexcept {
  const auto high = static_cast<std::uint32_t>(luid.HighPart);
  return static_cast<core::u64>(luid.LowPart) |
         (static_cast<core::u64>(high) << 32U);
}

[[nodiscard]] bool same_adapter(const LUID &left, const LUID &right) noexcept {
  return left.LowPart == right.LowPart && left.HighPart == right.HighPart;
}

[[nodiscard]] std::string utf8(std::wstring_view value) {
  if (value.empty()) {
    return {};
  }
  if (value.size() >
      static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return {};
  }
  const int wide_size = static_cast<int>(value.size());
  const int required =
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                          wide_size, nullptr, 0, nullptr, nullptr);
  if (required <= 0) {
    return {};
  }
  std::string result(static_cast<std::size_t>(required), '\0');
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                          wide_size, result.data(), required, nullptr,
                          nullptr) != required) {
    return {};
  }
  return result;
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

void append_diagnostic(ClusterMachineInfo &info, std::string_view text) {
  if (!info.message.empty()) {
    info.message += "; ";
  }
  info.message += text;
}

void mark_partial(ClusterMachineInfo &info, std::string_view text) {
  if (info.availability == ClusterMachineInfoAvailability::Available) {
    info.availability = ClusterMachineInfoAvailability::Partial;
  }
  append_diagnostic(info, text);
}

[[nodiscard]] ClusterVideoOutputTechnology
output_technology(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY technology) noexcept {
  switch (technology) {
  case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HD15:
    return ClusterVideoOutputTechnology::Hd15;
  case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_SVIDEO:
    return ClusterVideoOutputTechnology::SVideo;
  case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_COMPOSITE_VIDEO:
    return ClusterVideoOutputTechnology::Composite;
  case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_COMPONENT_VIDEO:
    return ClusterVideoOutputTechnology::Component;
  case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI:
    return ClusterVideoOutputTechnology::Dvi;
  case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI:
    return ClusterVideoOutputTechnology::Hdmi;
  case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS:
    return ClusterVideoOutputTechnology::Lvds;
  case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL:
  case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED:
    return ClusterVideoOutputTechnology::DisplayPort;
  case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED:
  case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL:
    return ClusterVideoOutputTechnology::Internal;
  case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EXTERNAL:
    return ClusterVideoOutputTechnology::Other;
  case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_SDI:
    return ClusterVideoOutputTechnology::Sdi;
  case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_MIRACAST:
    return ClusterVideoOutputTechnology::Miracast;
  case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED:
    return ClusterVideoOutputTechnology::Indirect;
  case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER:
  default:
    return ClusterVideoOutputTechnology::Other;
  }
}

void collect_host_info(ClusterMachineInfo &info) {
  std::array<wchar_t, 256U> name{};
  DWORD name_size = static_cast<DWORD>(name.size());
  if (GetComputerNameExW(ComputerNameDnsHostname, name.data(), &name_size) !=
      FALSE) {
    info.host_name = utf8({name.data(), name_size});
    if (info.host_name.empty()) {
      mark_partial(info, "host name unavailable");
    }
  } else {
    mark_partial(info, "host name unavailable");
  }

  SYSTEM_INFO system{};
  GetNativeSystemInfo(&system);
  info.logical_cpu_count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
  if (info.logical_cpu_count == 0U) {
    info.logical_cpu_count = system.dwNumberOfProcessors;
  }
  if (info.logical_cpu_count == 0U) {
    mark_partial(info, "processor count unavailable");
  }
  switch (system.wProcessorArchitecture) {
  case PROCESSOR_ARCHITECTURE_AMD64:
    info.operating_system = "Windows x64";
    break;
  case PROCESSOR_ARCHITECTURE_ARM64:
    info.operating_system = "Windows ARM64";
    break;
  case PROCESSOR_ARCHITECTURE_INTEL:
    info.operating_system = "Windows x86";
    break;
  default:
    info.operating_system = "Windows";
    break;
  }

  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof(memory);
  if (GlobalMemoryStatusEx(&memory) != FALSE) {
    info.physical_memory_bytes = memory.ullTotalPhys;
    info.available_memory_bytes = memory.ullAvailPhys;
    if (info.physical_memory_bytes == 0U) {
      info.available_memory_bytes = 0U;
      mark_partial(info, "memory status unavailable");
    }
  } else {
    mark_partial(info, "memory status unavailable");
  }
  info.uptime_ms = GetTickCount64();
}

void collect_adapters(ClusterMachineInfo &info) {
  ComPtr<IDXGIFactory1> factory;
  const HRESULT created =
      CreateDXGIFactory1(IID_PPV_ARGS(factory.ReleaseAndGetAddressOf()));
  if (FAILED(created)) {
    mark_partial(info, "DXGI adapter enumeration failed");
    return;
  }

  for (UINT index = 0U;; ++index) {
    ComPtr<IDXGIAdapter1> adapter;
    const HRESULT enumerated = factory->EnumAdapters1(index, &adapter);
    if (enumerated == DXGI_ERROR_NOT_FOUND) {
      break;
    }
    if (FAILED(enumerated)) {
      mark_partial(info, "DXGI adapter enumeration failed");
      break;
    }
    if (info.adapters.size() >= net::max_cluster_machine_gpu_adapters) {
      info.truncated = true;
      break;
    }
    DXGI_ADAPTER_DESC1 description{};
    if (FAILED(adapter->GetDesc1(&description))) {
      mark_partial(info, "DXGI adapter description failed");
      continue;
    }
    ClusterGpuAdapterInfo result{};
    result.adapter_id = adapter_id(description.AdapterLuid);
    result.vendor_id = description.VendorId;
    result.device_id = description.DeviceId;
    result.subsystem_id = description.SubSysId;
    result.revision = description.Revision;
    result.dedicated_video_memory_bytes = description.DedicatedVideoMemory;
    result.shared_system_memory_bytes = description.SharedSystemMemory;
    result.software = (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0U;
    result.remote = (description.Flags & DXGI_ADAPTER_FLAG_REMOTE) != 0U;
    result.name = utf8(terminated_view(description.Description));
    info.adapters.push_back(std::move(result));
  }
  std::ranges::sort(info.adapters, {}, &ClusterGpuAdapterInfo::adapter_id);
}

[[nodiscard]] LONG
query_display_configuration(std::vector<DISPLAYCONFIG_PATH_INFO> &paths,
                            std::vector<DISPLAYCONFIG_MODE_INFO> &modes) {
  constexpr UINT32 flags = QDC_ALL_PATHS | QDC_VIRTUAL_MODE_AWARE;
  constexpr UINT32 max_platform_paths = 4'096U;
  constexpr UINT32 max_platform_modes = 8'192U;
  for (unsigned attempt = 0U; attempt < 3U; ++attempt) {
    UINT32 path_count{};
    UINT32 mode_count{};
    LONG result = GetDisplayConfigBufferSizes(flags, &path_count, &mode_count);
    if (result != ERROR_SUCCESS) {
      return result;
    }
    if (path_count > max_platform_paths || mode_count > max_platform_modes) {
      return ERROR_NOT_ENOUGH_MEMORY;
    }
    paths.resize(path_count);
    modes.resize(mode_count);
    result = QueryDisplayConfig(flags, &path_count, paths.data(), &mode_count,
                                modes.data(), nullptr);
    if (result == ERROR_INSUFFICIENT_BUFFER) {
      continue;
    }
    if (result == ERROR_SUCCESS) {
      paths.resize(path_count);
      modes.resize(mode_count);
    }
    return result;
  }
  return ERROR_INSUFFICIENT_BUFFER;
}

void apply_target_mode(
    ClusterDisplayTargetInfo &target, const DISPLAYCONFIG_PATH_INFO &path,
    std::span<const DISPLAYCONFIG_MODE_INFO> modes) noexcept {
  for (const DISPLAYCONFIG_MODE_INFO &mode : modes) {
    if (mode.infoType != DISPLAYCONFIG_MODE_INFO_TYPE_TARGET ||
        mode.id != path.targetInfo.id ||
        !same_adapter(mode.adapterId, path.targetInfo.adapterId)) {
      continue;
    }
    const auto &signal = mode.targetMode.targetVideoSignalInfo;
    target.width = signal.activeSize.cx;
    target.height = signal.activeSize.cy;
    target.refresh_numerator = signal.vSyncFreq.Numerator;
    target.refresh_denominator = signal.vSyncFreq.Denominator;
    return;
  }
}

void collect_displays(ClusterMachineInfo &info) {
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  const LONG queried = query_display_configuration(paths, modes);
  if (queried != ERROR_SUCCESS) {
    info.display_state = queried == ERROR_ACCESS_DENIED
                             ? ClusterDisplayEnumerationState::AccessDenied
                         : queried == ERROR_NOT_SUPPORTED
                             ? ClusterDisplayEnumerationState::Unsupported
                             : ClusterDisplayEnumerationState::Failed;
    mark_partial(info, queried == ERROR_ACCESS_DENIED
                           ? "display enumeration access denied"
                       : queried == ERROR_NOT_SUPPORTED
                           ? "display enumeration unsupported"
                           : "display enumeration failed");
    return;
  }

  info.display_state = ClusterDisplayEnumerationState::Available;
  const auto path_priority = [](const DISPLAYCONFIG_PATH_INFO &path) {
    const bool active = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0U;
    const bool available = path.targetInfo.targetAvailable != FALSE;
    return active && available ? 0U : active ? 1U : available ? 2U : 3U;
  };
  std::stable_sort(paths.begin(), paths.end(),
                   [&](const DISPLAYCONFIG_PATH_INFO &left,
                       const DISPLAYCONFIG_PATH_INFO &right) {
                     return path_priority(left) < path_priority(right);
                   });
  std::set<std::pair<core::u64, core::u32>> seen;
  for (const DISPLAYCONFIG_PATH_INFO &path : paths) {
    const auto identity = std::pair{adapter_id(path.targetInfo.adapterId),
                                    static_cast<core::u32>(path.targetInfo.id)};
    if (!seen.insert(identity).second) {
      continue;
    }

    ClusterDisplayTargetInfo target{};
    target.adapter_id = identity.first;
    target.target_id = identity.second;
    target.technology = output_technology(path.targetInfo.outputTechnology);
    target.active = (path.flags & DISPLAYCONFIG_PATH_ACTIVE) != 0U;
    target.available = path.targetInfo.targetAvailable != FALSE;
    apply_target_mode(target, path, modes);

    DISPLAYCONFIG_TARGET_DEVICE_NAME device{};
    device.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    device.header.size = sizeof(device);
    device.header.adapterId = path.targetInfo.adapterId;
    device.header.id = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&device.header) == ERROR_SUCCESS) {
      target.connector_instance = device.connectorInstance;
      target.edid_manufacturer_id = device.edidManufactureId;
      target.edid_product_code_id = device.edidProductCodeId;
      target.name = utf8(terminated_view(device.monitorFriendlyDeviceName));
    }
    info.display_targets.push_back(std::move(target));
  }
  std::stable_sort(info.display_targets.begin(), info.display_targets.end(),
                   [](const ClusterDisplayTargetInfo &left,
                      const ClusterDisplayTargetInfo &right) {
                     const auto priority =
                         [](const ClusterDisplayTargetInfo &target) {
                           return target.active && target.available ? 0U
                                  : target.active                   ? 1U
                                  : target.available                ? 2U
                                                                    : 3U;
                         };
                     const auto left_priority = priority(left);
                     const auto right_priority = priority(right);
                     if (left_priority != right_priority) {
                       return left_priority < right_priority;
                     }
                     if (left.adapter_id != right.adapter_id) {
                       return left.adapter_id < right.adapter_id;
                     }
                     return left.target_id < right.target_id;
                   });
  if (info.display_targets.size() > net::max_cluster_machine_display_targets) {
    info.display_targets.resize(net::max_cluster_machine_display_targets);
    info.truncated = true;
  }
}

#endif

} // namespace

ClusterMachineInfo collect_local_machine_info() {
#if defined(_WIN32)
  ClusterMachineInfo result{};
  result.availability = ClusterMachineInfoAvailability::Available;
  result.display_state = ClusterDisplayEnumerationState::Available;
  collect_host_info(result);
  collect_adapters(result);
  collect_displays(result);
  if (result.adapters.empty()) {
    mark_partial(result, "no DXGI adapters reported");
  }
  return result;
#else
  ClusterMachineInfo result{};
  result.availability = ClusterMachineInfoAvailability::Unavailable;
  result.display_state = ClusterDisplayEnumerationState::Unsupported;
  result.message = "machine inventory is unsupported on this platform";
  return result;
#endif
}

struct SystemHardwareProbe::State final {
  State(SystemHardwareProbeConfig config, SystemHardwareCollectFunction collect)
      : interval(clamped_refresh_interval(config.refresh_interval)),
        collector(collect ? std::move(collect)
                          : SystemHardwareCollectFunction{
                                collect_local_machine_info}) {
    publish(collect_once());
    worker = std::thread{[this] { run(); }};
  }

  ~State() {
    {
      const std::lock_guard lock{stop_mutex};
      stop = true;
    }
    stop_condition.notify_all();
    if (worker.joinable()) {
      worker.join();
    }
  }

  [[nodiscard]] ClusterMachineInfo collect_once() {
    try {
      return collector();
    } catch (const std::bad_alloc &) {
      return failed_sample("machine inventory allocation failed");
    } catch (...) {
      return failed_sample("machine inventory collection failed");
    }
  }

  void publish(ClusterMachineInfo value) {
    value.sample_generation = ++generation;
    snapshot.store(std::make_shared<const ClusterMachineInfo>(std::move(value)),
                   std::memory_order_release);
  }

  void run() noexcept {
    std::unique_lock lock{stop_mutex};
    while (!stop_condition.wait_for(lock, interval, [this] { return stop; })) {
      lock.unlock();
      try {
        publish(collect_once());
      } catch (...) {
        // Retain the last immutable sample if allocating a replacement
        // fails; the listener remains responsive and bounded.
      }
      lock.lock();
    }
  }

  std::chrono::milliseconds interval;
  SystemHardwareCollectFunction collector;
  std::atomic<std::shared_ptr<const ClusterMachineInfo>> snapshot;
  core::u64 generation{};
  std::mutex stop_mutex;
  std::condition_variable stop_condition;
  bool stop{};
  std::thread worker;
};

SystemHardwareProbe::SystemHardwareProbe(
    SystemHardwareProbeConfig config, SystemHardwareCollectFunction collector)
    : state_(std::make_unique<State>(config, std::move(collector))) {}

SystemHardwareProbe::~SystemHardwareProbe() = default;

std::shared_ptr<const ClusterMachineInfo>
SystemHardwareProbe::snapshot() noexcept {
  return state_->snapshot.load(std::memory_order_acquire);
}

} // namespace mediaserver::app
