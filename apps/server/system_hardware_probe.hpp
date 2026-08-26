#pragma once

#include <mediaserver/net/cluster_lifecycle.hpp>

#include <chrono>
#include <functional>
#include <memory>

namespace mediaserver::app {

struct SystemHardwareProbeConfig final {
  // Platform display enumeration is relatively expensive. The listener only
  // reads an immutable cached snapshot; this worker refreshes it out of band.
  std::chrono::milliseconds refresh_interval{std::chrono::seconds{30}};
};

using SystemHardwareCollectFunction = std::function<net::ClusterMachineInfo()>;

// Performs one local platform query. Unsupported platforms report explicit
// Unavailable/Unsupported states rather than ambiguous zero-valued hardware.
[[nodiscard]] net::ClusterMachineInfo collect_local_machine_info();

class SystemHardwareProbe final : public net::ClusterMachineInfoProvider {
public:
  explicit SystemHardwareProbe(SystemHardwareProbeConfig config = {},
                               SystemHardwareCollectFunction collector = {});
  SystemHardwareProbe(const SystemHardwareProbe &) = delete;
  SystemHardwareProbe &operator=(const SystemHardwareProbe &) = delete;
  SystemHardwareProbe(SystemHardwareProbe &&) = delete;
  SystemHardwareProbe &operator=(SystemHardwareProbe &&) = delete;
  ~SystemHardwareProbe() override;

  [[nodiscard]] std::shared_ptr<const net::ClusterMachineInfo>
  snapshot() noexcept override;

private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace mediaserver::app
