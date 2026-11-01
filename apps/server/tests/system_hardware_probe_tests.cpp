#include "mosaic_layout.hpp"
#include "system_hardware_probe.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace app = mediaserver::app;
namespace net = mediaserver::net;
using namespace std::chrono_literals;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define PROBE_CHECK(expression)                                                \
  check(static_cast<bool>(expression), #expression, __LINE__)

void requests_read_a_cached_immutable_sample() {
  std::atomic_size_t collections{};
  app::SystemHardwareProbe probe{
      app::SystemHardwareProbeConfig{10min}, [&] {
        const auto call =
            collections.fetch_add(1U, std::memory_order_relaxed) + 1U;
        net::ClusterMachineInfo value{};
        value.availability = net::ClusterMachineInfoAvailability::Available;
        value.display_state = net::ClusterDisplayEnumerationState::Available;
        value.host_name = "cache-test-" + std::to_string(call);
        return value;
      }};

  const auto first = probe.snapshot();
  const auto second = probe.snapshot();
  const auto third = probe.snapshot();
  PROBE_CHECK(first != nullptr);
  PROBE_CHECK(first == second);
  PROBE_CHECK(second == third);
  PROBE_CHECK(first->host_name == "cache-test-1");
  PROBE_CHECK(first->sample_generation == 1U);
  PROBE_CHECK(collections.load(std::memory_order_relaxed) == 1U);
}

void collection_failures_publish_an_explicit_state() {
  app::SystemHardwareProbe probe{
      app::SystemHardwareProbeConfig{10min}, []() -> net::ClusterMachineInfo {
        throw std::runtime_error{"simulated probe failure"};
      }};
  const auto sample = probe.snapshot();
  PROBE_CHECK(sample != nullptr);
  PROBE_CHECK(sample->availability ==
              net::ClusterMachineInfoAvailability::Failed);
  PROBE_CHECK(sample->display_state ==
              net::ClusterDisplayEnumerationState::Failed);
  PROBE_CHECK(!sample->message.empty());
  PROBE_CHECK(sample->sample_generation == 1U);
}

void platform_collection_is_bounded_and_unambiguous() {
  const auto sample = app::collect_local_machine_info();
  PROBE_CHECK(sample.adapters.size() <= net::max_cluster_machine_gpu_adapters);
  PROBE_CHECK(sample.display_targets.size() <=
              net::max_cluster_machine_display_targets);
#if defined(_WIN32)
  PROBE_CHECK(
      sample.availability == net::ClusterMachineInfoAvailability::Available ||
      sample.availability == net::ClusterMachineInfoAvailability::Partial);
  PROBE_CHECK(sample.display_state !=
              net::ClusterDisplayEnumerationState::Unsupported);
  PROBE_CHECK(!sample.host_name.empty());
  PROBE_CHECK(!sample.operating_system.empty());
  PROBE_CHECK(sample.logical_cpu_count != 0U);
  PROBE_CHECK(sample.uptime_ms != 0U);
  PROBE_CHECK(sample.physical_memory_bytes != 0U);
  PROBE_CHECK(sample.available_memory_bytes <= sample.physical_memory_bytes);
#else
  PROBE_CHECK(sample.availability ==
              net::ClusterMachineInfoAvailability::Unavailable);
  PROBE_CHECK(sample.display_state ==
              net::ClusterDisplayEnumerationState::Unsupported);
  PROBE_CHECK(!sample.message.empty());
#endif
}

// NVIDIA reports a 2x2 Mosaic of 1920x1080 displays as rectangles in grid
// order, with unused slots zeroed; the tiles come out row by row whichever
// edge convention the driver uses.
void mosaic_viewports_become_tiles_row_by_row() {
  const std::vector<app::MosaicViewport> exclusive{
      {1920U, 1080U, 3840U, 2160U}, {0U, 0U, 1920U, 1080U},
      {0U, 0U, 0U, 0U},            {1920U, 0U, 3840U, 1080U},
      {0U, 1080U, 1920U, 2160U}};
  const std::vector<net::ClusterMosaicTile> expected{
      {0U, 0U, 1920U, 1080U},
      {1920U, 0U, 1920U, 1080U},
      {0U, 1080U, 1920U, 1080U},
      {1920U, 1080U, 1920U, 1080U}};
  PROBE_CHECK(app::mosaic_tiles_from_viewports(exclusive, 3840U, 2160U) ==
              expected);

  const std::vector<app::MosaicViewport> inclusive{
      {0U, 0U, 1919U, 1079U}, {1920U, 0U, 3839U, 1079U},
      {0U, 1080U, 1919U, 2159U}, {1920U, 1080U, 3839U, 2159U}};
  PROBE_CHECK(app::mosaic_tiles_from_viewports(inclusive, 3840U, 2160U) ==
              expected);

  // One display is an ordinary output, not a grid.
  const std::vector<app::MosaicViewport> single{{0U, 0U, 3840U, 2160U}};
  PROBE_CHECK(app::mosaic_tiles_from_viewports(single, 3840U, 2160U).empty());

  // Without NVAPI in the build, nothing is ever claimed to be Mosaic.
#if !defined(MEDIASERVER_HAS_NVAPI_RUNTIME)
  PROBE_CHECK(app::query_mosaic_tiles(R"(\\.\DISPLAY1)", 3840U, 2160U).empty());
#endif
}

} // namespace

int main() {
  const std::vector<std::pair<const char *, void (*)()>> tests{
      {"snapshots are cached", &requests_read_a_cached_immutable_sample},
      {"collection failures are explicit",
       &collection_failures_publish_an_explicit_state},
      {"platform collection is bounded",
       &platform_collection_is_bounded_and_unambiguous},
      {"mosaic viewports become tiles row by row",
       &mosaic_viewports_become_tiles_row_by_row},
  };
  std::size_t failures{};
  for (const auto &[name, test] : tests) {
    try {
      test();
      std::cout << "[pass] " << name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[fail] " << name << ": " << error.what() << '\n';
    }
  }
  std::cout << tests.size() - failures << '/' << tests.size()
            << " tests passed\n";
  return failures == 0U ? 0 : 1;
}
