#pragma once

#include <cstddef>
#include <optional>
#include <variant>
#include <vector>

#include <mediaserver/cmd/node_command_bus.hpp>

namespace mediaserver::cmd {

using LoopbackMessage = std::variant<Command, FrameSeal>;

// Deterministic in-memory transport used for cluster protocol integration and
// fault injection before the ordered network channel is introduced.
class LoopbackTransport final {
public:
    explicit LoopbackTransport(std::vector<NodeCommandBus*> nodes);

    void broadcast(Command command);
    void broadcast(FrameSeal seal);

    [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }
    [[nodiscard]] std::size_t pending(std::size_t node_index) const noexcept;
    [[nodiscard]] std::optional<ReceiveStatus> deliver(
        std::size_t node_index,
        std::size_t message_index);

private:
    std::vector<NodeCommandBus*> nodes_;
    std::vector<std::vector<LoopbackMessage>> queues_;
};

} // namespace mediaserver::cmd
