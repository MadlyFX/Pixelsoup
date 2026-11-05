#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/net/session_protocol.hpp>
#include <mediaserver/net/node_index_assignments.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <asio/ip/address_v4.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/ip/udp.hpp>

// Finding render machines and Directors on the local IPv4 network.
//
// A render machine's lifecycle agent answers a small UDP query on a
// well-known port with where its control listener is, which node it
// supervises, and whether it was given a password. The editor asks every
// couple of seconds - by broadcast on each IPv4 interface, and on loopback -
// and offers the machines that answered in the Hardware Map.
// Directors answer queries of their own kind with the show listener and show
// identity. Render machines use those answers to resolve an automatic address
// on process startup. Discovery carries public endpoint information only;
// normal show-session identity and readiness checks still apply.
//
// The exchange is information only. It carries no password or key and
// authorizes nothing: a machine found this way is still controlled over the
// authenticated lifecycle channel, and nodes validate the show session before
// rendering. A query is padded to the size of the largest answer, so an agent
// cannot be used to amplify traffic toward a third party.
namespace mediaserver::net {

// Just below the default lifecycle control range (47840 + node index).
inline constexpr std::uint16_t cluster_discovery_port = 47'839U;
inline constexpr std::size_t cluster_discovery_datagram_bytes = 160U;
inline constexpr std::size_t cluster_discovery_max_hostname_bytes = 63U;

enum class ClusterDiscoveryKind : std::uint8_t { RenderMachine, Director };

struct ClusterDiscoveryQuery final {
    std::uint64_t nonce{};
    ClusterDiscoveryKind kind{ClusterDiscoveryKind::RenderMachine};
    // Zero browses only. Enrollment targets a previously selected Director
    // instance, so browsing several editors never consumes node slots.
    Id128 node_id{};
    core::u64 director_instance{};
    core::u32 requested_node_index{automatic_node_index};

    bool operator==(const ClusterDiscoveryQuery&) const = default;
};

struct ClusterDiscoveryAnswer final {
    // Echoed from the query it answers.
    std::uint64_t nonce{};
    // Random for each agent run, so one machine answering on several
    // interfaces is recognised as one machine.
    std::uint64_t instance{};
    core::u32 node_index{};
    std::uint16_t control_port{};
    // The address the control listener is bound to. Unspecified when it
    // listens on every interface; the address the answer came from is then
    // the one to use.
    asio::ip::address_v4 control_address{};
    bool password_set{};
    // Printable ASCII, at most cluster_discovery_max_hostname_bytes.
    std::string hostname;
    ClusterDiscoveryKind kind{ClusterDiscoveryKind::RenderMachine};
    Id128 show_id{};
    Id128 enrolled_node_id{};
    NodeAssignmentStatus assignment{NodeAssignmentStatus::None};

    bool operator==(const ClusterDiscoveryAnswer&) const = default;
};

[[nodiscard]] std::array<std::uint8_t, cluster_discovery_datagram_bytes>
encode_cluster_discovery_query(const ClusterDiscoveryQuery& query) noexcept;
[[nodiscard]] std::optional<ClusterDiscoveryQuery>
decode_cluster_discovery_query(std::span<const std::uint8_t> bytes) noexcept;
// At most cluster_discovery_datagram_bytes. A hostname that is too long or
// not printable ASCII is truncated and its other bytes replaced.
[[nodiscard]] std::vector<std::uint8_t>
encode_cluster_discovery_answer(const ClusterDiscoveryAnswer& answer);
[[nodiscard]] std::optional<ClusterDiscoveryAnswer>
decode_cluster_discovery_answer(std::span<const std::uint8_t> bytes);

// This machine's name as discovery reports it: printable ASCII, bounded, and
// never empty.
[[nodiscard]] std::string cluster_discovery_hostname();

struct LocalIpv4Interface final {
    asio::ip::address_v4 address;
    // The interface's directed broadcast address.
    asio::ip::address_v4 broadcast;
    // Its subnet mask, so a search can walk the subnet's hosts.
    asio::ip::address_v4 netmask{asio::ip::address_v4::broadcast()};

    bool operator==(const LocalIpv4Interface&) const = default;
};

// Every IPv4 interface that is up, loopback excluded. Empty when the system
// cannot be asked.
[[nodiscard]] std::vector<LocalIpv4Interface> local_ipv4_interfaces();

// Agent side: answers queries for one lifecycle listener.
struct ClusterDiscoveryResponderConfig final {
    asio::ip::udp::endpoint bind_endpoint{asio::ip::address_v4::any(),
                                          cluster_discovery_port};
    core::u32 node_index{};
    // The lifecycle listener as bound. One on IPv4 loopback answers only
    // queries from this machine, naming 127.0.0.1; one on a specific IPv6
    // address (::1 included) cannot be described and makes the configuration
    // invalid.
    asio::ip::tcp::endpoint control_endpoint;
    bool password_set{};
    // Empty means cluster_discovery_hostname().
    std::string hostname;
    // Queries beyond this are ignored, so a flood costs this machine little.
    core::u32 max_answers_per_second{64U};
    ClusterDiscoveryKind kind{ClusterDiscoveryKind::RenderMachine};
    Id128 show_id{};
    std::shared_ptr<NodeIndexAssignments> node_assignments;
};

namespace detail {
class ClusterDiscoveryResponderState;
class ClusterDiscoveryBrowserState;
} // namespace detail

class ClusterDiscoveryResponder final {
public:
    explicit ClusterDiscoveryResponder(ClusterDiscoveryResponderConfig config);
    ClusterDiscoveryResponder(const ClusterDiscoveryResponder&) = delete;
    ClusterDiscoveryResponder& operator=(const ClusterDiscoveryResponder&) =
        delete;
    ~ClusterDiscoveryResponder();

    [[nodiscard]] static bool valid_configuration(
        const ClusterDiscoveryResponderConfig& config) noexcept;
    // Opens the port shared with any other agent on this machine. Returns
    // why it could not.
    [[nodiscard]] std::error_code start();
    void stop() noexcept;
    [[nodiscard]] std::optional<asio::ip::udp::endpoint> local_endpoint()
        const noexcept;
    [[nodiscard]] core::u64 answers() const noexcept;

private:
    std::unique_ptr<detail::ClusterDiscoveryResponderState> state_;
};

// Editor side: asks periodically and keeps the machines that answered.
struct ClusterDiscoveryBrowserConfig final {
    std::uint16_t port{cluster_discovery_port};
    std::chrono::milliseconds query_interval{2'000};
    // A machine that stops answering is dropped after this long.
    std::chrono::milliseconds expiry{7'000};
    // Where queries go. Empty means loopback, the limited broadcast address,
    // and each interface's directed broadcast.
    std::vector<asio::ip::address_v4> targets;
    ClusterDiscoveryKind kind{ClusterDiscoveryKind::RenderMachine};
    Id128 node_id{};
    core::u64 director_instance{};
    core::u32 requested_node_index{automatic_node_index};
};

struct ClusterDiscoveredMachine final {
    std::uint64_t instance{};
    std::string hostname;
    // Where to reach the control listener: its bound address when it has
    // one, otherwise the first non-loopback address it answered from, and
    // loopback only for a machine heard on nothing else.
    asio::ip::address_v4 address;
    // Every address it has recently answered from, in first-heard order.
    std::vector<asio::ip::address_v4> addresses;
    core::u32 node_index{};
    std::uint16_t control_port{};
    bool password_set{};
    // It answered on loopback or from one of this machine's own addresses.
    bool this_machine{};
    Id128 show_id{};
    ClusterDiscoveryKind kind{ClusterDiscoveryKind::RenderMachine};
    Id128 enrolled_node_id{};
    NodeAssignmentStatus assignment{NodeAssignmentStatus::None};

    bool operator==(const ClusterDiscoveredMachine&) const = default;
};

struct ClusterDiscoverySnapshot final {
    bool running{};
    // Why the browser is not running, once it has failed to start.
    std::error_code error;
    // Sorted by hostname, node index, then address.
    std::vector<ClusterDiscoveredMachine> machines;

    bool operator==(const ClusterDiscoverySnapshot&) const = default;
};

// Automatic connection requires exactly one live Director for this show.
// Several interfaces of that Director are already deduplicated by the browser.
[[nodiscard]] std::optional<asio::ip::tcp::endpoint>
discovered_director(const ClusterDiscoverySnapshot& snapshot, Id128 show_id);

// An empty show selects a single editor and adopts its advertised show ID.
// An optional endpoint restricts discovery for a manual address override.
[[nodiscard]] std::optional<ClusterDiscoveredMachine>
discovered_director_identity(const ClusterDiscoverySnapshot& snapshot,
                             Id128 show_id = {},
                             std::optional<asio::ip::tcp::endpoint> endpoint = {});

class ClusterDiscoveryBrowser final {
public:
    explicit ClusterDiscoveryBrowser(ClusterDiscoveryBrowserConfig config = {});
    ClusterDiscoveryBrowser(const ClusterDiscoveryBrowser&) = delete;
    ClusterDiscoveryBrowser& operator=(const ClusterDiscoveryBrowser&) = delete;
    ~ClusterDiscoveryBrowser();

    [[nodiscard]] std::error_code start();
    void stop() noexcept;
    [[nodiscard]] ClusterDiscoverySnapshot snapshot() const;
    // Asks again now rather than at the next interval.
    void query_now() noexcept;

private:
    std::unique_ptr<detail::ClusterDiscoveryBrowserState> state_;
};

} // namespace mediaserver::net
