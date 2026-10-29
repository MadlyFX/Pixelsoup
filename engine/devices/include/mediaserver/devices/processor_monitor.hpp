#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/topology/hardware_map.hpp>
#include <mediaserver/topology/hardware_runtime.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Watches the LED processors on the Hardware Map, on a thread of its own.
//
// The Director hands it the processors to watch every control frame - a
// comparison when nothing changed - and reads back what each last reported.
// Every request to a processor happens here: the control thread never waits
// on a device. A processor that stops answering keeps its last report,
// marked stale, until it answers again or leaves the map.
//
// It also searches the local network for processors, once, when asked: the
// controllers do not announce themselves, so a search tries the API port on
// every host of each local IPv4 subnet.
namespace mediaserver::devices {

struct ProcessorTarget final {
  doc::NodeId device{};
  topology::HardwareProcessorProtocol protocol{
      topology::HardwareProcessorProtocol::Generic};
  std::string address;

  bool operator==(const ProcessorTarget &) const = default;
};

// Sets one input's EDID to one mode. The input is the card's VIDEO IN
// number, as the map shows it.
struct ProcessorEdidWrite final {
  doc::NodeId device{};
  core::u8 input{};
  core::u32 width{};
  core::u32 height{};
  core::u32 refresh_millihertz{};

  bool operator==(const ProcessorEdidWrite &) const = default;
};

struct ProcessorMonitorConfig final {
  std::chrono::milliseconds poll_interval{2'000};
  std::chrono::milliseconds request_timeout{1'500};
  // Where a search looks. Empty means this machine and every host of each
  // local IPv4 subnet, at most 1024 per subnet (the /22 around this
  // machine's address on a larger one).
  std::vector<std::string> search_hosts;
  std::uint16_t search_port{8001U};
  std::chrono::milliseconds search_connect_timeout{400};
  std::size_t search_parallel{96U};
};

struct ProcessorSearchState final {
  std::vector<topology::HardwareDiscoveredProcessor> found;
  bool running{};
  core::u32 completed{};

  bool operator==(const ProcessorSearchState &) const = default;
};

class ProcessorMonitor final {
public:
  explicit ProcessorMonitor(ProcessorMonitorConfig config = {});
  ProcessorMonitor(const ProcessorMonitor &) = delete;
  ProcessorMonitor &operator=(const ProcessorMonitor &) = delete;
  ~ProcessorMonitor();

  void start();
  void stop() noexcept;

  // The processors to watch: every LED Processor card with a protocol and
  // an address. Cards that leave the list are forgotten.
  void watch(std::vector<ProcessorTarget> targets);
  [[nodiscard]] std::vector<topology::HardwareProcessorNode> processors() const;

  void search();
  [[nodiscard]] ProcessorSearchState search_state() const;

  // Queued for the monitor's thread; the outcome lands in the processor's
  // notice, and the processor is read again straight after.
  void write_edid(const ProcessorEdidWrite &write);

  struct State;

private:
  std::unique_ptr<State> state_;
};

// The targets a Hardware Map asks to watch.
[[nodiscard]] std::vector<ProcessorTarget>
processor_targets(const topology::HardwareMap &map);

} // namespace mediaserver::devices
