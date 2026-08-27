#include "sync_device_probe.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

namespace {

namespace app = mediaserver::app;
namespace net = mediaserver::net;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    std::cerr << "line " << line << ": check failed: " << expression << '\n';
    std::exit(1);
  }
}

#define PROBE_CHECK(...)                                                       \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

// The whole point of this provider is that it never guesses. With no vendor
// integration compiled in it must say Unsupported, which the readiness gate
// reads as SYNC UNVERIFIED rather than as an absence it can ignore.
void an_unintegrated_build_reports_unsupported_never_locked() {
  PROBE_CHECK(!app::SyncDeviceProbe::vendor_support_available());
  const net::ClusterSyncStatus status =
      app::collect_local_sync_status(app::SyncSimulationMode::Disabled, 3U);
  PROBE_CHECK(status.availability ==
              net::ClusterSyncStatusAvailability::Available);
  PROBE_CHECK(status.board ==
              net::ClusterSyncDeviceAvailability::Unsupported);
  PROBE_CHECK(status.reference ==
              net::ClusterSyncReferenceState::Unsupported);
  PROBE_CHECK(status.outputs.empty());
  PROBE_CHECK(!status.simulated);
  PROBE_CHECK(status.sample_generation == 3U);
  PROBE_CHECK(!status.detail.empty());
}

// Simulated samples describe the workflow, never the hardware, so every one
// of them is marked simulated all the way to the operator surface.
void every_simulated_sample_is_marked_simulated() {
  const app::SyncSimulationMode modes[] = {
      app::SyncSimulationMode::Locked, app::SyncSimulationMode::ReferenceLost,
      app::SyncSimulationMode::ReferenceIdle,
      app::SyncSimulationMode::NoDevice};
  for (const app::SyncSimulationMode mode : modes) {
    const net::ClusterSyncStatus status =
        app::collect_local_sync_status(mode, 1U);
    PROBE_CHECK(status.availability ==
                net::ClusterSyncStatusAvailability::Available);
    PROBE_CHECK(status.simulated);
    PROBE_CHECK(!status.detail.empty());
  }

  const net::ClusterSyncStatus locked =
      app::collect_local_sync_status(app::SyncSimulationMode::Locked, 1U);
  PROBE_CHECK(locked.board == net::ClusterSyncDeviceAvailability::Present);
  PROBE_CHECK(locked.reference == net::ClusterSyncReferenceState::InUse);
  for (const net::ClusterSyncOutputStatus &output : locked.outputs) {
    PROBE_CHECK(output.lock == net::ClusterSyncLockState::Locked);
  }

  const net::ClusterSyncStatus lost = app::collect_local_sync_status(
      app::SyncSimulationMode::ReferenceLost, 1U);
  PROBE_CHECK(lost.board == net::ClusterSyncDeviceAvailability::Present);
  PROBE_CHECK(lost.reference == net::ClusterSyncReferenceState::Absent);
  PROBE_CHECK(lost.loss_of_sync_events != 0U);
  for (const net::ClusterSyncOutputStatus &output : lost.outputs) {
    PROBE_CHECK(output.lock == net::ClusterSyncLockState::Unlocked);
  }

  const net::ClusterSyncStatus idle = app::collect_local_sync_status(
      app::SyncSimulationMode::ReferenceIdle, 1U);
  PROBE_CHECK(idle.reference == net::ClusterSyncReferenceState::Present);

  const net::ClusterSyncStatus absent =
      app::collect_local_sync_status(app::SyncSimulationMode::NoDevice, 1U);
  PROBE_CHECK(absent.board == net::ClusterSyncDeviceAvailability::Absent);
  PROBE_CHECK(absent.outputs.empty());
}

void simulation_modes_round_trip_their_operator_names() {
  const app::SyncSimulationMode modes[] = {
      app::SyncSimulationMode::Disabled, app::SyncSimulationMode::Locked,
      app::SyncSimulationMode::ReferenceLost,
      app::SyncSimulationMode::ReferenceIdle,
      app::SyncSimulationMode::NoDevice};
  for (const app::SyncSimulationMode mode : modes) {
    const auto parsed =
        app::parse_sync_simulation(app::sync_simulation_name(mode));
    PROBE_CHECK(parsed.has_value());
    PROBE_CHECK(*parsed == mode);
  }
  PROBE_CHECK(!app::parse_sync_simulation("").has_value());
  PROBE_CHECK(!app::parse_sync_simulation("locked ").has_value());
  PROBE_CHECK(!app::parse_sync_simulation("genlocked").has_value());
  PROBE_CHECK(app::parse_sync_simulation("disabled") ==
              app::SyncSimulationMode::Disabled);
}

// The listener only ever reads an immutable cached sample, so the provider
// must publish one before it starts serving and keep replacing it out of band.
void the_probe_publishes_an_immutable_sample_immediately() {
  app::SyncDeviceProbeConfig config{};
  config.simulation = app::SyncSimulationMode::Locked;
  config.refresh_interval = std::chrono::milliseconds{250};
  app::SyncDeviceProbe probe{config};
  const auto first = probe.snapshot();
  PROBE_CHECK(first != nullptr);
  PROBE_CHECK(first->simulated);
  PROBE_CHECK(first->sample_generation != 0U);

  const auto second = probe.snapshot();
  PROBE_CHECK(second == first);
  std::this_thread::sleep_for(std::chrono::milliseconds{700});
  const auto refreshed = probe.snapshot();
  PROBE_CHECK(refreshed != nullptr);
  PROBE_CHECK(refreshed->sample_generation > first->sample_generation);
  // The earlier sample is still valid: it is immutable and shared, never
  // rewritten under a reader.
  PROBE_CHECK(first->simulated);
}

} // namespace

int main() {
  an_unintegrated_build_reports_unsupported_never_locked();
  every_simulated_sample_is_marked_simulated();
  simulation_modes_round_trip_their_operator_names();
  the_probe_publishes_an_immutable_sample_immediately();
  std::cout << "4/4 sync device probe tests passed\n";
  return 0;
}
