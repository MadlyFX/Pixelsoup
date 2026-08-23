#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>

#include <mediaserver/core/result.hpp>
#include <mediaserver/persist/durable_director.hpp>

namespace mediaserver::net {

namespace detail {
struct DirectorHubControl;
struct DirectorHubFrameRecord;
struct DirectorHubSubscriptionState;
} // namespace detail

struct DirectorSessionHubLimits final {
    std::size_t max_subscriptions{1'024U};
    std::size_t max_backlog_frames{256U};

    // This is the sum of committed WAL entry sizes retained by one queue. It
    // is a deterministic serialized-byte budget, not an exact measurement of
    // allocator or C++ object residency. A blocked worker may additionally
    // retain the one frame it has already popped for socket transmission.
    std::uint64_t max_backlog_bytes{128U * 1024U * 1024U};
};

enum class DirectorSubscriptionState : std::uint8_t {
    Active,
    ResyncRequired,
    Closed,
};

struct DirectorSubscriptionId final {
    std::size_t slot{};
    std::uint64_t generation{};

    bool operator==(const DirectorSubscriptionId&) const = default;
};

enum class DirectorSessionHubErrorCode : std::uint8_t {
    InvalidConfiguration,
    Closed,
    SubscriptionCapacity,
    AllocationFailed,
    DurableFailure,
};

struct DirectorSessionHubError final {
    DirectorSessionHubErrorCode code{};
    std::optional<persist::DurableDirectorError> durable_error;
};

struct DirectorPublicationReport final {
    std::size_t queued{};
    std::size_t already_covered{};
    std::size_t resync_required{};
};

// Copyable immutable handle shared by the seal caller and every subscribed
// queue. The underlying move-only publication capability is created by the
// authoritative DurableDirector and cannot be forged by a session.
class PublishedDirectorFrame final {
public:
    PublishedDirectorFrame(const PublishedDirectorFrame&) noexcept = default;
    PublishedDirectorFrame& operator=(const PublishedDirectorFrame&) noexcept =
        default;
    PublishedDirectorFrame(PublishedDirectorFrame&&) noexcept = default;
    PublishedDirectorFrame& operator=(PublishedDirectorFrame&&) noexcept = default;
    ~PublishedDirectorFrame() = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(record_);
    }

    [[nodiscard]] const persist::CommittedFrameBatch& batch() const noexcept;
    [[nodiscard]] std::span<const cmd::Command> commands() const noexcept;
    [[nodiscard]] const cmd::FrameSeal& seal() const noexcept;
    [[nodiscard]] const cmd::StreamCursor& before_cursor() const noexcept;
    [[nodiscard]] cmd::StreamCursor after_cursor() const noexcept;
    [[nodiscard]] std::uint64_t serialized_bytes() const noexcept;

private:
    friend class DirectorSessionHub;
    friend class DirectorLiveSubscription;

    explicit PublishedDirectorFrame(
        std::shared_ptr<const detail::DirectorHubFrameRecord> record) noexcept
        : record_(std::move(record)) {}

    std::shared_ptr<const detail::DirectorHubFrameRecord> record_;
};

struct DirectorSubscriptionPoll final {
    DirectorSubscriptionState state{DirectorSubscriptionState::Closed};
    std::optional<PublishedDirectorFrame> frame;
};

// Single-consumer live backlog. Destruction closes the registration. Handles
// are slot-generation-bound, so calling close() again on an old handle cannot
// unregister a later subscriber that reused its slot.
class DirectorLiveSubscription final {
public:
    DirectorLiveSubscription(DirectorLiveSubscription&& other) noexcept;
    DirectorLiveSubscription& operator=(DirectorLiveSubscription&& other) noexcept;
    DirectorLiveSubscription(const DirectorLiveSubscription&) = delete;
    DirectorLiveSubscription& operator=(const DirectorLiveSubscription&) = delete;
    ~DirectorLiveSubscription();

    [[nodiscard]] DirectorSubscriptionPoll try_pop() noexcept;
    [[nodiscard]] DirectorSubscriptionPoll wait_for(
        std::chrono::milliseconds timeout) noexcept;

    void close() noexcept;

    [[nodiscard]] DirectorSubscriptionState state() const noexcept;
    [[nodiscard]] std::size_t queued_frames() const noexcept;
    [[nodiscard]] std::uint64_t queued_bytes() const noexcept;
    [[nodiscard]] std::optional<DirectorSubscriptionId> id() const noexcept;

private:
    friend class DirectorSessionHub;

    explicit DirectorLiveSubscription(
        std::shared_ptr<detail::DirectorHubSubscriptionState> state) noexcept
        : state_(std::move(state)) {}

    std::shared_ptr<detail::DirectorHubSubscriptionState> state_;
};

struct DirectorResumeSubscription final {
    persist::DurableResumeSource source;
    DirectorLiveSubscription live;
};

struct DirectorSealOutcome final {
    PublishedDirectorFrame frame;
    DirectorPublicationReport publication;
};

// Serializes seal requests and fans each durable result into fixed-capacity
// per-session rings. No publication path performs network I/O. Queue overflow
// fences only that subscriber to ResyncRequired; it never changes a completed
// durable seal into an error. Once constructed for a runtime, all production
// sealing must go through seal_and_publish(); calling DurableDirector::seal_frame
// directly bypasses live fan-out and violates the hub ownership contract.
class DirectorSessionHub final {
public:
    DirectorSessionHub(
        persist::DurableDirector& authority,
        DirectorSessionHubLimits limits = {});
    DirectorSessionHub(const DirectorSessionHub&) = delete;
    DirectorSessionHub& operator=(const DirectorSessionHub&) = delete;
    DirectorSessionHub(DirectorSessionHub&&) = delete;
    DirectorSessionHub& operator=(DirectorSessionHub&&) = delete;
    ~DirectorSessionHub();

    // The returned snapshot tail and live queue registration are one atomic
    // operation relative to DurableDirector::seal_frame().
    [[nodiscard]] core::Result<DirectorResumeSubscription, DirectorSessionHubError>
    subscribe();

    // Allocates the shared record before attempting the durable seal. After a
    // successful seal, record publication and all fixed-ring operations are
    // allocation-free. Overflow is represented only in publication report.
    [[nodiscard]] core::Result<DirectorSealOutcome, DirectorSessionHubError>
    seal_and_publish(cmd::FrameNumber frame);

    void close() noexcept;

    [[nodiscard]] std::size_t active_subscriptions() const noexcept;
    [[nodiscard]] const DirectorSessionHubLimits& limits() const noexcept {
        return limits_;
    }

private:
    persist::DurableDirector* authority_;
    DirectorSessionHubLimits limits_;
    std::shared_ptr<detail::DirectorHubControl> control_;
    mutable std::mutex seal_mutex_;
};

} // namespace mediaserver::net
