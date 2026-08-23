#include "playback.hpp"

#include <mediaserver/cmd/node_command_bus.hpp>
#include <mediaserver/net/director_frame_barrier.hpp>
#include <mediaserver/net/director_session_hub.hpp>
#include <mediaserver/net/director_session_service.hpp>
#include <mediaserver/net/durable_director_frame_release_authority.hpp>
#include <mediaserver/net/node_session_service.hpp>
#include <mediaserver/persist/durable_director.hpp>
#include <mediaserver/persist/release_ledger.hpp>
#include <mediaserver/persist/unresolved_release_inventory.hpp>

#include <asio/ip/address.hpp>

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>

namespace {

enum class Role : std::uint32_t {
    None = 0U,
    Director = 1U << 0U,
    Render = 1U << 1U,
    Receiver = 1U << 2U,
    Compositor = 1U << 3U,
    Output = 1U << 4U,
};

volatile std::sig_atomic_t stop_requested = 0;

extern "C" void request_stop(int) {
    stop_requested = 1;
}

[[nodiscard]] constexpr std::uint32_t bits(Role role) noexcept {
    return static_cast<std::uint32_t>(role);
}

[[nodiscard]] constexpr bool has_role(std::uint32_t roles, Role role) noexcept {
    return (roles & bits(role)) != 0U;
}

[[nodiscard]] constexpr bool valid_hub_limits(
    const mediaserver::net::DirectorSessionHubLimits& limits) noexcept {
    return limits.max_subscriptions != 0U && limits.max_backlog_frames != 0U
        && limits.max_backlog_bytes != 0U;
}

[[nodiscard]] bool add_role(std::string_view name, std::uint32_t& roles) {
    if (name == "director") {
        roles |= bits(Role::Director);
    } else if (name == "render") {
        roles |= bits(Role::Render);
    } else if (name == "receiver") {
        roles |= bits(Role::Receiver);
    } else if (name == "compositor") {
        roles |= bits(Role::Compositor);
    } else if (name == "output") {
        roles |= bits(Role::Output);
    } else {
        return false;
    }
    return true;
}

[[nodiscard]] bool parse_roles(std::string_view value, std::uint32_t& roles) {
    while (!value.empty()) {
        const auto separator = value.find(',');
        const auto role = value.substr(0U, separator);
        if (role.empty() || !add_role(role, roles)) {
            return false;
        }
        if (separator == std::string_view::npos) {
            return true;
        }
        value.remove_prefix(separator + 1U);
    }
    return false;
}

[[nodiscard]] bool parse_hex_u64(
    std::string_view value,
    mediaserver::core::u64& parsed) noexcept {
    if (value.size() != 16U) {
        return false;
    }
    const auto* const begin = value.data();
    const auto* const end = begin + value.size();
    const auto result = std::from_chars(begin, end, parsed, 16);
    return result.ec == std::errc{} && result.ptr == end;
}

[[nodiscard]] bool parse_u64_argument(
    std::string_view value,
    mediaserver::core::u64& parsed) noexcept {
    if (value.empty()) {
        return false;
    }
    const auto* const begin = value.data();
    const auto* const end = begin + value.size();
    const auto result = std::from_chars(begin, end, parsed, 10);
    return result.ec == std::errc{} && result.ptr == end;
}

[[nodiscard]] bool parse_window_position(
    std::string_view value,
    mediaserver::core::i32& x,
    mediaserver::core::i32& y) noexcept {
    const auto separator = value.find(',');
    if (separator == std::string_view::npos) {
        return false;
    }
    mediaserver::core::u64 parsed_x{};
    mediaserver::core::u64 parsed_y{};
    if (!parse_u64_argument(value.substr(0U, separator), parsed_x)
        || !parse_u64_argument(value.substr(separator + 1U), parsed_y)
        || parsed_x > 32'767U || parsed_y > 32'767U) {
        return false;
    }
    x = static_cast<mediaserver::core::i32>(parsed_x);
    y = static_cast<mediaserver::core::i32>(parsed_y);
    return true;
}

[[nodiscard]] bool parse_surface(
    std::string_view value,
    mediaserver::core::u32& width,
    mediaserver::core::u32& height) noexcept {
    const auto separator = value.find('x');
    if (separator == std::string_view::npos) {
        return false;
    }
    mediaserver::core::u64 parsed_width{};
    mediaserver::core::u64 parsed_height{};
    if (!parse_u64_argument(value.substr(0U, separator), parsed_width)
        || !parse_u64_argument(value.substr(separator + 1U), parsed_height)
        || parsed_width == 0U || parsed_height == 0U || parsed_width > 7'680U
        || parsed_height > 4'320U) {
        return false;
    }
    width = static_cast<mediaserver::core::u32>(parsed_width);
    height = static_cast<mediaserver::core::u32>(parsed_height);
    return true;
}

[[nodiscard]] bool parse_id128(
    std::string_view value,
    mediaserver::net::Id128& parsed) noexcept {
    mediaserver::core::u64 high{};
    mediaserver::core::u64 low{};
    if (value.size() != 32U || !parse_hex_u64(value.substr(0U, 16U), high)
        || !parse_hex_u64(value.substr(16U), low)) {
        return false;
    }
    parsed = mediaserver::net::Id128{high, low};
    return static_cast<bool>(parsed);
}

[[nodiscard]] constexpr mediaserver::core::u64 session_namespace(
    mediaserver::net::Id128 epoch) noexcept {
    constexpr mediaserver::core::u64 golden_ratio =
        0x9e3779b97f4a7c15ULL;
    const auto mixed = epoch.high
        ^ (epoch.low + golden_ratio + (epoch.high << 6U)
           + (epoch.high >> 2U));
    return mixed != 0U ? mixed : golden_ratio;
}

[[nodiscard]] std::optional<asio::ip::tcp::endpoint> parse_endpoint(
    std::string_view value) {
    std::string_view address_text;
    std::string_view port_text;
    if (value.starts_with('[')) {
        const auto closing = value.find(']');
        if (closing == std::string_view::npos || closing + 1U >= value.size()
            || value[closing + 1U] != ':') {
            return std::nullopt;
        }
        address_text = value.substr(1U, closing - 1U);
        port_text = value.substr(closing + 2U);
    } else {
        const auto separator = value.rfind(':');
        if (separator == std::string_view::npos
            || value.find(':') != separator) {
            return std::nullopt;
        }
        address_text = value.substr(0U, separator);
        port_text = value.substr(separator + 1U);
    }
    if (address_text.empty() || port_text.empty()) {
        return std::nullopt;
    }

    unsigned int port{};
    const auto* const port_begin = port_text.data();
    const auto* const port_end = port_begin + port_text.size();
    const auto parsed_port = std::from_chars(port_begin, port_end, port, 10);
    if (parsed_port.ec != std::errc{} || parsed_port.ptr != port_end
        || port > 65'535U) {
        return std::nullopt;
    }

    std::error_code error;
    const auto address = asio::ip::make_address(
        std::string{address_text}, error);
    if (error) {
        return std::nullopt;
    }
    return asio::ip::tcp::endpoint{
        address,
        static_cast<unsigned short>(port),
    };
}

[[nodiscard]] std::string endpoint_text(
    const asio::ip::tcp::endpoint& endpoint) {
    std::error_code error;
    const auto address = endpoint.address().to_string(error);
    if (error) {
        return "unavailable";
    }
    if (endpoint.address().is_v6()) {
        return '[' + address + "]:" + std::to_string(endpoint.port());
    }
    return address + ':' + std::to_string(endpoint.port());
}

[[nodiscard]] bool valid_node_connect_endpoint(
    const asio::ip::tcp::endpoint& endpoint) noexcept {
    const auto& address = endpoint.address();
    if (endpoint.port() == 0U || address.is_unspecified()
        || address.is_multicast()) {
        return false;
    }
    return !address.is_v4()
        || address.to_v4() != asio::ip::address_v4::broadcast();
}

void report_command_log_error(
    std::string_view operation,
    const mediaserver::persist::CommandLogError& error) {
    std::cerr << operation << " (category " << error.index() << ')';
    if (const auto* failure =
            std::get_if<mediaserver::persist::LogFailure>(&error)) {
        std::cerr << ": " << failure->error.detail;
        if (failure->error.system_error) {
            std::cerr << ": " << failure->error.system_error.message();
        }
    }
    std::cerr << '\n';
}

void report_release_ledger_error(
    std::string_view operation,
    const mediaserver::persist::ReleaseLedgerError& error) {
    std::cerr << operation << " (code "
              << static_cast<unsigned>(error.code) << "): " << error.detail;
    if (error.system_error) {
        std::cerr << ": " << error.system_error.message();
    }
    if (error.command_log_error) {
        std::cerr << "\n";
        report_command_log_error(
            "release ledger could not validate the Director WAL",
            *error.command_log_error);
        return;
    }
    std::cerr << '\n';
}

[[nodiscard]] bool install_service_stop_handlers() {
    if (std::signal(SIGINT, request_stop) == SIG_ERR
        || std::signal(SIGTERM, request_stop) == SIG_ERR) {
        std::cerr << "could not install service stop handlers\n";
        return false;
    }
#if defined(_WIN32)
    if (std::signal(SIGBREAK, request_stop) == SIG_ERR) {
        std::cerr << "could not install the Windows service stop handler\n";
        return false;
    }
#endif
    return true;
}

[[nodiscard]] int run_service_loop() {
    while (stop_requested == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    std::cout << "media-server stopping\n";
    return 0;
}

[[nodiscard]] int run_node_service_loop(
    const mediaserver::net::NodeSessionService& service) {
    while (stop_requested == 0) {
        const auto status = service.snapshot();
        if (status.state
            == mediaserver::net::NodeSessionServiceState::Faulted) {
            std::cerr << "node session supervisor faulted (failure "
                      << static_cast<unsigned>(status.last_failure) << ")\n";
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    std::cout << "media-server stopping\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::uint32_t roles = bits(Role::Output);
    std::filesystem::path wal_path{"media-server.wal"};
    bool wal_path_was_explicit = false;
    bool validate_startup = false;
    std::optional<asio::ip::tcp::endpoint> director_listen;
    std::optional<asio::ip::tcp::endpoint> director_connect;
    std::optional<mediaserver::net::Id128> show_id;
    std::optional<mediaserver::net::Id128> director_epoch;
    std::optional<mediaserver::net::Id128> node_id;
    std::optional<mediaserver::net::Id128> node_incarnation;
    mediaserver::app::PlaybackOptions playback{};
    // Durability is edit-shaped by default. Under restart-and-re-cue a control
    // frame ordinal is not something a show needs back after a failure, and
    // journalling every one of them caps the seal rate at the device's sync
    // rate. --durable-frames restores the frame-cadence journal for a run that
    // wants the complete audit trail and can accept that ceiling.
    auto frame_durability = mediaserver::persist::FrameDurability::EditsOnly;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--help") {
            std::cout
                << "usage: media-server "
                   "[--role=director,render,receiver,compositor,output] "
                   "[--wal=PATH] [--durable-frames] [--validate-startup] "
                   "[--director-listen=ADDRESS:PORT "
                   "--show-id=32_HEX --director-epoch=32_HEX] "
                   "[--director-connect=ADDRESS:PORT --show-id=32_HEX "
                   "--node-id=32_HEX --node-incarnation=32_HEX] "
                   "[--playback [--show-rate=NUM[/DEN]] "
                   "[--show-range=FIRST:LAST] [--show-loop] [--no-autoplay] "
                   "[--run-frames=N] [--status-interval=N] "
                   "[--node-index=N] [--surface=WIDTHxHEIGHT] "
                   "[--window [--gpu] [--window-at=X,Y] [--fullscreen] "
                   "[--borderless]]]\n"
                << "       --wal and --director-listen are owned only by the "
                   "Director role; --director-connect is node-only and "
                   "requires a numeric unicast address and nonzero port\n"
                << "       --playback drives the show clock: on the Director "
                   "it seals one control frame per show frame, and on a node "
                   "it paces its own frame boundary and presents only "
                   "Director-approved frames\n"
                << "       --durable-frames journals and releases every sealed "
                   "frame, including empty ones. The seal, release and permit "
                   "path is identical either way; only the write cadence "
                   "changes\n"
                << "       --gpu composites into a GPU target and blits it to "
                   "the window, instead of uploading a CPU surface every "
                   "frame; it requires --window and reports per-frame "
                   "submission time\n";
            return 0;
        }
        if (argument == "--validate-startup") {
            validate_startup = true;
            continue;
        }
        if (argument == "--durable-frames") {
            frame_durability = mediaserver::persist::FrameDurability::EveryFrame;
            continue;
        }
        if (argument == "--playback") {
            playback.enabled = true;
            continue;
        }
        if (argument == "--show-loop") {
            playback.loop = true;
            continue;
        }
        if (argument == "--window") {
            playback.windowed = true;
            continue;
        }
        if (argument == "--gpu") {
            playback.gpu = true;
            continue;
        }
        if (argument == "--fullscreen") {
            playback.fullscreen = true;
            continue;
        }
        if (argument == "--borderless") {
            playback.borderless = true;
            continue;
        }
        constexpr std::string_view window_at_prefix = "--window-at=";
        if (argument.starts_with(window_at_prefix)) {
            if (!parse_window_position(
                    argument.substr(window_at_prefix.size()),
                    playback.window_x,
                    playback.window_y)) {
                std::cerr << "window position must be X,Y within 32767\n";
                return 2;
            }
            continue;
        }
        if (argument == "--no-autoplay") {
            playback.autoplay = false;
            continue;
        }
        constexpr std::string_view rate_prefix = "--show-rate=";
        if (argument.starts_with(rate_prefix)) {
            if (!mediaserver::app::parse_frame_rate(
                    argument.substr(rate_prefix.size()), playback.rate)) {
                std::cerr << "show rate must be NUM or NUM/DEN with nonzero, "
                             "bounded terms\n";
                return 2;
            }
            continue;
        }
        constexpr std::string_view range_prefix = "--show-range=";
        if (argument.starts_with(range_prefix)) {
            if (!mediaserver::app::parse_show_range(
                    argument.substr(range_prefix.size()),
                    playback.first_frame,
                    playback.last_frame)) {
                std::cerr << "show range must be FIRST:LAST with FIRST no "
                             "greater than LAST\n";
                return 2;
            }
            continue;
        }
        constexpr std::string_view run_prefix = "--run-frames=";
        if (argument.starts_with(run_prefix)) {
            if (!parse_u64_argument(
                    argument.substr(run_prefix.size()), playback.run_frames)) {
                std::cerr << "run frame count must be a decimal number\n";
                return 2;
            }
            continue;
        }
        constexpr std::string_view status_prefix = "--status-interval=";
        if (argument.starts_with(status_prefix)) {
            if (!parse_u64_argument(
                    argument.substr(status_prefix.size()),
                    playback.status_interval)) {
                std::cerr << "status interval must be a decimal number\n";
                return 2;
            }
            continue;
        }
        constexpr std::string_view node_index_prefix = "--node-index=";
        if (argument.starts_with(node_index_prefix)) {
            mediaserver::core::u64 parsed{};
            if (!parse_u64_argument(
                    argument.substr(node_index_prefix.size()), parsed)
                || parsed > 4'095U) {
                std::cerr << "node index must be a decimal number no greater "
                             "than 4095\n";
                return 2;
            }
            playback.node_index = static_cast<mediaserver::core::u32>(parsed);
            continue;
        }
        constexpr std::string_view surface_prefix = "--surface=";
        if (argument.starts_with(surface_prefix)) {
            if (!parse_surface(
                    argument.substr(surface_prefix.size()),
                    playback.surface_width,
                    playback.surface_height)) {
                std::cerr << "surface must be WIDTHxHEIGHT within "
                             "7680x4320\n";
                return 2;
            }
            continue;
        }
        constexpr std::string_view role_prefix = "--role=";
        if (argument.starts_with(role_prefix)) {
            roles = bits(Role::None);
            if (!parse_roles(argument.substr(role_prefix.size()), roles)) {
                std::cerr << "invalid role list: "
                          << argument.substr(role_prefix.size()) << '\n';
                return 2;
            }
            continue;
        }
        constexpr std::string_view wal_prefix = "--wal=";
        if (argument.starts_with(wal_prefix)) {
            const auto value = argument.substr(wal_prefix.size());
            if (value.empty()) {
                std::cerr << "WAL path cannot be empty\n";
                return 2;
            }
            wal_path = std::filesystem::path{std::string{value}};
            wal_path_was_explicit = true;
            continue;
        }
        constexpr std::string_view listen_prefix = "--director-listen=";
        if (argument.starts_with(listen_prefix)) {
            director_listen = parse_endpoint(
                argument.substr(listen_prefix.size()));
            if (!director_listen) {
                std::cerr << "invalid numeric Director endpoint: "
                          << argument.substr(listen_prefix.size()) << '\n';
                return 2;
            }
            continue;
        }
        constexpr std::string_view connect_prefix = "--director-connect=";
        if (argument.starts_with(connect_prefix)) {
            director_connect = parse_endpoint(
                argument.substr(connect_prefix.size()));
            if (!director_connect
                || !valid_node_connect_endpoint(*director_connect)) {
                std::cerr << "Director connect endpoint must be a numeric "
                             "unicast address with a nonzero port: "
                          << argument.substr(connect_prefix.size()) << '\n';
                return 2;
            }
            continue;
        }
        constexpr std::string_view show_prefix = "--show-id=";
        if (argument.starts_with(show_prefix)) {
            mediaserver::net::Id128 parsed{};
            if (!parse_id128(argument.substr(show_prefix.size()), parsed)) {
                std::cerr << "show ID must be a nonzero 32-digit hexadecimal "
                             "value\n";
                return 2;
            }
            show_id = parsed;
            continue;
        }
        constexpr std::string_view epoch_prefix = "--director-epoch=";
        if (argument.starts_with(epoch_prefix)) {
            mediaserver::net::Id128 parsed{};
            if (!parse_id128(argument.substr(epoch_prefix.size()), parsed)) {
                std::cerr << "Director epoch must be a nonzero 32-digit "
                             "hexadecimal value\n";
                return 2;
            }
            director_epoch = parsed;
            continue;
        }
        constexpr std::string_view node_prefix = "--node-id=";
        if (argument.starts_with(node_prefix)) {
            mediaserver::net::Id128 parsed{};
            if (!parse_id128(argument.substr(node_prefix.size()), parsed)) {
                std::cerr << "node ID must be a nonzero 32-digit hexadecimal "
                             "value\n";
                return 2;
            }
            node_id = parsed;
            continue;
        }
        constexpr std::string_view incarnation_prefix = "--node-incarnation=";
        if (argument.starts_with(incarnation_prefix)) {
            mediaserver::net::Id128 parsed{};
            if (!parse_id128(
                    argument.substr(incarnation_prefix.size()), parsed)) {
                std::cerr << "node incarnation must be a nonzero 32-digit "
                             "hexadecimal value\n";
                return 2;
            }
            node_incarnation = parsed;
            continue;
        }

        std::cerr << "unknown argument: " << argument << '\n';
        return 2;
    }

    const auto is_director = has_role(roles, Role::Director);
    if (!is_director
        && frame_durability
            == mediaserver::persist::FrameDurability::EveryFrame) {
        std::cerr << "--durable-frames requires the Director role; node-only "
                     "startup does not open the Director WAL\n";
        return 2;
    }
    if (!is_director && wal_path_was_explicit) {
        std::cerr << "--wal requires the Director role; node-only startup does not "
                     "open the Director WAL\n";
        return 2;
    }
    if (!is_director && (director_listen || director_epoch)) {
        std::cerr << "Director listener identity options require the Director "
                     "role\n";
        return 2;
    }
    if (is_director && (director_connect || node_id || node_incarnation)) {
        std::cerr << "--director-connect, --node-id, and --node-incarnation "
                     "are node-only options and cannot be combined with the "
                     "Director role\n";
        return 2;
    }
    if (is_director
        && ((!director_listen && (show_id || director_epoch))
            || (director_listen && (!show_id || !director_epoch)))) {
        std::cerr << "--director-listen requires both --show-id and "
                     "--director-epoch, and listener identities are unused "
                     "without --director-listen\n";
        return 2;
    }
    const auto any_node_session_option =
        static_cast<bool>(director_connect) || static_cast<bool>(show_id)
        || static_cast<bool>(node_id) || static_cast<bool>(node_incarnation);
    const auto complete_node_session_options =
        static_cast<bool>(director_connect) && static_cast<bool>(show_id)
        && static_cast<bool>(node_id) && static_cast<bool>(node_incarnation);
    if (!is_director && any_node_session_option
        && !complete_node_session_options) {
        std::cerr << "node session configuration requires all of "
                     "--director-connect, --show-id, --node-id, and "
                     "--node-incarnation; identities are unused without "
                     "--director-connect\n";
        return 2;
    }

    if (playback.enabled && !mediaserver::app::valid_playback_options(playback)) {
        std::cerr << "invalid playback configuration: check --show-rate, "
                     "--show-range, and --surface\n";
        return 2;
    }
    if (playback.enabled && !is_director && !complete_node_session_options) {
        std::cerr << "node playback requires a Director connection: "
                     "--director-connect, --show-id, --node-id, and "
                     "--node-incarnation\n";
        return 2;
    }
    if (playback.gpu && !playback.windowed) {
        std::cerr << "--gpu requires --window; there is nothing to present to "
                     "otherwise\n";
        return 2;
    }
    if (playback.gpu && !mediaserver::app::gpu_output_available()) {
        std::cerr << "this build has no GPU output path: it needs the SDL3 "
                     "shell, shader bytecode from a dxc on the build machine, "
                     "and a GPU this machine can open\n";
        return 2;
    }
    if (playback.enabled && is_director && !director_listen) {
        std::cerr << "Director playback requires --director-listen so that "
                     "sealed frames are released through the commit barrier\n";
        return 2;
    }
    if (!playback.enabled
        && (playback.loop || !playback.autoplay || playback.run_frames != 0U
            || playback.status_interval != 0U || playback.windowed
            || playback.fullscreen || playback.borderless
            || playback.window_x >= 0)) {
        std::cerr << "show transport and output options require --playback\n";
        return 2;
    }
    if (playback.windowed && !mediaserver::app::window_output_available()) {
        std::cerr << "this build has no window output; configure with "
                     "MEDIASERVER_ENABLE_GUI=ON and the vcpkg gui feature\n";
        return 2;
    }
    if (!playback.windowed
        && (playback.fullscreen || playback.borderless
            || playback.window_x >= 0)) {
        std::cerr << "window placement options require --window\n";
        return 2;
    }

    if (!is_director) {
        mediaserver::cmd::NodeCommandBus command_bus;
        std::optional<mediaserver::net::NodeSessionService> node_session;
        if (complete_node_session_options) {
            mediaserver::net::NodeSessionConfig session_defaults;
            session_defaults.show_id = *show_id;
            session_defaults.node_id = *node_id;
            session_defaults.incarnation = *node_incarnation;
            mediaserver::net::NodeSessionServiceConfig service_config{
                *director_connect,
                std::move(session_defaults),
            };
            if (!mediaserver::net::NodeSessionService::valid_configuration(
                    service_config)) {
                std::cerr << "invalid node session service configuration\n";
                return 2;
            }
            try {
                node_session.emplace(
                    std::move(service_config),
                    command_bus);
            } catch (const std::bad_alloc&) {
                std::cerr << "could not initialize node session supervisor: "
                             "allocation failed\n";
                return 1;
            }
        }
        if (!validate_startup && !install_service_stop_handlers()) {
            return 1;
        }
        if (node_session && !validate_startup) {
            auto started = node_session->start();
            if (!started) {
                std::cerr << "could not start node session supervisor (code "
                          << static_cast<unsigned>(started.error().code)
                          << ")\n";
                return 1;
            }
        }
        std::cout << "media-server 0.1.0 "
                  << (validate_startup ? "validated" : "ready")
                  << "; role-mask=" << roles
                  << "; document-version="
                  << command_bus.document().version().value()
                  << "; committed-through="
                  << command_bus.committed_through().value()
                  << "; director-wal=disabled"
                  << "; node-session="
                  << (!node_session
                          ? "disabled"
                          : (validate_startup ? "configured" : "supervising"));
        if (node_session) {
            std::cout << "; director-connect="
                      << endpoint_text(*director_connect)
                      << "; boundary-driver="
                      << (playback.enabled ? "show-clock" : "unwired");
        }
        if (playback.enabled) {
            std::cout << "; show-rate=" << playback.rate.numerator << '/'
                      << playback.rate.denominator
                      << "; show-range=" << playback.first_frame.value() << ':'
                      << playback.last_frame.value()
                      << "; node-index=" << playback.node_index
                      << "; output="
                      << (playback.windowed
                              ? (playback.gpu ? "window-gpu" : "window-cpu")
                              : "headless");
        }
        std::cout << '\n'
                  << std::flush;
        const auto result = validate_startup ? 0
            : playback.enabled && node_session
            ? mediaserver::app::run_node_playback(
                  *node_session, command_bus, playback, stop_requested)
            : node_session ? run_node_service_loop(*node_session)
                           : run_service_loop();
        if (node_session) {
            node_session->stop();
        }
        return result;
    }

    const mediaserver::cmd::StreamCursor genesis{
        mediaserver::cmd::FrameNumber{0U},
        mediaserver::doc::Version{0U},
        mediaserver::cmd::CommandId{1U},
    };
    auto opened = mediaserver::persist::CommandLog::open_or_create(
        wal_path,
        genesis,
        mediaserver::persist::OpenOptions{
            mediaserver::persist::SyncPolicy::Full,
            true,
            {},
        });
    if (!opened) {
        report_command_log_error("could not open Director WAL", opened.error());
        return 1;
    }
    auto command_log = std::move(opened).value();

    auto wal_snapshot = command_log.capture_committed();
    if (!wal_snapshot) {
        report_command_log_error(
            "could not capture Director WAL for release validation",
            wal_snapshot.error());
        return 1;
    }
    auto release_path = wal_path;
    release_path += ".release";
    auto opened_releases = mediaserver::persist::ReleaseLedger::open_or_create(
        release_path,
        wal_snapshot.value(),
        mediaserver::persist::ReleaseLedgerOpenOptions{
            mediaserver::persist::SyncPolicy::Full,
            true,
        });
    if (!opened_releases) {
        report_release_ledger_error(
            "could not open Director release ledger",
            opened_releases.error());
        return 1;
    }
    auto release_ledger = std::move(opened_releases).value();
    auto release_inventory =
        mediaserver::persist::UnresolvedReleaseInventory::inspect(
            wal_snapshot.value(), release_ledger);
    if (!release_inventory) {
        std::cerr << "could not authenticate Director WAL/release inventory "
                     "(code "
                  << static_cast<unsigned>(release_inventory.error().code)
                  << ")\n";
        return 1;
    }
    if (release_inventory.value().released_frontier
            != release_inventory.value().wal_tail
        || release_inventory.value().unresolved_frame_count != 0U
        || release_inventory.value().unresolved_wal_bytes != 0U) {
        std::cerr
            << "Director WAL contains "
            << release_inventory.value().unresolved_frame_count
            << " frame(s) beyond the durable release frontier; startup is "
               "blocked until an explicit recovery/reconfiguration workflow "
               "is available\n";
        return 1;
    }

    auto recovered = mediaserver::persist::recover_genesis_runtime(command_log);
    if (!recovered) {
        std::cerr << "could not recover Director runtime (code "
                  << static_cast<unsigned>(recovered.error().code) << ")\n";
        if (recovered.error().command_log_error) {
            report_command_log_error(
                "Director WAL replay failed",
                *recovered.error().command_log_error);
        }
        return 1;
    }

    auto& runtime = recovered.value();
    mediaserver::persist::DurableDirector durable_director{
        runtime.director,
        command_log,
        release_ledger,
        frame_durability,
    };
    if (durable_director.recovery_required()) {
        std::cerr << "Director durable coordinator rejected the WAL/release "
                     "frontier pairing\n";
        return 1;
    }
    constexpr mediaserver::net::DirectorSessionHubLimits hub_limits{};
    if (!valid_hub_limits(hub_limits)) {
        std::cerr << "could not initialize Director session hub: invalid "
                     "capacity configuration\n";
        return 1;
    }

    // The hub is initialized before an optional listener and before readiness
    // is reported. It is non-movable, so optional gives it a stable address
    // and preserves destruction before its DurableDirector.
    std::optional<mediaserver::net::DirectorSessionHub> session_hub;
    try {
        session_hub.emplace(durable_director, hub_limits);
    } catch (const std::bad_alloc&) {
        std::cerr << "could not initialize Director session hub: allocation "
                     "failed\n";
        return 1;
    } catch (const std::length_error& error) {
        std::cerr << "could not initialize Director session hub: invalid "
                     "capacity: "
                  << error.what() << '\n';
        return 1;
    }

    constexpr std::size_t max_session_workers = 64U;
    std::optional<mediaserver::net::NodeSessionLeaseRegistry> session_leases;
    std::optional<
        mediaserver::net::DurableDirectorFrameReleaseAuthority>
        frame_release_authority;
    std::optional<mediaserver::net::DirectorFrameBarrier> frame_barrier;
    std::optional<mediaserver::net::DirectorSessionService> session_service;
    std::optional<asio::ip::tcp::endpoint> bound_listener;
    if (!validate_startup && !install_service_stop_handlers()) {
        return 1;
    }
    if (director_listen) {
        try {
            session_leases.emplace(max_session_workers);
            frame_release_authority.emplace(durable_director);
            frame_barrier.emplace(
                command_log.tail_cursor(),
                *frame_release_authority,
                mediaserver::net::DirectorFrameBarrierLimits{
                    max_session_workers,
                    hub_limits.max_backlog_frames,
                    hub_limits.max_backlog_frames,
                });

            mediaserver::net::DirectorSessionConfig session_defaults;
            session_defaults.show_id = *show_id;
            session_defaults.director_epoch = *director_epoch;
            session_service.emplace(
                mediaserver::net::DirectorSessionServiceConfig{
                    *director_listen,
                    max_session_workers,
                    session_namespace(*director_epoch),
                    1U,
                    256U * 1024U * 1024U,
                    std::move(session_defaults),
                },
                *session_hub,
                durable_director,
                *session_leases,
                &*frame_barrier);
        } catch (const std::bad_alloc&) {
            std::cerr << "could not initialize Director listener: allocation "
                         "failed\n";
            return 1;
        } catch (const std::length_error& error) {
            std::cerr << "could not initialize Director listener: invalid "
                         "capacity: "
                      << error.what() << '\n';
            return 1;
        }

        auto started = session_service->start();
        if (!started) {
            std::cerr << "could not start Director listener (code "
                      << static_cast<unsigned>(started.error().code) << ')';
            if (started.error().system_error) {
                std::cerr << ": " << started.error().system_error.message();
            }
            std::cerr << '\n';
            return 1;
        }
        bound_listener = started.value();
    }

    std::cout << "media-server 0.1.0 "
              << (validate_startup ? "validated" : "ready")
              << "; role-mask=" << roles
              << "; document-version="
              << runtime.local_node.document().version().value()
              << "; committed-through="
              << runtime.local_node.committed_through().value()
              << "; recovered-frames=" << runtime.replay.frames
              << "; undo-history=reset"
              << "; durable-state="
              << static_cast<unsigned>(durable_director.state())
              << "; director-session-hub=initialized"
              << "; director-listener="
              << (bound_listener ? endpoint_text(*bound_listener) : "disabled")
              << "; frame-durability="
              << (frame_durability
                          == mediaserver::persist::FrameDurability::EveryFrame
                      ? "every-frame"
                      : "edits-only")
              << "; commit-barrier="
              << (frame_barrier ? "durable-permit-gated" : "disabled")
              << "; release-frontier="
              << release_ledger.frontier().through_frame
              << "; show-clock="
              << (playback.enabled ? "director-transport" : "disabled")
              << "; output="
              << (!playback.enabled ? "disabled"
                      : playback.windowed ? "window"
                                          : "headless")
              << "; wal=" << command_log.path().string() << '\n'
              << std::flush;
    const auto result = validate_startup ? 0
        : playback.enabled && frame_barrier
        ? mediaserver::app::run_director_show(
              runtime.director, *session_hub, *frame_barrier, playback,
              stop_requested)
        : run_service_loop();
    if (session_service) {
        session_service->stop();
    }
    session_hub->close();
    const auto unjournalled = durable_director.unjournalled_edit_frames();
    if (unjournalled != 0U) {
        // Said plainly rather than left to be discovered: these frames were
        // replicated and released, and they are not in the durable artifact.
        std::cerr << "note: " << unjournalled
                  << " frame(s) carrying document edits were sealed without "
                     "being journalled; the durable log is still released "
                     "through frame "
                  << release_ledger.frontier().through_frame
                  << ". Run with --durable-frames for a complete audit trail\n"
                  << std::flush;
    }
    return result;
}
