#include <mediaserver/show/frame_root_mailbox.hpp>

#include <new>

namespace mediaserver::show {
namespace {

// A bounded ring may not admit more entries than it has slots, and its cursors
// are unsigned differences, so the capacity stays well inside the addressable
// domain as well as being a power of two.
inline constexpr std::size_t max_ring_capacity = std::size_t{1} << 20U;

[[nodiscard]] bool valid_capacity(std::size_t capacity) noexcept {
    return is_power_of_two(capacity) && capacity <= max_ring_capacity;
}

} // namespace

core::Result<FrameRootMailbox, MailboxError> FrameRootMailbox::create(
    std::size_t capacity) noexcept {
    using CreateResult = core::Result<FrameRootMailbox, MailboxError>;
    if (!valid_capacity(capacity)) {
        return CreateResult::failure(MailboxError::InvalidCapacity);
    }

    std::unique_ptr<PublishedFrame[]> slots{
        new (std::nothrow) PublishedFrame[capacity]};
    if (slots == nullptr) {
        return CreateResult::failure(MailboxError::AllocationFailed);
    }
    return CreateResult::success(FrameRootMailbox(std::move(slots), capacity));
}

FrameRootMailbox::FrameRootMailbox(FrameRootMailbox&& other) noexcept
    : slots_(std::move(other.slots_)),
      capacity_(other.capacity_),
      mask_(other.mask_),
      head_(other.head_.load(std::memory_order_relaxed)),
      tail_(other.tail_.load(std::memory_order_relaxed)),
      overflows_(other.overflows_.load(std::memory_order_relaxed)) {
    other.capacity_ = 0U;
    other.mask_ = 0U;
}

FrameRootMailbox& FrameRootMailbox::operator=(FrameRootMailbox&& other) noexcept {
    if (this != &other) {
        slots_ = std::move(other.slots_);
        capacity_ = other.capacity_;
        mask_ = other.mask_;
        head_.store(
            other.head_.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        tail_.store(
            other.tail_.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        overflows_.store(
            other.overflows_.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        other.capacity_ = 0U;
        other.mask_ = 0U;
    }
    return *this;
}

PublishStatus FrameRootMailbox::publish(PublishedFrame&& frame) noexcept {
    const auto tail = tail_.load(std::memory_order_relaxed);
    const auto head = head_.load(std::memory_order_acquire);
    if (static_cast<std::size_t>(tail - head) >= capacity_) {
        overflows_.fetch_add(1U, std::memory_order_relaxed);
        return PublishStatus::Overflow;
    }

    // The slot is either untouched since construction or was emptied by the
    // consumer, so this move assignment destroys nothing.
    slots_[static_cast<std::size_t>(tail) & mask_] = std::move(frame);
    tail_.store(tail + 1U, std::memory_order_release);
    return PublishStatus::Published;
}

bool FrameRootMailbox::consume(PublishedFrame& out) noexcept {
    const auto head = head_.load(std::memory_order_relaxed);
    const auto tail = tail_.load(std::memory_order_acquire);
    if (head == tail) {
        return false;
    }

    auto& slot = slots_[static_cast<std::size_t>(head) & mask_];
    out = std::move(slot);
    slot = PublishedFrame{};
    head_.store(head + 1U, std::memory_order_release);
    return true;
}

bool FrameRootMailbox::consume_latest(
    PublishedFrame& out,
    std::size_t& skipped,
    RootRetirementRing* retirement) noexcept {
    skipped = 0U;
    const auto head = head_.load(std::memory_order_relaxed);
    const auto tail = tail_.load(std::memory_order_acquire);
    if (head == tail) {
        return false;
    }

    // Skipped frames are accounted for rather than silently overwritten. Their
    // roots move to the retirement ring when one is supplied, so a caller on a
    // frame boundary never runs a trie destructor here; a full ring falls back
    // to releasing inline and records the rejection.
    for (auto cursor = head; cursor + 1U < tail; ++cursor) {
        auto& superseded = slots_[static_cast<std::size_t>(cursor) & mask_];
        if (retirement != nullptr
            && retirement->retire(std::move(superseded.root))) {
            superseded.root = doc::Root{};
        }
        superseded = PublishedFrame{};
        ++skipped;
    }

    auto& slot = slots_[static_cast<std::size_t>(tail - 1U) & mask_];
    out = std::move(slot);
    slot = PublishedFrame{};
    head_.store(tail, std::memory_order_release);
    return true;
}

core::Result<RootRetirementRing, MailboxError> RootRetirementRing::create(
    std::size_t capacity) noexcept {
    using CreateResult = core::Result<RootRetirementRing, MailboxError>;
    if (!valid_capacity(capacity)) {
        return CreateResult::failure(MailboxError::InvalidCapacity);
    }

    std::unique_ptr<doc::Root[]> slots{new (std::nothrow) doc::Root[capacity]};
    if (slots == nullptr) {
        return CreateResult::failure(MailboxError::AllocationFailed);
    }
    return CreateResult::success(RootRetirementRing(std::move(slots), capacity));
}

RootRetirementRing::RootRetirementRing(RootRetirementRing&& other) noexcept
    : slots_(std::move(other.slots_)),
      capacity_(other.capacity_),
      mask_(other.mask_),
      head_(other.head_.load(std::memory_order_relaxed)),
      tail_(other.tail_.load(std::memory_order_relaxed)),
      rejected_(other.rejected_.load(std::memory_order_relaxed)) {
    other.capacity_ = 0U;
    other.mask_ = 0U;
}

RootRetirementRing& RootRetirementRing::operator=(
    RootRetirementRing&& other) noexcept {
    if (this != &other) {
        slots_ = std::move(other.slots_);
        capacity_ = other.capacity_;
        mask_ = other.mask_;
        head_.store(
            other.head_.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        tail_.store(
            other.tail_.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        rejected_.store(
            other.rejected_.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        other.capacity_ = 0U;
        other.mask_ = 0U;
    }
    return *this;
}

bool RootRetirementRing::retire(doc::Root&& root) noexcept {
    const auto tail = tail_.load(std::memory_order_relaxed);
    const auto head = head_.load(std::memory_order_acquire);
    if (static_cast<std::size_t>(tail - head) >= capacity_) {
        rejected_.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }

    slots_[static_cast<std::size_t>(tail) & mask_] = std::move(root);
    tail_.store(tail + 1U, std::memory_order_release);
    return true;
}

std::size_t RootRetirementRing::drain() noexcept {
    const auto head = head_.load(std::memory_order_relaxed);
    const auto tail = tail_.load(std::memory_order_acquire);

    std::size_t released = 0U;
    for (auto cursor = head; cursor != tail; ++cursor) {
        slots_[static_cast<std::size_t>(cursor) & mask_] = doc::Root{};
        ++released;
    }
    head_.store(tail, std::memory_order_release);
    return released;
}

} // namespace mediaserver::show
