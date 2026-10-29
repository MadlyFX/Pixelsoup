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

// A rectangle in an LED processor's own pixel space: an input's picture or
// its screen.
struct HardwarePixelRect final {
  core::u32 x{};
  core::u32 y{};
  core::u32 width{};
  core::u32 height{};

  bool operator==(const HardwarePixelRect &) const = default;
};

// One video input of an LED processor, as the processor itself reports it.
struct HardwareProcessorInput final {
  // The VIDEO IN number (1-based) the input is shown as on its card. Inputs
  // are numbered in the order the processor lists them, internal test
  // sources left out.
  core::u8 number{};
  // The processor's own id for the input, which addresses it in requests.
  core::i64 vendor_id{};
  std::string name;
  std::string connector;
  // A source is connected and locked.
  bool signal{};
  // What arrives, while there is a signal.
  core::u32 width{};
  core::u32 height{};
  core::u32 refresh_millihertz{};
  // The mode the input's EDID offers whatever drives it. Zero width when
  // the processor does not say.
  core::u32 edid_width{};
  core::u32 edid_height{};
  core::u32 edid_refresh_millihertz{};
  // The processor accepts a new EDID for this input.
  bool edid_writable{};

  bool operator==(const HardwareProcessorInput &) const = default;
};

// Where part of one input's picture lands on a processor's screen.
struct HardwareProcessorLayer final {
  // The VIDEO IN number that feeds it; zero for a source that is not one of
  // the inputs (an internal pattern).
  core::u8 input{};
  // The part of the input picture it shows, in input pixels.
  HardwarePixelRect source;
  // Where that lands, in screen pixels.
  HardwarePixelRect screen;

  bool operator==(const HardwareProcessorLayer &) const = default;
};

// One LED screen a processor drives: its whole raster, the cabinets it is
// built from, and which input pixels fill it.
struct HardwareProcessorScreen final {
  std::string id;
  std::string name;
  core::u32 width{};
  core::u32 height{};
  core::u32 cabinets{};
  core::u32 refresh_millihertz{};
  std::vector<HardwareProcessorLayer> layers;

  bool operator==(const HardwareProcessorScreen &) const = default;
};

// What a monitored LED processor reported. Like a render machine's
// observation this is never document data.
struct HardwareProcessorNode final {
  doc::NodeId device{};
  HardwareRuntimeReachability reachability{
      HardwareRuntimeReachability::Unmonitored};
  // The last good report, kept after contact was lost.
  bool stale{};
  std::string model;
  // The processor's own name for itself.
  std::string name;
  // Why it is not online, or what it refused.
  std::string detail;
  // The outcome of the last action asked of it, such as an EDID write.
  std::string notice;
  bool notice_failed{};
  std::vector<HardwareProcessorInput> inputs;
  std::vector<HardwareProcessorScreen> screens;

  [[nodiscard]] const HardwareProcessorInput *
  input(core::u8 number) const noexcept {
    for (const HardwareProcessorInput &candidate : inputs) {
      if (candidate.number == number) {
        return &candidate;
      }
    }
    return nullptr;
  }

  bool operator==(const HardwareProcessorNode &) const = default;
};

// An LED processor that answered a search of the local network.
struct HardwareDiscoveredProcessor final {
  HardwareProcessorProtocol protocol{HardwareProcessorProtocol::NovastarCoex};
  std::string name;
  std::string model;
  // Where it answered: what a card's Address should hold.
  std::string address;

  bool operator==(const HardwareDiscoveredProcessor &) const = default;
};

// A render machine that answered discovery on the local network. Like every
// value here it is an observation, never document data: placing one on the
// map authors an ordinary Render Server card from these values.
struct HardwareDiscoveredMachine final {
  std::string hostname;
  // The numeric IPv4 address a card's Address should hold.
  std::string address;
  // Every address it has recently answered from, for recognising its card.
  std::vector<std::string> addresses;
  core::u32 node_index{};
  // The TCP port its lifecycle agent listens on.
  core::u32 control_port{};
  bool password_set{};
  // It is the machine this editor runs on.
  bool this_machine{};

  bool operator==(const HardwareDiscoveredMachine &) const = default;
};

enum class HardwareDiscoveryState : core::u8 {
  // Not running here (tests, headless runs): no section is shown.
  Off,
  Searching,
  // It could not start; `detail` says why.
  Unavailable,
};

struct HardwareDiscovery final {
  HardwareDiscoveryState state{HardwareDiscoveryState::Off};
  // The UDP port render machines answer on, for the firewall hint.
  core::u32 port{};
  // Whether this editor controls render machines with a password, so a
  // machine that would reject it can say so before it is placed.
  bool password_set{};
  std::string detail;
  // Sorted by hostname, node index, then address, so cards stay put.
  std::vector<HardwareDiscoveredMachine> machines;
  // LED processors are not asked every few seconds: a search sweeps the
  // local subnets once, when the operator asks. These answered the last one,
  // sorted by address.
  std::vector<HardwareDiscoveredProcessor> processors;
  // This editor can search for processors at all (a Director with its
  // processor monitor running), whether or not render machine discovery is.
  bool processor_search_available{};
  // A processor search is under way, and how many have finished.
  bool processor_search_running{};
  core::u32 processor_searches{};

  bool operator==(const HardwareDiscovery &) const = default;
};

struct HardwareRuntimeSnapshot final {
  std::vector<HardwareRuntimeNode> nodes;
  // One per monitored LED processor card.
  std::vector<HardwareProcessorNode> processors;
  HardwareDiscovery discovery;

  [[nodiscard]] const HardwareRuntimeNode *
  node(doc::NodeId device) const noexcept {
    for (const HardwareRuntimeNode &candidate : nodes) {
      if (candidate.device == device) {
        return &candidate;
      }
    }
    return nullptr;
  }

  [[nodiscard]] const HardwareProcessorNode *
  processor(doc::NodeId device) const noexcept {
    for (const HardwareProcessorNode &candidate : processors) {
      if (candidate.device == device) {
        return &candidate;
      }
    }
    return nullptr;
  }

  bool operator==(const HardwareRuntimeSnapshot &) const = default;
};

} // namespace mediaserver::topology
