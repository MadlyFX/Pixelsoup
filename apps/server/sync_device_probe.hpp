#pragma once

#include <mediaserver/net/cluster_lifecycle.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string_view>

namespace mediaserver::app {

// Explicitly requested fake sync hardware.
//
// Genlock cards and a house reference are not present on most development or
// CI machines, and the operator procedure around REFERENCE LOST is exactly the
// procedure that most needs rehearsing. A simulation is therefore a first
// class, opt-in mode - and every sample it produces is marked `simulated` all
// the way to the operator surface, where it can satisfy a DEGRADED policy but
// never a STRICT one. Simulated evidence is not evidence.
enum class SyncSimulationMode : core::u8 {
  // No simulation. The probe reports whatever the platform supports, which on
  // a build with no vendor integration is Unsupported.
  Disabled,
  // A sync device exists, a reference is present and in use, every enumerated
  // display target reports locked.
  Locked,
  // A sync device exists but the house reference has gone away.
  ReferenceLost,
  // A sync device exists and the reference is present but not disciplined to.
  ReferenceIdle,
  // No sync device on this machine.
  NoDevice,
};

[[nodiscard]] std::optional<SyncSimulationMode>
parse_sync_simulation(std::string_view text) noexcept;
[[nodiscard]] std::string_view
sync_simulation_name(SyncSimulationMode mode) noexcept;

struct SyncDeviceProbeConfig final {
  SyncSimulationMode simulation{SyncSimulationMode::Disabled};
  // Vendor sync queries are cheap compared with display enumeration, but they
  // are still platform calls. The listener reads an immutable cached sample;
  // this worker refreshes it out of band on the same contract as
  // SystemHardwareProbe.
  std::chrono::milliseconds refresh_interval{std::chrono::seconds{1}};
};

// The machine-local physical output-timing provider.
//
// This is the seam a vendor integration implements. NVIDIA G-Sync (via the
// NVAPI GSync topology/status functions) and the AMD equivalent are further
// implementations of net::ClusterSyncStatusProvider installed in place of, or
// beside, this one; nothing above the interface names a vendor.
//
// The built-in implementation reports Unsupported on every platform, because
// this build carries no vendor sync integration. That answer is deliberate:
// an unqueried sync device must never be reported as locked, and the Program
// readiness gate treats Unsupported as SYNC UNVERIFIED rather than as success.
class SyncDeviceProbe final : public net::ClusterSyncStatusProvider {
public:
  explicit SyncDeviceProbe(SyncDeviceProbeConfig config = {});
  SyncDeviceProbe(const SyncDeviceProbe &) = delete;
  SyncDeviceProbe &operator=(const SyncDeviceProbe &) = delete;
  SyncDeviceProbe(SyncDeviceProbe &&) = delete;
  SyncDeviceProbe &operator=(SyncDeviceProbe &&) = delete;
  ~SyncDeviceProbe() override;

  [[nodiscard]] std::shared_ptr<const net::ClusterSyncStatus>
  snapshot() noexcept override;

  // True when this build carries a vendor sync integration at all. False is
  // the current answer everywhere and is reported to the operator verbatim.
  [[nodiscard]] static bool vendor_support_available() noexcept;

private:
  struct State;
  std::unique_ptr<State> state_;
};

// One local sample. Exposed so a test can exercise the exact values the
// listener would serve without starting a service.
[[nodiscard]] net::ClusterSyncStatus
collect_local_sync_status(SyncSimulationMode simulation,
                          core::u64 sample_generation);

} // namespace mediaserver::app
