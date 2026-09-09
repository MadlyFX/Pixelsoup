#include <mediaserver/render/frame_graph.hpp>
#include <mediaserver/render/graph_transient_pool.hpp>
#include <algorithm>
#include <array>
#include <iostream>
#include <stdexcept>

namespace rhi = mediaserver::rhi;
namespace render = mediaserver::render;
namespace core = mediaserver::core;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
}
int main() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) return 77;
    try {
        auto made = rhi::Device::create({.debug = true, .backend = rhi::BackendKind::D3d12});
        check(static_cast<bool>(made), "device"); auto device = std::move(made).value();
        const rhi::TextureDesc desc{2048U, 2048U, rhi::TextureFormat::Rgba16Float, true, true};
        auto created = device.create_texture(desc); check(static_cast<bool>(created), "target");
        auto texture = std::move(created).value();
        auto buffer = device.create_storage_buffer(64U); check(static_cast<bool>(buffer), "upload buffer");
        render::FrameGraph graph;
        auto measure = [&](unsigned count) {
            graph.begin();
            auto acquired = device.acquire_commands(); check(static_cast<bool>(acquired), "commands");
            auto commands = std::move(acquired).value();
            check(commands.upload_bytes() == 0U, "upload count carried over a recycled recording");
            const std::array<core::u8, 32U> payload{1U, 3U, 7U};
            check(commands.upload(buffer.value(), payload), "upload");
            for (unsigned i = 0; i < count; ++i)
                check(graph.clear(commands, core::RenderPass::Shadows, texture,
                    {static_cast<float>(i & 1U), 0.25F, 0.5F, 1.0F}), "heavy pass");
            check(graph.clear(commands, core::RenderPass::Sky, texture, {0.25F, 0.5F, 0.75F, 1}), "light pass");
            auto submitted = device.submit(std::move(commands)); check(static_cast<bool>(submitted), "submit");
            auto work = std::move(submitted).value();
            // Uncompleted results are never advertised as valid. Completion
            // may race this observation, which is legal and must remain safe.
            const auto early = work.frame_timing();
            if (!early.sequence) check(!early.gpu_valid, "unfenced sample valid");
            check(work.wait(), "wait"); const auto timing = work.frame_timing();
            check(timing.gpu_valid && timing.sequence != 0U && timing.gpu_ns > 0U, "timestamps missing");
            check(timing.upload_bytes >= payload.size(), "submission dropped upload byte count");
            check(timing.pass_ns[static_cast<std::size_t>(core::RenderPass::Sky)] > 0U, "sky timestamp missing");
            check(timing.pass_ns[static_cast<std::size_t>(core::RenderPass::Shadows)] > 0U, "shadow timestamp missing");
            check(timing.gpu_ns >= timing.pass_ns[static_cast<std::size_t>(core::RenderPass::Shadows)], "interval outside submission");
            check(graph.stats().passes == count + 1U && graph.stats().refusals == 0U, "graph count");
            return timing;
        };
        static_cast<void>(measure(1U)); // warm GPU and descriptors
        std::array<core::u64, 5> light{}, heavy{};
        for (std::size_t i = 0; i < light.size(); ++i) {
            light[i] = measure(1U).pass_ns[static_cast<std::size_t>(core::RenderPass::Shadows)];
            const auto result = measure(64U);
            heavy[i] = result.pass_ns[static_cast<std::size_t>(core::RenderPass::Shadows)];
            check(result.slowest_pass() == core::RenderPass::Shadows, "cost attributed to wrong pass");
        }
        std::sort(light.begin(), light.end()); std::sort(heavy.begin(), heavy.end());
        check(heavy[2] > light[2] * 4U, "heavy pass did not increase its measured cost");
        std::cout << "[pass] median shadow GPU ns: 1 clear=" << light[2] << ", 64 clears=" << heavy[2] << '\n';

        // Sealed pools only reuse construction allocations, including on a
        // descriptor miss. Exhaustion is visible and cannot grow the pool.
        render::GraphTransientPool pool{{1U}};
        check(pool.adopt(std::move(texture)), "pool adoption"); pool.seal(); pool.begin_frame();
        check(static_cast<bool>(pool.acquire(device, desc)), "pool reuse");
        check(!pool.acquire(device, desc), "pool allowed double acquisition");
        pool.begin_frame(); auto changed = desc; changed.width = 1024U;
        check(!pool.acquire(device, changed), "sealed pool allocated");
        check(pool.stats().textures == 1U && pool.stats().refusals == 1U, "pool refusal unreported");

        // A graph-cancelled list must release its texture state claims, while
        // timing data from the discarded recording must never become a sample.
        graph.begin();
        {
            auto canceled = device.acquire_commands(); check(static_cast<bool>(canceled), "cancel acquire");
            check(graph.clear(canceled.value(), core::RenderPass::FogComposite, pool.texture(0U), {1,0,0,1}), "cancel graph");
        }
        graph.begin();
        auto acquired = device.acquire_commands(); check(static_cast<bool>(acquired), "reuse canceled context");
        auto commands = std::move(acquired).value();
        check(graph.clear(commands, core::RenderPass::Sky, pool.texture(0U), {0,1,0,1}), "canceled claim retained");
        // Bound query exhaustion reports incomplete measurements without
        // changing image work or resolving uninitialized query slots.
        for (unsigned i = 0; i < 4100U; ++i) {
            auto token = commands.begin_gpu_pass(core::RenderPass::Other);
            commands.end_gpu_pass(token);
        }
        auto submitted = device.submit(std::move(commands)); check(static_cast<bool>(submitted), "overflow submit");
        check(submitted.value().wait(), "overflow wait");
        const auto overflow = submitted.value().frame_timing();
        check(!overflow.gpu_valid && overflow.query_overflows > 0U, "overflow not reported");
        check(overflow.pass_ns[static_cast<std::size_t>(core::RenderPass::FogComposite)] == 0U, "canceled timestamp leaked");
        std::cout << "[pass] pooled reuse, cancellation, query capacity and fenced observations\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[fail] " << error.what() << '\n'; return 1;
    }
}
