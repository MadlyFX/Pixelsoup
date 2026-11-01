#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/topology/hardware_map.hpp>
#include <mediaserver/topology/hardware_runtime.hpp>

#include <string>
#include <vector>

// LED Wall cards a processor's own report describes.
//
// A processor that reports its screens says how big each one is and at what
// rate it runs. That is a starting point for the map: one wall card per
// screen, cabled from the processor's LED OUT. It is only offered while the
// LED OUT drives nothing, so a wall the operator has cabled, resized or
// deleted and replaced is never second-guessed. Once placed, a wall is an
// ordinary card: the operator may resize, rename or delete it.
//
// This is pure: the same map and report always propose the same walls.
namespace mediaserver::topology {

struct HardwareWallProposal final {
  std::string name;
  core::u32 width{};
  core::u32 height{};
  core::u32 refresh_numerator{};
  core::u32 refresh_denominator{1U};
  core::i32 grid_x{};
  core::i32 grid_y{};

  bool operator==(const HardwareWallProposal &) const = default;
};

// The walls for one processor card, one per reported screen with a raster,
// in the report's order. Empty when the card is not an LED Processor, the
// report is not current, or its LED OUT is already cabled. Each wall stands
// beside the processor, on a cell no other card is near.
[[nodiscard]] std::vector<HardwareWallProposal>
proposed_walls(const HardwareMap &map, const HardwareProcessorNode &report);

} // namespace mediaserver::topology
