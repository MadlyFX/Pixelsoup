#include <mediaserver/rhi/device.hpp>

#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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
        const rhi::NativeHandle& device) const noexcept override {
        return device.value != nullptr ? rhi::PresentBarrierSupport::Available
                                       : rhi::PresentBarrierSupport::
                                             Unsupported;
    }
    [[nodiscard]] rhi::PresentBarrierStatus join(
        const rhi::PresentBarrierTarget& target,
        const rhi::PresentBarrierDesc& desc) noexcept override {
        ++joins;
        last = desc;
        last_target = target.swapchain;
        status_ = refuse ? rhi::PresentBarrierStatus::Failed
                         : rhi::PresentBarrierStatus::Joined;
        return status_;
    }
    void leave(const rhi::NativeHandle&) noexcept override {
        ++leaves;
        status_ = rhi::PresentBarrierStatus::Left;
    }
    [[nodiscard]] rhi::PresentBarrierStatus status(
        const rhi::NativeHandle&) const noexcept override {
        return status_;
    }
    [[nodiscard]] rhi::PresentBarrierStatistics statistics(
        const rhi::NativeHandle&) const noexcept override {
        auto result = statistics_;
        result.leave_events += leaves;
        return result;
    }
    [[nodiscard]] rhi::PresentBarrierPresentResult present(
        const rhi::NativeHandle&, const rhi::NativeHandle&,
        const rhi::NativeHandle&, core::u32, core::u32) noexcept override {
        ++presents;
        if (left_after_presents != 0U && presents >= left_after_presents) {
            status_ = rhi::PresentBarrierStatus::Left;
            ++leaves;
            return rhi::PresentBarrierPresentResult::Failed;
        }
        return status_ == rhi::PresentBarrierStatus::Joined
                   ? rhi::PresentBarrierPresentResult::Presented
                   : rhi::PresentBarrierPresentResult::Failed;
    }

    core::u64 joins{};
    core::u64 leaves{};
    core::u64 presents{};
    core::u64 left_after_presents{};
    bool refuse{};
    rhi::PresentBarrierDesc last{};
    rhi::NativeHandle last_target{};
    rhi::PresentBarrierStatistics statistics_{12U, 11U, 11U, 0U};

private:
    rhi::PresentBarrierStatus status_{rhi::PresentBarrierStatus::NotJoined};
};

struct BackendScope final {
    explicit BackendScope(rhi::PresentBarrierBackendHandle backend) noexcept
        : backend_(std::move(backend)) {
        rhi::install_present_barrier_backend(backend_);
    }
    BackendScope(const BackendScope&) = delete;
    BackendScope& operator=(const BackendScope&) = delete;
    BackendScope(BackendScope&&) = delete;
    BackendScope& operator=(BackendScope&&) = delete;
    ~BackendScope() { rhi::install_present_barrier_backend({}); }

private:
    rhi::PresentBarrierBackendHandle backend_;
};

void no_backend_means_unsupported_not_success() {
    rhi::install_present_barrier_backend({});
    BARRIER_CHECK(!rhi::acquire_present_barrier_backend());

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
    auto backend = std::make_shared<RecordingBackend>();
    const BackendScope scope{backend};
    BARRIER_CHECK(rhi::acquire_present_barrier_backend() == backend);
    BARRIER_CHECK(backend->support({}) ==
                  rhi::PresentBarrierSupport::Unsupported);
    int fake_device{};
    BARRIER_CHECK(backend->support(rhi::NativeHandle{
                      rhi::BackendKind::D3d12, rhi::NativeObjectKind::Device,
                      &fake_device}) ==
                  rhi::PresentBarrierSupport::Available);
    rhi::install_present_barrier_backend({});
    BARRIER_CHECK(!rhi::acquire_present_barrier_backend());
}

void a_device_without_a_backend_explains_itself() {
    rhi::install_present_barrier_backend({});
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
    auto backend = std::make_shared<RecordingBackend>();
    const BackendScope scope{backend};
    rhi::Swapchain swapchain;
    BARRIER_CHECK(swapchain.join_present_barrier(
                      rhi::PresentBarrierDesc{0U, 1U}) ==
                  rhi::PresentBarrierStatus::Failed);
    BARRIER_CHECK(swapchain.join_present_barrier(
                      rhi::PresentBarrierDesc{1U, 0U}) ==
                  rhi::PresentBarrierStatus::Failed);
    BARRIER_CHECK(backend->joins == 0U);
}

void a_backend_loss_after_n_presents_is_visible_as_left() {
    auto backend = std::make_shared<RecordingBackend>();
    backend->left_after_presents = 3U;

    int fake_device{};
    int fake_swapchain{};
    int fake_image{};
    const rhi::NativeHandle device{rhi::BackendKind::D3d12,
                                   rhi::NativeObjectKind::Device,
                                   &fake_device};
    const rhi::NativeHandle swapchain{rhi::BackendKind::D3d12,
                                      rhi::NativeObjectKind::Swapchain,
                                      &fake_swapchain};
    const rhi::NativeHandle image{rhi::BackendKind::D3d12,
                                  rhi::NativeObjectKind::Texture,
                                  &fake_image};
    const rhi::PresentBarrierTarget target{device, swapchain,
                                           std::span{&image, 1U}};

    BARRIER_CHECK(backend->join(target, rhi::PresentBarrierDesc{1U, 1U}) ==
                  rhi::PresentBarrierStatus::Joined);
    BARRIER_CHECK(backend->present(device, swapchain, image, 0U, 0U) ==
                  rhi::PresentBarrierPresentResult::Presented);
    BARRIER_CHECK(backend->present(device, swapchain, image, 0U, 0U) ==
                  rhi::PresentBarrierPresentResult::Presented);
    BARRIER_CHECK(backend->status(swapchain) ==
                  rhi::PresentBarrierStatus::Joined);
    BARRIER_CHECK(backend->present(device, swapchain, image, 0U, 0U) ==
                  rhi::PresentBarrierPresentResult::Failed);
    BARRIER_CHECK(backend->status(swapchain) ==
                  rhi::PresentBarrierStatus::Left);
    BARRIER_CHECK(backend->presents == 3U);
}

void an_acquired_backend_lease_survives_uninstall() {
    auto backend = std::make_shared<RecordingBackend>();
    rhi::install_present_barrier_backend(backend);
    auto lease = rhi::acquire_present_barrier_backend();
    rhi::install_present_barrier_backend({});
    backend.reset();

    int fake_device{};
    BARRIER_CHECK(lease->support(rhi::NativeHandle{
                      rhi::BackendKind::D3d12, rhi::NativeObjectKind::Device,
                      &fake_device}) ==
                  rhi::PresentBarrierSupport::Available);
    lease.reset();
    BARRIER_CHECK(!rhi::acquire_present_barrier_backend());
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 6U> cases{{
    {"no backend means unsupported not success",
     &no_backend_means_unsupported_not_success},
    {"an installed backend is visible through the seam",
     &an_installed_backend_is_visible_through_the_seam},
    {"a device without a backend explains itself",
     &a_device_without_a_backend_explains_itself},
    {"a zero group or barrier is never joined",
     &a_zero_group_or_barrier_is_never_joined},
    {"a backend loss after N presents is visible as left",
     &a_backend_loss_after_n_presents_is_visible_as_left},
    {"an acquired backend lease survives uninstall",
     &an_acquired_backend_lease_survives_uninstall},
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
