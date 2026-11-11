#pragma once

#include <memory>

#include <mediaserver/rhi/device.hpp>

namespace mediaserver::app {

// Creates the Windows/NVIDIA implementation when nvapi64.dll is present and
// the driver exposes the D3D12 present-barrier interfaces. A machine with no
// NVIDIA driver receives no backend. Any NVIDIA driver exposes the interfaces,
// so a GeForce host does receive one: it then answers per device that the
// adapter supports no present barrier and has no reference frame counter.
[[nodiscard]] rhi::PresentBarrierBackendHandle
create_nvapi_present_barrier_backend() noexcept;

} // namespace mediaserver::app
