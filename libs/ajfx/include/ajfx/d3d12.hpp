#pragma once

#include <ajfx/gpu/device.hpp>
#include <ajfx/result.hpp>

#include <memory>

// The D3D12 reference backend (target ajfx::d3d12, Windows only).
namespace ajfx::d3d12 {

struct DeviceOptions final {
    // Use the WARP software rasterizer instead of a hardware adapter.
    bool warp{false};
    // Enable the D3D12 debug layer when it is installed.
    bool debug_layer{false};
};

// A headless device on the first hardware adapter with shader model 6.2, or
// on WARP when asked for.
[[nodiscard]] Result<std::unique_ptr<gpu::Device>> create_device(const DeviceOptions& options = {});

// A compiler backed by dxcompiler.dll (and dxil.dll, which signs the output),
// loaded from the executable's directory or the DLL search path.
[[nodiscard]] Result<std::unique_ptr<gpu::ShaderCompiler>> create_shader_compiler();

} // namespace ajfx::d3d12
