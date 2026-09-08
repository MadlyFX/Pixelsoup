#pragma once

#include <mediaserver/core/render_conformance.hpp>
#include <mediaserver/rhi/device.hpp>
#include <memory>

namespace mediaserver::render {

// Optional diagnostic tap. Only the isolated conformance recording supplies
// one; production requests leave it null. Copies are queued at the pass
// boundary, before reusable targets can be overwritten by later passes.
struct ConformanceCapture final {
    void* context{};
    bool (*capture)(void*, rhi::CommandList&, core::ConformancePass, const rhi::Texture&){};
    [[nodiscard]] bool record(rhi::CommandList& commands, core::ConformancePass pass,
                              const rhi::Texture& texture) const noexcept {
        return capture && capture(context, commands, pass, texture);
    }
};

struct ConformanceRun final {
    core::RenderConformance evidence{};
    // Sum of sixteen GPU timestamp intervals. Readbacks are included when
    // capture_bytes is true; benchmarking disables them on the same fixture.
    core::u64 gpu_nanoseconds{};
    core::u64 environment_gpu_nanoseconds{};
};

[[nodiscard]] ConformanceRun run_render_conformance(rhi::Device& device,
    core::u64 request = 0U, bool capture_bytes = true) noexcept;

// One persistent worker, constructed outside the control loop. Rendering,
// allocation, waiting and hashing happen on its private D3D12 device. The
// expected adapter identity must match the actual Program rendering device.
// poll uses try_lock and returns the caller's previous snapshot while busy.
// A failed run is sticky until an explicit new request or adapter change.
class RenderConformanceService final {
public:
    struct Stats final { core::u64 requests_coalesced{}, stale_results{}; };
    RenderConformanceService();
    ~RenderConformanceService();
    RenderConformanceService(const RenderConformanceService&) = delete;
    RenderConformanceService& operator=(const RenderConformanceService&) = delete;
    void poll(const core::RenderAdapterIdentity& adapter, core::u64 request,
              core::RenderConformance& result) noexcept;
    [[nodiscard]] Stats stats() const noexcept;
private:
    struct State;
    std::unique_ptr<State> state_;
};
} // namespace mediaserver::render
