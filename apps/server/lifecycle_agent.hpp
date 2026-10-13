#pragma once

#include "sync_device_probe.hpp"

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/net/cluster_discovery.hpp>
#include <mediaserver/net/cluster_lifecycle.hpp>

#include <asio/ip/tcp.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mediaserver::app {

// One machine's render-process agent: the supervised render process, the
// authenticated control listener Output Monitor talks to, and the discovery
// answer that lists the machine in the editor's Hardware Map. The
// --lifecycle-listen command line and the render-machine window both run
// exactly this, so a machine set up either way behaves the same.
struct LifecycleAgentConfig final {
    asio::ip::tcp::endpoint listen;
    core::u32 node_index{};
    // The lifecycle key, as net::lifecycle_key_from_password derives it.
    std::string secret;
    // An existing executable, as an absolute path.
    std::filesystem::path child;
    // The render process's own arguments. The agent appends --node-index.
    std::vector<std::string> child_arguments;
    // Start the render process before accepting control requests.
    bool autostart{};
    SyncSimulationMode sync_simulation{SyncSimulationMode::Disabled};
    // Zero turns discovery off.
    std::uint16_t discovery_port{net::cluster_discovery_port};
    // See ManagedRenderProcessConfig::no_console_window.
    bool no_console_window{};
};

enum class LifecycleAgentFailure : core::u8 {
    // The render process did not start (autostart only).
    Autostart,
    // The control listener could not be opened - usually its port is taken.
    Listener,
};

struct LifecycleAgentStartError final {
    LifecycleAgentFailure failure{LifecycleAgentFailure::Listener};
    // Autostart: the supervisor's reason.
    std::string message;
    // Listener: what the service reported.
    net::ClusterLifecycleServiceError listener{};
};

namespace detail {
struct LifecycleAgentState;
} // namespace detail

class LifecycleAgent final {
public:
    // Builds the supervisor, the machine probes and the control service
    // without starting any of them. Throws what they throw: std::bad_alloc
    // and std::system_error.
    explicit LifecycleAgent(LifecycleAgentConfig config);
    LifecycleAgent(const LifecycleAgent&) = delete;
    LifecycleAgent& operator=(const LifecycleAgent&) = delete;
    // Stops discovery, then the listener, then the render process.
    ~LifecycleAgent();

    [[nodiscard]] static bool valid_configuration(
        const LifecycleAgentConfig& config);

    // Starts the render process (when autostart is set), then the control
    // listener, then discovery. Returns the listener's bound endpoint.
    // Discovery is a convenience - a machine without it is entered by hand -
    // so a discovery port another program holds is reported by
    // discovery_text(), not as a failure.
    [[nodiscard]] core::Result<asio::ip::tcp::endpoint,
                               LifecycleAgentStartError>
    start();
    void stop() noexcept;

    // Local control, the same commands Output Monitor sends. Serialized with
    // the remote ones by the supervisor.
    [[nodiscard]] net::ClusterLifecycleReply apply(
        net::ClusterLifecycleCommand command) noexcept;

    [[nodiscard]] const LifecycleAgentConfig& config() const noexcept;
    // True when the key is the one the empty password derives.
    [[nodiscard]] bool unsecured() const noexcept;
    // "udp/47839", "off", or "off (why)".
    [[nodiscard]] std::string_view discovery_text() const noexcept;
    [[nodiscard]] net::ClusterLifecycleServiceCounters counters()
        const noexcept;

private:
    std::unique_ptr<detail::LifecycleAgentState> state_;
};

} // namespace mediaserver::app
