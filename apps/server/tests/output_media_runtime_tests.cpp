#include "output_media_runtime.hpp"

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

namespace app = mediaserver::app;
namespace net = mediaserver::net;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line) +
            ": check failed: " + expression};
    }
}

#define RUNTIME_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] net::RuntimeMediaAssetStatus status(
    net::RuntimeMediaDecodeState decode,
    net::RuntimeProxyState proxy = net::RuntimeProxyState::ProxyReady) {
    net::RuntimeMediaAssetStatus result{};
    result.asset_id = 7U;
    result.decode = decode;
    result.proxy = proxy;
    result.proxy_reason = proxy == net::RuntimeProxyState::ProxyReady
        ? net::RuntimeProxyReason::None
        : net::RuntimeProxyReason::CacheGenerating;
    return result;
}

void software_only_on_any_output_wins() {
    app::OutputMediaRuntimeAccumulator accumulator{
        3U, net::RuntimeProxyState::PreferFallback,
        net::RuntimeProxyReason::CacheObservationMissing};
    const auto hardware = status(
        net::RuntimeMediaDecodeState::HardwareAccelerated);
    const auto software = status(
        net::RuntimeMediaDecodeState::SoftwareOnly);
    accumulator.observe(&hardware);
    accumulator.observe(&software);
    accumulator.observe(&hardware);
    const auto result = accumulator.finish();
    RUNTIME_CHECK(result.decode ==
                  net::RuntimeMediaDecodeState::SoftwareOnly);
    RUNTIME_CHECK(result.proxy == net::RuntimeProxyState::ProxyReady);
}

void missing_output_keeps_proxy_unready() {
    app::OutputMediaRuntimeAccumulator accumulator{
        2U, net::RuntimeProxyState::RequiredUnready,
        net::RuntimeProxyReason::CacheObservationMissing};
    const auto ready = status(
        net::RuntimeMediaDecodeState::HardwareAccelerated);
    accumulator.observe(&ready);
    accumulator.observe(nullptr);
    const auto result = accumulator.finish();
    RUNTIME_CHECK(result.decode == net::RuntimeMediaDecodeState::Unknown);
    RUNTIME_CHECK(result.proxy ==
                  net::RuntimeProxyState::RequiredUnready);
    RUNTIME_CHECK(result.proxy_reason ==
                  net::RuntimeProxyReason::CacheObservationMissing);
}

void worst_proxy_and_slowest_progress_win() {
    app::OutputMediaRuntimeAccumulator accumulator{
        2U, net::RuntimeProxyState::PreferFallback,
        net::RuntimeProxyReason::CacheObservationMissing};
    auto first = status(net::RuntimeMediaDecodeState::HardwareAccelerated,
                        net::RuntimeProxyState::ProxyReady);
    first.has_transcode_progress = true;
    first.transcode_completed_frames = 80U;
    first.transcode_total_frames = 100U;
    auto second = status(net::RuntimeMediaDecodeState::HardwareAccelerated,
                         net::RuntimeProxyState::PreferFallback);
    second.has_transcode_progress = true;
    second.transcode_completed_frames = 20U;
    second.transcode_total_frames = 100U;
    accumulator.observe(&first);
    accumulator.observe(&second);
    const auto result = accumulator.finish();
    RUNTIME_CHECK(result.decode ==
                  net::RuntimeMediaDecodeState::HardwareAccelerated);
    RUNTIME_CHECK(result.proxy == net::RuntimeProxyState::PreferFallback);
    RUNTIME_CHECK(result.transcode_progress.has_value());
    RUNTIME_CHECK(result.transcode_progress->completed_frames == 20U);
}

struct Case final {
    const char* name{};
    void (*function)(){};
};

constexpr std::array<Case, 3U> cases{{
    {"software-only on any output wins", &software_only_on_any_output_wins},
    {"missing output keeps proxy unready", &missing_output_keeps_proxy_unready},
    {"worst proxy and slowest progress win",
     &worst_proxy_and_slowest_progress_win},
}};

} // namespace

int main() {
    std::size_t failures{};
    for (const auto& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
