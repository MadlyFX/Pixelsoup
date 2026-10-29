#include "check.hpp"

#include <ajfx/d3d12.hpp>

#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

// The D3D12 backend on its own, with the debug layer on. A separate program
// because enabling the debug layer removes any device created before it.
namespace {

using namespace ajfx;

std::unique_ptr<gpu::Device> g_device;

std::vector<std::uint8_t> pattern(std::size_t count, std::uint8_t seed) {
    std::vector<std::uint8_t> bytes(count);
    for (std::size_t i = 0U; i < count; ++i) {
        bytes[i] = static_cast<std::uint8_t>(seed + i * 7U);
    }
    return bytes;
}

void writes_copies_and_reads_round_trip() {
    gpu::Device& d = *g_device;
    const gpu::Buffer a = d.create_buffer(300U, "a").value();
    const gpu::Buffer b = d.create_buffer(300U, "b").value();
    // New buffers read back as zeros.
    AJFX_CHECK(d.read_buffer(a, 0U, 300U).value() == std::vector<std::uint8_t>(300U, 0U));

    const std::vector<std::uint8_t> data = pattern(200U, 3U);
    d.write_buffer(a, 40U, data);
    d.copy_buffer(b, 0U, a, 40U, 200U);
    const Result<std::vector<std::uint8_t>> read = d.read_buffer(b, 0U, 200U);
    AJFX_CHECK(read.ok() && read.value() == data);
    AJFX_CHECK(d.read_buffer(a, 40U, 200U).value() == data);
    d.destroy_buffer(a);
    d.destroy_buffer(b);
    d.destroy_buffer(gpu::Buffer{}); // ignored
}

void large_writes_span_upload_pages() {
    gpu::Device& d = *g_device;
    const std::vector<std::uint8_t> big = pattern(3U << 20U, 11U);
    const gpu::Buffer buffer = d.create_buffer(big.size(), "big").value();
    d.write_buffer(buffer, 0U, std::span{big}.first(1000U));
    d.write_buffer(buffer, 1000U, std::span{big}.subspan(1000U));
    AJFX_CHECK(d.read_buffer(buffer, 0U, big.size()).value() == big);
    d.destroy_buffer(buffer);
}

void invalid_use_is_reported_and_the_device_recovers() {
    gpu::Device& d = *g_device;
    const gpu::Buffer a = d.create_buffer(64U, "a").value();
    const gpu::Buffer b = d.create_buffer(64U, "b").value();

    // Only the debug layer stops an out-of-bounds copy from reaching the GPU.
    if (LoadLibraryW(L"d3d12SDKLayers.dll") != nullptr) {
        d.copy_buffer(a, 0U, b, 32U, 64U); // reads past the end of b
        const Result<bool> failed = d.flush();
        AJFX_CHECK(!failed.ok() && failed.error().code == ErrorCode::DeviceFailure);
        AJFX_CHECK(failed.error().message.find("CopyBufferRegion") != std::string::npos);
    } else {
        std::cout << "  (debug layer not installed; skipping the out-of-bounds copy)\n";
    }

    d.dispatch(gpu::Dispatch{.pipeline = gpu::Pipeline{9999U}});
    const Result<bool> unknown = d.flush();
    AJFX_CHECK(!unknown.ok() && unknown.error().message.find("unknown pipeline") != std::string::npos);

    const std::vector<std::uint8_t> data = pattern(64U, 5U);
    d.write_buffer(b, 0U, data);
    d.copy_buffer(a, 0U, b, 0U, 64U);
    const Result<std::vector<std::uint8_t>> read = d.read_buffer(a, 0U, 64U);
    if (!read) {
        std::cout << read.error().message << '\n';
    }
    AJFX_CHECK(read.ok() && read.value() == data);
    d.destroy_buffer(a);
    d.destroy_buffer(b);
}

} // namespace

int main() {
    Result<std::unique_ptr<gpu::Device>> device = d3d12::create_device({.debug_layer = true});
    if (!device) {
        device = d3d12::create_device({.warp = true, .debug_layer = true});
    }
    if (!device) {
        std::cout << "skipped: " << device.error().message << '\n';
        return 77;
    }
    g_device = std::move(device).value();
    std::cout << "adapter: " << g_device->adapter_name() << '\n';
    const ajfx::test::Case cases[] = {
        {"writes_copies_and_reads_round_trip", writes_copies_and_reads_round_trip},
        {"large_writes_span_upload_pages", large_writes_span_upload_pages},
        {"invalid_use_is_reported_and_the_device_recovers", invalid_use_is_reported_and_the_device_recovers},
    };
    const int result = ajfx::test::run(cases);
    g_device.reset();
    return result;
}
