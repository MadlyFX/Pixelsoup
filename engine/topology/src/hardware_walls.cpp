#include <mediaserver/topology/hardware_walls.hpp>

#include <mediaserver/topology/hardware_signal.hpp>

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace mediaserver::topology {
namespace {

// A card's sprite covers about one cell around its own, so another card
// closer than this would sit on it.
constexpr core::i32 clearance = 1;

// Cells tried before giving up and stacking on the last one.
constexpr core::i32 max_cell_tries = 64;

[[nodiscard]] bool free_cell(const std::vector<std::pair<core::i32, core::i32>> &taken,
                             core::i32 x, core::i32 y) noexcept {
  return std::none_of(taken.begin(), taken.end(), [&](const auto &cell) {
    return std::abs(cell.first - x) <= clearance &&
           std::abs(cell.second - y) <= clearance;
  });
}

} // namespace

std::vector<HardwareWallProposal>
proposed_walls(const HardwareMap &map, const HardwareProcessorNode &report) {
  std::vector<HardwareWallProposal> walls;
  const HardwareDevice *const processor = map.device(report.device);
  if (processor == nullptr ||
      processor->kind != HardwareDeviceKind::LedProcessor || report.stale ||
      report.reachability != HardwareRuntimeReachability::Online) {
    return walls;
  }
  const bool cabled =
      std::any_of(map.links.begin(), map.links.end(),
                  [&](const HardwareLink &link) {
                    return link.from.device == processor->node &&
                           link.from.port == processor_led_output_port;
                  });
  if (cabled) {
    return walls;
  }

  std::vector<std::pair<core::i32, core::i32>> taken;
  taken.reserve(map.devices.size() + report.screens.size());
  for (const HardwareDevice &device : map.devices) {
    taken.emplace_back(device.grid_x, device.grid_y);
  }
  const std::string base =
      processor->name.empty() ? std::string{"LED wall"} : processor->name + " wall";
  std::size_t unnamed{};
  for (const HardwareProcessorScreen &screen : report.screens) {
    if (screen.width == 0U || screen.height == 0U) {
      continue;
    }
    HardwareWallProposal wall{};
    if (!screen.name.empty()) {
      wall.name = screen.name;
    } else {
      ++unnamed;
      wall.name = unnamed == 1U ? base : base + ' ' + std::to_string(unnamed);
    }
    wall.width = std::min(screen.width, 32'768U);
    wall.height = std::min(screen.height, 32'768U);
    const HardwareRefresh rate = refresh_from_millihertz(screen.refresh_millihertz);
    // A screen that does not say keeps a new card's 60 Hz.
    wall.refresh_numerator = rate.numerator == 0U ? 60U : rate.numerator;
    wall.refresh_denominator = rate.numerator == 0U ? 1U : rate.denominator;
    // Up and to the right of the processor on the map, then further along
    // that row until a cell is clear.
    wall.grid_x = processor->grid_x + 3;
    wall.grid_y = processor->grid_y - 3;
    for (core::i32 attempt = 0; attempt < max_cell_tries; ++attempt) {
      const core::i32 y = processor->grid_y - 3 + attempt * 3;
      if (free_cell(taken, wall.grid_x, y)) {
        wall.grid_y = y;
        break;
      }
    }
    taken.emplace_back(wall.grid_x, wall.grid_y);
    walls.push_back(std::move(wall));
  }
  return walls;
}

} // namespace mediaserver::topology
