#include "mosaic_layout.hpp"

#include <algorithm>
#include <array>
#include <string>

#if defined(_WIN32) && defined(MEDIASERVER_HAS_NVAPI_RUNTIME)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdint>
#endif

namespace mediaserver::app {

std::vector<net::ClusterMosaicTile>
mosaic_tiles_from_viewports(std::span<const MosaicViewport> viewports,
                            core::u32 surface_width,
                            core::u32 surface_height) {
  std::vector<MosaicViewport> used;
  core::u32 far_right{};
  core::u32 far_bottom{};
  for (const MosaicViewport &viewport : viewports) {
    if (viewport.right <= viewport.left || viewport.bottom <= viewport.top) {
      continue;
    }
    used.push_back(viewport);
    far_right = std::max(far_right, viewport.right);
    far_bottom = std::max(far_bottom, viewport.bottom);
  }
  if (used.size() < 2U || used.size() > net::max_cluster_display_mosaic_tiles) {
    return {};
  }
  // Inclusive edges stop one short of the surface's size.
  const core::u32 extra_x =
      surface_width != 0U && far_right + 1U == surface_width ? 1U : 0U;
  const core::u32 extra_y =
      surface_height != 0U && far_bottom + 1U == surface_height ? 1U : 0U;
  std::stable_sort(used.begin(), used.end(),
                   [](const MosaicViewport &a, const MosaicViewport &b) {
                     return a.top != b.top ? a.top < b.top : a.left < b.left;
                   });
  std::vector<net::ClusterMosaicTile> tiles;
  tiles.reserve(used.size());
  for (const MosaicViewport &viewport : used) {
    tiles.push_back(net::ClusterMosaicTile{
        viewport.left, viewport.top, viewport.right - viewport.left + extra_x,
        viewport.bottom - viewport.top + extra_y});
  }
  return tiles;
}

#if defined(_WIN32) && defined(MEDIASERVER_HAS_NVAPI_RUNTIME)
namespace {

// Kept independent of the NVAPI SDK, like the sync and present-barrier
// backends: the two Mosaic entry points used here, declared from NVIDIA's
// public headers and resolved at runtime.
using NvStatus = std::int32_t;
using NvU32 = std::uint32_t;
using NvU8 = std::uint8_t;

struct NvRect final {
  NvU32 left;
  NvU32 top;
  NvU32 right;
  NvU32 bottom;
};
static_assert(sizeof(NvRect) == 16U);

constexpr NvStatus nv_ok = 0;
constexpr std::size_t nv_mosaic_max_displays = 64U;

using NvQueryInterface = void *(__cdecl *)(NvU32);
using NvInitialize = NvStatus(__cdecl *)();
using NvDispGetDisplayIdByDisplayName = NvStatus(__cdecl *)(const char *,
                                                            NvU32 *);
using NvMosaicGetDisplayViewportsByResolution =
    NvStatus(__cdecl *)(NvU32, NvU32, NvU32, NvRect *, NvU8 *);

// Interface identifiers are published in NVIDIA's nvapi_interface.h.
constexpr NvU32 nvapi_initialize_id = 0x0150e828U;
constexpr NvU32 nvapi_disp_get_display_id_by_name_id = 0xae457190U;
constexpr NvU32 nvapi_mosaic_get_display_viewports_by_resolution_id =
    0xdc6dc8d3U;

struct MosaicApi final {
  NvDispGetDisplayIdByDisplayName display_id_by_name{};
  NvMosaicGetDisplayViewportsByResolution viewports_by_resolution{};

  [[nodiscard]] bool ready() const noexcept {
    return display_id_by_name != nullptr && viewports_by_resolution != nullptr;
  }
};

// Loaded once and kept for the process: the probe asks every few seconds,
// and NVAPI is not unloaded while the sync provider may hold it too.
[[nodiscard]] const MosaicApi &mosaic_api() noexcept {
  static const MosaicApi api = [] {
    MosaicApi loaded{};
    const HMODULE module = ::LoadLibraryW(L"nvapi64.dll");
    if (module == nullptr) {
      return loaded;
    }
    const auto query = reinterpret_cast<NvQueryInterface>(
        ::GetProcAddress(module, "nvapi_QueryInterface"));
    if (query == nullptr) {
      return loaded;
    }
    const auto initialize =
        reinterpret_cast<NvInitialize>(query(nvapi_initialize_id));
    if (initialize == nullptr || initialize() != nv_ok) {
      return loaded;
    }
    loaded.display_id_by_name = reinterpret_cast<NvDispGetDisplayIdByDisplayName>(
        query(nvapi_disp_get_display_id_by_name_id));
    loaded.viewports_by_resolution =
        reinterpret_cast<NvMosaicGetDisplayViewportsByResolution>(
            query(nvapi_mosaic_get_display_viewports_by_resolution_id));
    return loaded;
  }();
  return api;
}

} // namespace
#endif

std::vector<net::ClusterMosaicTile>
query_mosaic_tiles(std::string_view gdi_name, core::u32 surface_width,
                   core::u32 surface_height) noexcept {
#if defined(_WIN32) && defined(MEDIASERVER_HAS_NVAPI_RUNTIME)
  try {
    const MosaicApi &api = mosaic_api();
    if (!api.ready() || gdi_name.empty() || surface_width == 0U ||
        surface_height == 0U) {
      return {};
    }
    const std::string name{gdi_name};
    NvU32 display_id{};
    if (api.display_id_by_name(name.c_str(), &display_id) != nv_ok) {
      return {};
    }
    std::array<NvRect, nv_mosaic_max_displays> rects{};
    NvU8 bezel_corrected{};
    // Any status but OK - NVAPI_MOSAIC_NOT_ACTIVE above all - is a display
    // that is not a Mosaic surface.
    if (api.viewports_by_resolution(display_id, surface_width, surface_height,
                                    rects.data(), &bezel_corrected) != nv_ok) {
      return {};
    }
    std::array<MosaicViewport, nv_mosaic_max_displays> viewports{};
    for (std::size_t index = 0U; index < rects.size(); ++index) {
      viewports[index] = MosaicViewport{rects[index].left, rects[index].top,
                                        rects[index].right,
                                        rects[index].bottom};
    }
    return mosaic_tiles_from_viewports(viewports, surface_width,
                                       surface_height);
  } catch (...) {
    return {};
  }
#else
  static_cast<void>(gdi_name);
  static_cast<void>(surface_width);
  static_cast<void>(surface_height);
  return {};
#endif
}

} // namespace mediaserver::app
