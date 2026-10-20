#include <mediaserver/show/presenter.hpp>

#include <mediaserver/core/output_limits.hpp>

#include <new>
#include <span>
#include <utility>

namespace mediaserver::show {

PresenterSurface::PresenterSurface(
    std::unique_ptr<core::u8[]> storage,
    std::size_t bytes,
    core::u32 width,
    core::u32 height,
    core::u32 stride) noexcept
    : storage_(std::move(storage)) {
    view_ = FrameBufferView{
        std::span<core::u8>{storage_.get(), bytes},
        width,
        height,
        stride,
        PixelFormat::Rgba8888,
    };
}

PresenterSurface::PresenterSurface(PresenterSurface&& other) noexcept
    : storage_(std::move(other.storage_)), view_(other.view_) {
    // The view describes storage this object now owns, so the source must not
    // keep describing it.
    other.view_ = FrameBufferView{};
}

PresenterSurface& PresenterSurface::operator=(PresenterSurface&& other) noexcept {
    if (this != &other) {
        storage_ = std::move(other.storage_);
        view_ = other.view_;
        other.view_ = FrameBufferView{};
    }
    return *this;
}

core::Result<PresenterSurface, PresenterError> PresenterSurface::create(
    core::u32 width,
    core::u32 height) noexcept {
    using CreateResult = core::Result<PresenterSurface, PresenterError>;
    // The window extent is the authored output extent, which may span a
    // Mosaic desktop; the shared limit keeps the two from disagreeing.
    if (width == 0U || height == 0U || width > core::max_output_dimension
        || height > core::max_output_dimension) {
        return CreateResult::failure(PresenterError::InvalidConfiguration);
    }

    const auto stride = static_cast<std::size_t>(width) * 4U;
    const auto bytes = stride * static_cast<std::size_t>(height);
    std::unique_ptr<core::u8[]> storage{new (std::nothrow) core::u8[bytes]{}};
    if (storage == nullptr) {
        return CreateResult::failure(PresenterError::AllocationFailed);
    }

    return CreateResult::success(PresenterSurface(
        std::move(storage),
        bytes,
        width,
        height,
        static_cast<core::u32>(stride)));
}

PresenterResult HeadlessPresenter::create(
    core::u32 width,
    core::u32 height) noexcept {
    auto surface = PresenterSurface::create(width, height);
    if (!surface) {
        return PresenterResult::failure(surface.error());
    }

    std::unique_ptr<Presenter> presenter{
        new (std::nothrow) HeadlessPresenter(std::move(surface).value())};
    if (presenter == nullptr) {
        return PresenterResult::failure(PresenterError::AllocationFailed);
    }
    return PresenterResult::success(std::move(presenter));
}

PresenterStatus HeadlessPresenter::present(
    const TestPatternInput& frame) noexcept {
    return render_test_pattern(surface_.view(), frame)
        ? PresenterStatus::Presented
        : PresenterStatus::Failed;
}

} // namespace mediaserver::show
