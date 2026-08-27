#include "sync_device_probe.hpp"

#include "system_hardware_probe.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace mediaserver::app {
namespace {

using net::ClusterSyncDeviceAvailability;
using net::ClusterSyncLockState;
using net::ClusterSyncOutputStatus;
using net::ClusterSyncReferenceState;
using net::ClusterSyncStatus;
using net::ClusterSyncStatusAvailability;

constexpr auto minimum_refresh_interval = std::chrono::milliseconds{250};
constexpr auto maximum_refresh_interval = std::chrono::minutes{5};

[[nodiscard]] std::chrono::milliseconds
clamped_refresh_interval(std::chrono::milliseconds value) noexcept {
  return std::clamp(value, minimum_refresh_interval,
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        maximum_refresh_interval));
}

[[nodiscard]] ClusterSyncStatus failed_sample(std::string detail) {
  ClusterSyncStatus status{};
  status.availability = ClusterSyncStatusAvailability::Failed;
  status.board = ClusterSyncDeviceAvailability::Failed;
  status.reference = ClusterSyncReferenceState::Unknown;
  status.detail = std::move(detail);
  return status;
}

// The answer on every platform this build targets: no vendor sync integration
// is compiled in, so nothing here has queried a sync device. Reporting
// Unsupported rather than Absent keeps the distinction an operator needs -
// "this software cannot see a sync card" is not "this machine has none" - and
// the readiness gate treats both as unproven.
[[nodiscard]] ClusterSyncStatus unsupported_sample() {
  ClusterSyncStatus status{};
  status.availability = ClusterSyncStatusAvailability::Available;
  status.board = ClusterSyncDeviceAvailability::Unsupported;
  status.reference = ClusterSyncReferenceState::Unsupported;
  status.detail = "no vendor sync integration in this build";
  return status;
}

// Simulated samples describe the enumerated display targets of this machine so
// the whole path - hardware map binding, group validation, readiness gate,
// operator surface - is exercised against real target identities rather than
// invented ones.
[[nodiscard]] ClusterSyncStatus simulated_sample(SyncSimulationMode mode) {
  ClusterSyncStatus status{};
  status.availability = ClusterSyncStatusAvailability::Available;
  status.simulated = true;
  switch (mode) {
  case SyncSimulationMode::Locked:
    status.board = ClusterSyncDeviceAvailability::Present;
    status.reference = ClusterSyncReferenceState::InUse;
    status.detail = "simulated sync device, reference in use";
    break;
  case SyncSimulationMode::ReferenceLost:
    status.board = ClusterSyncDeviceAvailability::Present;
    status.reference = ClusterSyncReferenceState::Absent;
    status.loss_of_sync_events = 1U;
    status.detail = "simulated sync device, reference absent";
    break;
  case SyncSimulationMode::ReferenceIdle:
    status.board = ClusterSyncDeviceAvailability::Present;
    status.reference = ClusterSyncReferenceState::Present;
    status.detail = "simulated sync device, reference present but not in use";
    break;
  case SyncSimulationMode::NoDevice:
    status.board = ClusterSyncDeviceAvailability::Absent;
    status.reference = ClusterSyncReferenceState::Absent;
    status.detail = "simulated machine with no sync device";
    return status;
  case SyncSimulationMode::Disabled:
    return unsupported_sample();
  }

  const net::ClusterMachineInfo machine = collect_local_machine_info();
  const bool locked = mode == SyncSimulationMode::Locked;
  for (const net::ClusterDisplayTargetInfo &target : machine.display_targets) {
    if (!target.active) {
      continue;
    }
    if (status.outputs.size() >= net::max_cluster_sync_outputs) {
      status.truncated = true;
      break;
    }
    ClusterSyncOutputStatus output{};
    output.adapter_id = target.adapter_id;
    output.target_id = target.target_id;
    output.connector_instance = target.connector_instance;
    output.width = target.width;
    output.height = target.height;
    output.refresh_numerator = target.refresh_numerator;
    output.refresh_denominator = target.refresh_denominator;
    output.timing_reported = target.width != 0U && target.height != 0U &&
                             target.refresh_numerator != 0U &&
                             target.refresh_denominator != 0U;
    if (!output.timing_reported) {
      output.width = 0U;
      output.height = 0U;
      output.refresh_numerator = 0U;
      output.refresh_denominator = 0U;
    }
    output.lock =
        locked ? ClusterSyncLockState::Locked : ClusterSyncLockState::Unlocked;
    status.outputs.push_back(output);
  }
  return status;
}

} // namespace

std::optional<SyncSimulationMode>
parse_sync_simulation(std::string_view text) noexcept {
  if (text == "off" || text == "disabled") {
    return SyncSimulationMode::Disabled;
  }
  if (text == "locked") {
    return SyncSimulationMode::Locked;
  }
  if (text == "reference-lost") {
    return SyncSimulationMode::ReferenceLost;
  }
  if (text == "reference-idle") {
    return SyncSimulationMode::ReferenceIdle;
  }
  if (text == "no-device") {
    return SyncSimulationMode::NoDevice;
  }
  return std::nullopt;
}

std::string_view sync_simulation_name(SyncSimulationMode mode) noexcept {
  switch (mode) {
  case SyncSimulationMode::Disabled:
    return "off";
  case SyncSimulationMode::Locked:
    return "locked";
  case SyncSimulationMode::ReferenceLost:
    return "reference-lost";
  case SyncSimulationMode::ReferenceIdle:
    return "reference-idle";
  case SyncSimulationMode::NoDevice:
    return "no-device";
  }
  return "off";
}

net::ClusterSyncStatus
collect_local_sync_status(SyncSimulationMode simulation,
                          core::u64 sample_generation) {
  ClusterSyncStatus status = simulation == SyncSimulationMode::Disabled
                                 ? unsupported_sample()
                                 : simulated_sample(simulation);
  status.sample_generation = sample_generation;
  return status;
}

bool SyncDeviceProbe::vendor_support_available() noexcept {
  // Set by a vendor integration that implements ClusterSyncStatusProvider.
  // Nothing in this build does, and saying so plainly is the point.
  return false;
}

struct SyncDeviceProbe::State final {
  explicit State(SyncDeviceProbeConfig configured)
      : interval(clamped_refresh_interval(configured.refresh_interval)),
        simulation(configured.simulation) {
    publish();
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

  void publish() {
    ClusterSyncStatus value{};
    try {
      value = collect_local_sync_status(simulation, ++generation);
    } catch (const std::bad_alloc &) {
      value = failed_sample("sync status allocation failed");
      value.sample_generation = generation;
    } catch (...) {
      value = failed_sample("sync status collection failed");
      value.sample_generation = generation;
    }
    snapshot.store(std::make_shared<const ClusterSyncStatus>(std::move(value)),
                   std::memory_order_release);
  }

  void run() noexcept {
    std::unique_lock lock{stop_mutex};
    while (!stop_condition.wait_for(lock, interval, [this] { return stop; })) {
      lock.unlock();
      try {
        publish();
      } catch (...) {
        // Retain the last immutable sample. A listener that answers with a
        // slightly stale sample is far better than one that stops answering.
      }
      lock.lock();
    }
  }

  std::chrono::milliseconds interval;
  SyncSimulationMode simulation{SyncSimulationMode::Disabled};
  std::atomic<std::shared_ptr<const ClusterSyncStatus>> snapshot;
  core::u64 generation{};
  std::mutex stop_mutex;
  std::condition_variable stop_condition;
  bool stop{};
  std::thread worker;
};

SyncDeviceProbe::SyncDeviceProbe(SyncDeviceProbeConfig config)
    : state_(std::make_unique<State>(config)) {}

SyncDeviceProbe::~SyncDeviceProbe() = default;

std::shared_ptr<const ClusterSyncStatus> SyncDeviceProbe::snapshot() noexcept {
  return state_->snapshot.load(std::memory_order_acquire);
}

} // namespace mediaserver::app
