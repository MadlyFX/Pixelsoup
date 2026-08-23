#pragma once

#include <mediaserver/core/types.hpp>

// The failure vocabulary shared by the render module's GPU passes.
//
// Both the scene renderer and the output transform can fail for the same
// reasons - no device, no bytecode the device accepts, a bad configuration, an
// allocation that did not land, or a backend that refused - so the enum lives
// here rather than in either pass, and neither has to depend on the other's
// header to name it.
namespace mediaserver::render {

enum class RenderError : core::u8 {
    // No GPU, or a device that could not be used.
    Unavailable,
    // This build carries no shader bytecode the device accepts. Distinct from
    // Unavailable because the fix is a build change, not a machine change.
    ShaderFormatUnsupported,
    InvalidConfiguration,
    AllocationFailed,
    BackendFailure,
};

} // namespace mediaserver::render
