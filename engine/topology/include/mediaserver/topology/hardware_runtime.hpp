#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/topology/hardware_map.hpp>

#include <string>
#include <vector>

// Transient observations reported by configured hardware. These values are
// deliberately separate from HardwareMap: they are never document properties,
// never contribute to a project digest, and never make a project dirty.
namespace mediaserver::topology {

enum class HardwareRuntimeReachability : core::u8 {
  Unmonitored,
  Probing,
  Online,
  Offline,
  ConfigurationError,
};

enum class HardwareRuntimeProcessState : core::u8 {
  Unknown,
  Stopped,
  Starting,
  Running,
  Stopping,
  Faulted,
};

// Whether a machine's output inventory is authoritative. Keeping this
// separate from an empty video_outputs vector prevents an unavailable probe
// from being presented as a successfully detected machine with zero outputs.
enum class HardwareRuntimeInventoryState : core::u8 {
  Unknown,
  Available,
  Partial,
  Unsupported,
  Failed,
};

struct HardwareVideoOutput final {
  HardwareDisplayTargetKey target;
  std::string name;
  std::string connector;
  core::u32 width{};
  core::u32 height{};
  core::u32 refresh_numerator{};
  core::u32 refresh_denominator{1U};
  bool active{true};
  bool available{true};

  bool operator==(const HardwareVideoOutput &) const = default;
};

struct HardwareGpuAdapter final {
  std::string name;
  core::u64 dedicated_video_memory_bytes{};
  bool software{};
  bool remote{};

  bool operator==(const HardwareGpuAdapter &) const = default;
};

struct HardwareRuntimeNode final {
  // The Director binds a successfully authenticated machine observation to
  // its configured document object. Network identities never enter the UI as
  // document ids directly.
  doc::NodeId device{};
  HardwareRuntimeReachability reachability{
      HardwareRuntimeReachability::Unmonitored};
  HardwareRuntimeProcessState process{HardwareRuntimeProcessState::Unknown};
  HardwareRuntimeInventoryState inventory{
      HardwareRuntimeInventoryState::Unknown};
  std::string hostname;
  std::string operating_system;
  core::u32 logical_processor_count{};
  core::u64 total_memory_bytes{};
  std::vector<HardwareGpuAdapter> gpu_adapters;
  std::vector<HardwareVideoOutput> video_outputs;
  // A last-known observation retained after contact was lost. The editor may
  // continue drawing its pins, but must distinguish them from current data.
  bool stale{};
  std::string detail;

  bool operator==(const HardwareRuntimeNode &) const = default;
};

struct HardwareRuntimeSnapshot final {
  std::vector<HardwareRuntimeNode> nodes;

  [[nodiscard]] const HardwareRuntimeNode *
  node(doc::NodeId device) const noexcept {
    for (const HardwareRuntimeNode &candidate : nodes) {
      if (candidate.device == device) {
        return &candidate;
      }
    }
    return nullptr;
  }

  bool operator==(const HardwareRuntimeSnapshot &) const = default;
};

} // namespace mediaserver::topology
