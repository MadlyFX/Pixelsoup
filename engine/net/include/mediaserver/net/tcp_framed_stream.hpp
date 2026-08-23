#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <system_error>
#include <variant>
#include <vector>

#include <asio/ip/tcp.hpp>

#include <mediaserver/core/result.hpp>

namespace mediaserver::net {

struct FramedStreamLimits final {
    // A 64 MiB aggregate frame can require roughly 6 MiB of FlatBuffers
    // packet-table overhead at the maximum record count.
    std::size_t max_message_bytes{72U * 1024U * 1024U};
    // A zero timeout disables that deadline. The first valid operation in each
    // direction is considered a handshake operation. Later operations use the
    // read or write timeout for their direction.
    std::chrono::milliseconds handshake_timeout{};
    std::chrono::milliseconds read_timeout{};
    std::chrono::milliseconds write_timeout{};
};

// A non-blocking aggregate reservation shared by all streams admitted by one
// listener. Reservations cover the complete receive allocation from prefix
// validation until the body read finishes. This prevents many prefix-only
// peers from each allocating max_message_bytes concurrently.
class FramedStreamAdmissionBudget final {
public:
    explicit FramedStreamAdmissionBudget(std::size_t capacity_bytes) noexcept
        : capacity_bytes_(capacity_bytes) {}

    FramedStreamAdmissionBudget(const FramedStreamAdmissionBudget&) = delete;
    FramedStreamAdmissionBudget& operator=(const FramedStreamAdmissionBudget&) = delete;

    [[nodiscard]] bool try_reserve(std::size_t bytes) noexcept;
    void release(std::size_t bytes) noexcept;

    [[nodiscard]] std::size_t capacity_bytes() const noexcept {
        return capacity_bytes_;
    }
    [[nodiscard]] std::size_t reserved_bytes() const noexcept {
        return reserved_bytes_.load(std::memory_order_acquire);
    }

private:
    std::size_t capacity_bytes_{};
    std::atomic_size_t reserved_bytes_{0U};
};

enum class FramedStreamErrorCode : std::uint8_t {
    Closed,
    InvalidSizePrefix,
    SizeLimitExceeded,
    AdmissionLimitExceeded,
    AllocationFailed,
    TimedOut,
    ReadFailed,
    WriteFailed,
};

struct FramedStreamError final {
    FramedStreamErrorCode code;
    std::error_code system_error{};
    std::size_t transferred{};
};

// Blocking, size-prefixed TCP record stream. A complete encoded session packet
// is returned to the caller or no packet is returned at all. A short read/write
// or I/O error poisons the stream; reconnect is the only recovery path.
//
// The socket may have one reader and one writer on separate threads. Multiple
// concurrent readers or writers require synchronization by the owner. close()
// may run concurrently with those calls: it rejects new operations, uses
// shutdown to wake active calls, waits for them to unwind, and only then closes
// the socket. Owners must call close and join operation threads before destroying
// the stream; destruction from inside send/receive is not supported.
class TcpFramedStream final {
public:
    explicit TcpFramedStream(
        asio::ip::tcp::socket socket,
        FramedStreamLimits limits = {},
        std::shared_ptr<FramedStreamAdmissionBudget> admission_budget = {}) noexcept;

    TcpFramedStream(TcpFramedStream&&) = delete;
    TcpFramedStream& operator=(TcpFramedStream&&) = delete;
    TcpFramedStream(const TcpFramedStream&) = delete;
    TcpFramedStream& operator=(const TcpFramedStream&) = delete;
    ~TcpFramedStream();

    [[nodiscard]] core::Result<std::monostate, FramedStreamError> send(
        std::span<const std::byte> size_prefixed_message);
    [[nodiscard]] core::Result<std::vector<std::byte>, FramedStreamError> receive();

    void close() noexcept;

    // Ordered half-close of the send direction. The FIN follows every byte
    // already written, so a peer that keeps reading is guaranteed to see the
    // complete record stream before it sees the end of it. The receive
    // direction stays open, which is the point: shutting receive down while
    // the peer's own traffic is still queued makes the eventual close an
    // abort, and an abort discards whatever the peer has not read yet.
    void shutdown_send() noexcept;

    // Completes a graceful close begun by shutdown_send(). Inbound bytes are
    // read and discarded until the peer closes, `linger` elapses, or the
    // stream fails, and only then is the socket closed. The peer's own close
    // is the proof that it consumed everything written before the FIN; only a
    // peer that never closes pays the full linger. The caller must own the
    // receive direction: no other read may be in flight.
    void drain_and_close(std::chrono::milliseconds linger) noexcept;

    [[nodiscard]] bool usable() const noexcept {
        return usable_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] const FramedStreamLimits& limits() const noexcept { return limits_; }
    [[nodiscard]] const FramedStreamAdmissionBudget& admission_budget() const noexcept;

private:
    enum class Direction : std::uint8_t {
        Read,
        Write,
    };

    enum class Lifecycle : std::uint8_t {
        Open,
        Closing,
        Closed,
    };

    class ActiveOperation final {
    public:
        explicit ActiveOperation(TcpFramedStream& owner) noexcept : owner_(&owner) {}
        ActiveOperation(const ActiveOperation&) = delete;
        ActiveOperation& operator=(const ActiveOperation&) = delete;
        ~ActiveOperation();

    private:
        TcpFramedStream* owner_;
    };

    struct TransferProgress final {
        std::error_code system_error{};
        std::size_t transferred{};
        bool timed_out{};
        bool closed{};
    };

    [[nodiscard]] bool begin_operation(
        Direction direction,
        std::chrono::milliseconds& timeout) noexcept;
    [[nodiscard]] TransferProgress read_exact(
        std::span<std::byte> destination,
        std::chrono::steady_clock::time_point deadline,
        bool deadline_enabled) noexcept;
    [[nodiscard]] TransferProgress write_exact(
        std::span<const std::byte> source,
        std::chrono::steady_clock::time_point deadline,
        bool deadline_enabled) noexcept;
    void end_operation() noexcept;
    void poison_and_shutdown() noexcept;
    [[nodiscard]] bool closing_or_closed() const noexcept;

    asio::ip::tcp::socket socket_;
    FramedStreamLimits limits_{};
    FramedStreamAdmissionBudget local_admission_budget_;
    std::shared_ptr<FramedStreamAdmissionBudget> shared_admission_budget_;

    mutable std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_changed_;
    Lifecycle lifecycle_{Lifecycle::Open};
    std::size_t active_operations_{};
    bool first_read_{true};
    bool first_write_{true};
    // A reader and writer may fail independently on separate threads. Keep the
    // poison latch race-free. The lifecycle mutex keeps close/is_open and the
    // non-thread-safe native close operation away from active calls.
    std::atomic_bool usable_{true};
};

} // namespace mediaserver::net
