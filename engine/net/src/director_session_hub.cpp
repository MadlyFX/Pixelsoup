#include <mediaserver/net/director_session_hub.hpp>

#include <cassert>
#include <condition_variable>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace mediaserver::net {

namespace detail {

struct DirectorHubFrameRecord final {
    std::optional<persist::CommittedFrameBatch> batch;
};

struct DirectorHubSubscriptionState final {
    explicit DirectorHubSubscriptionState(std::size_t queue_capacity)
        : queue(queue_capacity) {}

    mutable std::mutex mutex;
    std::condition_variable changed;
    DirectorSubscriptionState status{DirectorSubscriptionState::Closed};
    std::vector<std::shared_ptr<const DirectorHubFrameRecord>> queue;
    std::size_t head{};
    std::size_t count{};
    std::uint64_t serialized_bytes{};
    cmd::StreamCursor expected_cursor{};
    std::weak_ptr<DirectorHubControl> control;
    std::optional<DirectorSubscriptionId> id;
};

struct DirectorHubControl final {
    struct Slot final {
        std::weak_ptr<DirectorHubSubscriptionState> state;
        std::uint64_t generation{};
    };

    explicit DirectorHubControl(std::size_t capacity) : slots(capacity) {}

    mutable std::mutex mutex;
    bool closed{};
    std::vector<Slot> slots;
};

} // namespace detail

namespace {

[[nodiscard]] bool same_cursor(
    const cmd::StreamCursor& left,
    const cmd::StreamCursor& right) noexcept {
    return left.through_frame == right.through_frame && left.revision == right.revision
        && left.next_command == right.next_command;
}

[[nodiscard]] bool valid_limits(
    const DirectorSessionHubLimits& limits) noexcept {
    return limits.max_subscriptions != 0U && limits.max_backlog_frames != 0U
        && limits.max_backlog_bytes != 0U;
}

[[nodiscard]] DirectorSessionHubError hub_error(
    DirectorSessionHubErrorCode code,
    std::optional<persist::DurableDirectorError> durable_error = std::nullopt) {
    return DirectorSessionHubError{code, std::move(durable_error)};
}

void clear_queue_locked(
    detail::DirectorHubSubscriptionState& state) noexcept {
    for (auto& queued : state.queue) {
        queued.reset();
    }
    state.head = 0U;
    state.count = 0U;
    state.serialized_bytes = 0U;
}

void transition_locked(
    detail::DirectorHubSubscriptionState& state,
    DirectorSubscriptionState status) noexcept {
    clear_queue_locked(state);
    state.status = status;
    state.changed.notify_all();
}

void close_subscription(
    const std::shared_ptr<detail::DirectorHubSubscriptionState>& state) noexcept {
    if (!state) {
        return;
    }

    // All paths that need both locks use control -> subscription. The
    // generation check prevents a repeated close on an old handle from
    // clearing a slot that has since been reused.
    if (auto control = state->control.lock()) {
        const std::lock_guard control_lock{control->mutex};
        if (state->id && state->id->slot < control->slots.size()) {
            auto& slot = control->slots[state->id->slot];
            auto current = slot.state.lock();
            if (slot.generation == state->id->generation
                && current.get() == state.get()) {
                slot.state.reset();
            }
        }
        const std::lock_guard state_lock{state->mutex};
        transition_locked(*state, DirectorSubscriptionState::Closed);
        return;
    }

    const std::lock_guard state_lock{state->mutex};
    transition_locked(*state, DirectorSubscriptionState::Closed);
}

enum class RegistrationFailure : std::uint8_t {
    None,
    Closed,
    Capacity,
};

class HubRegistration final : public persist::ResumeSourceRegistration {
public:
    HubRegistration(
        std::shared_ptr<detail::DirectorHubControl> control,
        std::shared_ptr<detail::DirectorHubSubscriptionState> state) noexcept
        : control_(std::move(control)), state_(std::move(state)) {}

    [[nodiscard]] bool register_captured_tail(
        const cmd::StreamCursor& tail) noexcept override {
        const std::lock_guard control_lock{control_->mutex};
        if (control_->closed) {
            failure_ = RegistrationFailure::Closed;
            return false;
        }

        for (std::size_t index = 0U; index < control_->slots.size(); ++index) {
            auto& slot = control_->slots[index];
            if (!slot.state.expired()) {
                continue;
            }
            if (slot.generation == std::numeric_limits<std::uint64_t>::max()) {
                continue;
            }

            ++slot.generation;
            const auto id = DirectorSubscriptionId{index, slot.generation};
            {
                const std::lock_guard state_lock{state_->mutex};
                state_->expected_cursor = tail;
                state_->control = control_;
                state_->id = id;
                state_->status = DirectorSubscriptionState::Active;
            }
            slot.state = state_;
            failure_ = RegistrationFailure::None;
            return true;
        }

        failure_ = RegistrationFailure::Capacity;
        return false;
    }

    [[nodiscard]] RegistrationFailure failure() const noexcept {
        return failure_;
    }

private:
    std::shared_ptr<detail::DirectorHubControl> control_;
    std::shared_ptr<detail::DirectorHubSubscriptionState> state_;
    RegistrationFailure failure_{RegistrationFailure::None};
};

struct RawSubscriptionPoll final {
    DirectorSubscriptionState state{DirectorSubscriptionState::Closed};
    std::shared_ptr<const detail::DirectorHubFrameRecord> record;
};

[[nodiscard]] RawSubscriptionPoll pop_locked(
    detail::DirectorHubSubscriptionState& state) noexcept {
    RawSubscriptionPoll poll{state.status, nullptr};
    if (state.count == 0U) {
        return poll;
    }

    auto record = std::move(state.queue[state.head]);
    state.queue[state.head].reset();
    state.head = (state.head + 1U) % state.queue.size();
    --state.count;
    assert(record);
    assert(record->batch);
    const auto bytes = record->batch->receipt().bytes_written;
    assert(bytes <= state.serialized_bytes);
    state.serialized_bytes -= bytes;
    poll.record = std::move(record);
    return poll;
}

} // namespace

const persist::CommittedFrameBatch& PublishedDirectorFrame::batch() const noexcept {
    assert(record_ && record_->batch);
    return *record_->batch;
}

std::span<const cmd::Command> PublishedDirectorFrame::commands() const noexcept {
    return batch().commands();
}

const cmd::FrameSeal& PublishedDirectorFrame::seal() const noexcept {
    return batch().seal();
}

const cmd::StreamCursor& PublishedDirectorFrame::before_cursor() const noexcept {
    return batch().before_cursor();
}

cmd::StreamCursor PublishedDirectorFrame::after_cursor() const noexcept {
    return batch().after_cursor();
}

std::uint64_t PublishedDirectorFrame::serialized_bytes() const noexcept {
    return batch().receipt().bytes_written;
}

DirectorLiveSubscription::DirectorLiveSubscription(
    DirectorLiveSubscription&& other) noexcept
    : state_(std::move(other.state_)) {}

DirectorLiveSubscription& DirectorLiveSubscription::operator=(
    DirectorLiveSubscription&& other) noexcept {
    if (this != &other) {
        close();
        state_ = std::move(other.state_);
    }
    return *this;
}

DirectorLiveSubscription::~DirectorLiveSubscription() {
    close();
}

DirectorSubscriptionPoll DirectorLiveSubscription::try_pop() noexcept {
    if (!state_) {
        return DirectorSubscriptionPoll{DirectorSubscriptionState::Closed, std::nullopt};
    }
    const std::lock_guard lock{state_->mutex};
    auto raw = pop_locked(*state_);
    DirectorSubscriptionPoll poll{raw.state, std::nullopt};
    if (raw.record) {
        poll.frame.emplace(PublishedDirectorFrame{std::move(raw.record)});
    }
    return poll;
}

DirectorSubscriptionPoll DirectorLiveSubscription::wait_for(
    std::chrono::milliseconds timeout) noexcept {
    if (!state_) {
        return DirectorSubscriptionPoll{DirectorSubscriptionState::Closed, std::nullopt};
    }
    std::unique_lock lock{state_->mutex};
    if (timeout > std::chrono::milliseconds::zero()) {
        static_cast<void>(state_->changed.wait_for(
            lock,
            timeout,
            [this] {
                return state_->count != 0U
                    || state_->status != DirectorSubscriptionState::Active;
            }));
    }
    auto raw = pop_locked(*state_);
    DirectorSubscriptionPoll poll{raw.state, std::nullopt};
    if (raw.record) {
        poll.frame.emplace(PublishedDirectorFrame{std::move(raw.record)});
    }
    return poll;
}

void DirectorLiveSubscription::close() noexcept {
    close_subscription(state_);
}

DirectorSubscriptionState DirectorLiveSubscription::state() const noexcept {
    if (!state_) {
        return DirectorSubscriptionState::Closed;
    }
    const std::lock_guard lock{state_->mutex};
    return state_->status;
}

std::size_t DirectorLiveSubscription::queued_frames() const noexcept {
    if (!state_) {
        return 0U;
    }
    const std::lock_guard lock{state_->mutex};
    return state_->count;
}

std::uint64_t DirectorLiveSubscription::queued_bytes() const noexcept {
    if (!state_) {
        return 0U;
    }
    const std::lock_guard lock{state_->mutex};
    return state_->serialized_bytes;
}

std::optional<DirectorSubscriptionId> DirectorLiveSubscription::id() const noexcept {
    if (!state_) {
        return std::nullopt;
    }
    const std::lock_guard lock{state_->mutex};
    return state_->id;
}

DirectorSessionHub::DirectorSessionHub(
    persist::DurableDirector& authority,
    DirectorSessionHubLimits limits)
    : authority_(&authority),
      limits_(limits),
      control_(std::make_shared<detail::DirectorHubControl>(
          limits.max_subscriptions)) {}

DirectorSessionHub::~DirectorSessionHub() {
    close();
}

core::Result<DirectorResumeSubscription, DirectorSessionHubError>
DirectorSessionHub::subscribe() {
    using Result =
        core::Result<DirectorResumeSubscription, DirectorSessionHubError>;
    if (!valid_limits(limits_)) {
        return Result::failure(
            hub_error(DirectorSessionHubErrorCode::InvalidConfiguration));
    }

    try {
        auto state = std::make_shared<detail::DirectorHubSubscriptionState>(
            limits_.max_backlog_frames);
        HubRegistration registration{control_, state};
        auto captured = authority_->capture_resume_source(registration);
        if (!captured) {
            close_subscription(state);
            if (registration.failure() == RegistrationFailure::Closed) {
                return Result::failure(
                    hub_error(DirectorSessionHubErrorCode::Closed));
            }
            if (registration.failure() == RegistrationFailure::Capacity) {
                return Result::failure(
                    hub_error(DirectorSessionHubErrorCode::SubscriptionCapacity));
            }
            return Result::failure(hub_error(
                DirectorSessionHubErrorCode::DurableFailure,
                std::move(captured.error())));
        }
        return Result::success(DirectorResumeSubscription{
            std::move(captured).value(),
            DirectorLiveSubscription{std::move(state)},
        });
    } catch (const std::bad_alloc&) {
        return Result::failure(
            hub_error(DirectorSessionHubErrorCode::AllocationFailed));
    } catch (const std::length_error&) {
        return Result::failure(
            hub_error(DirectorSessionHubErrorCode::AllocationFailed));
    }
}

core::Result<DirectorSealOutcome, DirectorSessionHubError>
DirectorSessionHub::seal_and_publish(cmd::FrameNumber frame) {
    using Result = core::Result<DirectorSealOutcome, DirectorSessionHubError>;
    const std::lock_guard seal_lock{seal_mutex_};
    if (!valid_limits(limits_)) {
        return Result::failure(
            hub_error(DirectorSessionHubErrorCode::InvalidConfiguration));
    }
    {
        const std::lock_guard control_lock{control_->mutex};
        if (control_->closed) {
            return Result::failure(
                hub_error(DirectorSessionHubErrorCode::Closed));
        }
    }

    std::shared_ptr<detail::DirectorHubFrameRecord> record;
    try {
        // This is the only allocation owned by the publication path, and it
        // deliberately precedes the durable operation.
        record = std::make_shared<detail::DirectorHubFrameRecord>();
    } catch (const std::bad_alloc&) {
        return Result::failure(
            hub_error(DirectorSessionHubErrorCode::AllocationFailed));
    }

    auto sealed = authority_->seal_frame(frame);
    if (!sealed) {
        return Result::failure(hub_error(
            DirectorSessionHubErrorCode::DurableFailure,
            std::move(sealed.error())));
    }

    static_assert(std::is_nothrow_move_constructible_v<
                  persist::CommittedFrameBatch>);
    record->batch.emplace(std::move(sealed).value());
    const std::shared_ptr<const detail::DirectorHubFrameRecord> published = record;
    const auto& batch = *record->batch;
    const auto bytes = batch.receipt().bytes_written;
    DirectorPublicationReport report;

    const std::lock_guard control_lock{control_->mutex};
    for (auto& slot : control_->slots) {
        auto state = slot.state.lock();
        if (!state) {
            slot.state.reset();
            continue;
        }

        const std::lock_guard state_lock{state->mutex};
        if (state->status != DirectorSubscriptionState::Active) {
            slot.state.reset();
            continue;
        }
        if (same_cursor(state->expected_cursor, batch.after_cursor())) {
            ++report.already_covered;
            continue;
        }

        const auto contiguous =
            same_cursor(state->expected_cursor, batch.before_cursor());
        const auto frame_capacity = state->count < state->queue.size();
        const auto byte_capacity = bytes <= limits_.max_backlog_bytes
            && state->serialized_bytes <= limits_.max_backlog_bytes - bytes;
        if (!contiguous || !frame_capacity || !byte_capacity) {
            transition_locked(*state, DirectorSubscriptionState::ResyncRequired);
            slot.state.reset();
            ++report.resync_required;
            continue;
        }

        const auto tail = (state->head + state->count) % state->queue.size();
        state->queue[tail] = published;
        ++state->count;
        state->serialized_bytes += bytes;
        state->expected_cursor = batch.after_cursor();
        state->changed.notify_one();
        ++report.queued;
    }

    return Result::success(DirectorSealOutcome{
        PublishedDirectorFrame{std::move(published)},
        report,
    });
}

void DirectorSessionHub::close() noexcept {
    const std::lock_guard seal_lock{seal_mutex_};
    const std::lock_guard control_lock{control_->mutex};
    if (control_->closed) {
        return;
    }
    control_->closed = true;
    for (auto& slot : control_->slots) {
        if (auto state = slot.state.lock()) {
            const std::lock_guard state_lock{state->mutex};
            transition_locked(*state, DirectorSubscriptionState::Closed);
        }
        slot.state.reset();
    }
}

std::size_t DirectorSessionHub::active_subscriptions() const noexcept {
    const std::lock_guard control_lock{control_->mutex};
    std::size_t active = 0U;
    for (const auto& slot : control_->slots) {
        if (auto state = slot.state.lock()) {
            const std::lock_guard state_lock{state->mutex};
            if (state->status == DirectorSubscriptionState::Active) {
                ++active;
            }
        }
    }
    return active;
}

} // namespace mediaserver::net
