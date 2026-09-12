#include <mediaserver/net/tcp_framed_stream.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <limits>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>

#include <asio/read.hpp>
#include <asio/write.hpp>

#include "socket_readiness.hpp"

namespace mediaserver::net {
namespace {

constexpr std::size_t size_prefix_bytes = 4U;

[[nodiscard]] std::uint32_t read_u32_le(
    std::span<const std::byte, size_prefix_bytes> bytes) noexcept {
    return static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[0U]))
        | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[1U])) << 8U)
        | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[2U])) << 16U)
        | (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[3U])) << 24U);
}

[[nodiscard]] FramedStreamError make_error(
    FramedStreamErrorCode code,
    std::error_code system_error = {},
    std::size_t transferred = 0U) noexcept {
    return FramedStreamError{code, system_error, transferred};
}

// Fallback dwell for the rare paths where a readiness wait is unusable. On a
// default Windows timer resolution this really sleeps for about one 15.6 ms
// scheduler quantum, which is exactly why it must not be on the hot path.
constexpr auto nonblocking_retry_pause = std::chrono::milliseconds{1};

// An upper bound on one readiness wait, not a polling period: a socket that
// becomes ready wakes the wait immediately, so a round trip no longer pays a
// scheduler quantum per blocked transfer. The bound exists only so close() and
// interrupt_transport() still unwind an active transfer promptly on a stack
// where shutdown() does not by itself wake a pending wait. It sits below the
// quantum the sleeping retry used to cost, so that unwind does not regress.
constexpr auto readiness_wait_slice = std::chrono::milliseconds{10};

// A readiness report is advisory. Should a stack keep reporting readiness that
// the following transfer cannot use, degrade to the sleeping retry rather than
// spin a core.
constexpr unsigned max_ready_without_progress = 8U;

class BudgetReservation final {
public:
    BudgetReservation(
        FramedStreamAdmissionBudget& budget,
        std::size_t bytes) noexcept
        : budget_(&budget), bytes_(bytes), acquired_(budget.try_reserve(bytes)) {}

    BudgetReservation(const BudgetReservation&) = delete;
    BudgetReservation& operator=(const BudgetReservation&) = delete;

    ~BudgetReservation() {
        if (acquired_) {
            budget_->release(bytes_);
        }
    }

    [[nodiscard]] bool acquired() const noexcept { return acquired_; }

private:
    FramedStreamAdmissionBudget* budget_;
    std::size_t bytes_;
    bool acquired_;
};

} // namespace

bool FramedStreamAdmissionBudget::try_reserve(std::size_t bytes) noexcept {
    auto current = reserved_bytes_.load(std::memory_order_acquire);
    while (true) {
        if (bytes > capacity_bytes_ - std::min(current, capacity_bytes_)) {
            return false;
        }
        if (reserved_bytes_.compare_exchange_weak(
                current,
                current + bytes,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
    }
}

void FramedStreamAdmissionBudget::release(std::size_t bytes) noexcept {
    const auto previous = reserved_bytes_.fetch_sub(bytes, std::memory_order_acq_rel);
    if (previous < bytes) {
        std::terminate();
    }
}

TcpFramedStream::TcpFramedStream(
    asio::ip::tcp::socket socket,
    FramedStreamLimits limits,
    std::shared_ptr<FramedStreamAdmissionBudget> admission_budget) noexcept
    : socket_(std::move(socket)),
      limits_(limits),
      local_admission_budget_(limits.max_message_bytes),
      shared_admission_budget_(std::move(admission_budget)) {
    // A non-blocking native socket lets the public synchronous API enforce a
    // steady-clock deadline portably. Each call retries locally, so no thread
    // is trapped in an OS receive/send while close waits for it to unwind.
    std::error_code error;
    socket_.non_blocking(true, error);
    if (error) {
        usable_.store(false, std::memory_order_release);
    }
}

TcpFramedStream::~TcpFramedStream() {
    close();
}

TcpFramedStream::ActiveOperation::~ActiveOperation() {
    owner_->end_operation();
}

bool TcpFramedStream::begin_operation(
    Direction direction,
    std::chrono::milliseconds& timeout) noexcept {
    const std::lock_guard lock{lifecycle_mutex_};
    if (lifecycle_ != Lifecycle::Open
        || !usable_.load(std::memory_order_acquire)
        || !socket_.is_open()) {
        return false;
    }

    ++active_operations_;
    auto& first = direction == Direction::Read ? first_read_ : first_write_;
    if (first) {
        first = false;
        timeout = limits_.handshake_timeout;
    } else {
        timeout = direction == Direction::Read
            ? limits_.read_timeout
            : limits_.write_timeout;
    }
    return true;
}

TcpFramedStream::TransferProgress TcpFramedStream::read_exact(
    std::span<std::byte> destination,
    std::chrono::steady_clock::time_point deadline,
    bool deadline_enabled) noexcept {
    TransferProgress progress{};
    unsigned ready_without_progress = 0U;
    while (progress.transferred < destination.size()) {
        if (closing_or_closed()) {
            progress.closed = true;
            return progress;
        }
        if (deadline_enabled && std::chrono::steady_clock::now() >= deadline) {
            progress.timed_out = true;
            return progress;
        }

        std::error_code error;
        const auto remaining = destination.subspan(progress.transferred);
        const auto transferred = socket_.read_some(
            asio::buffer(remaining.data(), remaining.size()), error);
        progress.transferred += transferred;
        if (transferred != 0U) {
            ready_without_progress = 0U;
        }
        if (!error) {
            if (transferred == 0U) {
                await_readiness(
                    Direction::Read, deadline, deadline_enabled,
                    ready_without_progress);
            }
            continue;
        }
        if (error == asio::error::would_block || error == asio::error::try_again) {
            await_readiness(
                Direction::Read, deadline, deadline_enabled,
                ready_without_progress);
            continue;
        }
        if (closing_or_closed()) {
            progress.closed = true;
            return progress;
        }
        progress.system_error = error;
        return progress;
    }
    return progress;
}

TcpFramedStream::TransferProgress TcpFramedStream::write_exact(
    std::span<const std::byte> source,
    std::chrono::steady_clock::time_point deadline,
    bool deadline_enabled) noexcept {
    TransferProgress progress{};
    unsigned ready_without_progress = 0U;
    while (progress.transferred < source.size()) {
        if (closing_or_closed()) {
            progress.closed = true;
            return progress;
        }
        if (deadline_enabled && std::chrono::steady_clock::now() >= deadline) {
            progress.timed_out = true;
            return progress;
        }

        std::error_code error;
        const auto remaining = source.subspan(progress.transferred);
        const auto transferred = socket_.write_some(
            asio::buffer(remaining.data(), remaining.size()), error);
        progress.transferred += transferred;
        if (transferred != 0U) {
            ready_without_progress = 0U;
        }
        if (!error) {
            if (transferred == 0U) {
                await_readiness(
                    Direction::Write, deadline, deadline_enabled,
                    ready_without_progress);
            }
            continue;
        }
        if (error == asio::error::would_block || error == asio::error::try_again) {
            await_readiness(
                Direction::Write, deadline, deadline_enabled,
                ready_without_progress);
            continue;
        }
        if (closing_or_closed()) {
            progress.closed = true;
            return progress;
        }
        progress.system_error = error;
        return progress;
    }
    return progress;
}

void TcpFramedStream::end_operation() noexcept {
    {
        const std::lock_guard lock{lifecycle_mutex_};
        if (active_operations_ == 0U) {
            std::terminate();
        }
        --active_operations_;
    }
    lifecycle_changed_.notify_all();
}

void TcpFramedStream::await_readiness(
    Direction direction,
    std::chrono::steady_clock::time_point deadline,
    bool deadline_enabled,
    unsigned& consecutive_ready_without_progress) noexcept {
    auto slice = readiness_wait_slice;
    if (deadline_enabled) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero()) {
            // The caller re-checks the deadline immediately; waiting here
            // would only delay the timeout it is about to report.
            return;
        }
        slice = std::min(slice, remaining);
    }

    if (consecutive_ready_without_progress >= max_ready_without_progress) {
        std::this_thread::sleep_for(std::min(slice, nonblocking_retry_pause));
        return;
    }

    // The native handle stays valid for the whole wait: close() only shuts the
    // socket down before it waits for active operations to unwind, and closes
    // the descriptor after.
    switch (detail::wait_for_socket(
        socket_.native_handle(),
        direction == Direction::Write ? detail::WaitDirection::Write
                                      : detail::WaitDirection::Read,
        slice)) {
    case detail::WaitOutcome::Ready:
        ++consecutive_ready_without_progress;
        break;
    case detail::WaitOutcome::TimedOut:
        // The whole slice elapsed, so this path cannot be spinning.
        consecutive_ready_without_progress = 0U;
        break;
    case detail::WaitOutcome::Unavailable:
        std::this_thread::sleep_for(std::min(slice, nonblocking_retry_pause));
        break;
    }
}

void TcpFramedStream::poison_and_shutdown() noexcept {
    usable_.store(false, std::memory_order_release);
    std::error_code ignored;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
}

bool TcpFramedStream::closing_or_closed() const noexcept {
    const std::lock_guard lock{lifecycle_mutex_};
    return lifecycle_ != Lifecycle::Open;
}

bool TcpFramedStream::is_open() const noexcept {
    const std::lock_guard lock{lifecycle_mutex_};
    return lifecycle_ != Lifecycle::Closed;
}

void TcpFramedStream::interrupt() noexcept {
    const std::lock_guard lock{lifecycle_mutex_};
    if (lifecycle_ == Lifecycle::Open) poison_and_shutdown();
}

const FramedStreamAdmissionBudget& TcpFramedStream::admission_budget() const noexcept {
    return shared_admission_budget_
        ? *shared_admission_budget_
        : local_admission_budget_;
}

core::Result<std::monostate, FramedStreamError> TcpFramedStream::send(
    std::span<const std::byte> size_prefixed_message) {
    using Result = core::Result<std::monostate, FramedStreamError>;
    if (size_prefixed_message.size() < size_prefix_bytes) {
        return Result::failure(make_error(FramedStreamErrorCode::InvalidSizePrefix));
    }

    const auto prefix = std::span<const std::byte, size_prefix_bytes>{
        size_prefixed_message.data(), size_prefix_bytes};
    const auto body_bytes = static_cast<std::size_t>(read_u32_le(prefix));
    if (body_bytes != size_prefixed_message.size() - size_prefix_bytes) {
        return Result::failure(make_error(FramedStreamErrorCode::InvalidSizePrefix));
    }
    if (body_bytes == 0U || size_prefixed_message.size() > limits_.max_message_bytes) {
        return Result::failure(make_error(FramedStreamErrorCode::SizeLimitExceeded));
    }

    std::chrono::milliseconds timeout{};
    if (!begin_operation(Direction::Write, timeout)) {
        return Result::failure(make_error(FramedStreamErrorCode::Closed));
    }
    const ActiveOperation active{*this};

    const auto deadline_enabled = timeout > std::chrono::milliseconds::zero();
    const auto deadline = deadline_enabled
        ? std::chrono::steady_clock::now() + timeout
        : std::chrono::steady_clock::time_point::max();
    const auto progress = write_exact(
        size_prefixed_message, deadline, deadline_enabled);
    if (progress.timed_out || progress.closed || progress.system_error
        || progress.transferred != size_prefixed_message.size()) {
        poison_and_shutdown();
        return Result::failure(make_error(
            progress.timed_out
                ? FramedStreamErrorCode::TimedOut
                : (progress.closed ? FramedStreamErrorCode::Closed
                                   : FramedStreamErrorCode::WriteFailed),
            progress.system_error,
            progress.transferred));
    }
    return Result::success(std::monostate{});
}

core::Result<std::vector<std::byte>, FramedStreamError> TcpFramedStream::receive() {
    using Result = core::Result<std::vector<std::byte>, FramedStreamError>;
    std::chrono::milliseconds timeout{};
    if (!begin_operation(Direction::Read, timeout)) {
        return Result::failure(make_error(FramedStreamErrorCode::Closed));
    }
    const ActiveOperation active{*this};

    const auto deadline_enabled = timeout > std::chrono::milliseconds::zero();
    const auto deadline = deadline_enabled
        ? std::chrono::steady_clock::now() + timeout
        : std::chrono::steady_clock::time_point::max();

    std::array<std::byte, size_prefix_bytes> prefix{};
    const auto prefix_progress = read_exact(prefix, deadline, deadline_enabled);
    if (prefix_progress.timed_out || prefix_progress.closed
        || prefix_progress.system_error
        || prefix_progress.transferred != prefix.size()) {
        poison_and_shutdown();
        return Result::failure(make_error(
            prefix_progress.timed_out
                ? FramedStreamErrorCode::TimedOut
                : (prefix_progress.closed ? FramedStreamErrorCode::Closed
                                          : FramedStreamErrorCode::ReadFailed),
            prefix_progress.system_error,
            prefix_progress.transferred));
    }

    const auto body_bytes = static_cast<std::size_t>(read_u32_le(prefix));
    if (body_bytes == 0U
        || body_bytes > std::numeric_limits<std::size_t>::max() - size_prefix_bytes
        || body_bytes + size_prefix_bytes > limits_.max_message_bytes) {
        poison_and_shutdown();
        return Result::failure(make_error(
            FramedStreamErrorCode::SizeLimitExceeded, {}, prefix.size()));
    }

    const auto message_bytes = body_bytes + size_prefix_bytes;
    BudgetReservation reservation{
        shared_admission_budget_ ? *shared_admission_budget_ : local_admission_budget_,
        message_bytes};
    if (!reservation.acquired()) {
        poison_and_shutdown();
        return Result::failure(make_error(
            FramedStreamErrorCode::AdmissionLimitExceeded, {}, prefix.size()));
    }

    std::vector<std::byte> message;
    try {
        message.resize(message_bytes);
    } catch (const std::bad_alloc&) {
        poison_and_shutdown();
        return Result::failure(make_error(
            FramedStreamErrorCode::AllocationFailed, {}, prefix.size()));
    } catch (const std::length_error&) {
        poison_and_shutdown();
        return Result::failure(make_error(
            FramedStreamErrorCode::AllocationFailed, {}, prefix.size()));
    }
    std::copy(prefix.begin(), prefix.end(), message.begin());

    const auto body = std::span<std::byte>{message}.subspan(size_prefix_bytes);
    const auto body_progress = read_exact(body, deadline, deadline_enabled);
    if (body_progress.timed_out || body_progress.closed
        || body_progress.system_error
        || body_progress.transferred != body.size()) {
        poison_and_shutdown();
        return Result::failure(make_error(
            body_progress.timed_out
                ? FramedStreamErrorCode::TimedOut
                : (body_progress.closed ? FramedStreamErrorCode::Closed
                                        : FramedStreamErrorCode::ReadFailed),
            body_progress.system_error,
            prefix.size() + body_progress.transferred));
    }
    return Result::success(std::move(message));
}

void TcpFramedStream::close() noexcept {
    {
        std::unique_lock lock{lifecycle_mutex_};
        if (lifecycle_ == Lifecycle::Closed) {
            return;
        }
        if (lifecycle_ == Lifecycle::Closing) {
            lifecycle_changed_.wait(
                lock, [this] { return lifecycle_ == Lifecycle::Closed; });
            return;
        }
        lifecycle_ = Lifecycle::Closing;
        usable_.store(false, std::memory_order_release);
    }

    std::error_code ignored;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);

    {
        std::unique_lock lock{lifecycle_mutex_};
        lifecycle_changed_.wait(
            lock, [this] { return active_operations_ == 0U; });
    }

    socket_.close(ignored);

    {
        const std::lock_guard lock{lifecycle_mutex_};
        lifecycle_ = Lifecycle::Closed;
    }
    lifecycle_changed_.notify_all();
}

void TcpFramedStream::shutdown_send() noexcept {
    const std::lock_guard lock{lifecycle_mutex_};
    if (lifecycle_ != Lifecycle::Open || !socket_.is_open()) {
        return;
    }
    std::error_code ignored;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_send, ignored);
}

void TcpFramedStream::drain_and_close(
    std::chrono::milliseconds linger) noexcept {
    bool registered = false;
    {
        const std::lock_guard lock{lifecycle_mutex_};
        if (lifecycle_ == Lifecycle::Open
            && usable_.load(std::memory_order_acquire) && socket_.is_open()) {
            // Registered as an ordinary read so a concurrent close waits for
            // this loop to unwind before it touches the native socket.
            ++active_operations_;
            registered = true;
        }
    }
    if (registered) {
        const ActiveOperation active{*this};
        const auto deadline = std::chrono::steady_clock::now() + linger;
        std::array<std::byte, 512U> discarded{};
        unsigned ready_without_progress = 0U;
        while (std::chrono::steady_clock::now() < deadline) {
            if (closing_or_closed()) {
                break;
            }
            std::error_code error;
            const auto transferred = socket_.read_some(
                asio::buffer(discarded.data(), discarded.size()), error);
            if (transferred != 0U) {
                ready_without_progress = 0U;
            }
            if (error == asio::error::would_block
                || error == asio::error::try_again) {
                await_readiness(
                    Direction::Read, deadline, true, ready_without_progress);
                continue;
            }
            // The peer's close, a reset, or any other failure: there is
            // nothing further to wait for either way.
            if (error || transferred == 0U) {
                break;
            }
        }
    }
    close();
}

} // namespace mediaserver::net
