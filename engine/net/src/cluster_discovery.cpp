#include <mediaserver/net/cluster_discovery.hpp>

#include "socket_readiness.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <mutex>
#include <new>
#include <string_view>
#include <thread>
#include <utility>

#include <asio/io_context.hpp>
#include <asio/ip/host_name.hpp>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <mstcpip.h>
#include <bcrypt.h>
#elif defined(__linux__)
#include <cerrno>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/random.h>
#else
#include <cstdlib>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

namespace mediaserver::net {
namespace {

constexpr std::array<std::uint8_t, 4U> query_magic{'M', 'S', 'D', 'Q'};
constexpr std::array<std::uint8_t, 4U> answer_magic{'M', 'S', 'D', 'A'};
constexpr std::uint8_t protocol_version = 1U;
constexpr std::size_t query_header_bytes = 16U;
constexpr std::size_t answer_header_bytes = 36U;
constexpr std::uint8_t password_flag = 0x01U;
// Bounds how long stop() waits for a loop to notice.
constexpr auto wait_slice = std::chrono::milliseconds{50};
// Answers to a query this many intervals old are still accepted, so a slow
// machine is not dropped for answering late.
constexpr std::size_t remembered_nonces = 4U;

static_assert(answer_header_bytes + cluster_discovery_max_hostname_bytes <=
              cluster_discovery_datagram_bytes);

void put_u16(std::uint8_t* out, std::uint16_t value) noexcept {
    out[0] = static_cast<std::uint8_t>(value >> 8U);
    out[1] = static_cast<std::uint8_t>(value);
}

void put_u32(std::uint8_t* out, std::uint32_t value) noexcept {
    for (std::size_t index = 0U; index < 4U; ++index) {
        out[index] = static_cast<std::uint8_t>(value >> (24U - index * 8U));
    }
}

void put_u64(std::uint8_t* out, std::uint64_t value) noexcept {
    for (std::size_t index = 0U; index < 8U; ++index) {
        out[index] = static_cast<std::uint8_t>(value >> (56U - index * 8U));
    }
}

[[nodiscard]] std::uint16_t get_u16(const std::uint8_t* in) noexcept {
    return static_cast<std::uint16_t>((in[0] << 8U) | in[1]);
}

[[nodiscard]] std::uint32_t get_u32(const std::uint8_t* in) noexcept {
    std::uint32_t value{};
    for (std::size_t index = 0U; index < 4U; ++index) {
        value = (value << 8U) | in[index];
    }
    return value;
}

[[nodiscard]] std::uint64_t get_u64(const std::uint8_t* in) noexcept {
    std::uint64_t value{};
    for (std::size_t index = 0U; index < 8U; ++index) {
        value = (value << 8U) | in[index];
    }
    return value;
}

[[nodiscard]] bool printable(char character) noexcept {
    return character >= 0x20 && character <= 0x7E;
}

[[nodiscard]] std::string clean_hostname(std::string_view text) {
    std::string cleaned;
    cleaned.reserve(std::min(text.size(), cluster_discovery_max_hostname_bytes));
    for (const char character : text) {
        if (cleaned.size() == cluster_discovery_max_hostname_bytes) {
            break;
        }
        cleaned.push_back(printable(character) ? character : '?');
    }
    return cleaned;
}

[[nodiscard]] std::uint64_t random_u64() noexcept {
    std::array<std::uint8_t, 8U> bytes{};
    bool filled = false;
#if defined(_WIN32)
    filled = BCryptGenRandom(nullptr, bytes.data(),
                             static_cast<ULONG>(bytes.size()),
                             BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(__linux__)
    filled = getrandom(bytes.data(), bytes.size(), 0U) ==
             static_cast<ssize_t>(bytes.size());
#else
    arc4random_buf(bytes.data(), bytes.size());
    filled = true;
#endif
    std::uint64_t value = get_u64(bytes.data());
    if (!filled) {
        // Discovery nonces only pair answers with queries; they guard
        // nothing, so a clock-derived value is an acceptable fallback.
        value = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
    }
    return value == 0U ? 1U : value;
}

[[nodiscard]] bool would_block(const std::error_code& error) noexcept {
    return error == asio::error::would_block ||
           error == asio::error::try_again;
}

// Without this a Windows UDP socket reports the ICMP "port unreachable" that
// a query to a machine with no agent provokes as an error on a later receive.
void ignore_unreachable_reports(asio::ip::udp::socket& socket) noexcept {
#if defined(_WIN32)
    BOOL report = FALSE;
    DWORD returned{};
    static_cast<void>(WSAIoctl(socket.native_handle(), SIO_UDP_CONNRESET,
                               &report, sizeof(report), nullptr, 0U, &returned,
                               nullptr, nullptr));
#else
    static_cast<void>(socket);
#endif
}

// The readiness wait is written for TCP handles; on Windows Asio wraps each
// protocol's SOCKET in its own handle type, so unwrap and rewrap it.
[[nodiscard]] detail::WaitOutcome
wait_readable(asio::ip::udp::socket& socket,
              std::chrono::milliseconds timeout) noexcept {
    const asio::detail::socket_type handle = socket.native_handle();
    return detail::wait_for_socket(
        asio::ip::tcp::socket::native_handle_type(handle),
        detail::WaitDirection::Read, timeout);
}

} // namespace

std::array<std::uint8_t, cluster_discovery_datagram_bytes>
encode_cluster_discovery_query(const ClusterDiscoveryQuery& query) noexcept {
    std::array<std::uint8_t, cluster_discovery_datagram_bytes> bytes{};
    std::copy(query_magic.begin(), query_magic.end(), bytes.begin());
    bytes[4] = protocol_version;
    put_u64(bytes.data() + 8U, query.nonce);
    return bytes;
}

std::optional<ClusterDiscoveryQuery>
decode_cluster_discovery_query(std::span<const std::uint8_t> bytes) noexcept {
    // Exactly the padded size: a short query would let an agent answer with
    // more than it was sent.
    if (bytes.size() != cluster_discovery_datagram_bytes ||
        !std::equal(query_magic.begin(), query_magic.end(), bytes.begin()) ||
        bytes[4] != protocol_version) {
        return std::nullopt;
    }
    static_assert(query_header_bytes <= cluster_discovery_datagram_bytes);
    return ClusterDiscoveryQuery{get_u64(bytes.data() + 8U)};
}

std::vector<std::uint8_t>
encode_cluster_discovery_answer(const ClusterDiscoveryAnswer& answer) {
    const std::string hostname = clean_hostname(answer.hostname);
    std::vector<std::uint8_t> bytes(answer_header_bytes + hostname.size(), 0U);
    std::copy(answer_magic.begin(), answer_magic.end(), bytes.begin());
    bytes[4] = protocol_version;
    bytes[5] = answer.password_set ? password_flag : std::uint8_t{0U};
    bytes[6] = static_cast<std::uint8_t>(hostname.size());
    put_u64(bytes.data() + 8U, answer.nonce);
    put_u64(bytes.data() + 16U, answer.instance);
    put_u32(bytes.data() + 24U, answer.node_index);
    put_u16(bytes.data() + 28U, answer.control_port);
    const auto address = answer.control_address.to_bytes();
    std::copy(address.begin(), address.end(), bytes.begin() + 32);
    std::memcpy(bytes.data() + answer_header_bytes, hostname.data(),
                hostname.size());
    return bytes;
}

std::optional<ClusterDiscoveryAnswer>
decode_cluster_discovery_answer(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < answer_header_bytes ||
        bytes.size() > cluster_discovery_datagram_bytes ||
        !std::equal(answer_magic.begin(), answer_magic.end(), bytes.begin()) ||
        bytes[4] != protocol_version) {
        return std::nullopt;
    }
    const std::size_t hostname_bytes = bytes[6];
    if (hostname_bytes > cluster_discovery_max_hostname_bytes ||
        bytes.size() != answer_header_bytes + hostname_bytes) {
        return std::nullopt;
    }
    ClusterDiscoveryAnswer answer{};
    answer.password_set = (bytes[5] & password_flag) != 0U;
    answer.nonce = get_u64(bytes.data() + 8U);
    answer.instance = get_u64(bytes.data() + 16U);
    answer.node_index = get_u32(bytes.data() + 24U);
    answer.control_port = get_u16(bytes.data() + 28U);
    asio::ip::address_v4::bytes_type address{};
    std::copy_n(bytes.begin() + 32, address.size(), address.begin());
    answer.control_address = asio::ip::address_v4{address};
    if (answer.control_port == 0U) {
        return std::nullopt;
    }
    answer.hostname.assign(
        reinterpret_cast<const char*>(bytes.data() + answer_header_bytes),
        hostname_bytes);
    if (!std::all_of(answer.hostname.begin(), answer.hostname.end(),
                     printable)) {
        return std::nullopt;
    }
    return answer;
}

std::string cluster_discovery_hostname() {
    std::error_code error;
    std::string name = asio::ip::host_name(error);
    if (error) {
        name.clear();
    }
    name = clean_hostname(name);
    return name.empty() ? std::string{"render machine"} : name;
}

std::vector<LocalIpv4Interface> local_ipv4_interfaces() {
    std::vector<LocalIpv4Interface> interfaces;
    const auto add = [&](std::uint32_t address, std::uint32_t mask) {
        const asio::ip::address_v4 host{address};
        if (host.is_loopback() || host.is_unspecified()) {
            return;
        }
        const LocalIpv4Interface entry{
            host, asio::ip::address_v4{address | ~mask},
            asio::ip::address_v4{mask}};
        if (std::find(interfaces.begin(), interfaces.end(), entry) ==
            interfaces.end()) {
            interfaces.push_back(entry);
        }
    };
#if defined(_WIN32)
    ULONG size = 16U * 1024U;
    std::vector<std::byte> buffer;
    ULONG status = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && status == ERROR_BUFFER_OVERFLOW;
         ++attempt) {
        buffer.resize(size);
        status = GetAdaptersAddresses(
            AF_INET,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                GAA_FLAG_SKIP_DNS_SERVER,
            nullptr,
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (status != NO_ERROR) {
        return interfaces;
    }
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
         adapter != nullptr; adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp ||
            adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr;
             unicast = unicast->Next) {
            const sockaddr* const socket_address =
                unicast->Address.lpSockaddr;
            if (socket_address == nullptr ||
                socket_address->sa_family != AF_INET ||
                unicast->OnLinkPrefixLength > 32U) {
                continue;
            }
            const auto* const inet =
                reinterpret_cast<const sockaddr_in*>(socket_address);
            const std::uint32_t address = ntohl(inet->sin_addr.s_addr);
            const std::uint32_t mask =
                unicast->OnLinkPrefixLength == 0U
                    ? 0U
                    : ~std::uint32_t{0U}
                          << (32U - unicast->OnLinkPrefixLength);
            add(address, mask);
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) {
        return interfaces;
    }
    for (const ifaddrs* entry = list; entry != nullptr;
         entry = entry->ifa_next) {
        if (entry->ifa_addr == nullptr || entry->ifa_netmask == nullptr ||
            entry->ifa_addr->sa_family != AF_INET ||
            (entry->ifa_flags & IFF_UP) == 0U ||
            (entry->ifa_flags & IFF_LOOPBACK) != 0U) {
            continue;
        }
        const auto* const address =
            reinterpret_cast<const sockaddr_in*>(entry->ifa_addr);
        const auto* const mask =
            reinterpret_cast<const sockaddr_in*>(entry->ifa_netmask);
        add(ntohl(address->sin_addr.s_addr), ntohl(mask->sin_addr.s_addr));
    }
    freeifaddrs(list);
#endif
    return interfaces;
}

namespace detail {

class ClusterDiscoveryResponderState final {
public:
    explicit ClusterDiscoveryResponderState(
        ClusterDiscoveryResponderConfig value)
        : config(std::move(value)), socket(context) {}

    void loop() noexcept {
        std::array<std::uint8_t, cluster_discovery_datagram_bytes + 1U>
            buffer{};
        auto window = std::chrono::steady_clock::now();
        core::u32 answered_in_window{};
        while (!stop_requested.load(std::memory_order_acquire)) {
            if (wait_readable(socket, wait_slice) ==
                detail::WaitOutcome::Unavailable) {
                std::this_thread::sleep_for(wait_slice);
                continue;
            }
            for (;;) {
                asio::ip::udp::endpoint sender;
                std::error_code error;
                const std::size_t received = socket.receive_from(
                    asio::buffer(buffer), sender, 0, error);
                if (error) {
                    // Would-block ends the batch; anything else is a
                    // transient report about an earlier datagram.
                    break;
                }
                const auto query = decode_cluster_discovery_query(
                    std::span<const std::uint8_t>{buffer.data(), received});
                if (!query || !sender.address().is_v4()) {
                    continue;
                }
                if (loopback_only &&
                    !from_this_machine(sender.address().to_v4())) {
                    continue;
                }
                const auto now = std::chrono::steady_clock::now();
                if (now - window >= std::chrono::seconds{1}) {
                    window = now;
                    answered_in_window = 0U;
                }
                if (answered_in_window >= config.max_answers_per_second) {
                    continue;
                }
                ++answered_in_window;
                ClusterDiscoveryAnswer reply = answer;
                reply.nonce = query->nonce;
                try {
                    const std::vector<std::uint8_t> bytes =
                        encode_cluster_discovery_answer(reply);
                    std::error_code send_error;
                    static_cast<void>(socket.send_to(asio::buffer(bytes),
                                                     sender, 0, send_error));
                    if (!send_error) {
                        answer_count.fetch_add(1U, std::memory_order_relaxed);
                    }
                } catch (const std::bad_alloc&) {
                }
            }
        }
    }

    // A listener on loopback still answers this machine's own broadcast
    // queries, which arrive from its interface addresses: several local
    // agents share the port, and a unicast loopback query reaches only one.
    [[nodiscard]] bool from_this_machine(const asio::ip::address_v4& sender) {
        if (sender.is_loopback()) {
            return true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now - local_checked > std::chrono::seconds{5} || local.empty()) {
            try {
                local = local_ipv4_interfaces();
            } catch (const std::bad_alloc&) {
                local.clear();
            }
            local_checked = now;
        }
        return std::any_of(local.begin(), local.end(),
                           [&](const LocalIpv4Interface& own) {
                               return own.address == sender;
                           });
    }

    ClusterDiscoveryResponderConfig config;
    ClusterDiscoveryAnswer answer;
    bool loopback_only{};
    std::vector<LocalIpv4Interface> local;
    std::chrono::steady_clock::time_point local_checked{};
    asio::io_context context;
    asio::ip::udp::socket socket;
    std::thread thread;
    std::atomic_bool stop_requested{};
    std::atomic_bool running{};
    std::atomic<core::u64> answer_count{};
    mutable std::mutex endpoint_mutex;
    std::optional<asio::ip::udp::endpoint> endpoint;
};

} // namespace detail

namespace {

// The control listener as discovery describes it: an IPv4 address (any for
// every interface, loopback for this machine only) and whether only this
// machine can reach it.
struct ControlAddress final {
    asio::ip::address_v4 address;
    bool loopback_only{};
};

[[nodiscard]] std::optional<ControlAddress>
describe_control(const asio::ip::tcp::endpoint& endpoint) noexcept {
    const asio::ip::address address = endpoint.address();
    if (address.is_v4()) {
        const asio::ip::address_v4 v4 = address.to_v4();
        return ControlAddress{v4, v4.is_loopback()};
    }
    const asio::ip::address_v6 v6 = address.to_v6();
    if (v6.is_unspecified()) {
        return ControlAddress{};
    }
    if (v6.is_v4_mapped()) {
        const asio::ip::address_v4 v4 =
            asio::ip::make_address_v4(asio::ip::v4_mapped, v6);
        return ControlAddress{v4, v4.is_loopback()};
    }
    // Including ::1, which an IPv4 answer cannot name either.
    return std::nullopt;
}

} // namespace

ClusterDiscoveryResponder::ClusterDiscoveryResponder(
    ClusterDiscoveryResponderConfig config)
    : state_(std::make_unique<detail::ClusterDiscoveryResponderState>(
          std::move(config))) {}

ClusterDiscoveryResponder::~ClusterDiscoveryResponder() { stop(); }

bool ClusterDiscoveryResponder::valid_configuration(
    const ClusterDiscoveryResponderConfig& config) noexcept {
    // Port 0 binds an ephemeral port, which only tests want.
    return config.bind_endpoint.address().is_v4() &&
           config.control_endpoint.port() != 0U &&
           config.max_answers_per_second != 0U &&
           describe_control(config.control_endpoint).has_value();
}

std::error_code ClusterDiscoveryResponder::start() {
    if (!valid_configuration(state_->config)) {
        return std::make_error_code(std::errc::invalid_argument);
    }
    bool expected = false;
    if (!state_->running.compare_exchange_strong(expected, true)) {
        return std::make_error_code(std::errc::operation_in_progress);
    }
    const ControlAddress control = *describe_control(state_->config.control_endpoint);
    state_->loopback_only = control.loopback_only;
    state_->answer = ClusterDiscoveryAnswer{
        0U,
        random_u64(),
        state_->config.node_index,
        state_->config.control_endpoint.port(),
        control.address,
        state_->config.password_set,
        state_->config.hostname.empty()
            ? cluster_discovery_hostname()
            : clean_hostname(state_->config.hostname)};
    state_->stop_requested.store(false, std::memory_order_release);
    std::error_code error;
    const auto fail = [&](std::error_code reason) {
        std::error_code ignored;
        state_->socket.close(ignored);
        state_->running.store(false, std::memory_order_release);
        return reason;
    };
    state_->socket.open(asio::ip::udp::v4(), error);
    if (error) {
        return fail(error);
    }
    // Every agent on this machine shares the one well-known port; broadcast
    // queries reach each of them.
    state_->socket.set_option(asio::socket_base::reuse_address{true}, error);
    if (!error) {
        state_->socket.bind(state_->config.bind_endpoint, error);
    }
    if (!error) {
        state_->socket.non_blocking(true, error);
    }
    if (error) {
        return fail(error);
    }
    ignore_unreachable_reports(state_->socket);
    {
        const std::lock_guard lock{state_->endpoint_mutex};
        state_->endpoint = state_->socket.local_endpoint(error);
        if (error) {
            state_->endpoint.reset();
            return fail(error);
        }
    }
    try {
        state_->thread =
            std::thread{[state = state_.get()] { state->loop(); }};
    } catch (const std::system_error& thread_error) {
        return fail(thread_error.code());
    }
    return {};
}

void ClusterDiscoveryResponder::stop() noexcept {
    if (!state_) {
        return;
    }
    state_->stop_requested.store(true, std::memory_order_release);
    if (state_->thread.joinable()) {
        state_->thread.join();
    }
    std::error_code ignored;
    state_->socket.close(ignored);
    {
        const std::lock_guard lock{state_->endpoint_mutex};
        state_->endpoint.reset();
    }
    state_->running.store(false, std::memory_order_release);
}

std::optional<asio::ip::udp::endpoint>
ClusterDiscoveryResponder::local_endpoint() const noexcept {
    const std::lock_guard lock{state_->endpoint_mutex};
    return state_->endpoint;
}

core::u64 ClusterDiscoveryResponder::answers() const noexcept {
    return state_->answer_count.load(std::memory_order_relaxed);
}

namespace detail {

class ClusterDiscoveryBrowserState final {
public:
    using Clock = std::chrono::steady_clock;

    struct Heard final {
        asio::ip::address_v4 address;
        Clock::time_point at;
    };

    struct Entry final {
        ClusterDiscoveryAnswer answer;
        std::vector<Heard> heard;
        Clock::time_point last_heard;
    };

    explicit ClusterDiscoveryBrowserState(ClusterDiscoveryBrowserConfig value)
        : config(std::move(value)), socket(context) {}

    void send_queries(Clock::time_point now) {
        const std::uint64_t nonce = random_u64();
        nonces[next_nonce % nonces.size()] = nonce;
        ++next_nonce;
        local = local_ipv4_interfaces();
        std::vector<asio::ip::address_v4> targets = config.targets;
        if (targets.empty()) {
            targets.push_back(asio::ip::address_v4::loopback());
            targets.push_back(asio::ip::address_v4::broadcast());
            for (const LocalIpv4Interface& entry : local) {
                if (std::find(targets.begin(), targets.end(),
                              entry.broadcast) == targets.end()) {
                    targets.push_back(entry.broadcast);
                }
            }
        }
        const auto bytes = encode_cluster_discovery_query({nonce});
        for (const asio::ip::address_v4& target : targets) {
            std::error_code ignored;
            // A target with no route (no network yet) just finds nothing.
            static_cast<void>(socket.send_to(
                asio::buffer(bytes),
                asio::ip::udp::endpoint{target, config.port}, 0, ignored));
        }
        next_query = now + config.query_interval;
    }

    void receive(Clock::time_point now) {
        std::array<std::uint8_t, cluster_discovery_datagram_bytes + 1U>
            buffer{};
        for (;;) {
            asio::ip::udp::endpoint sender;
            std::error_code error;
            const std::size_t received =
                socket.receive_from(asio::buffer(buffer), sender, 0, error);
            if (error) {
                if (would_block(error)) {
                    return;
                }
                continue;
            }
            if (!sender.address().is_v4()) {
                continue;
            }
            auto answer = decode_cluster_discovery_answer(
                std::span<const std::uint8_t>{buffer.data(), received});
            if (!answer || answer->nonce == 0U ||
                std::find(nonces.begin(), nonces.end(), answer->nonce) ==
                    nonces.end()) {
                continue;
            }
            const asio::ip::address_v4 from = sender.address().to_v4();
            auto entry = std::find_if(
                entries.begin(), entries.end(), [&](const Entry& candidate) {
                    return candidate.answer.instance == answer->instance &&
                           candidate.answer.node_index == answer->node_index;
                });
            if (entry == entries.end()) {
                if (entries.size() >= 1'024U) {
                    continue;
                }
                entries.push_back(Entry{*answer, {}, now});
                entry = std::prev(entries.end());
            }
            entry->answer = std::move(*answer);
            entry->last_heard = now;
            auto heard = std::find_if(
                entry->heard.begin(), entry->heard.end(),
                [&](const Heard& candidate) { return candidate.address == from; });
            if (heard == entry->heard.end()) {
                entry->heard.push_back(Heard{from, now});
            } else {
                heard->at = now;
            }
        }
    }

    void expire(Clock::time_point now) {
        for (Entry& entry : entries) {
            std::erase_if(entry.heard, [&](const Heard& heard) {
                return now - heard.at > config.expiry;
            });
        }
        std::erase_if(entries, [&](const Entry& entry) {
            return entry.heard.empty() || now - entry.last_heard > config.expiry;
        });
    }

    [[nodiscard]] std::vector<ClusterDiscoveredMachine> machines() const {
        std::vector<ClusterDiscoveredMachine> result;
        result.reserve(entries.size());
        for (const Entry& entry : entries) {
            ClusterDiscoveredMachine machine{};
            machine.instance = entry.answer.instance;
            machine.hostname = entry.answer.hostname;
            machine.node_index = entry.answer.node_index;
            machine.control_port = entry.answer.control_port;
            machine.password_set = entry.answer.password_set;
            std::optional<asio::ip::address_v4> lan;
            for (const Heard& heard : entry.heard) {
                machine.addresses.push_back(heard.address);
                const bool own =
                    heard.address.is_loopback() ||
                    std::any_of(local.begin(), local.end(),
                                [&](const LocalIpv4Interface& own) {
                                    return own.address == heard.address;
                                });
                machine.this_machine = machine.this_machine || own;
                if (!lan && !heard.address.is_loopback()) {
                    lan = heard.address;
                }
            }
            if (!entry.answer.control_address.is_unspecified()) {
                machine.address = entry.answer.control_address;
            } else if (lan) {
                machine.address = *lan;
            } else {
                machine.address = asio::ip::address_v4::loopback();
            }
            result.push_back(std::move(machine));
        }
        std::sort(result.begin(), result.end(),
                  [](const ClusterDiscoveredMachine& left,
                     const ClusterDiscoveredMachine& right) {
                      if (left.hostname != right.hostname) {
                          return left.hostname < right.hostname;
                      }
                      if (left.node_index != right.node_index) {
                          return left.node_index < right.node_index;
                      }
                      if (left.address != right.address) {
                          return left.address < right.address;
                      }
                      return left.instance < right.instance;
                  });
        return result;
    }

    void loop() noexcept {
        try {
            while (!stop_requested.load(std::memory_order_acquire)) {
                auto now = Clock::now();
                if (now >= next_query ||
                    query_requested.exchange(false, std::memory_order_acq_rel)) {
                    send_queries(now);
                }
                const auto until_query =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        next_query - now);
                const auto wait = std::clamp(until_query,
                                             std::chrono::milliseconds{1},
                                             wait_slice);
                if (wait_readable(socket, wait) ==
                    detail::WaitOutcome::Unavailable) {
                    std::this_thread::sleep_for(wait);
                }
                now = Clock::now();
                receive(now);
                expire(now);
                std::vector<ClusterDiscoveredMachine> current = machines();
                const std::lock_guard lock{snapshot_mutex};
                if (published.machines != current) {
                    published.machines = std::move(current);
                }
            }
        } catch (const std::bad_alloc&) {
            const std::lock_guard lock{snapshot_mutex};
            published.running = false;
            published.error = std::make_error_code(std::errc::not_enough_memory);
            published.machines.clear();
        }
    }

    ClusterDiscoveryBrowserConfig config;
    asio::io_context context;
    asio::ip::udp::socket socket;
    std::thread thread;
    std::atomic_bool stop_requested{};
    std::atomic_bool query_requested{};
    std::atomic_bool running{};
    // Loop-thread state.
    std::array<std::uint64_t, remembered_nonces> nonces{};
    std::size_t next_nonce{};
    Clock::time_point next_query{};
    std::vector<LocalIpv4Interface> local;
    std::vector<Entry> entries;
    // Shared with snapshot().
    mutable std::mutex snapshot_mutex;
    ClusterDiscoverySnapshot published;
};

} // namespace detail

ClusterDiscoveryBrowser::ClusterDiscoveryBrowser(
    ClusterDiscoveryBrowserConfig config)
    : state_(std::make_unique<detail::ClusterDiscoveryBrowserState>(
          std::move(config))) {}

ClusterDiscoveryBrowser::~ClusterDiscoveryBrowser() { stop(); }

std::error_code ClusterDiscoveryBrowser::start() {
    if (state_->config.port == 0U ||
        state_->config.query_interval <= std::chrono::milliseconds::zero() ||
        state_->config.expiry <= std::chrono::milliseconds::zero()) {
        return std::make_error_code(std::errc::invalid_argument);
    }
    bool expected = false;
    if (!state_->running.compare_exchange_strong(expected, true)) {
        return std::make_error_code(std::errc::operation_in_progress);
    }
    state_->stop_requested.store(false, std::memory_order_release);
    std::error_code error;
    const auto fail = [&](std::error_code reason) {
        std::error_code ignored;
        state_->socket.close(ignored);
        state_->running.store(false, std::memory_order_release);
        const std::lock_guard lock{state_->snapshot_mutex};
        state_->published = ClusterDiscoverySnapshot{false, reason, {}};
        return reason;
    };
    state_->socket.open(asio::ip::udp::v4(), error);
    if (!error) {
        state_->socket.set_option(asio::socket_base::broadcast{true}, error);
    }
    if (!error) {
        // An ephemeral port of its own, so every editor on a machine hears
        // the answers to its own queries.
        state_->socket.bind(
            asio::ip::udp::endpoint{asio::ip::address_v4::any(), 0U}, error);
    }
    if (!error) {
        state_->socket.non_blocking(true, error);
    }
    if (error) {
        return fail(error);
    }
    ignore_unreachable_reports(state_->socket);
    state_->next_query = {};
    state_->entries.clear();
    {
        const std::lock_guard lock{state_->snapshot_mutex};
        state_->published = ClusterDiscoverySnapshot{true, {}, {}};
    }
    try {
        state_->thread =
            std::thread{[state = state_.get()] { state->loop(); }};
    } catch (const std::system_error& thread_error) {
        return fail(thread_error.code());
    }
    return {};
}

void ClusterDiscoveryBrowser::stop() noexcept {
    if (!state_) {
        return;
    }
    state_->stop_requested.store(true, std::memory_order_release);
    if (state_->thread.joinable()) {
        state_->thread.join();
    }
    std::error_code ignored;
    state_->socket.close(ignored);
    if (state_->running.exchange(false, std::memory_order_acq_rel)) {
        const std::lock_guard lock{state_->snapshot_mutex};
        state_->published.running = false;
        state_->published.machines.clear();
    }
}

ClusterDiscoverySnapshot ClusterDiscoveryBrowser::snapshot() const {
    const std::lock_guard lock{state_->snapshot_mutex};
    return state_->published;
}

void ClusterDiscoveryBrowser::query_now() noexcept {
    state_->query_requested.store(true, std::memory_order_release);
}

} // namespace mediaserver::net
