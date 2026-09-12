#pragma once

// Bounded readiness waiting for the non-blocking sockets this module drives
// from synchronous loops.
//
// A loop that cannot block in the OS call has to wait somewhere between
// retries. Sleeping is the wrong place: at the default Windows timer
// resolution the shortest sleep really takes about one 15.6 ms scheduler
// quantum, so every park costs a quantum and an exchange costs one per park,
// whatever it was carrying. Waiting on the socket itself wakes the moment the
// socket is usable, and the timeout is what keeps a stop flag or a deadline
// responsive.

#include <chrono>
#include <cstdint>

#include <asio/ip/tcp.hpp>

namespace mediaserver::net::detail {

enum class WaitOutcome : std::uint8_t {
    // The socket reported readiness, an error, or a hangup. Which of those it
    // was belongs to the transfer that follows, not to the wait.
    Ready,
    // The whole timeout elapsed with nothing to report.
    TimedOut,
    // The wait itself could not run. Callers fall back to a short sleep; this
    // is not a path that repeats in normal operation.
    Unavailable,
};

enum class WaitDirection : std::uint8_t {
    // For a listening socket, readable means a connection is pending.
    Read,
    Write,
};

// Waits until `handle` is ready in `direction`, `timeout` elapses, or the wait
// is unusable. Never blocks longer than `timeout`.
[[nodiscard]] WaitOutcome wait_for_socket(
    asio::ip::tcp::socket::native_handle_type handle,
    WaitDirection direction,
    std::chrono::milliseconds timeout) noexcept;

} // namespace mediaserver::net::detail
