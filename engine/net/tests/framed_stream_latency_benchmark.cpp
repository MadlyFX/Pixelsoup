// Measures request/response latency through TcpFramedStream on loopback.
//
// The framed stream drives a non-blocking socket from a synchronous API, so
// every transfer that finds the socket not ready has to wait somewhere. What
// that wait costs is the whole story for interactive round trips: a sleeping
// retry rounds up to a scheduler quantum, which on a default Windows timer
// resolution is about 15.6 ms rather than the 1 ms requested. A round trip
// blocks at least once by construction, so the quantum lands on every one of
// them and a multi-exchange transfer pays it repeatedly no matter how small
// the payload is.
//
// Not registered with CTest: it is a stopwatch, not a pass/fail assertion.

#include <mediaserver/net/tcp_framed_stream.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/address_v4.hpp>
#include <asio/ip/tcp.hpp>

namespace net = mediaserver::net;

namespace {

using namespace std::chrono_literals;

#define BENCH_CHECK(condition)                                                 \
    do {                                                                       \
        if (!(condition)) {                                                    \
            throw std::runtime_error{                                          \
                std::string{"check failed: "} + #condition};                   \
        }                                                                      \
    } while (false)

void put_u32_le(std::span<std::byte, 4U> bytes, std::uint32_t value) noexcept {
    bytes[0U] = static_cast<std::byte>(value & 0xffU);
    bytes[1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[2U] = static_cast<std::byte>((value >> 16U) & 0xffU);
    bytes[3U] = static_cast<std::byte>((value >> 24U) & 0xffU);
}

[[nodiscard]] std::vector<std::byte> packet(std::size_t body_bytes) {
    BENCH_CHECK(
        body_bytes <= static_cast<std::size_t>(
                          std::numeric_limits<std::uint32_t>::max()));
    std::vector<std::byte> bytes(body_bytes + 4U, std::byte{0x5aU});
    put_u32_le(
        std::span<std::byte, 4U>{bytes.data(), 4U},
        static_cast<std::uint32_t>(body_bytes));
    return bytes;
}

class TcpPair final {
public:
    TcpPair() : acceptor_(context_), client_(context_), server_(context_) {
        const asio::ip::tcp::endpoint listen_endpoint{
            asio::ip::address_v4::loopback(), 0U};
        std::error_code error;
        acceptor_.open(listen_endpoint.protocol(), error);
        BENCH_CHECK(!error);
        acceptor_.set_option(asio::socket_base::reuse_address{true}, error);
        BENCH_CHECK(!error);
        acceptor_.bind(listen_endpoint, error);
        BENCH_CHECK(!error);
        acceptor_.listen(asio::socket_base::max_listen_connections, error);
        BENCH_CHECK(!error);
        client_.connect(acceptor_.local_endpoint(), error);
        BENCH_CHECK(!error);
        acceptor_.accept(server_, error);
        BENCH_CHECK(!error);
    }

    [[nodiscard]] asio::ip::tcp::socket take_client() { return std::move(client_); }
    [[nodiscard]] asio::ip::tcp::socket take_server() { return std::move(server_); }

private:
    asio::io_context context_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket client_;
    asio::ip::tcp::socket server_;
};

[[nodiscard]] double percentile(
    const std::vector<double>& sorted_samples, double fraction) {
    BENCH_CHECK(!sorted_samples.empty());
    const auto last = static_cast<double>(sorted_samples.size() - 1U);
    const auto index = static_cast<std::size_t>(fraction * last + 0.5);
    return sorted_samples[std::min(index, sorted_samples.size() - 1U)];
}

void report(const std::string& label, std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    double total = 0.0;
    for (const auto sample : samples) {
        total += sample;
    }
    std::cout << std::left << std::setw(38) << label << std::right
              << std::fixed << std::setprecision(2)
              << "  n=" << std::setw(4) << samples.size()
              << "  min=" << std::setw(8) << samples.front()
              << "  p50=" << std::setw(8) << percentile(samples, 0.50)
              << "  p95=" << std::setw(8) << percentile(samples, 0.95)
              << "  p99=" << std::setw(8) << percentile(samples, 0.99)
              << "  max=" << std::setw(8) << samples.back()
              << "  mean=" << std::setw(8)
              << total / static_cast<double>(samples.size()) << "  ms\n";
}

// Echoes every record it receives until the peer closes the connection.
void serve(net::TcpFramedStream& stream, std::size_t response_body_bytes) {
    const auto response = packet(response_body_bytes);
    while (true) {
        auto request = stream.receive();
        if (!request) {
            return;
        }
        if (!stream.send(response)) {
            return;
        }
    }
}

[[nodiscard]] double elapsed_milliseconds(
    std::chrono::steady_clock::time_point started) {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - started)
        .count();
}

// One exchange of `round_trips` sequential request/response pairs. The final
// request carries `payload_body_bytes`; the earlier ones are negotiation-sized.
// This is the shape that made a small resource transfer expensive: the cost
// tracks the number of blocking exchanges, not the number of bytes.
[[nodiscard]] std::vector<double> measure_exchange(
    net::TcpFramedStream& client,
    std::size_t round_trips,
    std::size_t payload_body_bytes,
    std::size_t iterations) {
    const auto small_request = packet(64U);
    const auto payload_request = packet(payload_body_bytes);

    std::vector<double> samples;
    samples.reserve(iterations);
    for (std::size_t iteration = 0U; iteration < iterations; ++iteration) {
        const auto started = std::chrono::steady_clock::now();
        for (std::size_t trip = 0U; trip < round_trips; ++trip) {
            const auto& request =
                trip + 1U == round_trips ? payload_request : small_request;
            BENCH_CHECK(client.send(request).has_value());
            BENCH_CHECK(client.receive().has_value());
        }
        samples.push_back(elapsed_milliseconds(started));
    }
    return samples;
}

void report_sleep_quantum() {
    constexpr std::size_t samples = 50U;
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t index = 0U; index < samples; ++index) {
        std::this_thread::sleep_for(1ms);
    }
    const auto each = elapsed_milliseconds(started) / static_cast<double>(samples);
    std::cout << std::left << std::setw(38) << "sleep_for(1ms) actual cost"
              << std::right << std::fixed << std::setprecision(2)
              << "  " << each << " ms per sleep\n\n";
}

} // namespace

int main() {
    try {
        report_sleep_quantum();

        TcpPair pair;
        net::TcpFramedStream client{pair.take_client()};
        net::TcpFramedStream server{pair.take_server()};

        constexpr std::size_t response_body_bytes = 256U;
        std::thread responder{[&server] { serve(server, response_body_bytes); }};

        // Warm the connection so congestion-window growth is not attributed to
        // the wait strategy under test.
        static_cast<void>(measure_exchange(client, 1U, 256U, 50U));

        report(
            "event round trip (256 B)",
            measure_exchange(client, 1U, 256U, 400U));
        report(
            "resource transfer 3 KB (3 trips)",
            measure_exchange(client, 3U, 3U * 1024U, 200U));
        report(
            "resource transfer 256 KB (3 trips)",
            measure_exchange(client, 3U, 256U * 1024U, 200U));

        client.close();
        responder.join();
        server.close();
    } catch (const std::exception& error) {
        std::cerr << "benchmark failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
