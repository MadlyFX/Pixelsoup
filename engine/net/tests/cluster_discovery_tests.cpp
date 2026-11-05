#include <mediaserver/net/cluster_discovery.hpp>
#include <mediaserver/persist/settings.hpp>

#include <asio/io_context.hpp>
#include <asio/ip/udp.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <functional>
#include <filesystem>
#include <fstream>
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
    wrong_version[4] = 4U;
    DISCOVERY_CHECK(!net::decode_cluster_discovery_query(wrong_version));
    const auto editor_query = net::encode_cluster_discovery_query(
        {0xFEDCBA9876543210ULL, net::ClusterDiscoveryKind::Director});
    DISCOVERY_CHECK(editor_query[5] == 1U);
    DISCOVERY_CHECK(editor_query[8] == 0xFEU && editor_query[15] == 0x10U);
    const auto editor_decoded = net::decode_cluster_discovery_query(editor_query);
    DISCOVERY_CHECK(editor_decoded->kind == net::ClusterDiscoveryKind::Director);
    DISCOVERY_CHECK(editor_decoded->nonce == 0xFEDCBA9876543210ULL);
    auto unknown_kind = bytes;
    unknown_kind[5] = 2U;
    DISCOVERY_CHECK(!net::decode_cluster_discovery_query(unknown_kind));
    const net::ClusterDiscoveryQuery enrollment{0x1020304050607080ULL,
        net::ClusterDiscoveryKind::Director, {0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL},
        0xFEDCBA9876543210ULL, 37U};
    const auto identity_query = net::encode_cluster_discovery_query(enrollment);
    DISCOVERY_CHECK(net::decode_cluster_discovery_query(identity_query) == enrollment);
    DISCOVERY_CHECK(identity_query[16] == 0x11U && identity_query[24] == 0x99U);
    DISCOVERY_CHECK(identity_query[32] == 0xFEU && identity_query[43] == 37U);
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
    answer.kind = net::ClusterDiscoveryKind::Director;
    answer.show_id = {0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};
    answer.enrolled_node_id = {0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL};
    answer.assignment = net::NodeAssignmentStatus::Assigned;
    const auto bytes = net::encode_cluster_discovery_answer(answer);
    DISCOVERY_CHECK(bytes.size() <= net::cluster_discovery_datagram_bytes);
    const auto decoded = net::decode_cluster_discovery_answer(bytes);
    DISCOVERY_CHECK(bytes[7] == 1U);
    DISCOVERY_CHECK(bytes[36] == 0x01U && bytes[43] == 0xEFU);
    DISCOVERY_CHECK(bytes[44] == 0xFEU && bytes[51] == 0x10U);
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
    auto unknown_kind = bytes;
    unknown_kind[7] = 2U;
    DISCOVERY_CHECK(!net::decode_cluster_discovery_answer(unknown_kind));
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
    auto editor_config = responder_config(
        {asio::ip::address_v4::loopback(), 45'321U});
    editor_config.bind_endpoint.port(shared_port);
    editor_config.kind = net::ClusterDiscoveryKind::Director;
    editor_config.show_id = {17U, 23U};
    net::ClusterDiscoveryResponder editor{editor_config};
    DISCOVERY_CHECK(!editor.start());
    net::ClusterDiscoveryBrowser browser{config};
    DISCOVERY_CHECK(!browser.start());
    DISCOVERY_CHECK(eventually([&] { return browser.snapshot().machines.size() == 2U; }));
    for (const auto& machine : browser.snapshot().machines) {
        DISCOVERY_CHECK(machine.address == asio::ip::address_v4::loopback());
        DISCOVERY_CHECK(machine.control_port == 47'990U + machine.node_index);
    }
    config.kind = net::ClusterDiscoveryKind::Director;
    net::ClusterDiscoveryBrowser editors{config};
    DISCOVERY_CHECK(!editors.start());
    DISCOVERY_CHECK(eventually([&] {
        return net::discovered_director(editors.snapshot(), {17U, 23U}) ==
            std::optional<asio::ip::tcp::endpoint>{{asio::ip::address_v4::loopback(), 45'321U}};
    }));
    DISCOVERY_CHECK(editors.snapshot().machines.size() == 1U);
    browser.stop();
}

void directors_are_discovered_separately_and_selected_by_show() {
    const net::Id128 show{0x1122334455667788ULL, 0x99AABBCCDDEEFF01ULL};
    auto config = responder_config({asio::ip::address_v4::loopback(), 45'123U});
    config.kind = net::ClusterDiscoveryKind::Director;
    config.show_id = show;
    net::ClusterDiscoveryResponder responder{config};
    DISCOVERY_CHECK(!responder.start());
    auto search = browser_config(responder.local_endpoint()->port(),
                                  {asio::ip::address_v4::loopback()});
    net::ClusterDiscoveryBrowser nodes{search};
    DISCOVERY_CHECK(!nodes.start());
    // Queries for render machines must never get an editor answer.
    std::this_thread::sleep_for(200ms);
    DISCOVERY_CHECK(nodes.snapshot().machines.empty());
    DISCOVERY_CHECK(responder.answers() == 0U);
    search.kind = net::ClusterDiscoveryKind::Director;
    net::ClusterDiscoveryBrowser editors{search};
    DISCOVERY_CHECK(!editors.start());
    DISCOVERY_CHECK(eventually([&] {
        return net::discovered_director(editors.snapshot(), show).has_value();
    }));
    auto found = editors.snapshot();
    const auto endpoint = net::discovered_director(found, show);
    DISCOVERY_CHECK(endpoint->address() == asio::ip::address_v4::loopback());
    DISCOVERY_CHECK(endpoint->port() == 45'123U);
    DISCOVERY_CHECK(!net::discovered_director(found, {7U, 9U}));
    // Different instances with the same show are ambiguous, even at one IP.
    found.machines.push_back(found.machines.front());
    found.machines.back().instance += 1U;
    found.machines.back().control_port = 45'124U;
    DISCOVERY_CHECK(!net::discovered_director(found, show));
    found.machines.back().show_id = {11U, 13U};
    DISCOVERY_CHECK(net::discovered_director(found, show) == endpoint);
    found.running = false;
    DISCOVERY_CHECK(!net::discovered_director(found, show));
    responder.stop();
    DISCOVERY_CHECK(eventually([&] {
        return !net::discovered_director(editors.snapshot(), show);
    }));
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

struct IdentityDirectory {
    std::filesystem::path root = std::filesystem::current_path() / "out" /
        ("node-identity-" + std::to_string(net::new_render_machine_id().low));
    IdentityDirectory() { std::filesystem::create_directories(root); }
    ~IdentityDirectory() { std::error_code ignored; std::filesystem::remove_all(root, ignored); }
};

void node_indices_are_unique_durable_and_fail_closed() {
    const IdentityDirectory temporary;
    const auto file = temporary.root / "assignments.bin";
    const net::Id128 first{0x1234U, 0x5678U}, second{0xABCDU, 0xEF12U};
    mediaserver::core::u32 first_index{}, second_index{};
    {
        net::NodeIndexAssignments registry{file};
        DISCOVERY_CHECK(registry.ready());
        net::NodeIndexAssignments other_editor{file};
        DISCOVERY_CHECK(!other_editor.ready());
        DISCOVERY_CHECK(registry.assign({99U, 101U}, 0U).status == net::NodeAssignmentStatus::Assigned);
        net::NodeIndexAssignment a, b;
        std::thread one{[&] { a = registry.assign(first); }};
        std::thread two{[&] { b = registry.assign(second); }};
        one.join(); two.join();
        DISCOVERY_CHECK(a.status == net::NodeAssignmentStatus::Assigned);
        DISCOVERY_CHECK(b.status == net::NodeAssignmentStatus::Assigned);
        DISCOVERY_CHECK(a.index != b.index);
        DISCOVERY_CHECK(a.index + b.index == 3U);
        first_index = a.index; second_index = b.index;
        DISCOVERY_CHECK(registry.assign({13U, 17U}, first_index).status == net::NodeAssignmentStatus::Conflict);
        DISCOVERY_CHECK(registry.assign(first).index == first_index);
    }
    {
        net::NodeIndexAssignments restarted{file};
        DISCOVERY_CHECK(restarted.ready());
        DISCOVERY_CHECK(restarted.assign(second).index == second_index);
        DISCOVERY_CHECK(restarted.assign(first).index == first_index);
        DISCOVERY_CHECK(restarted.assign({19U, 23U}).index == 3U);
    }
    // A damaged saved table must never become an empty table and reassign outputs.
    {
        std::fstream bytes{file, std::ios::binary | std::ios::in | std::ios::out};
        bytes.seekp(-1, std::ios::end);
        bytes.put('\x7f');
    }
    net::NodeIndexAssignments damaged{file};
    DISCOVERY_CHECK(!damaged.ready());
    DISCOVERY_CHECK(damaged.assign({29U, 31U}).status == net::NodeAssignmentStatus::Unavailable);
}

void discovery_adopts_show_and_assigns_stable_node_indices() {
    const IdentityDirectory temporary;
    auto registry = std::make_shared<net::NodeIndexAssignments>(temporary.root / "enrollment.bin");
    auto config = responder_config({asio::ip::address_v4::loopback(), 45'789U});
    config.kind = net::ClusterDiscoveryKind::Director;
    config.show_id = {0xAB123U, 0xCD456U};
    config.node_assignments = registry;
    net::ClusterDiscoveryResponder responder{config};
    DISCOVERY_CHECK(!responder.start());
    auto search = browser_config(responder.local_endpoint()->port(), {asio::ip::address_v4::loopback()});
    search.kind = net::ClusterDiscoveryKind::Director;
    net::ClusterDiscoveryBrowser browse{search};
    DISCOVERY_CHECK(!browse.start());
    DISCOVERY_CHECK(eventually([&] { return net::discovered_director_identity(browse.snapshot()).has_value(); }));
    const auto editor = *net::discovered_director_identity(browse.snapshot());
    DISCOVERY_CHECK(editor.show_id == config.show_id);
    search.director_instance = editor.instance;
    search.node_id = {0x1111U, 0x2222U};
    net::ClusterDiscoveryBrowser first{search};
    DISCOVERY_CHECK(!first.start());
    search.node_id = {0x3333U, 0x4444U};
    net::ClusterDiscoveryBrowser second{search};
    DISCOVERY_CHECK(!second.start());
    DISCOVERY_CHECK(eventually([&] {
        return !first.snapshot().machines.empty() && !second.snapshot().machines.empty();
    }));
    const auto a = *net::discovered_director_identity(first.snapshot());
    const auto b = *net::discovered_director_identity(second.snapshot());
    DISCOVERY_CHECK(a.assignment == net::NodeAssignmentStatus::Assigned);
    DISCOVERY_CHECK(b.assignment == net::NodeAssignmentStatus::Assigned);
    DISCOVERY_CHECK(a.enrolled_node_id == (net::Id128{0x1111U, 0x2222U}));
    DISCOVERY_CHECK(b.enrolled_node_id == search.node_id);
    DISCOVERY_CHECK(a.node_index != b.node_index);
    DISCOVERY_CHECK(a.node_index + b.node_index == 1U);
    // A new browser/process for the same saved ID receives the original index.
    net::ClusterDiscoveryBrowser again{search};
    DISCOVERY_CHECK(!again.start());
    DISCOVERY_CHECK(eventually([&] { return !again.snapshot().machines.empty(); }));
    DISCOVERY_CHECK(again.snapshot().machines.front().node_index == b.node_index);
}

void assignment_capacity_and_failed_saves_are_explicit() {
    const IdentityDirectory temporary;
    const auto full_file = temporary.root / "full.bin";
    std::vector<std::byte> payload;
    // A pre-existing table fills all 4096 valid indices. IDs are distinct and
    // deliberately unrelated to their index-derived legacy identities.
    for (std::uint64_t index = 0; index < 4096U; ++index) {
        for (const auto value : {std::uint64_t{0xDEADBEEF}, index + 101U}) {
            for (unsigned byte = 0; byte < 8U; ++byte)
                payload.push_back(static_cast<std::byte>(value >> (8U * byte)));
        }
        for (unsigned byte = 0; byte < 4U; ++byte)
            payload.push_back(static_cast<std::byte>(index >> (8U * byte)));
    }
    DISCOVERY_CHECK(mediaserver::persist::save_node_assignments(full_file, payload));
    DISCOVERY_CHECK(!mediaserver::persist::load_workspace_layout(full_file));
    net::NodeIndexAssignments full{full_file};
    DISCOVERY_CHECK(full.ready());
    DISCOVERY_CHECK(full.assign({0xDEADBEEFU, 4196U}).index == 4095U);
    DISCOVERY_CHECK(full.assign({55U, 77U}).status == net::NodeAssignmentStatus::Full);
    const auto blocked_file = temporary.root / "blocked.bin";
    net::NodeIndexAssignments blocked{blocked_file};
    DISCOVERY_CHECK(blocked.ready());
    std::filesystem::create_directory(blocked_file);
    DISCOVERY_CHECK(blocked.assign({11U, 33U}).status == net::NodeAssignmentStatus::Unavailable);
    std::filesystem::remove(blocked_file);
    DISCOVERY_CHECK(blocked.assign({55U, 77U}).index == 0U);
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
        {"node indices are unique, durable and fail closed", &node_indices_are_unique_durable_and_fail_closed},
        {"discovery adopts show and assigns stable indices", &discovery_adopts_show_and_assigns_stable_node_indices},
        {"assignment capacity and failed saves are explicit", &assignment_capacity_and_failed_saves_are_explicit},
        {"directors are discovered separately and selected by show",
         &directors_are_discovered_separately_and_selected_by_show},
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
