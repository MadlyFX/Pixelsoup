#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include "lifecycle_agent.hpp"
#include "playback.hpp"
#include "render_machine.hpp"
#include "render_machine_system.hpp"
#if defined(MEDIASERVER_HAS_GUI)
#include "render_machine_window.hpp"
#endif
#include "sync_device_probe.hpp"
#if defined(MEDIASERVER_HAS_NVAPI_PRESENT_BARRIER)
#include "nvapi_present_barrier.hpp"
#include <mediaserver/rhi/device.hpp>
#endif

#include <mediaserver/cmd/node_command_bus.hpp>
#include <mediaserver/net/cluster_discovery.hpp>
#include <mediaserver/net/cluster_lifecycle.hpp>
#include <mediaserver/net/director_frame_barrier.hpp>
#include <mediaserver/net/director_session_hub.hpp>
#include <mediaserver/net/director_session_service.hpp>
#include <mediaserver/net/durable_director_frame_release_authority.hpp>
#include <mediaserver/net/node_session_service.hpp>
#include <mediaserver/persist/durable_director.hpp>
#include <mediaserver/persist/release_ledger.hpp>
#include <mediaserver/persist/settings.hpp>

#include <asio/ip/address.hpp>

#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

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

extern "C" void request_stop(int) { stop_requested = 1; }
[[nodiscard]] constexpr std::uint32_t bits(Role role) noexcept {
    return static_cast<std::uint32_t>(role);
}

[[nodiscard]] constexpr bool has_role(std::uint32_t roles, Role role) noexcept {
    return (roles & bits(role)) != 0U;
}

[[nodiscard]] constexpr bool valid_hub_limits(
    const mediaserver::net::DirectorSessionHubLimits &limits) noexcept {
    return limits.max_subscriptions != 0U && limits.max_backlog_frames != 0U &&
           limits.max_backlog_bytes != 0U;
}

[[nodiscard]] std::string path_argument(const std::filesystem::path &path) {
    const std::u8string encoded = path.u8string();
    return std::string{reinterpret_cast<const char *>(encoded.data()),
                       encoded.size()};
}

[[nodiscard]] std::filesystem::path path_from_utf8(std::string_view value) {
    std::u8string encoded;
    encoded.resize(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        encoded[index] =
            static_cast<char8_t>(static_cast<unsigned char>(value[index]));
    }
    return std::filesystem::path{std::move(encoded)};
}

#if defined(_WIN32)
[[nodiscard]] std::optional<std::string>
utf8_argument(const wchar_t *argument) {
    if (argument == nullptr) {
        return std::nullopt;
    }
    const std::size_t length = std::char_traits<wchar_t>::length(argument);
    if (length > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    if (length == 0U) {
        return std::string{};
    }
    const int source_length = static_cast<int>(length);
    const int required =
        ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, argument,
                              source_length, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return std::nullopt;
    }
    std::string encoded(static_cast<std::size_t>(required), '\0');
    const int written = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, argument, source_length, encoded.data(),
        required, nullptr, nullptr);
    if (written != required) {
        return std::nullopt;
    }
    return encoded;
}
#endif

[[nodiscard]] bool add_role(std::string_view name, std::uint32_t &roles) {
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

[[nodiscard]] bool parse_roles(std::string_view value, std::uint32_t &roles) {
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

[[nodiscard]] bool parse_hex_u64(std::string_view value,
                                 mediaserver::core::u64 &parsed) noexcept {
    if (value.size() != 16U) {
        return false;
    }
    const auto *const begin = value.data();
    const auto *const end = begin + value.size();
    const auto result = std::from_chars(begin, end, parsed, 16);
    return result.ec == std::errc{} && result.ptr == end;
}

[[nodiscard]] bool parse_u64_argument(std::string_view value,
                                      mediaserver::core::u64 &parsed) noexcept {
    if (value.empty()) {
        return false;
    }
    const auto *const begin = value.data();
    const auto *const end = begin + value.size();
    const auto result = std::from_chars(begin, end, parsed, 10);
    return result.ec == std::errc{} && result.ptr == end;
}

[[nodiscard]] bool parse_i32_argument(std::string_view value,
                                      mediaserver::core::i32 &parsed) noexcept {
    if (value.empty()) {
        return false;
    }
    const auto *const begin = value.data();
    const auto *const end = begin + value.size();
    const auto result = std::from_chars(begin, end, parsed, 10);
    return result.ec == std::errc{} && result.ptr == end;
}

[[nodiscard]] bool parse_window_position(std::string_view value,
                                         mediaserver::core::i32 &x,
                                         mediaserver::core::i32 &y) noexcept {
    const auto separator = value.find(',');
    if (separator == std::string_view::npos) {
        return false;
    }
    mediaserver::core::i32 parsed_x{};
    mediaserver::core::i32 parsed_y{};
    if (!parse_i32_argument(value.substr(0U, separator), parsed_x) ||
        !parse_i32_argument(value.substr(separator + 1U), parsed_y)) {
        return false;
    }
    x = parsed_x;
    y = parsed_y;
    return true;
}

[[nodiscard]] bool parse_surface(std::string_view value,
                                 mediaserver::core::u32 &width,
                                 mediaserver::core::u32 &height) noexcept {
    const auto separator = value.find('x');
    if (separator == std::string_view::npos) {
        return false;
    }
    mediaserver::core::u64 parsed_width{};
    mediaserver::core::u64 parsed_height{};
    if (!parse_u64_argument(value.substr(0U, separator), parsed_width) ||
        !parse_u64_argument(value.substr(separator + 1U), parsed_height) ||
        parsed_width == 0U || parsed_height == 0U || parsed_width > 7'680U ||
        parsed_height > 4'320U) {
        return false;
    }
    width = static_cast<mediaserver::core::u32>(parsed_width);
    height = static_cast<mediaserver::core::u32>(parsed_height);
    return true;
}

[[nodiscard]] bool parse_id128(std::string_view value,
                               mediaserver::net::Id128 &parsed) noexcept {
    mediaserver::core::u64 high{};
    mediaserver::core::u64 low{};
    if (value.size() != 32U || !parse_hex_u64(value.substr(0U, 16U), high) ||
        !parse_hex_u64(value.substr(16U), low)) {
        return false;
    }
    parsed = mediaserver::net::Id128{high, low};
    return static_cast<bool>(parsed);
}

[[nodiscard]] std::string format_id128(mediaserver::net::Id128 value) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string text(32U, '0');
    for (std::size_t index = 0U; index < 16U; ++index) {
        text[15U - index] = digits[static_cast<std::size_t>(
            (value.high >> (index * 4U)) & 0xfU)];
        text[31U - index] = digits[static_cast<std::size_t>(
            (value.low >> (index * 4U)) & 0xfU)];
    }
    return text;
}

[[nodiscard]] constexpr mediaserver::core::u64
session_namespace(mediaserver::net::Id128 epoch) noexcept {
    constexpr mediaserver::core::u64 golden_ratio = 0x9e3779b97f4a7c15ULL;
    const auto mixed = epoch.high ^ (epoch.low + golden_ratio +
                                     (epoch.high << 6U) + (epoch.high >> 2U));
    return mixed != 0U ? mixed : golden_ratio;
}

[[nodiscard]] std::optional<asio::ip::tcp::endpoint>
parse_endpoint(std::string_view value) {
    std::string_view address_text;
    std::string_view port_text;
    if (value.starts_with('[')) {
        const auto closing = value.find(']');
        if (closing == std::string_view::npos || closing + 1U >= value.size() ||
            value[closing + 1U] != ':') {
            return std::nullopt;
        }
        address_text = value.substr(1U, closing - 1U);
        port_text = value.substr(closing + 2U);
    } else {
        const auto separator = value.rfind(':');
        if (separator == std::string_view::npos ||
            value.find(':') != separator) {
            return std::nullopt;
        }
        address_text = value.substr(0U, separator);
        port_text = value.substr(separator + 1U);
    }
    if (address_text.empty() || port_text.empty()) {
        return std::nullopt;
    }

    unsigned int port{};
    const auto *const port_begin = port_text.data();
    const auto *const port_end = port_begin + port_text.size();
    const auto parsed_port = std::from_chars(port_begin, port_end, port, 10);
    if (parsed_port.ec != std::errc{} || parsed_port.ptr != port_end ||
        port > 65'535U) {
        return std::nullopt;
    }

    std::error_code error;
    const auto address =
        asio::ip::make_address(std::string{address_text}, error);
    if (error) {
        return std::nullopt;
    }
    return asio::ip::tcp::endpoint{
        address,
        static_cast<unsigned short>(port),
    };
}

[[nodiscard]] std::string
endpoint_text(const asio::ip::tcp::endpoint &endpoint) {
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

[[nodiscard]] constexpr std::string_view cluster_process_state_text(
    mediaserver::net::ClusterProcessState state) noexcept {
    using mediaserver::net::ClusterProcessState;
    switch (state) {
    case ClusterProcessState::Stopped:
        return "stopped";
    case ClusterProcessState::Starting:
        return "starting";
    case ClusterProcessState::Running:
        return "running";
    case ClusterProcessState::Stopping:
        return "stopping";
    case ClusterProcessState::Faulted:
        return "faulted";
    }
    return "faulted";
}

[[nodiscard]] bool
valid_node_connect_endpoint(const asio::ip::tcp::endpoint &endpoint) noexcept {
    const auto &address = endpoint.address();
    if (endpoint.port() == 0U || address.is_unspecified() ||
        address.is_multicast()) {
        return false;
    }
    return !address.is_v4() ||
           address.to_v4() != asio::ip::address_v4::broadcast();
}

// A lifecycle password kept in a file: the same password a person would type,
// trailing line breaks removed. An empty file is no password.
[[nodiscard]] std::optional<std::string>
read_lifecycle_password_file(const std::filesystem::path &path) {
    // Anything but a readable file is refused rather than read as "no
    // password": a mistyped path must not silently leave a node unsecured.
    std::error_code file_error;
    if (!std::filesystem::is_regular_file(path, file_error) || file_error) {
        return std::nullopt;
    }
    const auto size = std::filesystem::file_size(path, file_error);
    if (file_error || size > 258U) {
        return std::nullopt;
    }
    if (size == 0U) {
        return std::string{};
    }
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return std::nullopt;
    }
    std::string secret(static_cast<std::size_t>(size), '\0');
    input.read(secret.data(), static_cast<std::streamsize>(secret.size()));
    if (!input ||
        input.gcount() != static_cast<std::streamsize>(secret.size())) {
        return std::nullopt;
    }
    while (!secret.empty() &&
           (secret.back() == '\n' || secret.back() == '\r')) {
        secret.pop_back();
    }
    if (secret.size() > 256U || secret.find('\0') != std::string::npos) {
        return std::nullopt;
    }
    return secret;
}

void report_command_log_error(
    std::string_view operation,
    const mediaserver::persist::CommandLogError &error) {
    std::cerr << operation << " (category " << error.index() << ')';
    if (const auto *failure =
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
    const mediaserver::persist::ReleaseLedgerError &error) {
    std::cerr << operation << " (code " << static_cast<unsigned>(error.code)
              << "): " << error.detail;
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
    if (std::signal(SIGINT, request_stop) == SIG_ERR ||
        std::signal(SIGTERM, request_stop) == SIG_ERR) {
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

[[nodiscard]] int
run_node_service_loop(const mediaserver::net::NodeSessionService &service) {
    while (stop_requested == 0) {
        const auto status = service.snapshot();
        if (status.state ==
            mediaserver::net::NodeSessionServiceState::Faulted) {
            std::cerr << "node session supervisor faulted (failure "
                      << static_cast<unsigned>(status.last_failure) << ")\n";
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }
    std::cout << "media-server stopping\n";
    return 0;
}

// A render machine run from its stored settings: the setup form on the first
// launch, the lifecycle agent from then on. --validate-startup reports what a
// launch would start without opening a window, a port, or a process.
[[nodiscard]] int run_render_machine(bool validate_startup) {
    mediaserver::app::RenderMachineWindowOptions options{};
    options.executable = mediaserver::app::current_executable();
    if (options.executable.empty()) {
        std::cerr << "could not find this program's own path, which the "
                     "render machine runs as its render process\n";
        return 1;
    }
    if (auto preferences_file =
            mediaserver::persist::default_preferences_file()) {
        options.preferences_file = preferences_file.value();
        auto stored =
            mediaserver::persist::load_preferences(options.preferences_file);
        if (stored) {
            options.stored = std::move(stored).value();
        } else {
            options.load_message =
                "This machine's saved settings could not be read (code " +
                std::to_string(static_cast<unsigned>(stored.error().code)) +
                "). Set it up again; saving replaces them.";
        }
    }

    if (validate_startup) {
        if (!options.load_message.empty()) {
            std::cerr << "render machine settings could not be read from "
                      << path_argument(options.preferences_file) << '\n';
            return 2;
        }
        auto config = mediaserver::app::render_machine_config(options.stored);
        if (!config) {
            if (config.error().issue ==
                mediaserver::app::RenderMachineConfigIssue::NotSetUp) {
                std::cout << "media-server render machine validated; "
                             "not set up - a launch opens the setup window\n";
                return 0;
            }
            std::cerr << "render machine settings cannot be used: "
                      << config.error().message << '\n';
            return 2;
        }
        auto agent_config = mediaserver::app::render_machine_agent_config(
            config.value(), options.executable);
        if (!mediaserver::app::LifecycleAgent::valid_configuration(
                agent_config)) {
            std::cerr << "invalid render machine agent configuration\n";
            return 2;
        }
        try {
            const mediaserver::app::LifecycleAgent agent{
                std::move(agent_config)};
            std::cout << "media-server render machine validated; node-index="
                      << config.value().node_index
                      << "; director=" << endpoint_text(config.value().director)
                      << "; control-port=" << config.value().control_port
                      << "; password="
                      << (config.value().password_set ? "set" : "none")
                      << "; child=" << path_argument(options.executable)
                      << '\n';
        } catch (const std::exception &error) {
            std::cerr << "could not initialize the render machine agent: "
                      << error.what() << '\n';
            return 1;
        }
        return 0;
    }

#if defined(MEDIASERVER_HAS_GUI)
    // Started from Explorer or a shortcut, the console window is noise and
    // closing it would stop the machine; from a terminal it is kept, and the
    // render process shares it as it does under the scripts.
    options.no_console_window = mediaserver::app::leave_unshared_console();
    const bool headless = options.no_console_window;
    const int result =
        mediaserver::app::run_render_machine_window(std::move(options));
    if (result != 0 && headless) {
        mediaserver::app::show_error_dialog(
            "Media Server",
            "The render machine window could not open: this machine needs a "
            "display and a GPU Media Server can use. A machine without one "
            "can still run the lifecycle agent from the command line (see "
            "media-server --help).");
    }
    return result;
#else
    std::cerr << "this build has no render machine window; run the lifecycle "
                 "agent with --lifecycle-listen (see --help) or "
                 "scripts/run-output-node.ps1\n";
    return 2;
#endif
}

[[nodiscard]] int run_utf8_main(int argc, char **argv) {
#if defined(MEDIASERVER_HAS_NVAPI_PRESENT_BARRIER)
    // This must happen before a presenter can claim a swapchain. The registry
    // owns a shared lease for the process lifetime; a no-driver/no-Quadro host
    // intentionally installs nothing and continues to report Unsupported.
    if (auto backend = mediaserver::app::create_nvapi_present_barrier_backend()) {
        mediaserver::rhi::install_present_barrier_backend(std::move(backend));
    }
#endif
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
    std::optional<asio::ip::tcp::endpoint> lifecycle_listen;
    std::optional<mediaserver::core::u32> lifecycle_node_index;
    std::filesystem::path lifecycle_secret_file;
    std::optional<std::string> lifecycle_password;
    std::optional<std::uint16_t> lifecycle_discovery_port;
    std::filesystem::path lifecycle_child;
    std::vector<std::string> lifecycle_child_arguments;
    bool lifecycle_autostart = false;
    mediaserver::app::SyncSimulationMode lifecycle_sync_simulation =
        mediaserver::app::SyncSimulationMode::Disabled;
    bool normal_service_option_was_explicit = false;
    bool render_machine = false;
    int render_machine_other_options = 0;
    mediaserver::app::PlaybackOptions playback{};
    // Durability is edit-shaped by default. Under restart-and-re-cue a control
    // frame ordinal is not something a show needs back after a failure, and
    // journalling every one of them caps the seal rate at the device's sync
    // rate. --durable-frames restores the frame-cadence journal for a run that
    // wants the complete audit trail and can accept that ceiling.
    auto frame_durability = mediaserver::persist::FrameDurability::EditsOnly;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        const bool lifecycle_agent_option =
            argument == "--lifecycle-autostart" ||
            argument.starts_with("--lifecycle-listen=") ||
            argument.starts_with("--lifecycle-node-index=") ||
            argument.starts_with("--lifecycle-secret-file=") ||
            argument.starts_with("--lifecycle-password=") ||
            argument.starts_with("--lifecycle-discovery-port=") ||
            argument.starts_with("--lifecycle-child=") ||
            argument.starts_with("--lifecycle-child-arg=") ||
            argument.starts_with("--sync-simulate=");
        if (argument != "--help" && argument != "--validate-startup" &&
            !lifecycle_agent_option) {
            normal_service_option_was_explicit = true;
        }
        if (argument != "--render-machine" &&
            argument != "--validate-startup") {
            ++render_machine_other_options;
        }
        if (argument == "--help") {
            std::cout
                << "usage: media-server [--render-machine] "
                   "[--role=director,render,receiver,compositor,output] "
                   "[--wal=PATH] [--durable-frames] [--validate-startup] "
                   "[--director-listen=ADDRESS:PORT "
                   "--show-id=32_HEX --director-epoch=32_HEX] "
                   "[--director-connect=ADDRESS:PORT --show-id=32_HEX "
                   "--node-id=32_HEX --node-incarnation=32_HEX] "
                   "[--lifecycle-listen=ADDRESS:PORT "
                   "--lifecycle-node-index=N "
                   "[--lifecycle-password=TEXT | "
                   "--lifecycle-secret-file=PATH] "
                   "[--lifecycle-discovery-port=N] "
                   "--lifecycle-child=EXE "
                   "[--lifecycle-child-arg=ARG]... "
                   "[--lifecycle-autostart] [--sync-simulate=MODE]] "
                   "[--playback [--show-rate=NUM[/DEN]] "
                   "[--show-range=FIRST:LAST] [--show-loop] [--no-autoplay] "
                   "[--run-frames=N] [--status-interval=N] "
                   "[--node-index=N] [--surface=WIDTHxHEIGHT] "
                   "[--window [--gpu] [--scene] [--output] [--window-at=X,Y] "
                   "[--fullscreen] [--borderless]] [--edit]]\n"
                << "       --render-machine, or no arguments at all, runs this "
                   "machine as a render machine from its stored settings: a "
                   "window opens, asks for the control machine's address, "
                   "node index and render node password the first time, and "
                   "from then on starts the lifecycle agent and its render "
                   "process at once. With --validate-startup it reports what "
                   "would start and opens nothing\n"
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
                   "submission time\n"
                << "       --scene presents a replicated 3D scene rather than "
                   "the test pattern: the Director authors a built-in scene "
                   "into the document, and every node draws its --node-index "
                   "slice of the shared canvas from the committed document. It "
                   "implies a GPU window\n"
                << "       --output puts each node's slice through its feed "
                   "map "
                   "before presenting: the Director also authors a built-in "
                   "feed map, and every output window composites all mapped "
                   "viewports assigned to its feed index inside its fixed "
                   "raster. Pixels outside those viewports are black. It "
                   "implies "
                   "--scene, the source it transforms\n"
                << "       --edit opens the docked operator editor "
                   "(Director-only) instead of a preview window: Scene and "
                   "Library rails, Stage with Program monitor, selection "
                   "outlines, gizmos/guides, Properties, Timeline, compositor, "
                   "hardware/feed mapping, and output monitoring. Assets, "
                   "effects, keyframes, scenes, switcher state, and graph "
                   "edits "
                   "all author ordinary document commands. Requires "
                   "--playback. Output Monitor controls render machines "
                   "with the render node password set in Preferences; "
                   "--lifecycle-password=TEXT or --lifecycle-secret-file="
                   "PATH overrides it for this run\n"
                << "       --lifecycle-listen runs a standalone per-machine "
                   "render-process agent. Its executable and repeatable child "
                   "arguments are local immutable configuration; remote peers "
                   "can send only status/start/restart/stop. The agent appends "
                   "its --node-index automatically. A password "
                   "(--lifecycle-password, or the contents of "
                   "--lifecycle-secret-file) must match the editor's render "
                   "node password; with none, any Media Server on the network "
                   "can control the agent. "
                   "--lifecycle-autostart starts that fixed local child before "
                   "the agent accepts control requests; it is useful for "
                   "service startup and the bundled demos\n"
                << "       --lifecycle-discovery-port is the UDP port an "
                   "agent answers discovery on, so the editor's Hardware Map "
                   "can list it under DETECTED ON NETWORK, and the port the "
                   "editor asks on. Default 47839; 0 turns discovery off. "
                   "The answer carries the agent's name, node index, control "
                   "port, and whether it has a password - never the password "
                   "itself\n"
                << "       --sync-simulate makes the agent report fabricated "
                   "physical output timing (off, locked, reference-lost, "
                   "reference-idle, no-device) so REFERENCE LOST and SWAP "
                   "BARRIER INCOMPLETE procedures can be rehearsed without a "
                   "genlock card. Every simulated sample is marked as such all "
                   "the way to the operator surface and can satisfy only a "
                   "DEGRADED sync policy, never a STRICT one\n"
                << "       assets are referenced by absolute path and are "
                   "never copied. Every machine in a show must see the same "
                   "path for the same file; each node reports which of them it "
                   "cannot open\n";
            return 0;
        }
        if (argument == "--validate-startup") {
            validate_startup = true;
            continue;
        }
        if (argument == "--render-machine") {
            render_machine = true;
            continue;
        }
        if (argument == "--durable-frames") {
            frame_durability =
                mediaserver::persist::FrameDurability::EveryFrame;
            continue;
        }
        constexpr std::string_view lifecycle_listen_prefix =
            "--lifecycle-listen=";
        if (argument.starts_with(lifecycle_listen_prefix)) {
            lifecycle_listen =
                parse_endpoint(argument.substr(lifecycle_listen_prefix.size()));
            if (!lifecycle_listen || lifecycle_listen->port() == 0U ||
                lifecycle_listen->address().is_multicast() ||
                (lifecycle_listen->address().is_v4() &&
                 lifecycle_listen->address().to_v4() ==
                     asio::ip::address_v4::broadcast())) {
                std::cerr << "lifecycle listener must be a numeric, "
                             "non-multicast address with a nonzero port\n";
                return 2;
            }
            continue;
        }
        constexpr std::string_view lifecycle_index_prefix =
            "--lifecycle-node-index=";
        if (argument.starts_with(lifecycle_index_prefix)) {
            mediaserver::core::u64 parsed{};
            if (!parse_u64_argument(
                    argument.substr(lifecycle_index_prefix.size()), parsed) ||
                parsed > 4'095U) {
                std::cerr << "lifecycle node index must be a decimal number "
                             "no greater than 4095\n";
                return 2;
            }
            lifecycle_node_index = static_cast<mediaserver::core::u32>(parsed);
            continue;
        }
        constexpr std::string_view lifecycle_secret_prefix =
            "--lifecycle-secret-file=";
        if (argument.starts_with(lifecycle_secret_prefix)) {
            const auto value = argument.substr(lifecycle_secret_prefix.size());
            if (value.empty()) {
                std::cerr << "lifecycle secret file path cannot be empty\n";
                return 2;
            }
            lifecycle_secret_file = path_from_utf8(value);
            continue;
        }
        constexpr std::string_view lifecycle_password_prefix =
            "--lifecycle-password=";
        if (argument.starts_with(lifecycle_password_prefix)) {
            const auto value =
                argument.substr(lifecycle_password_prefix.size());
            if (value.size() > 256U) {
                std::cerr << "lifecycle password must be at most 256 bytes\n";
                return 2;
            }
            lifecycle_password = std::string{value};
            continue;
        }
        constexpr std::string_view lifecycle_discovery_port_prefix =
            "--lifecycle-discovery-port=";
        if (argument.starts_with(lifecycle_discovery_port_prefix)) {
            const auto value =
                argument.substr(lifecycle_discovery_port_prefix.size());
            unsigned parsed{};
            const auto [end, error] = std::from_chars(
                value.data(), value.data() + value.size(), parsed);
            if (value.empty() || error != std::errc{} ||
                end != value.data() + value.size() || parsed > 65'535U) {
                std::cerr << "lifecycle discovery port must be 0-65535\n";
                return 2;
            }
            lifecycle_discovery_port = static_cast<std::uint16_t>(parsed);
            continue;
        }
        constexpr std::string_view lifecycle_child_prefix =
            "--lifecycle-child=";
        if (argument.starts_with(lifecycle_child_prefix)) {
            const auto value = argument.substr(lifecycle_child_prefix.size());
            if (value.empty()) {
                std::cerr << "lifecycle child executable cannot be empty\n";
                return 2;
            }
            lifecycle_child = path_from_utf8(value);
            continue;
        }
        constexpr std::string_view lifecycle_child_argument_prefix =
            "--lifecycle-child-arg=";
        if (argument.starts_with(lifecycle_child_argument_prefix)) {
            lifecycle_child_arguments.emplace_back(
                argument.substr(lifecycle_child_argument_prefix.size()));
            continue;
        }
        constexpr std::string_view sync_simulate_prefix = "--sync-simulate=";
        if (argument.starts_with(sync_simulate_prefix)) {
            const auto parsed = mediaserver::app::parse_sync_simulation(
                argument.substr(sync_simulate_prefix.size()));
            if (!parsed) {
                std::cerr << "--sync-simulate must be one of off, locked, "
                             "reference-lost, reference-idle, no-device\n";
                return 2;
            }
            lifecycle_sync_simulation = *parsed;
            continue;
        }

        if (argument == "--lifecycle-autostart") {
            lifecycle_autostart = true;
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
        if (argument == "--scene") {
            // A shaded, depth-tested scene has no CPU path, so asking for one
            // asks for the GPU window too rather than being refused for want of
            // a flag the request already implies.
            playback.scene = true;
            playback.windowed = true;
            playback.gpu = true;
            continue;
        }
        if (argument == "--output") {
            // The output stage transforms the scene render, so asking for it
            // asks for the scene - and therefore the GPU window - too.
            playback.output = true;
            playback.scene = true;
            playback.windowed = true;
            playback.gpu = true;
            continue;
        }
        if (argument == "--edit") {
            // The editor owns its own window and device, so unlike --scene it
            // does not turn on the presenter's window/gpu flags.
            playback.edit = true;
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
            if (!parse_window_position(argument.substr(window_at_prefix.size()),
                                       playback.window_x, playback.window_y)) {
                std::cerr
                    << "window position must be signed 32-bit X,Y values\n";
                return 2;
            }
            playback.window_position_explicit = true;
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
                    argument.substr(range_prefix.size()), playback.first_frame,
                    playback.last_frame)) {
                std::cerr << "show range must be FIRST:LAST with FIRST no "
                             "greater than LAST\n";
                return 2;
            }
            continue;
        }
        constexpr std::string_view run_prefix = "--run-frames=";
        if (argument.starts_with(run_prefix)) {
            if (!parse_u64_argument(argument.substr(run_prefix.size()),
                                    playback.run_frames)) {
                std::cerr << "run frame count must be a decimal number\n";
                return 2;
            }
            continue;
        }
        constexpr std::string_view status_prefix = "--status-interval=";
        if (argument.starts_with(status_prefix)) {
            if (!parse_u64_argument(argument.substr(status_prefix.size()),
                                    playback.status_interval)) {
                std::cerr << "status interval must be a decimal number\n";
                return 2;
            }
            continue;
        }
        constexpr std::string_view node_index_prefix = "--node-index=";
        if (argument.starts_with(node_index_prefix)) {
            mediaserver::core::u64 parsed{};
            if (!parse_u64_argument(argument.substr(node_index_prefix.size()),
                                    parsed) ||
                parsed > 4'095U) {
                std::cerr << "node index must be a decimal number no greater "
                             "than 4095\n";
                return 2;
            }
            playback.node_index = static_cast<mediaserver::core::u32>(parsed);
            continue;
        }
        constexpr std::string_view surface_prefix = "--surface=";
        if (argument.starts_with(surface_prefix)) {
            if (!parse_surface(argument.substr(surface_prefix.size()),
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
            wal_path = path_from_utf8(value);
            wal_path_was_explicit = true;
            continue;
        }
        constexpr std::string_view listen_prefix = "--director-listen=";
        if (argument.starts_with(listen_prefix)) {
            director_listen =
                parse_endpoint(argument.substr(listen_prefix.size()));
            if (!director_listen) {
                std::cerr << "invalid numeric Director endpoint: "
                          << argument.substr(listen_prefix.size()) << '\n';
                return 2;
            }
            continue;
        }
        constexpr std::string_view connect_prefix = "--director-connect=";
        if (argument.starts_with(connect_prefix)) {
            director_connect =
                parse_endpoint(argument.substr(connect_prefix.size()));
            if (!director_connect ||
                !valid_node_connect_endpoint(*director_connect)) {
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
            if (!parse_id128(argument.substr(incarnation_prefix.size()),
                             parsed)) {
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

    // Nothing on the command line is a double-click or a shortcut: a person
    // at a render machine, not a script, so it gets the render-machine window
    // rather than a process idling in a console.
    if (render_machine || argc <= 1) {
        if (render_machine_other_options != 0) {
            std::cerr << "--render-machine runs from stored settings and "
                         "takes no other options except --validate-startup; "
                         "use --lifecycle-listen for a scripted agent\n";
            return 2;
        }
        return run_render_machine(validate_startup);
    }

    playback.associate_recent_project_after_recovery = !wal_path_was_explicit;

    // Preferences fill in only what the command line did not say. An explicit
    // argument always wins, so a demo or deployment script stays exactly
    // reproducible on a machine whose operator has saved settings.
    playback.director_listen_from_command_line = director_listen.has_value();
    playback.show_id_from_command_line = show_id.has_value();
    playback.director_epoch_from_command_line = director_epoch.has_value();
    playback.lifecycle_password_from_command_line =
        lifecycle_password.has_value() || !lifecycle_secret_file.empty();
    playback.wal_path_from_command_line = wal_path_was_explicit;
    if (auto preferences_file =
            mediaserver::persist::default_preferences_file()) {
        playback.preferences_file = preferences_file.value();
        auto stored =
            mediaserver::persist::load_preferences(playback.preferences_file);
        if (stored) {
            playback.stored_preferences = std::move(stored).value();
        } else {
            // Saying so beats running on defaults the operator did not pick.
            std::cerr << "stored preferences could not be read (code "
                      << static_cast<unsigned>(stored.error().code)
                      << "); using built-in defaults\n";
        }
    }
    // The Director's listener identity is the Director's alone. A render
    // process started on the operator's own machine must not inherit it, or
    // it would be refused as a node carrying Director options.
    if (has_role(roles, Role::Director)) {
        const auto &stored = playback.stored_preferences;
        if (!director_listen && !stored.director_listen.empty()) {
            director_listen = parse_endpoint(stored.director_listen);
            if (!director_listen) {
                std::cerr << "stored Director endpoint is not a numeric "
                             "ADDRESS:PORT: "
                          << stored.director_listen << '\n';
                return 2;
            }
        }
        if (!show_id && !stored.show_id.empty()) {
            mediaserver::net::Id128 parsed{};
            if (!parse_id128(stored.show_id, parsed)) {
                std::cerr << "stored show id is not 32 hexadecimal "
                             "characters: "
                          << stored.show_id << '\n';
                return 2;
            }
            show_id = parsed;
        }
        if (!director_epoch && !stored.director_epoch.empty()) {
            mediaserver::net::Id128 parsed{};
            if (!parse_id128(stored.director_epoch, parsed)) {
                std::cerr << "stored Director epoch is not 32 hexadecimal "
                             "characters: "
                          << stored.director_epoch << '\n';
                return 2;
            }
            director_epoch = parsed;
        }
    }
    {
        const auto &stored = playback.stored_preferences;
        if (!wal_path_was_explicit && !stored.wal_path.empty()) {
            wal_path = path_from_utf8(stored.wal_path);
            // A stored WAL is as deliberate a choice as an argument, so it
            // does not adopt the recent project as a saved baseline either.
            playback.associate_recent_project_after_recovery = false;
        }
    }

    if (lifecycle_listen) {
        if (normal_service_option_was_explicit) {
            std::cerr << "--lifecycle-listen is a standalone agent mode and "
                         "cannot be combined with Director, node, role, WAL, "
                         "or playback options\n";
            return 2;
        }
        if (!lifecycle_node_index || lifecycle_child.empty()) {
            std::cerr << "--lifecycle-listen requires "
                         "--lifecycle-node-index and --lifecycle-child\n";
            return 2;
        }
        for (const std::string &argument : lifecycle_child_arguments) {
            if (std::string_view{argument}.starts_with("--node-index=")) {
                std::cerr << "do not pass --node-index as a lifecycle child "
                             "argument; the agent appends its configured "
                             "identity automatically\n";
                return 2;
            }
        }
        // A password typed on the command line wins over one in a file; with
        // neither, the agent runs unsecured and says so.
        if (!lifecycle_password && !lifecycle_secret_file.empty()) {
            lifecycle_password =
                read_lifecycle_password_file(lifecycle_secret_file);
            if (!lifecycle_password) {
                std::cerr << "could not read a lifecycle password (at most "
                             "256 bytes, no NUL) from "
                          << path_argument(lifecycle_secret_file) << '\n';
                return 2;
            }
        }
        const std::string secret = mediaserver::net::lifecycle_key_from_password(
            lifecycle_password.value_or(std::string{}));
        std::error_code path_error;
        const auto child =
            std::filesystem::absolute(lifecycle_child, path_error);
        if (path_error ||
            !std::filesystem::is_regular_file(child, path_error) ||
            path_error) {
            std::cerr << "lifecycle child must name an existing executable "
                         "file\n";
            return 2;
        }
        mediaserver::app::LifecycleAgentConfig agent_config{};
        agent_config.listen = *lifecycle_listen;
        agent_config.node_index = *lifecycle_node_index;
        agent_config.secret = secret;
        agent_config.child = child;
        agent_config.child_arguments = std::move(lifecycle_child_arguments);
        agent_config.autostart = lifecycle_autostart;
        agent_config.sync_simulation = lifecycle_sync_simulation;
        agent_config.discovery_port = lifecycle_discovery_port.value_or(
            mediaserver::net::cluster_discovery_port);
        if (!mediaserver::app::LifecycleAgent::valid_configuration(
                agent_config)) {
            std::cerr << "invalid lifecycle agent configuration\n";
            return 2;
        }
        try {
            mediaserver::app::LifecycleAgent agent{std::move(agent_config)};
            if (validate_startup) {
                std::cout << "media-server lifecycle agent validated; "
                          << "node-index=" << *lifecycle_node_index
                          << "; child=" << path_argument(child) << '\n';
                return 0;
            }
            if (!install_service_stop_handlers()) {
                return 1;
            }
            auto started = agent.start();
            if (!started) {
                const auto &failure = started.error();
                if (failure.failure ==
                    mediaserver::app::LifecycleAgentFailure::Autostart) {
                    std::cerr << "lifecycle child autostart failed: "
                              << failure.message << '\n';
                    return 1;
                }
                std::cerr << "could not start lifecycle listener (code "
                          << static_cast<unsigned>(failure.listener.code)
                          << ')';
                if (failure.listener.system_error) {
                    std::cerr << ": "
                              << failure.listener.system_error.message();
                }
                std::cerr << '\n';
                return 1;
            }
            const auto child_status =
                agent.apply(mediaserver::net::ClusterLifecycleCommand::Status);
            std::cout << "media-server lifecycle agent ready; listen="
                      << endpoint_text(started.value())
                      << "; node-index=" << *lifecycle_node_index
                      << "; process="
                      << cluster_process_state_text(child_status.state)
                      << "; child=" << path_argument(child)
                      << "; sync-simulate="
                      << mediaserver::app::sync_simulation_name(
                             lifecycle_sync_simulation)
                      << "; password="
                      << (agent.unsecured()
                              ? "none (UNSECURED: any Media Server on "
                                "this network can control this node)"
                              : "set")
                      << "; discovery=" << agent.discovery_text() << '\n'
                      << std::flush;
            const int result = run_service_loop();
            agent.stop();
            return result;
        } catch (const std::bad_alloc &) {
            std::cerr << "could not initialize lifecycle agent: allocation "
                         "failed\n";
            return 1;
        } catch (const std::system_error &error) {
            std::cerr << "could not initialize lifecycle agent: "
                      << error.what() << '\n';
            return 1;
        }
    }

    if (lifecycle_node_index || !lifecycle_child.empty() ||
        lifecycle_autostart || !lifecycle_child_arguments.empty()) {
        std::cerr << "lifecycle node/child options require "
                     "--lifecycle-listen\n";
        return 2;
    }

    const auto is_director = has_role(roles, Role::Director);
    if (!is_director &&
        frame_durability == mediaserver::persist::FrameDurability::EveryFrame) {
        std::cerr << "--durable-frames requires the Director role; node-only "
                     "startup does not open the Director WAL\n";
        return 2;
    }
    if (!is_director && wal_path_was_explicit) {
        std::cerr
            << "--wal requires the Director role; node-only startup does not "
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
    if (is_director && ((!director_listen && (show_id || director_epoch)) ||
                        (director_listen && (!show_id || !director_epoch)))) {
        std::cerr << "--director-listen requires both --show-id and "
                     "--director-epoch, and listener identities are unused "
                     "without --director-listen\n";
        return 2;
    }
    const auto any_node_session_option =
        static_cast<bool>(director_connect) || static_cast<bool>(show_id) ||
        static_cast<bool>(node_id) || static_cast<bool>(node_incarnation);
    const auto complete_node_session_options =
        static_cast<bool>(director_connect) && static_cast<bool>(show_id) &&
        static_cast<bool>(node_id) && static_cast<bool>(node_incarnation);
    if (!is_director && any_node_session_option &&
        !complete_node_session_options) {
        std::cerr << "node session configuration requires all of "
                     "--director-connect, --show-id, --node-id, and "
                     "--node-incarnation; identities are unused without "
                     "--director-connect\n";
        return 2;
    }

    if (lifecycle_password.has_value() || !lifecycle_secret_file.empty()) {
        if (!is_director || !playback.edit) {
            std::cerr << "outside lifecycle agent mode, "
                         "--lifecycle-password and --lifecycle-secret-file "
                         "are used only by a Director operator editor\n";
            return 2;
        }
        if (!lifecycle_password) {
            lifecycle_password =
                read_lifecycle_password_file(lifecycle_secret_file);
            if (!lifecycle_password) {
                std::cerr << "could not read a lifecycle password (at most "
                             "256 bytes, no NUL) from "
                          << path_argument(lifecycle_secret_file) << '\n';
                return 2;
            }
        }
        playback.lifecycle_secret =
            mediaserver::net::lifecycle_key_from_password(*lifecycle_password);
    } else if (is_director && playback.edit) {
        // The render node password from Preferences, kept as its derived key;
        // none stored is the unsecured key, which still works with agents
        // started without a password.
        const std::string &stored =
            playback.stored_preferences.lifecycle_password_key;
        playback.lifecycle_secret =
            stored.size() == 64U &&
                    stored.find_first_not_of("0123456789abcdef") ==
                        std::string::npos
                ? stored
                : mediaserver::net::lifecycle_key_from_password({});
    }

    if (lifecycle_discovery_port.has_value()) {
        if (!is_director || !playback.edit) {
            std::cerr << "outside lifecycle agent mode, "
                         "--lifecycle-discovery-port is used only by a "
                         "Director operator editor\n";
            return 2;
        }
        playback.lifecycle_discovery_port = *lifecycle_discovery_port;
    }

    // What this run resolved to, whatever the source. The Preferences panel
    // shows it beside each stored value so an argument-driven launch reads as
    // deliberate rather than as settings being ignored.
    playback.active_preferences.director_listen =
        director_listen ? endpoint_text(*director_listen) : std::string{};
    playback.active_preferences.show_id =
        show_id ? format_id128(*show_id) : std::string{};
    playback.active_preferences.director_epoch =
        director_epoch ? format_id128(*director_epoch) : std::string{};
    playback.active_preferences.lifecycle_password_key =
        playback.lifecycle_secret;
    playback.active_preferences.wal_path = path_argument(wal_path);
    playback.active_preferences.proxy_cache_directory =
        playback.stored_preferences.proxy_cache_directory;
    playback.active_preferences.proxy_cache_next_to_source =
        playback.stored_preferences.proxy_cache_next_to_source;
    playback.active_preferences.proxy_cache_gibibytes =
        playback.stored_preferences.proxy_cache_gibibytes;
    playback.active_preferences.output_buffer_frames =
        playback.stored_preferences.output_buffer_frames;
    playback.active_preferences.output_read_ahead_frames =
        playback.stored_preferences.output_read_ahead_frames;
    playback.active_preferences.playback_cache_mebibytes =
        playback.stored_preferences.playback_cache_mebibytes;
    playback.active_preferences.playback_trail_frames =
        playback.stored_preferences.playback_trail_frames;
    playback.active_preferences.proxy_transcode_priority =
        playback.stored_preferences.proxy_transcode_priority;

    if (playback.enabled &&
        !mediaserver::app::valid_playback_options(playback)) {
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
    if (playback.scene && !mediaserver::app::scene_output_available()) {
        std::cerr << "this build has no 3D scene path: it needs the scene "
                     "shader bytecode and a GPU this machine can open\n";
        return 2;
    }
    if (playback.output && !mediaserver::app::output_output_available()) {
        std::cerr << "this build has no output stage: it needs the output "
                     "shader bytecode, the scene path it transforms, and a GPU "
                     "this machine can open\n";
        return 2;
    }
    if (playback.edit && !mediaserver::app::editor_available()) {
        std::cerr << "this build has no operator editor: it needs the editor "
                     "module (ImGui and the scene shaders) and a GPU this "
                     "machine can open\n";
        return 2;
    }
    if (playback.enabled && is_director && !director_listen) {
        std::cerr << "Director playback requires --director-listen so that "
                     "sealed frames are released through the commit barrier\n";
        return 2;
    }
    if (!playback.enabled &&
        (playback.loop || !playback.autoplay || playback.run_frames != 0U ||
         playback.status_interval != 0U || playback.windowed ||
         playback.fullscreen || playback.borderless || playback.edit ||
         playback.window_position_explicit)) {
        std::cerr << "show transport and output options require --playback\n";
        return 2;
    }
    if (playback.windowed && !mediaserver::app::window_output_available()) {
        std::cerr << "this build has no window output; configure with "
                     "MEDIASERVER_ENABLE_GUI=ON and the vcpkg gui feature\n";
        return 2;
    }
    // The editor owns its own window, so it honours --surface and --window-at
    // without --window; fullscreen/borderless remain presenter-only.
    if (!playback.windowed &&
        (playback.fullscreen || playback.borderless ||
         (playback.window_position_explicit && !playback.edit))) {
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
            session_defaults.node_index = playback.node_index;
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
                node_session.emplace(std::move(service_config), command_bus);
            } catch (const std::bad_alloc &) {
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
                  << "; role-mask=" << roles << "; document-version="
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
                      << "; node-index=" << playback.node_index << "; output="
                      << (playback.windowed
                              ? (playback.gpu ? "window-gpu" : "window-cpu")
                              : "headless");
            if (playback.window_position_explicit) {
                std::cout << "; window-at=" << playback.window_x << ','
                          << playback.window_y;
            }
        }
        std::cout << '\n' << std::flush;
        const auto result =
            validate_startup ? 0
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
        wal_path, genesis,
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
        release_path, wal_snapshot.value(),
        mediaserver::persist::ReleaseLedgerOpenOptions{
            mediaserver::persist::SyncPolicy::Full,
            true,
        });
    if (!opened_releases) {
        report_release_ledger_error("could not open Director release ledger",
                                    opened_releases.error());
        return 1;
    }
    auto release_ledger = std::move(opened_releases).value();
    const auto wal_tail = command_log.tail_cursor();
    const auto release_frontier = release_ledger.frontier();
    if (release_frontier.through_frame != wal_tail.through_frame.value() ||
        release_frontier.revision != wal_tail.revision.value() ||
        release_frontier.next_command != wal_tail.next_command.value() ||
        release_ledger.recovery_report().unresolved_wal_frames != 0U) {
        std::cerr << "Director WAL and release ledger do not share the same "
                     "committed frontier; startup is blocked until the project "
                     "files are repaired or removed\n";
        return 1;
    }

    auto recovered = mediaserver::persist::recover_genesis_runtime(command_log);
    if (!recovered) {
        std::cerr << "could not recover Director runtime (code "
                  << static_cast<unsigned>(recovered.error().code) << ")\n";
        if (recovered.error().command_log_error) {
            report_command_log_error("Director WAL replay failed",
                                     *recovered.error().command_log_error);
        }
        return 1;
    }

    auto &runtime = recovered.value();
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
    } catch (const std::bad_alloc &) {
        std::cerr << "could not initialize Director session hub: allocation "
                     "failed\n";
        return 1;
    } catch (const std::length_error &error) {
        std::cerr << "could not initialize Director session hub: invalid "
                     "capacity: "
                  << error.what() << '\n';
        return 1;
    }

    constexpr std::size_t max_session_workers = 64U;
    std::optional<mediaserver::net::NodeSessionLeaseRegistry> session_leases;
    std::optional<mediaserver::net::DurableDirectorFrameReleaseAuthority>
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
            frame_barrier.emplace(command_log.tail_cursor(),
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
                *session_hub, durable_director, *session_leases,
                &*frame_barrier);
        } catch (const std::bad_alloc &) {
            std::cerr << "could not initialize Director listener: allocation "
                         "failed\n";
            return 1;
        } catch (const std::length_error &error) {
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
              << "; role-mask=" << roles << "; document-version="
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
              << (frame_durability ==
                          mediaserver::persist::FrameDurability::EveryFrame
                      ? "every-frame"
                      : "edits-only")
              << "; commit-barrier="
              << (frame_barrier ? "durable-permit-gated" : "disabled")
              << "; release-frontier="
              << release_ledger.frontier().through_frame << "; show-clock="
              << (playback.enabled ? "director-transport" : "disabled")
              << "; output="
              << (!playback.enabled   ? "disabled"
                  : playback.windowed ? "window"
                                      : "headless")
              << "; wal=" << path_argument(command_log.path()) << '\n'
              << std::flush;
    const auto result =
        validate_startup ? 0
        : playback.enabled && frame_barrier
            ? mediaserver::app::run_director_show(
                  runtime.director, command_log, frame_durability, *session_hub,
                  *frame_barrier, session_service ? &*session_service : nullptr,
                  playback, stop_requested)
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

} // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t **argv) {
    if (argc < 0 || (argc != 0 && argv == nullptr)) {
        std::cerr << "could not read the Windows command line\n";
        return 2;
    }
    try {
        std::vector<std::string> arguments;
        arguments.reserve(static_cast<std::size_t>(argc));
        for (int index = 0; index < argc; ++index) {
            auto converted = utf8_argument(argv[index]);
            if (!converted) {
                std::cerr << "Windows command line argument " << index
                          << " is not valid UTF-16 and cannot be converted "
                             "to UTF-8\n";
                return 2;
            }
            arguments.push_back(std::move(*converted));
        }

        std::vector<char *> argument_pointers;
        argument_pointers.reserve(arguments.size() + 1U);
        for (std::string &argument : arguments) {
            argument_pointers.push_back(argument.data());
        }
        argument_pointers.push_back(nullptr);
        return run_utf8_main(argc, argument_pointers.data());
    } catch (const std::bad_alloc &) {
        std::cerr << "could not initialize media-server: allocation failed\n";
        return 1;
    } catch (const std::length_error &error) {
        std::cerr << "could not initialize media-server: invalid command line "
                     "capacity: "
                  << error.what() << '\n';
        return 1;
    } catch (const std::exception &error) {
        std::cerr << "could not initialize media-server: " << error.what()
                  << '\n';
        return 1;
    }
}
#else
int main(int argc, char **argv) {
    try {
        return run_utf8_main(argc, argv);
    } catch (const std::bad_alloc &) {
        std::cerr << "could not initialize media-server: allocation failed\n";
        return 1;
    } catch (const std::exception &error) {
        std::cerr << "could not initialize media-server: " << error.what()
                  << '\n';
        return 1;
    }
}
#endif
