#include <mediaserver/cmd/loopback_transport.hpp>

#include <utility>

namespace mediaserver::cmd {

LoopbackTransport::LoopbackTransport(std::vector<NodeCommandBus*> nodes)
    : nodes_(std::move(nodes)), queues_(nodes_.size()) {}

void LoopbackTransport::broadcast(Command command) {
    for (auto& queue : queues_) {
        queue.emplace_back(command);
    }
}

void LoopbackTransport::broadcast(FrameSeal seal) {
    for (auto& queue : queues_) {
        queue.emplace_back(seal);
    }
}

std::size_t LoopbackTransport::pending(std::size_t node_index) const noexcept {
    return node_index < queues_.size() ? queues_[node_index].size() : 0U;
}

std::optional<ReceiveStatus> LoopbackTransport::deliver(
    std::size_t node_index,
    std::size_t message_index) {
    if (node_index >= nodes_.size() || nodes_[node_index] == nullptr) {
        return std::nullopt;
    }
    auto& queue = queues_[node_index];
    if (message_index >= queue.size()) {
        return std::nullopt;
    }

    auto message = std::move(queue[message_index]);
    queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(message_index));
    return std::visit(
        [node = nodes_[node_index]](auto&& value) {
            return node->receive(std::forward<decltype(value)>(value));
        },
        std::move(message));
}

} // namespace mediaserver::cmd
