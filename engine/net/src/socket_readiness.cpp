#include "socket_readiness.hpp"

#include <algorithm>
#include <limits>

// The socket headers are already in scope through <asio/ip/tcp.hpp> above;
// naming them here is what makes the wait below legible.
#if defined(_WIN32)
#include <winsock2.h>
#else
#include <poll.h>
#endif

namespace mediaserver::net::detail {

WaitOutcome wait_for_socket(
    asio::ip::tcp::socket::native_handle_type handle,
    WaitDirection direction,
    std::chrono::milliseconds timeout) noexcept {
    // Callers bound the timeout to keep a stop flag or a deadline responsive,
    // so it always fits; clamp rather than let a surprising value wrap.
    // Parenthesised so a windows.h that leaked its max() macro cannot claim
    // this call.
    constexpr auto longest = static_cast<std::chrono::milliseconds::rep>(
        (std::numeric_limits<int>::max)());
    const auto milliseconds = timeout.count() <= 0
        ? 0
        : static_cast<int>(std::min(timeout.count(), longest));

    const auto for_write = direction == WaitDirection::Write;
#if defined(_WIN32)
    WSAPOLLFD entry{};
    entry.fd = handle;
    entry.events = static_cast<SHORT>(for_write ? POLLWRNORM : POLLRDNORM);
    const auto ready = ::WSAPoll(&entry, 1UL, milliseconds);
#else
    ::pollfd entry{};
    entry.fd = handle;
    entry.events = static_cast<short>(for_write ? POLLOUT : POLLIN);
    const auto ready = ::poll(&entry, 1U, milliseconds);
#endif
    if (ready < 0) {
        // Including an interrupted wait. The caller's sleeping fallback is
        // correct for both, and neither repeats in normal operation.
        return WaitOutcome::Unavailable;
    }
    // Error and hangup flags are reported whether or not they were requested.
    // Treat them as readiness: the operation that follows turns them into the
    // caller's own error, which is where that classification belongs.
    return ready == 0 ? WaitOutcome::TimedOut : WaitOutcome::Ready;
}

} // namespace mediaserver::net::detail
