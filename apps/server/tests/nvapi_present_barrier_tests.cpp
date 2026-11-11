#include "nvapi_present_barrier.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <span>
#include <utility>

namespace {

namespace app = mediaserver::app;
namespace rhi = mediaserver::rhi;

void check(bool condition, const char *expression, int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression
                  << '\n';
        std::exit(1);
    }
}

#define BARRIER_CHECK(...)                                                     \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

// An NVIDIA adapter with no swap groups - every GeForce - is asked to join a
// present barrier whenever a project's sync policy is FRAMELOCK or EXTERNAL
// GENLOCK. It cannot, and has to say so; what it must not do is stop drawing.
//
// This drives the real driver, so it only has something to say on a machine
// with an NVIDIA driver whose adapter does not support the barrier. Anywhere
// else it reports why it skipped. The swapchain and image handles are
// stand-ins: on an unsupported adapter the backend never dereferences them.
int an_adapter_without_a_barrier_reports_it_and_keeps_presenting() {
    auto backend = app::create_nvapi_present_barrier_backend();
    if (backend == nullptr) {
        std::cout << "[skip] no NVIDIA driver on this machine\n";
        return 0;
    }
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        std::cout << "[skip] no D3D12 device on this machine\n";
        return 0;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto created = rhi::Device::create(config);
    if (!created) {
        std::cout << "[skip] the D3D12 device could not be created\n";
        return 0;
    }
    auto device = std::move(created).value();
    const rhi::NativeHandle native_device = device.native_handle();
    if (backend->support(native_device) ==
        rhi::PresentBarrierSupport::Available) {
        std::cout << "[skip] this adapter supports a present barrier; "
                     "joining one needs a real swapchain\n";
        return 0;
    }

    int swapchain_stand_in{};
    int image_stand_in{};
    const rhi::NativeHandle swapchain{rhi::BackendKind::D3d12,
                                      rhi::NativeObjectKind::Swapchain,
                                      &swapchain_stand_in};
    const rhi::NativeHandle image{rhi::BackendKind::D3d12,
                                  rhi::NativeObjectKind::SwapchainImage,
                                  &image_stand_in};
    const std::array<rhi::NativeHandle, 1U> images{image};
    const rhi::PresentBarrierTarget target{
        native_device, swapchain, std::span<const rhi::NativeHandle>{images}};

    // Before anything is asked of it the swapchain is an ordinary output.
    BARRIER_CHECK(backend->status(swapchain) ==
                  rhi::PresentBarrierStatus::NotJoined);
    BARRIER_CHECK(backend->present(native_device, swapchain, image, 0U, 0U) ==
                  rhi::PresentBarrierPresentResult::PassThrough);

    // Asked to join group 3, barrier 2, it answers that it cannot, and goes
    // on answering that - which is what Output Monitor reports.
    BARRIER_CHECK(backend->join(target, rhi::PresentBarrierDesc{3U, 2U}) ==
                  rhi::PresentBarrierStatus::Unsupported);
    BARRIER_CHECK(backend->status(swapchain) ==
                  rhi::PresentBarrierStatus::Unsupported);
    BARRIER_CHECK(backend->statistics(swapchain) ==
                  rhi::PresentBarrierStatistics{});

    // The swapchain was never joined to anything, so its frames still go to
    // the ordinary present path: neither held back nor refused.
    BARRIER_CHECK(backend->render_ready(swapchain, image));
    for (int frame = 0; frame < 3; ++frame) {
        BARRIER_CHECK(
            backend->present(native_device, swapchain, image, 0U, 0U) ==
            rhi::PresentBarrierPresentResult::PassThrough);
    }
    BARRIER_CHECK(backend->status(swapchain) ==
                  rhi::PresentBarrierStatus::Unsupported);

    // No swap groups means no swap-group frame counter to pace a show from.
    BARRIER_CHECK(backend->video_reference(native_device).state ==
                  rhi::VideoReferenceState::Unsupported);

    // Dropping the sync policy forgets the request.
    backend->leave(swapchain);
    BARRIER_CHECK(backend->status(swapchain) ==
                  rhi::PresentBarrierStatus::NotJoined);
    BARRIER_CHECK(backend->present(native_device, swapchain, image, 0U, 0U) ==
                  rhi::PresentBarrierPresentResult::PassThrough);
    std::cout << "[pass] an adapter without a barrier reports it and keeps "
                 "presenting\n";
    return 0;
}

} // namespace

int main() {
    return an_adapter_without_a_barrier_reports_it_and_keeps_presenting();
}
