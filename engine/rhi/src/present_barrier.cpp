#include <mediaserver/rhi/device.hpp>

#include <atomic>
#include <memory>

// The process-wide native presentation backend registry.
//
// It is deliberately a separate translation unit from the SDL_GPU backend:
// joining a vendor present barrier requires a native D3D12 (or Vulkan)
// presentation path, which SDL_GPU does not expose, so the barrier backend
// must be installable from outside the RHI without the RHI naming a vendor.
//
// With nothing installed every query answers Unsupported. That is the honest
// answer for the current SDL_GPU presentation path, and the readiness gate
// above treats it as SWAP BARRIER INCOMPLETE rather than as success.
namespace mediaserver::rhi {
namespace {

std::atomic<PresentBarrierBackendHandle>& backend_slot() noexcept {
    static std::atomic<PresentBarrierBackendHandle> slot;
    return slot;
}

} // namespace

void install_present_barrier_backend(PresentBarrierBackendHandle backend)
    noexcept {
    backend_slot().store(std::move(backend), std::memory_order_release);
}

PresentBarrierBackendHandle acquire_present_barrier_backend() noexcept {
    return backend_slot().load(std::memory_order_acquire);
}

} // namespace mediaserver::rhi
