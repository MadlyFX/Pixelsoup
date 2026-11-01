#pragma once

#include <mediaserver/net/cluster_lifecycle.hpp>

#include <span>
#include <string_view>
#include <vector>

// Which physical displays an NVIDIA Mosaic surface spans, and where each sits
// in it. Windows sees a Mosaic surface as one display, so the render machine
// asks the NVIDIA driver; the Director uses the tiles to tell which part of
// the one output each cabled LED processor receives.
namespace mediaserver::app {

// One display's rectangle as NVAPI reports it, edges in surface pixels.
struct MosaicViewport final {
  core::u32 left{};
  core::u32 top{};
  core::u32 right{};
  core::u32 bottom{};
};

// The displays of a surface `surface_width` x `surface_height`, row by row
// (top, then left). Empty rectangles are unused grid slots and are skipped.
// NVAPI's right and bottom edges may be inclusive or exclusive depending on
// the driver; the edge that reaches the surface's far side decides which.
// Fewer than two displays is no Mosaic at all, and reports nothing.
[[nodiscard]] std::vector<net::ClusterMosaicTile>
mosaic_tiles_from_viewports(std::span<const MosaicViewport> viewports,
                            core::u32 surface_width, core::u32 surface_height);

// The tiles of the Mosaic surface Windows names `gdi_name` (`\\.\DISPLAY1`),
// whose desktop is `surface_width` x `surface_height`. Empty when the display
// is not a Mosaic surface, the NVIDIA driver cannot say, or this build has no
// NVAPI support.
[[nodiscard]] std::vector<net::ClusterMosaicTile>
query_mosaic_tiles(std::string_view gdi_name, core::u32 surface_width,
                   core::u32 surface_height) noexcept;

} // namespace mediaserver::app
