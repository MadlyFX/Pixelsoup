#pragma once

#include <mediaserver/net/cluster_lifecycle.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace mediaserver::app {

struct ManagedRenderProcessConfig final {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::filesystem::path working_directory;
    std::chrono::milliseconds stop_timeout{3'000};
    // Windows only: start the child without a console window. A supervisor
    // that has none of its own (the render-machine window) sets this, since
    // otherwise every start opens a console an operator can close by mistake.
    // A supervisor in a console leaves it off, and the child shares it.
    bool no_console_window{};
};

namespace detail {
class ManagedRenderProcessState;
} // namespace detail

// Supervises one immutable, machine-local render-node command. The network
// listener can choose only status/start/restart/stop; it cannot replace this
// executable or its arguments.
class ManagedRenderProcess final : public net::ClusterProcessHost {
public:
    explicit ManagedRenderProcess(ManagedRenderProcessConfig config);
    ManagedRenderProcess(const ManagedRenderProcess&) = delete;
    ManagedRenderProcess& operator=(const ManagedRenderProcess&) = delete;
    ManagedRenderProcess(ManagedRenderProcess&&) = delete;
    ManagedRenderProcess& operator=(ManagedRenderProcess&&) = delete;
    ~ManagedRenderProcess() override;

    [[nodiscard]] static bool valid_configuration(
        const ManagedRenderProcessConfig& config) noexcept;

    [[nodiscard]] net::ClusterLifecycleReply apply(
        net::ClusterLifecycleCommand command) noexcept override;

private:
    std::unique_ptr<detail::ManagedRenderProcessState> state_;
};

} // namespace mediaserver::app
