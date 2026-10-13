#include <mediaserver/net/cluster_discovery.hpp>

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace net = mediaserver::net;
using namespace std::chrono_literals;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define DISCOVERY_CHECK(expression) \
    check(static_cast<bool>(expression), #expression, __LINE__)

// Polls until `ready` holds or the deadline passes; discovery is periodic, so
// tests wait for an outcome rather than for a fixed time.
bool eventually(const std::function<bool()>& ready,
                std::chrono::milliseconds limit = 3'000ms) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (ready()) {
            return true;
        }
        std::this_thread::sleep_for(20ms);
    }
    return ready();
}

net::ClusterDiscoveryResponderConfig responder_config(
    asio::ip::tcp::endpoint control, net::ClusterDiscoveryResponderConfig base = {}) {
    // An ephemeral port, never the well-known one: a real agent on this
    // machine must neither answer these tests nor be answered by them.
    base.bind_endpoint = {asio::ip::address_v4::any(), 0U};
    base.control_endpoint = control;
    return base;
}

net::ClusterDiscoveryBrowserConfig browser_config(
    std::uint16_t port, std::vector<asio::ip::address_v4> targets) {
    net::ClusterDiscoveryBrowserConfig config{};
    config.port = port;
    config.query_interval = 100ms;
    config.expiry = 600ms;
    config.targets = std::move(targets);
    return config;
}

void queries_are_padded_and_strict() {
    const auto bytes = net::encode_cluster_discovery_query({0x0123456789ABCDEFULL});
    DISCOVERY_CHECK(bytes.size() == net::cluster_discovery_datagram_bytes);
    const auto decoded = net::decode_cluster_discovery_query(bytes);
    DISCOVERY_CHECK(decoded.has_value());
    DISCOVERY_CHECK(decoded->nonce == 0x0123456789ABCDEFULL);

    // A short query would let an agent answer with more than it was sent.
    DISCOVERY_CHECK(!net::decode_cluster_discovery_query(
        std::span<const std::uint8_t>{bytes.data(), 16U}));
    std::vector<std::uint8_t> longer(bytes.begin(), bytes.end());
    longer.push_back(0U);
    DISCOVERY_CHECK(!net::decode_cluster_discovery_query(longer));
    auto wrong_magic = bytes;
    wrong_magic[3] = 'A';
    DISCOVERY_CHECK(!net::decode_cluster_discovery_query(wrong_magic));
    auto wrong_version = bytes;
    wrong_version[4] = 2U;
    DISCOVERY_CHECK(!net::decode_cluster_discovery_query(wrong_version));
}

void answers_round_trip_and_fail_closed() {
    net::ClusterDiscoveryAnswer answer{};
    answer.nonce = 42U;
    answer.instance = 0xFEEDFACECAFEBEEFULL;
    answer.node_index = 4'095U;
    answer.control_port = 51'000U;
    answer.control_address = asio::ip::make_address_v4("192.0.2.17");
    answer.password_set = true;
    answer.hostname = "RENDER-07";
    const auto bytes = net::encode_cluster_discovery_answer(answer);
    DISCOVERY_CHECK(bytes.size() <= net::cluster_discovery_datagram_bytes);
    const auto decoded = net::decode_cluster_discovery_answer(bytes);
    DISCOVERY_CHECK(decoded.has_value());
    DISCOVERY_CHECK(*decoded == answer);

    // A hostname is bounded printable ASCII however the system spells it.
    net::ClusterDiscoveryAnswer messy = answer;
    messy.hostname = std::string(80U, 'x');
    messy.hostname[1] = '\n';
    const auto cleaned =
        net::decode_cluster_discovery_answer(net::encode_cluster_discovery_answer(messy));
    DISCOVERY_CHECK(cleaned.has_value());
    DISCOVERY_CHECK(cleaned->hostname.size() ==
                    net::cluster_discovery_max_hostname_bytes);
    DISCOVERY_CHECK(cleaned->hostname[1] == '?');
    const auto largest = net::encode_cluster_discovery_answer(messy);
    DISCOVERY_CHECK(largest.size() <= net::cluster_discovery_datagram_bytes);

    auto truncated = bytes;
    truncated.pop_back();
    DISCOVERY_CHECK(!net::decode_cluster_discovery_answer(truncated));
    auto padded = bytes;
    padded.push_back('x');
    DISCOVERY_CHECK(!net::decode_cluster_discovery_answer(padded));
    auto no_port = bytes;
    no_port[28] = 0U;
    no_port[29] = 0U;
    DISCOVERY_CHECK(!net::decode_cluster_discovery_answer(no_port));
    auto control_character = bytes;
    control_character.back() = 0x07U;
    DISCOVERY_CHECK(!net::decode_cluster_discovery_answer(control_character));
    DISCOVERY_CHECK(!net::cluster_discovery_hostname().empty());
}

void responder_configuration_describes_only_reachable_listeners() {
    const auto config = [](const char* address) {
        return responder_config(
            {asio::ip::make_address(address), 47'840U});
    };
    DISCOVERY_CHECK(net::ClusterDiscoveryResponder::valid_configuration(
        config("0.0.0.0")));
    DISCOVERY_CHECK(net::ClusterDiscoveryResponder::valid_configuration(
        config("127.0.0.1")));
    DISCOVERY_CHECK(net::ClusterDiscoveryResponder::valid_configuration(
        config("::")));
    DISCOVERY_CHECK(net::ClusterDiscoveryResponder::valid_configuration(
        config("::ffff:192.0.2.4")));
    // An IPv4 answer cannot say where a specific IPv6 listener is.
    DISCOVERY_CHECK(!net::ClusterDiscoveryResponder::valid_configuration(
        config("2001:db8::4")));
    DISCOVERY_CHECK(!net::ClusterDiscoveryResponder::valid_configuration(
        config("::1")));
    auto no_port = config("0.0.0.0");
    no_port.control_endpoint.port(0U);
    DISCOVERY_CHECK(!net::ClusterDiscoveryResponder::valid_configuration(no_port));
}

void a_machine_is_found_on_loopback_and_expires() {
    net::ClusterDiscoveryResponderConfig base{};
    base.node_index = 3U;
    base.password_set = true;
    base.hostname = "TEST-RIG";
    net::ClusterDiscoveryResponder responder{responder_config(
        {asio::ip::address_v4::any(), 47'999U}, base)};
    DISCOVERY_CHECK(!responder.start());
    const auto bound = responder.local_endpoint();
    DISCOVERY_CHECK(bound.has_value());

    net::ClusterDiscoveryBrowser browser{browser_config(
        bound->port(), {asio::ip::address_v4::loopback()})};
    DISCOVERY_CHECK(!browser.start());
    DISCOVERY_CHECK(browser.snapshot().running);
    DISCOVERY_CHECK(eventually([&] { return !browser.snapshot().machines.empty(); }));
    const net::ClusterDiscoverySnapshot found = browser.snapshot();
    DISCOVERY_CHECK(found.machines.size() == 1U);
    const net::ClusterDiscoveredMachine& machine = found.machines.front();
    DISCOVERY_CHECK(machine.hostname == "TEST-RIG");
    DISCOVERY_CHECK(machine.node_index == 3U);
    DISCOVERY_CHECK(machine.control_port == 47'999U);
    DISCOVERY_CHECK(machine.password_set);
    DISCOVERY_CHECK(machine.this_machine);
    DISCOVERY_CHECK(machine.address == asio::ip::address_v4::loopback());
    DISCOVERY_CHECK(responder.answers() >= 1U);

    // Asking again keeps one entry, however many answers arrive.
    browser.query_now();
    std::this_thread::sleep_for(250ms);
    DISCOVERY_CHECK(browser.snapshot().machines.size() == 1U);

    responder.stop();
    DISCOVERY_CHECK(eventually([&] { return browser.snapshot().machines.empty(); }));
    browser.stop();
    DISCOVERY_CHECK(!browser.snapshot().running);
}

void a_lan_address_is_preferred_and_loopback_listeners_name_loopback() {
    const std::vector<net::LocalIpv4Interface> interfaces =
        net::local_ipv4_interfaces();
    if (interfaces.empty()) {
        std::cout << "  (no IPv4 interface up; skipped)\n";
        return;
    }
    const asio::ip::address_v4 lan = interfaces.front().address;

    net::ClusterDiscoveryResponderConfig open_base{};
    open_base.node_index = 1U;
    net::ClusterDiscoveryResponder open{responder_config(
        {asio::ip::address_v4::any(), 47'901U}, open_base)};
    DISCOVERY_CHECK(!open.start());
    net::ClusterDiscoveryBrowser browser{browser_config(
        open.local_endpoint()->port(),
        {asio::ip::address_v4::loopback(), lan})};
    DISCOVERY_CHECK(!browser.start());
    // Heard on loopback and on the LAN address: the LAN address is the one a
    // card should carry, and the machine is still known to be this one.
    DISCOVERY_CHECK(eventually([&] {
        const auto machines = browser.snapshot().machines;
        return machines.size() == 1U && machines.front().addresses.size() == 2U;
    }));
    const auto machine = browser.snapshot().machines.front();
    DISCOVERY_CHECK(machine.address == lan);
    DISCOVERY_CHECK(machine.this_machine);
    browser.stop();
    open.stop();

    // A listener bound to loopback answers this machine's own queries, which
    // arrive from its LAN address when broadcast, and names loopback as the
    // address to use. (Another machine's queries it ignores; that needs a
    // second machine to show.)
    net::ClusterDiscoveryResponderConfig private_base{};
    private_base.node_index = 2U;
    net::ClusterDiscoveryResponder private_listener{responder_config(
        {asio::ip::address_v4::loopback(), 47'902U}, private_base)};
    DISCOVERY_CHECK(!private_listener.start());
    net::ClusterDiscoveryBrowser lan_only{browser_config(
        private_listener.local_endpoint()->port(), {lan})};
    DISCOVERY_CHECK(!lan_only.start());
    DISCOVERY_CHECK(eventually([&] { return !lan_only.snapshot().machines.empty(); }));
    const auto local = lan_only.snapshot().machines.front();
    DISCOVERY_CHECK(local.address == asio::ip::address_v4::loopback());
    DISCOVERY_CHECK(local.this_machine);
    lan_only.stop();
    private_listener.stop();
}

void every_agent_sharing_the_port_on_this_machine_is_found() {
    if (net::local_ipv4_interfaces().empty()) {
        std::cout << "  (no IPv4 interface up; skipped)\n";
        return;
    }
    // A fixed port two agents share, as every agent on a machine shares the
    // real one; a loopback query reaches only one of them, so the default
    // targets' broadcasts must find the other.
    constexpr std::uint16_t shared_port = 47'979U;
    std::vector<std::unique_ptr<net::ClusterDiscoveryResponder>> agents;
    for (mediaserver::core::u32 node = 0U; node < 2U; ++node) {
        net::ClusterDiscoveryResponderConfig config{};
        config.bind_endpoint = {asio::ip::address_v4::any(), shared_port};
        config.control_endpoint = {asio::ip::address_v4::loopback(),
                                   static_cast<std::uint16_t>(47'990U + node)};
        config.node_index = node;
        agents.push_back(
            std::make_unique<net::ClusterDiscoveryResponder>(config));
        DISCOVERY_CHECK(!agents.back()->start());
    }
    net::ClusterDiscoveryBrowserConfig config{};
    config.port = shared_port;
    config.query_interval = 100ms;
    net::ClusterDiscoveryBrowser browser{config};
    DISCOVERY_CHECK(!browser.start());
    DISCOVERY_CHECK(eventually([&] { return browser.snapshot().machines.size() == 2U; }));
    for (const auto& machine : browser.snapshot().machines) {
        DISCOVERY_CHECK(machine.address == asio::ip::address_v4::loopback());
        DISCOVERY_CHECK(machine.control_port == 47'990U + machine.node_index);
    }
    browser.stop();
}

void answers_are_rate_limited() {
    net::ClusterDiscoveryResponderConfig base{};
    base.max_answers_per_second = 2U;
    net::ClusterDiscoveryResponder responder{responder_config(
        {asio::ip::address_v4::any(), 47'903U}, base)};
    DISCOVERY_CHECK(!responder.start());
    asio::io_context context;
    asio::ip::udp::socket socket{context, asio::ip::udp::endpoint{
                                              asio::ip::address_v4::loopback(), 0U}};
    const asio::ip::udp::endpoint target{asio::ip::address_v4::loopback(),
                                         responder.local_endpoint()->port()};
    for (std::uint64_t nonce = 1U; nonce <= 6U; ++nonce) {
        socket.send_to(asio::buffer(net::encode_cluster_discovery_query({nonce})),
                       target);
    }
    DISCOVERY_CHECK(eventually([&] { return responder.answers() >= 2U; }));
    std::this_thread::sleep_for(200ms);
    DISCOVERY_CHECK(responder.answers() == 2U);
    responder.stop();
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests{
        {"queries are padded and strict", &queries_are_padded_and_strict},
        {"answers round trip and fail closed",
         &answers_round_trip_and_fail_closed},
        {"responder configuration describes only reachable listeners",
         &responder_configuration_describes_only_reachable_listeners},
        {"a machine is found on loopback and expires",
         &a_machine_is_found_on_loopback_and_expires},
        {"a LAN address is preferred and loopback listeners name loopback",
         &a_lan_address_is_preferred_and_loopback_listeners_name_loopback},
        {"every agent sharing the port on this machine is found",
         &every_agent_sharing_the_port_on_this_machine_is_found},
        {"answers are rate limited", &answers_are_rate_limited},
    };
    std::size_t failures{};
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[pass] " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << '/' << tests.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
