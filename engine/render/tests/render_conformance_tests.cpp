#include <mediaserver/render/render_conformance.hpp>
#include <mediaserver/render/frame_graph.hpp>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <thread>
#include <vector>

namespace core = mediaserver::core;
namespace rhi = mediaserver::rhi;
namespace render = mediaserver::render;

int main(int argc, char** argv) {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        std::cout << "[skip] D3D12 unavailable\n"; return 77;
    }
    rhi::DeviceConfig config{}; config.backend = rhi::BackendKind::D3d12;
    auto device = rhi::Device::create(config); if (!device) return 1;
    const bool benchmark = argc > 1 && std::string_view{argv[1]} == "--benchmark";
    const auto first = render::run_render_conformance(device.value(), 7U);
    if (!first.evidence.complete()) {
        std::cerr << "[fail] fixture: " << core::conformance_detail({core::ConformanceMismatch::Failed, first.evidence.failed_pass}) << '\n'; return 1;
    }
    std::cout << "adapter=" << first.evidence.adapter.name.data() << " driver=" << first.evidence.adapter.driver_version
              << " strict=" << first.evidence.strict_float << " bundle=";
    for (auto byte : first.evidence.shader_bundle) std::cout << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
    std::cout << std::dec << '\n';
    for (std::size_t i = 0; i < core::conformance_pass_count; ++i)
        std::cout << core::conformance_pass_names[i] << '=' << std::hex << first.evidence.passes[i] << std::dec << '\n';
    if (benchmark) {
        std::vector<core::u64> times;
        std::vector<core::u64> environment_times;
        for (unsigned i = 0; i < 13; ++i) {
            const auto run = render::run_render_conformance(device.value(), 7U, false);
            if (run.evidence.state != core::ConformanceState::Passed || !run.gpu_nanoseconds) return 1;
            if (i >= 3) {
                times.push_back(run.gpu_nanoseconds);
                environment_times.push_back(run.environment_gpu_nanoseconds);
            }
        }
        std::sort(times.begin(), times.end());
        std::cout << "GPU sixteen frames ns min=" << times.front() << " median=" << (times[4] + times[5]) / 2U
                  << " max=" << times.back() << " (10 runs, 3 warmups, no readbacks; creation and IBL setup excluded)\n";
        std::sort(environment_times.begin(), environment_times.end());
        std::cout << "GPU environment ns min=" << environment_times.front() << " median="
                  << (environment_times[4] + environment_times[5]) / 2U << " max=" << environment_times.back()
                  << " (upload and full IBL prefilter; no readbacks)\n";
        return 0;
    }
    render::FrameGraph::reference_recording_for_tests(true);
    const auto reference = render::run_render_conformance(device.value(), 7U);
    render::FrameGraph::reference_recording_for_tests(false);
    if (!reference.evidence.complete() || first.evidence.passes != reference.evidence.passes) {
        std::cerr << "[fail] graph migration changed a fixture product\n"; return 1;
    }
    std::cout << "[pass] graph equals reference for every product over sixteen frames\n";
    const auto second = render::run_render_conformance(device.value(), 9U);
    if (!second.evidence.complete() || first.evidence.passes != second.evidence.passes) {
        std::cerr << "[fail] repeated fixture differs\n"; return 1;
    }
    auto altered = first.evidence;
    altered.passes[static_cast<std::size_t>(core::ConformancePass::Fog)] ^= 1U;
    const auto comparison = core::compare_render_conformance(first.evidence, altered);
    if (comparison.reason != core::ConformanceMismatch::Pass || comparison.pass != core::ConformancePass::Fog) return 1;
    // Production worker: current request only, actual Program adapter binding,
    // sticky failure, and an explicit retry rather than timer-driven reruns.
    render::RenderConformanceService service;
    core::RenderConformance observed{};
    const auto await_report = [&](const core::RenderAdapterIdentity& adapter, core::u64 request) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
        do {
            service.poll(adapter, request, observed);
            if (observed.state == core::ConformanceState::Passed || observed.state == core::ConformanceState::Failed) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    };
    service.poll(first.evidence.adapter, 42U, observed);
    if (observed.state != core::ConformanceState::Running || observed.request != 42U) return 1;
    if (!await_report(first.evidence.adapter, 42U) || !observed.complete() || observed.passes != first.evidence.passes) return 1;
    auto wrong_adapter = first.evidence.adapter; ++wrong_adapter.driver_version;
    if (!await_report(wrong_adapter, 43U) || observed.state != core::ConformanceState::Failed) return 1;
    for (unsigned i = 0; i < 20U; ++i) {
        service.poll(wrong_adapter, 43U, observed);
        if (observed.state != core::ConformanceState::Failed || observed.adapter != wrong_adapter) return 1;
    }
    service.poll(first.evidence.adapter, 44U, observed);
    service.poll(first.evidence.adapter, 45U, observed);
    if (observed.complete()) return 1;
    if (!await_report(first.evidence.adapter, 45U) || !observed.complete() || observed.request != 45U
        || observed.passes != first.evidence.passes) return 1;
    std::cout << "[pass] all pass digests repeat across fresh renderers and request generations\n";
    return 0;
}
