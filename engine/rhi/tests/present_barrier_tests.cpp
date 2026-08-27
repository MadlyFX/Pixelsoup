#include <mediaserver/rhi/device.hpp>

#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace core = mediaserver::core;
namespace rhi = mediaserver::rhi;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define BARRIER_CHECK(...)                                                     \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

// A stand-in for a native D3D12/NVAPI presentation backend. It records what
// the seam asked for, which is what the plumbing tests need; it makes no
// claim about real hardware.
class RecordingBackend final : public rhi::PresentBarrierBackend {
public:
    [[nodiscard]] std::string_view name() const noexcept override {
        return "recording";
    }
    [[nodiscard]] std::string_view detail() const noexcept override {
        return "test backend";
    }
    [[nodiscard]] rhi::PresentBarrierSupport support(
        void* device_handle) const noexcept override {
        return device_handle != nullptr ? rhi::PresentBarrierSupport::Available
                                        : rhi::PresentBarrierSupport::
                                              Unsupported;
    }
    [[nodiscard]] rhi::PresentBarrierStatus join(
        void*, void*, const rhi::PresentBarrierDesc& desc) noexcept override {
        ++joins;
        last = desc;
        status_ = refuse ? rhi::PresentBarrierStatus::Failed
                         : rhi::PresentBarrierStatus::Joined;
        return status_;
    }
    void leave(void*) noexcept override {
        ++leaves;
        status_ = rhi::PresentBarrierStatus::Left;
    }
    [[nodiscard]] rhi::PresentBarrierStatus status(
        void*) const noexcept override {
        return status_;
    }
    [[nodiscard]] rhi::PresentBarrierStatistics statistics(
        void*) const noexcept override {
        return rhi::PresentBarrierStatistics{12U, 11U, 11U, leaves};
    }

    core::u64 joins{};
    core::u64 leaves{};
    bool refuse{};
    rhi::PresentBarrierDesc last{};

private:
    rhi::PresentBarrierStatus status_{rhi::PresentBarrierStatus::NotJoined};
};

struct BackendScope final {
    explicit BackendScope(rhi::PresentBarrierBackend* backend) noexcept {
        rhi::install_present_barrier_backend(backend);
    }
    BackendScope(const BackendScope&) = delete;
    BackendScope& operator=(const BackendScope&) = delete;
    BackendScope(BackendScope&&) = delete;
    BackendScope& operator=(BackendScope&&) = delete;
    ~BackendScope() { rhi::install_present_barrier_backend(nullptr); }
};

void no_backend_means_unsupported_not_success() {
    rhi::install_present_barrier_backend(nullptr);
    BARRIER_CHECK(rhi::present_barrier_backend() == nullptr);

    // A default-constructed swapchain is the honest stand-in for a device
    // that never claimed a window: it must still refuse rather than pretend.
    rhi::Swapchain swapchain;
    BARRIER_CHECK(swapchain.present_barrier_status() ==
                  rhi::PresentBarrierStatus::Unsupported);
    BARRIER_CHECK(swapchain.present_barrier_statistics() ==
                  rhi::PresentBarrierStatistics{});
    BARRIER_CHECK(swapchain.join_present_barrier(rhi::PresentBarrierDesc{}) ==
                  rhi::PresentBarrierStatus::Failed);
}

void an_installed_backend_is_visible_through_the_seam() {
    RecordingBackend backend;
    const BackendScope scope{&backend};
    BARRIER_CHECK(rhi::present_barrier_backend() == &backend);
    BARRIER_CHECK(backend.support(nullptr) ==
                  rhi::PresentBarrierSupport::Unsupported);
    int fake_device{};
    BARRIER_CHECK(backend.support(&fake_device) ==
                  rhi::PresentBarrierSupport::Available);
    rhi::install_present_barrier_backend(nullptr);
    BARRIER_CHECK(rhi::present_barrier_backend() == nullptr);
}

void a_device_without_a_backend_explains_itself() {
    rhi::install_present_barrier_backend(nullptr);
    if (!rhi::Device::available()) {
        return;
    }
    auto created = rhi::Device::create(rhi::DeviceConfig{});
    if (!created) {
        return;
    }
    auto device = std::move(created).value();
    BARRIER_CHECK(device.present_barrier_support() ==
                  rhi::PresentBarrierSupport::Unsupported);
    BARRIER_CHECK(!device.present_barrier_detail().empty());
    BARRIER_CHECK(device.present_barrier_detail().find("SDL_GPU") !=
                  std::string_view::npos);
}

void a_zero_group_or_barrier_is_never_joined() {
    RecordingBackend backend;
    const BackendScope scope{&backend};
    rhi::Swapchain swapchain;
    BARRIER_CHECK(swapchain.join_present_barrier(
                      rhi::PresentBarrierDesc{0U, 1U}) ==
                  rhi::PresentBarrierStatus::Failed);
    BARRIER_CHECK(swapchain.join_present_barrier(
                      rhi::PresentBarrierDesc{1U, 0U}) ==
                  rhi::PresentBarrierStatus::Failed);
    BARRIER_CHECK(backend.joins == 0U);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 4U> cases{{
    {"no backend means unsupported not success",
     &no_backend_means_unsupported_not_success},
    {"an installed backend is visible through the seam",
     &an_installed_backend_is_visible_through_the_seam},
    {"a device without a backend explains itself",
     &a_device_without_a_backend_explains_itself},
    {"a zero group or barrier is never joined",
     &a_zero_group_or_barrier_is_never_joined},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const Case& entry : cases) {
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
