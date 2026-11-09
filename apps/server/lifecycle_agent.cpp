#include "lifecycle_agent.hpp"

#include "asset_readiness_provider.hpp"
#include "managed_process.hpp"
#include "process_log.hpp"
#include "system_hardware_probe.hpp"

#include <asio/ip/address_v4.hpp>
#include <asio/ip/udp.hpp>

#include <chrono>
#include <optional>
#include <utility>

namespace mediaserver::app {
namespace {

[[nodiscard]] ManagedRenderProcessConfig
process_config(const LifecycleAgentConfig& config) {
    std::vector<std::string> arguments = config.child_arguments;
    arguments.push_back("--node-index=" + std::to_string(config.node_index));
    if (!config.log_directory.empty()) {
        const std::u8string directory = config.log_directory.u8string();
        arguments.push_back(
            "--log-dir=" +
            std::string{reinterpret_cast<const char*>(directory.data()),
                        directory.size()});
    }
    std::filesystem::path working_directory;
#if defined(_WIN32)
    working_directory = config.child.parent_path();
#endif
    ManagedRenderProcessConfig process{config.child, std::move(arguments),
                                       std::move(working_directory),
                                       std::chrono::seconds{3}};
    process.no_console_window = config.no_console_window;
    return process;
}

[[nodiscard]] net::ClusterLifecycleServiceConfig
service_config(const LifecycleAgentConfig& config) {
    return net::ClusterLifecycleServiceConfig{
        config.listen, config.node_index, config.secret,
        std::chrono::milliseconds{1'500}, std::chrono::milliseconds{1'500}};
}

} // namespace

namespace detail {

// Declared in the order each depends on the last, so destruction stops
// discovery, then the listener, then the render process.
struct LifecycleAgentState final {
    explicit LifecycleAgentState(LifecycleAgentConfig agent_config)
        : config(std::move(agent_config)), process(process_config(config)),
          sync_probe(SyncDeviceProbeConfig{.simulation =
                                               config.sync_simulation}),
          node_log(config.log_directory,
                   node_process_log_name(config.node_index)),
          service(service_config(config), process, &hardware_probe,
                  &asset_readiness, &sync_probe,
                  config.log_directory.empty() ? nullptr : &node_log),
          unsecured(net::lifecycle_key_is_unsecured(config.secret)) {}

    LifecycleAgentConfig config;
    ManagedRenderProcess process;
    SystemHardwareProbe hardware_probe{};
    PathAssetReadinessProvider asset_readiness{};
    SyncDeviceProbe sync_probe;
    ProcessLogTailProvider node_log;
    net::ClusterLifecycleService service;
    std::optional<net::ClusterDiscoveryResponder> discovery;
    std::string discovery_text{"off"};
    bool unsecured{};
};

} // namespace detail

LifecycleAgent::LifecycleAgent(LifecycleAgentConfig config)
    : state_(std::make_unique<detail::LifecycleAgentState>(
          std::move(config))) {}

LifecycleAgent::~LifecycleAgent() { stop(); }

bool LifecycleAgent::valid_configuration(const LifecycleAgentConfig& config) {
    return ManagedRenderProcess::valid_configuration(process_config(config)) &&
           net::ClusterLifecycleService::valid_configuration(
               service_config(config));
}

core::Result<asio::ip::tcp::endpoint, LifecycleAgentStartError>
LifecycleAgent::start() {
    using Result =
        core::Result<asio::ip::tcp::endpoint, LifecycleAgentStartError>;
    detail::LifecycleAgentState& state = *state_;
    if (state.config.autostart) {
        const auto started =
            state.process.apply(net::ClusterLifecycleCommand::Start);
        if (started.outcome == net::ClusterLifecycleOutcome::Failed ||
            started.state != net::ClusterProcessState::Running) {
            return Result::failure(LifecycleAgentStartError{
                LifecycleAgentFailure::Autostart, started.message, {}});
        }
    }
    auto listening = state.service.start();
    if (!listening) {
        return Result::failure(LifecycleAgentStartError{
            LifecycleAgentFailure::Listener, {}, listening.error()});
    }

    // Discovery lets an editor offer this machine in its Hardware Map. It is
    // a convenience: without it the machine is entered by hand, so a port
    // another program holds is reported, not fatal.
    const std::uint16_t discovery_port = state.config.discovery_port;
    state.discovery_text = "off";
    if (discovery_port != 0U) {
        net::ClusterDiscoveryResponderConfig discovery_config{};
        discovery_config.bind_endpoint = {asio::ip::address_v4::any(),
                                          discovery_port};
        discovery_config.node_index = state.config.node_index;
        discovery_config.control_endpoint = listening.value();
        discovery_config.password_set = !state.unsecured;
        if (!net::ClusterDiscoveryResponder::valid_configuration(
                discovery_config)) {
            state.discovery_text = "off (an IPv6 listener cannot be "
                                   "described; enter this machine by hand)";
        } else {
            state.discovery.emplace(std::move(discovery_config));
            const std::error_code discovery_error = state.discovery->start();
            if (discovery_error) {
                state.discovery.reset();
                state.discovery_text = "off (could not open UDP " +
                                       std::to_string(discovery_port) + ": " +
                                       discovery_error.message() + ")";
            } else {
                state.discovery_text = "udp/" + std::to_string(discovery_port);
            }
        }
    }
    return Result::success(listening.value());
}

void LifecycleAgent::stop() noexcept {
    if (!state_) {
        return;
    }
    if (state_->discovery) {
        state_->discovery->stop();
    }
    state_->service.stop();
}

net::ClusterLifecycleReply
LifecycleAgent::apply(net::ClusterLifecycleCommand command) noexcept {
    return state_->process.apply(command);
}

const LifecycleAgentConfig& LifecycleAgent::config() const noexcept {
    return state_->config;
}

bool LifecycleAgent::unsecured() const noexcept { return state_->unsecured; }

std::string_view LifecycleAgent::discovery_text() const noexcept {
    return state_->discovery_text;
}

net::ClusterLifecycleServiceCounters
LifecycleAgent::counters() const noexcept {
    return state_->service.counters();
}

} // namespace mediaserver::app
