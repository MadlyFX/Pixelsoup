#include <mediaserver/net/tcp_framed_stream.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <asio/io_context.hpp>
#include <asio/ip/address_v4.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/write.hpp>

namespace net = mediaserver::net;

namespace {

using namespace std::chrono_literals;

#define NET_CHECK(condition)                                                        \
    do {                                                                            \
        if (!(condition)) {                                                          \
            throw std::runtime_error{                                                \
                std::string{"check failed: "} + #condition};                       \
        }                                                                            \
    } while (false)

void put_u32_le(std::span<std::byte, 4U> bytes, std::uint32_t value) noexcept {
    bytes[0U] = static_cast<std::byte>(value & 0xffU);
    bytes[1U] = static_cast<std::byte>((value >> 8U) & 0xffU);
    bytes[2U] = static_cast<std::byte>((value >> 16U) & 0xffU);
    bytes[3U] = static_cast<std::byte>((value >> 24U) & 0xffU);
}

[[nodiscard]] std::vector<std::byte> packet(std::size_t body_bytes, std::byte fill) {
    NET_CHECK(
        body_bytes <= static_cast<std::size_t>(
                          std::numeric_limits<std::uint32_t>::max()));
    std::vector<std::byte> bytes(body_bytes + 4U, fill);
    put_u32_le(
        std::span<std::byte, 4U>{bytes.data(), 4U},
        static_cast<std::uint32_t>(body_bytes));
    return bytes;
}

class TcpPair final {
public:
    TcpPair()
        : acceptor_(context_), client_(context_), server_(context_) {
        const asio::ip::tcp::endpoint listen_endpoint{
            asio::ip::address_v4::loopback(), 0U};
        std::error_code error;
        acceptor_.open(listen_endpoint.protocol(), error);
        NET_CHECK(!error);
        acceptor_.set_option(asio::socket_base::reuse_address{true}, error);
        NET_CHECK(!error);
        acceptor_.bind(listen_endpoint, error);
        NET_CHECK(!error);
        acceptor_.listen(asio::socket_base::max_listen_connections, error);
        NET_CHECK(!error);
        client_.connect(acceptor_.local_endpoint(), error);
        NET_CHECK(!error);
        acceptor_.accept(server_, error);
        NET_CHECK(!error);
    }

    [[nodiscard]] asio::ip::tcp::socket take_server() {
        return std::move(server_);
    }
    [[nodiscard]] asio::ip::tcp::socket take_client() {
        return std::move(client_);
    }
    [[nodiscard]] asio::ip::tcp::socket& client() noexcept { return client_; }

private:
    asio::io_context context_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket client_;
    asio::ip::tcp::socket server_;
};

[[nodiscard]] bool wait_for_reservation(
    const net::FramedStreamAdmissionBudget& budget,
    std::size_t expected) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (budget.reserved_bytes() == expected) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return budget.reserved_bytes() == expected;
}

void complete_packets_round_trip_in_order() {
    TcpPair pair;
    net::TcpFramedStream server{pair.take_server()};
    const auto first = packet(7U, std::byte{0x31U});
    const auto second = packet(19U, std::byte{0x72U});

    NET_CHECK(asio::write(pair.client(), asio::buffer(first)) == first.size());
    NET_CHECK(asio::write(pair.client(), asio::buffer(second)) == second.size());

    auto received_first = server.receive();
    auto received_second = server.receive();
    NET_CHECK(received_first);
    NET_CHECK(received_second);
    NET_CHECK(received_first.value() == first);
    NET_CHECK(received_second.value() == second);
}

void one_byte_writes_reassemble_one_packet() {
    TcpPair pair;
    net::TcpFramedStream server{pair.take_server()};
    const auto expected = packet(257U, std::byte{0xa5U});
    for (const auto value : expected) {
        const std::array<std::byte, 1U> byte{value};
        NET_CHECK(asio::write(pair.client(), asio::buffer(byte)) == byte.size());
    }

    auto received = server.receive();
    NET_CHECK(received);
    NET_CHECK(received.value() == expected);
}

void oversized_prefix_is_rejected_before_payload_read() {
    TcpPair pair;
    net::TcpFramedStream server{
        pair.take_server(),
        net::FramedStreamLimits{32U}};
    std::array<std::byte, 4U> prefix{};
    put_u32_le(prefix, 1024U);
    NET_CHECK(asio::write(pair.client(), asio::buffer(prefix)) == prefix.size());

    auto received = server.receive();
    NET_CHECK(!received);
    NET_CHECK(
        received.error().code == net::FramedStreamErrorCode::SizeLimitExceeded);
    NET_CHECK(received.error().transferred == prefix.size());
    NET_CHECK(!server.usable());
}

void truncated_body_poisoning_is_explicit() {
    TcpPair pair;
    net::TcpFramedStream server{pair.take_server()};
    std::array<std::byte, 4U> prefix{};
    put_u32_le(prefix, 10U);
    const std::array<std::byte, 3U> partial{
        std::byte{1U}, std::byte{2U}, std::byte{3U}};
    NET_CHECK(asio::write(pair.client(), asio::buffer(prefix)) == prefix.size());
    NET_CHECK(asio::write(pair.client(), asio::buffer(partial)) == partial.size());
    std::error_code ignored;
    pair.client().shutdown(asio::ip::tcp::socket::shutdown_send, ignored);

    auto received = server.receive();
    NET_CHECK(!received);
    NET_CHECK(received.error().code == net::FramedStreamErrorCode::ReadFailed);
    NET_CHECK(received.error().transferred == prefix.size() + partial.size());
    NET_CHECK(!server.usable());
    auto repeated = server.receive();
    NET_CHECK(!repeated);
    NET_CHECK(repeated.error().code == net::FramedStreamErrorCode::Closed);
}

void local_send_validation_does_not_poison_the_socket() {
    TcpPair pair;
    net::TcpFramedStream server{pair.take_server()};
    const std::array<std::byte, 4U> invalid{};
    auto rejected = server.send(invalid);
    NET_CHECK(!rejected);
    NET_CHECK(
        rejected.error().code == net::FramedStreamErrorCode::SizeLimitExceeded);
    NET_CHECK(server.usable());

    const auto valid = packet(5U, std::byte{0x19U});
    auto sent = server.send(valid);
    NET_CHECK(sent);
    std::vector<std::byte> received(valid.size());
    std::error_code error;
    const auto read = asio::read(pair.client(), asio::buffer(received), error);
    NET_CHECK(!error);
    NET_CHECK(read == valid.size());
    NET_CHECK(received == valid);
}

void one_reader_and_one_writer_can_progress_concurrently() {
    TcpPair pair;
    net::TcpFramedStream server{pair.take_server()};
    const auto inbound = packet(4096U, std::byte{0x2aU});
    const auto outbound = packet(3072U, std::byte{0x6bU});

    std::optional<decltype(server.receive())> received;
    std::optional<decltype(server.send(outbound))> sent;
    std::thread reader{[&server, &received] { received = server.receive(); }};
    std::thread writer{
        [&server, &outbound, &sent] { sent = server.send(outbound); }};

    std::error_code write_error;
    const auto client_written = asio::write(
        pair.client(), asio::buffer(inbound), write_error);
    std::vector<std::byte> client_received(outbound.size());
    std::error_code read_error;
    const auto client_read = asio::read(
        pair.client(), asio::buffer(client_received), read_error);
    reader.join();
    writer.join();

    NET_CHECK(!write_error);
    NET_CHECK(client_written == inbound.size());
    NET_CHECK(!read_error);
    NET_CHECK(client_read == outbound.size());
    NET_CHECK(client_received == outbound);
    NET_CHECK(received.has_value());
    NET_CHECK(*received);
    NET_CHECK(received->value() == inbound);
    NET_CHECK(sent.has_value());
    NET_CHECK(*sent);
    NET_CHECK(server.usable());
}

void prefix_only_handshake_timeout_releases_budget() {
    constexpr std::size_t body_bytes = 32U;
    constexpr std::size_t message_bytes = body_bytes + 4U;
    TcpPair pair;
    auto budget = std::make_shared<net::FramedStreamAdmissionBudget>(message_bytes);
    net::FramedStreamLimits limits{message_bytes};
    limits.handshake_timeout = 75ms;
    net::TcpFramedStream server{pair.take_server(), limits, budget};

    std::array<std::byte, 4U> prefix{};
    put_u32_le(prefix, static_cast<std::uint32_t>(body_bytes));
    NET_CHECK(asio::write(pair.client(), asio::buffer(prefix)) == prefix.size());

    auto received = server.receive();
    NET_CHECK(!received);
    NET_CHECK(received.error().code == net::FramedStreamErrorCode::TimedOut);
    NET_CHECK(received.error().transferred == prefix.size());
    NET_CHECK(budget->reserved_bytes() == 0U);
    NET_CHECK(!server.usable());
}

void shared_budget_rejects_second_prefix_before_allocation() {
    constexpr std::size_t body_bytes = 32U;
    constexpr std::size_t message_bytes = body_bytes + 4U;
    TcpPair first_pair;
    TcpPair second_pair;
    auto budget = std::make_shared<net::FramedStreamAdmissionBudget>(message_bytes);
    const net::FramedStreamLimits limits{message_bytes};
    net::TcpFramedStream first{first_pair.take_server(), limits, budget};
    net::TcpFramedStream second{second_pair.take_server(), limits, budget};

    std::array<std::byte, 4U> prefix{};
    put_u32_le(prefix, static_cast<std::uint32_t>(body_bytes));
    NET_CHECK(
        asio::write(first_pair.client(), asio::buffer(prefix)) == prefix.size());

    std::optional<decltype(first.receive())> first_result;
    std::thread first_reader{[&first, &first_result] {
        first_result = first.receive();
    }};

    const auto reservation_observed = wait_for_reservation(*budget, message_bytes);
    if (!reservation_observed) {
        first.close();
        first_reader.join();
    }
    NET_CHECK(reservation_observed);

    NET_CHECK(
        asio::write(second_pair.client(), asio::buffer(prefix)) == prefix.size());
    auto second_result = second.receive();
    NET_CHECK(!second_result);
    NET_CHECK(
        second_result.error().code
        == net::FramedStreamErrorCode::AdmissionLimitExceeded);
    NET_CHECK(second_result.error().transferred == prefix.size());
    NET_CHECK(budget->reserved_bytes() == message_bytes);

    first.close();
    first_reader.join();
    NET_CHECK(first_result.has_value());
    NET_CHECK(!*first_result);
    NET_CHECK(first_result->error().code == net::FramedStreamErrorCode::Closed);
    NET_CHECK(budget->reserved_bytes() == 0U);
}

void close_wakes_blocked_read_then_closes_native_socket() {
    constexpr std::size_t body_bytes = 64U;
    constexpr std::size_t message_bytes = body_bytes + 4U;
    TcpPair pair;
    auto budget = std::make_shared<net::FramedStreamAdmissionBudget>(message_bytes);
    net::TcpFramedStream server{
        pair.take_server(), net::FramedStreamLimits{message_bytes}, budget};

    std::array<std::byte, 4U> prefix{};
    put_u32_le(prefix, static_cast<std::uint32_t>(body_bytes));
    NET_CHECK(asio::write(pair.client(), asio::buffer(prefix)) == prefix.size());

    std::optional<decltype(server.receive())> received;
    std::thread reader{[&server, &received] { received = server.receive(); }};
    const auto reservation_observed = wait_for_reservation(*budget, message_bytes);
    if (!reservation_observed) {
        server.close();
        reader.join();
    }
    NET_CHECK(reservation_observed);

    const auto close_started = std::chrono::steady_clock::now();
    server.close();
    reader.join();
    // The read is parked in a readiness wait, not in a sleeping retry. Bound
    // the unwind so a wait that close() cannot interrupt fails here rather
    // than hanging the suite.
    NET_CHECK(std::chrono::steady_clock::now() - close_started < 1s);
    NET_CHECK(received.has_value());
    NET_CHECK(!*received);
    NET_CHECK(received->error().code == net::FramedStreamErrorCode::Closed);
    NET_CHECK(!server.is_open());
    NET_CHECK(!server.usable());
    NET_CHECK(budget->reserved_bytes() == 0U);

    auto repeated = server.receive();
    NET_CHECK(!repeated);
    NET_CHECK(repeated.error().code == net::FramedStreamErrorCode::Closed);
}

void round_trips_do_not_cost_a_scheduler_quantum() {
    constexpr std::size_t round_trips = 50U;
    TcpPair pair;
    net::TcpFramedStream client{pair.take_client()};
    net::TcpFramedStream server{pair.take_server()};

    std::thread responder{[&server] {
        const auto response = packet(64U, std::byte{0x11U});
        for (std::size_t trip = 0U; trip < round_trips; ++trip) {
            if (!server.receive() || !server.send(response)) {
                return;
            }
        }
    }};

    const auto request = packet(64U, std::byte{0x22U});
    std::size_t completed = 0U;
    const auto started = std::chrono::steady_clock::now();
    for (; completed < round_trips; ++completed) {
        if (!client.send(request) || !client.receive()) {
            break;
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    responder.join();
    NET_CHECK(completed == round_trips);

    // Every round trip parks at least twice, once in each peer's receive. A
    // sleeping retry rounds each of those up to a scheduler quantum -- about
    // 15.6 ms at the default Windows timer resolution -- which put a bare
    // loopback exchange past 30 ms whatever the payload was. Waiting on socket
    // readiness instead costs tens of microseconds. This bound sits far above
    // that and far below one quantum, so it holds on a loaded machine and
    // fails if a sleeping retry returns to the transfer loops.
    NET_CHECK(elapsed / round_trips < 5ms);
}

void subsequent_receive_uses_read_deadline() {
    TcpPair pair;
    net::FramedStreamLimits limits{64U};
    limits.handshake_timeout = 1s;
    limits.read_timeout = 75ms;
    net::TcpFramedStream server{pair.take_server(), limits};
    const auto initial = packet(4U, std::byte{0x44U});
    NET_CHECK(asio::write(pair.client(), asio::buffer(initial)) == initial.size());
    auto first = server.receive();
    NET_CHECK(first);

    auto second = server.receive();
    NET_CHECK(!second);
    NET_CHECK(second.error().code == net::FramedStreamErrorCode::TimedOut);
    NET_CHECK(second.error().transferred == 0U);
}

void subsequent_send_uses_write_deadline() {
    constexpr std::size_t large_body_bytes = 8U * 1024U * 1024U;
    TcpPair pair;
    auto server_socket = pair.take_server();
    std::error_code option_error;
    server_socket.set_option(asio::socket_base::send_buffer_size{4096}, option_error);
    NET_CHECK(!option_error);
    pair.client().set_option(
        asio::socket_base::receive_buffer_size{4096}, option_error);
    NET_CHECK(!option_error);

    net::FramedStreamLimits limits{large_body_bytes + 4U};
    limits.handshake_timeout = 1s;
    limits.write_timeout = 100ms;
    net::TcpFramedStream server{std::move(server_socket), limits};

    const auto initial = packet(1U, std::byte{0x21U});
    auto first = server.send(initial);
    NET_CHECK(first);
    std::array<std::byte, 5U> consumed{};
    NET_CHECK(
        asio::read(pair.client(), asio::buffer(consumed)) == consumed.size());

    const auto blocked = packet(large_body_bytes, std::byte{0x63U});
    auto attempted = server.send(blocked);
    for (std::size_t attempt = 1U; attempted && attempt < 16U; ++attempt) {
        attempted = server.send(blocked);
    }
    NET_CHECK(!attempted);
    NET_CHECK(attempted.error().code == net::FramedStreamErrorCode::TimedOut);
    NET_CHECK(attempted.error().transferred < blocked.size());
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"ordered packet round trip", &complete_packets_round_trip_in_order},
        {"one-byte fragmentation", &one_byte_writes_reassemble_one_packet},
        {"oversized prefix preflight", &oversized_prefix_is_rejected_before_payload_read},
        {"truncated body poisons stream", &truncated_body_poisoning_is_explicit},
        {"local send validation retry", &local_send_validation_does_not_poison_the_socket},
        {"concurrent reader and writer", &one_reader_and_one_writer_can_progress_concurrently},
        {"prefix-only timeout releases budget", &prefix_only_handshake_timeout_releases_budget},
        {"aggregate reservation rejects second peer", &shared_budget_rejects_second_prefix_before_allocation},
        {"close wakes blocked read", &close_wakes_blocked_read_then_closes_native_socket},
        {"round trips avoid the scheduler quantum",
         &round_trips_do_not_cost_a_scheduler_quantum},
        {"subsequent read deadline", &subsequent_receive_uses_read_deadline},
        {"subsequent write deadline", &subsequent_send_uses_write_deadline},
    };

    std::size_t failures = 0U;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[pass] " << name << std::endl;
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << (tests.size() - failures) << '/' << tests.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
