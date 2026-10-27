#pragma once

#include <mediaserver/core/types.hpp>

#include <memory>
#include <optional>

struct ID3D12Device;
struct ID3D12Resource;

namespace mediaserver::render::detail {

// A half-size copy of a decoded video surface, on the device the decoder wrote
// it on. `owner` keeps the surface alive and returns it to a small pool when
// the last reference goes, so a playing clip reuses a handful of surfaces
// rather than allocating one per frame.
struct HalvedVideoSurface final {
    ID3D12Resource *resource{};
    std::shared_ptr<void> owner;
    core::u32 width{};
    core::u32 height{};
};

// Writes the 2x2 mean of a decoded NV12 or P010 surface into a new surface of
// the same format and half the size, and waits for the GPU to finish, so the
// caller can release the decoder's surface the moment this returns. Called on
// a decode worker, never on a render or control thread: it runs on queues of
// its own and does not touch the renderer's command recording.
//
// `source` must be idle (the decoder's fence already waited) and in COMMON;
// it is read through implicit promotion and decays back, as the renderer
// reads it. The result is left in COMMON for the renderer to import as plane
// views. The picture must halve to an even extent, as NV12 and P010 require;
// anything else, or any D3D12 failure, returns nothing and the caller keeps
// the full-size surface.
[[nodiscard]] std::optional<HalvedVideoSurface> halve_video_surface(
    ID3D12Device *device, ID3D12Resource *source, core::u32 width,
    core::u32 height, bool ten_bit) noexcept;

} // namespace mediaserver::render::detail
