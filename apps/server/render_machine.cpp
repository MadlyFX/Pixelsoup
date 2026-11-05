#include "render_machine.hpp"

#include <mediaserver/net/cluster_discovery.hpp>
#include <mediaserver/net/cluster_lifecycle.hpp>

#include <asio/ip/address.hpp>
#include <asio/ip/address_v4.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>
#include <system_error>
#include <utility>

namespace mediaserver::app {
namespace {

// The incarnation the scripts give every render process. A restart under the
// agent is the same process identity coming back, not a new one.
constexpr std::string_view render_node_incarnation =
    "00000000000000000000000000000001";
// The output window a render process opens until the project's Hardware Map
// places it, as run-output-node.ps1 defaults it.
constexpr std::string_view fallback_surface = "1920x1080";
constexpr std::string_view fallback_window_at = "0,0";
constexpr std::string_view status_interval = "120";

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    const auto is_space = [](char character) {
        return character == ' ' || character == '\t' || character == '\r' ||
               character == '\n';
    };
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1U);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1U);
    }
    return text;
}

[[nodiscard]] std::optional<std::uint32_t>
parse_decimal(std::string_view text, std::uint32_t maximum) noexcept {
    std::uint32_t value{};
    const auto* const end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, value, 10);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != end ||
        value > maximum) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<net::Id128>
parse_id128(std::string_view text) noexcept {
    if (text.size() != 32U) {
        return std::nullopt;
    }
    net::Id128 value{};
    const auto half = [](std::string_view digits, core::u64& out) {
        const auto* const end = digits.data() + digits.size();
        const auto parsed = std::from_chars(digits.data(), end, out, 16);
        return parsed.ec == std::errc{} && parsed.ptr == end;
    };
    if (!half(text.substr(0U, 16U), value.high) ||
        !half(text.substr(16U), value.low) || !value) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::string format_id128(net::Id128 value) {
    constexpr std::string_view digits = "0123456789ABCDEF";
    std::string text(32U, '0');
    for (std::size_t index = 0U; index < 16U; ++index) {
        text[15U - index] =
            digits[static_cast<std::size_t>((value.high >> (index * 4U)) & 0xfU)];
        text[31U - index] =
            digits[static_cast<std::size_t>((value.low >> (index * 4U)) & 0xfU)];
    }
    return text;
}

[[nodiscard]] bool connectable(const asio::ip::address& address) noexcept {
    if (address.is_unspecified() || address.is_multicast()) {
        return false;
    }
    return !address.is_v4() ||
           address.to_v4() != asio::ip::address_v4::broadcast();
}

[[nodiscard]] std::optional<asio::ip::address>
parse_address(std::string_view text) {
    if (text.size() > 2U && text.front() == '[' && text.back() == ']') {
        text = text.substr(1U, text.size() - 2U);
    }
    std::error_code error;
    const auto address = asio::ip::make_address(std::string{text}, error);
    if (error) {
        return std::nullopt;
    }
    // Windows also reads classful shorthand ("10.0.0" as 10.0.0.0) and
    // leading zeros; for a typed address those are typos, not intent.
    if (address.is_v4() && address.to_string(error) != text) {
        return std::nullopt;
    }
    return address;
}

// "ADDRESS:PORT", or "[IPV6]:PORT". A bare IPv6 address has colons of its
// own and is not an endpoint.
[[nodiscard]] std::optional<asio::ip::tcp::endpoint>
parse_endpoint(std::string_view text) {
    std::string_view address_text;
    std::string_view port_text;
    if (text.starts_with('[')) {
        const auto closing = text.find(']');
        if (closing == std::string_view::npos || closing + 1U >= text.size() ||
            text[closing + 1U] != ':') {
            return std::nullopt;
        }
        address_text = text.substr(1U, closing - 1U);
        port_text = text.substr(closing + 2U);
    } else {
        const auto separator = text.rfind(':');
        if (separator == std::string_view::npos ||
            text.find(':') != separator) {
            return std::nullopt;
        }
        address_text = text.substr(0U, separator);
        port_text = text.substr(separator + 1U);
    }
    const auto port = parse_decimal(port_text, 65'535U);
    const auto address = parse_address(address_text);
    if (!port || !address) {
        return std::nullopt;
    }
    return asio::ip::tcp::endpoint{*address, static_cast<std::uint16_t>(*port)};
}

[[nodiscard]] std::string address_text(const asio::ip::address& address) {
    std::error_code error;
    std::string text = address.to_string(error);
    return error ? std::string{} : text;
}

[[nodiscard]] std::string
endpoint_text(const asio::ip::tcp::endpoint& endpoint) {
    const std::string address = address_text(endpoint.address());
    if (endpoint.address().is_v6()) {
        return '[' + address + "]:" + std::to_string(endpoint.port());
    }
    return address + ':' + std::to_string(endpoint.port());
}

[[nodiscard]] bool valid_key(std::string_view key) noexcept {
    return key.size() == 64U &&
           key.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

[[nodiscard]] bool looks_like_a_name(std::string_view text) noexcept {
    return std::any_of(text.begin(), text.end(), [](char character) {
        const auto byte = static_cast<unsigned char>(character);
        return std::isalpha(byte) != 0 && !std::isxdigit(byte);
    });
}

[[nodiscard]] core::Result<RenderMachineConfig, RenderMachineConfigError>
invalid(std::string message) {
    return core::Result<RenderMachineConfig, RenderMachineConfigError>::failure(
        RenderMachineConfigError{RenderMachineConfigIssue::Invalid,
                                 std::move(message)});
}

[[nodiscard]] core::Result<persist::Preferences, RenderMachineFormError>
form_error(RenderMachineField field, std::string message) {
    return core::Result<persist::Preferences, RenderMachineFormError>::failure(
        RenderMachineFormError{field, std::move(message)});
}

} // namespace

std::uint16_t
render_machine_default_control_port(core::u32 node_index) noexcept {
    return static_cast<std::uint16_t>(
        render_machine_base_control_port +
        std::min(node_index, persist::maximum_render_machine_node_index));
}

net::Id128 render_machine_node_id(core::u32 node_index) noexcept {
    return net::Id128{0U, 0x2AU + static_cast<core::u64>(node_index)};
}

core::Result<RenderMachineConfig, RenderMachineConfigError>
render_machine_config(const persist::Preferences& stored) {
    if (stored.render_machine_director.empty()) {
        return core::Result<RenderMachineConfig, RenderMachineConfigError>::
            failure(RenderMachineConfigError{
                RenderMachineConfigIssue::NotSetUp,
                "This machine is not set up as a render machine yet."});
    }
    RenderMachineConfig config{};
    config.discover_director = stored.render_machine_director == "auto";
    const auto director = config.discover_director
        ? std::optional<asio::ip::tcp::endpoint>{asio::ip::tcp::endpoint{}}
        : parse_endpoint(stored.render_machine_director);
    if (!config.discover_director && (!director || director->port() == 0U ||
        !connectable(director->address()))) {
        return invalid("The saved control machine address (" +
                       stored.render_machine_director +
                       ") cannot be used. Enter it again.");
    }
    config.director = *director;
    config.automatic_index = stored.render_machine_node_index == net::automatic_node_index;
    if (!config.automatic_index && stored.render_machine_node_index >
        persist::maximum_render_machine_node_index) {
        return invalid("The saved node index is out of range. Enter it again.");
    }
    config.node_index = stored.render_machine_node_index;
    if (!stored.render_machine_node_id.empty()) {
        const auto id = parse_id128(stored.render_machine_node_id);
        if (!id) return invalid("The saved machine identity is invalid. Restore this machine's settings.");
        config.node_id = *id;
    } else if (config.automatic_index) {
        return invalid("Save settings to generate this render machine's identity.");
    } else {
        config.node_id = render_machine_node_id(config.node_index);
    }
    if (stored.render_machine_control_port > 65'535U) {
        return invalid(
            "The saved control port is out of range. Enter it again.");
    }
    config.automatic_control_port = stored.render_machine_control_port == 0U;
    config.control_port =
        stored.render_machine_control_port == 0U
            ? render_machine_default_control_port(config.node_index)
            : static_cast<std::uint16_t>(stored.render_machine_control_port);
    const auto show = parse_id128(stored.render_machine_show_id);
    if (!stored.render_machine_show_id.empty() && !show) {
        return invalid("The saved show ID (" + stored.render_machine_show_id +
                       ") is not 32 hexadecimal digits. Enter it again.");
    }
    config.show_id = show.value_or(net::Id128{});
    // A damaged key is refused rather than read as "no password": a machine
    // its operator secured must not quietly come up open to the network.
    if (stored.lifecycle_password_key.empty()) {
        config.lifecycle_key = net::lifecycle_key_from_password({});
    } else if (valid_key(stored.lifecycle_password_key)) {
        config.lifecycle_key = stored.lifecycle_password_key;
    } else {
        return invalid(
            "The saved render node password could not be read. Enter it "
            "again.");
    }
    config.password_set = !net::lifecycle_key_is_unsecured(config.lifecycle_key);
    return core::Result<RenderMachineConfig, RenderMachineConfigError>::success(
        std::move(config));
}

std::vector<std::string>
render_machine_child_arguments(const RenderMachineConfig& config) {
    // Physical/feed mapping is part of the output path. --scene would bypass
    // the Feed Map and present the plain camera view.
    return {
        "--role=render,output",
        "--director-connect=" + (config.discover_director
                                     ? std::string{"auto"}
                                     : endpoint_text(config.director)),
        "--show-id=" + (config.show_id ? format_id128(config.show_id) : std::string{"auto"}),
        "--node-id=" + format_id128(config.node_id ? config.node_id : render_machine_node_id(config.node_index)),
        "--node-incarnation=" + std::string{render_node_incarnation},
        "--playback",
        "--output",
        "--surface=" + std::string{fallback_surface},
        "--window-at=" + std::string{fallback_window_at},
        "--status-interval=" + std::string{status_interval},
        "--borderless",
    };
}

std::vector<std::string>
editor_launch_arguments(const persist::Preferences& stored) {
    std::vector<std::string> arguments{"--role=director", "--playback",
                                       "--edit"};
    if (stored.director_listen.empty()) {
        arguments.push_back(
            "--director-listen=0.0.0.0:" +
            std::to_string(render_machine_default_director_port));
    }
    if (stored.show_id.empty()) {
        arguments.push_back("--show-id=" +
                            std::string{render_machine_default_show_id});
    }
    if (stored.director_epoch.empty()) {
        arguments.push_back("--director-epoch=" +
                            std::string{render_machine_default_director_epoch});
    }
    return arguments;
}

LifecycleAgentConfig
render_machine_agent_config(const RenderMachineConfig& config,
                            const std::filesystem::path& executable) {
    LifecycleAgentConfig agent{};
    agent.listen = asio::ip::tcp::endpoint{asio::ip::address_v4::any(),
                                           config.control_port};
    agent.node_index = config.node_index;
    agent.secret = config.lifecycle_key;
    agent.child = executable;
    agent.child_arguments = render_machine_child_arguments(config);
    agent.autostart = true;
    agent.discovery_port = net::cluster_discovery_port;
    return agent;
}

RenderMachineForm render_machine_form(const persist::Preferences& stored) {
    RenderMachineForm form{};
    form.director_port = std::to_string(render_machine_default_director_port);
    if (const auto director = parse_endpoint(stored.render_machine_director)) {
        form.director_address = address_text(director->address());
        form.director_port = std::to_string(director->port());
    } else {
        // Shown as stored, so the operator sees what needs correcting.
        if (stored.render_machine_director != "auto") {
            form.director_address = stored.render_machine_director;
        }
    }
    if (!stored.render_machine_director.empty() &&
        stored.render_machine_node_index != net::automatic_node_index) {
        form.node_index = std::to_string(stored.render_machine_node_index);
    }
    if (stored.render_machine_control_port != 0U) {
        form.control_port = std::to_string(stored.render_machine_control_port);
    }
    form.show_id = stored.render_machine_show_id;
    form.password_set =
        !stored.lifecycle_password_key.empty() &&
        !net::lifecycle_key_is_unsecured(stored.lifecycle_password_key);
    return form;
}

core::Result<persist::Preferences, RenderMachineFormError>
apply_render_machine_form(const RenderMachineForm& form,
                          persist::Preferences stored) {
    const std::string_view typed_address = trim(form.director_address);
    const bool automatic = typed_address.empty();
    std::optional<asio::ip::tcp::endpoint> director =
        parse_endpoint(typed_address);
    if (!automatic && !director) {
        const auto address = parse_address(typed_address);
        if (!address) {
            return form_error(
                RenderMachineField::DirectorAddress,
                looks_like_a_name(typed_address)
                    ? "Enter the control machine's numeric address, such as "
                      "192.168.1.20, not its name. ipconfig on that machine "
                      "shows it."
                    : "That is not an address. Enter the control machine's "
                      "numeric address, such as 192.168.1.20.");
        }
        const std::string_view typed_port = trim(form.director_port);
        const auto port =
            typed_port.empty()
                ? std::optional<std::uint32_t>{render_machine_default_director_port}
                : parse_decimal(typed_port, 65'535U);
        if (!port || *port == 0U) {
            return form_error(RenderMachineField::DirectorPort,
                              "The Director port is a number from 1 to "
                              "65535 - normally 45060.");
        }
        director = asio::ip::tcp::endpoint{*address,
                                           static_cast<std::uint16_t>(*port)};
    }
    if (!automatic && director->port() == 0U) {
        return form_error(RenderMachineField::DirectorPort,
                          "The Director port is a number from 1 to 65535 - "
                          "normally 45060.");
    }
    if (!automatic && !connectable(director->address())) {
        return form_error(RenderMachineField::DirectorAddress,
                          "That address cannot be connected to. Enter the "
                          "control machine's own address, such as "
                          "192.168.1.20.");
    }

    const auto node_index = trim(form.node_index).empty()
        ? std::optional<std::uint32_t>{net::automatic_node_index}
        : parse_decimal(trim(form.node_index), persist::maximum_render_machine_node_index);
    if (!node_index) {
        return form_error(RenderMachineField::NodeIndex,
                          "The node index is a whole number from 0 to 4095: "
                          "0 for the first render machine, 1 for the next, "
                          "and so on.");
    }

    std::uint32_t control_port = 0U;
    if (const std::string_view typed = trim(form.control_port); !typed.empty()) {
        const auto parsed = parse_decimal(typed, 65'535U);
        if (!parsed || *parsed == 0U) {
            return form_error(RenderMachineField::ControlPort,
                              "Leave the control port empty for the default, "
                              "or enter a number from 1 to 65535.");
        }
        control_port = *parsed;
    }

    std::string show_id;
    if (const std::string_view typed = trim(form.show_id); !typed.empty()) {
        const auto parsed = parse_id128(typed);
        if (!parsed) {
            return form_error(RenderMachineField::ShowId,
                              "Leave the show ID empty for the standard show, "
                              "or enter the control machine's: exactly 32 "
                              "hexadecimal digits.");
        }
        show_id = format_id128(*parsed);
    }

    if (form.password_edited) {
        if (form.password.size() > render_machine_max_password_bytes) {
            return form_error(RenderMachineField::Password,
                              "The password can be at most 256 characters.");
        }
        // Kept only as the key it derives, like the editor's copy of it.
        stored.lifecycle_password_key =
            form.password.empty()
                ? std::string{}
                : net::lifecycle_key_from_password(form.password);
    }
    if (stored.render_machine_node_id.empty()) {
        // A configured machine already has an index-derived session identity.
        // Keep that identity when clearing overrides so its editor assignment
        // and existing output binding survive the change to automatic setup.
        const auto id = !stored.render_machine_director.empty() &&
                        stored.render_machine_node_index <= persist::maximum_render_machine_node_index
            ? render_machine_node_id(stored.render_machine_node_index)
            : net::new_render_machine_id();
        stored.render_machine_node_id = format_id128(id);
    }
    stored.render_machine_director = automatic ? "auto" : endpoint_text(*director);
    stored.render_machine_node_index = *node_index;
    stored.render_machine_control_port = control_port;
    stored.render_machine_show_id = std::move(show_id);
    return core::Result<persist::Preferences, RenderMachineFormError>::success(
        std::move(stored));
}

std::vector<FirewallRule>
render_machine_firewall_rules(std::uint16_t control_port,
                              std::uint16_t discovery_port) {
    std::vector<FirewallRule> rules;
    rules.push_back(FirewallRule{"Media Server Lifecycle (TCP " +
                                     std::to_string(control_port) + ")",
                                 "TCP", control_port});
    if (discovery_port != 0U) {
        rules.push_back(FirewallRule{"Media Server Discovery (UDP " +
                                         std::to_string(discovery_port) + ")",
                                     "UDP", discovery_port});
    }
    return rules;
}

std::string firewall_rules_command(const std::vector<FirewallRule>& rules) {
    // Every stale copy goes first, then the adds, chained so the command's
    // exit status is the first add that failed (or the last one).
    std::string command = "/c ";
    for (const FirewallRule& rule : rules) {
        command += "netsh advfirewall firewall delete rule name=\"" +
                   rule.name + "\" >nul 2>&1 & ";
    }
    for (std::size_t index = 0U; index < rules.size(); ++index) {
        const FirewallRule& rule = rules[index];
        if (index != 0U) {
            command += " && ";
        }
        command += "netsh advfirewall firewall add rule name=\"" + rule.name +
                   "\" dir=in action=allow protocol=" + rule.protocol +
                   " localport=" + std::to_string(rule.port);
    }
    return command;
}

} // namespace mediaserver::app
