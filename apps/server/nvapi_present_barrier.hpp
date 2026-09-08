#pragma once

#include <memory>

#include <mediaserver/rhi/device.hpp>

namespace mediaserver::app {

// Creates the Windows/NVIDIA implementation when nvapi64.dll is present and
// the driver exposes the D3D12 present-barrier interfaces. The factory is
// intentionally fail-closed: an ordinary NVIDIA driver or a machine without
// Quadro Sync receives no backend and therefore reports Unsupported.
[[nodiscard]] rhi::PresentBarrierBackendHandle
create_nvapi_present_barrier_backend() noexcept;

} // namespace mediaserver::app
