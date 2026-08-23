#include <mediaserver/cmd/reducer.hpp>

#include <utility>

namespace mediaserver::cmd {

core::Result<doc::Root, PayloadError> apply_payload(
    const doc::Root& root,
    const Payload& payload) {
    if (const auto* put = std::get_if<PutNode>(&payload)) {
        return core::Result<doc::Root, PayloadError>::success(root.set(put->node));
    }

    const auto id = std::get<EraseNode>(payload).id;
    auto erased = root.erase(id);
    if (!erased) {
        return core::Result<doc::Root, PayloadError>::failure(PayloadError::MissingNode);
    }
    return core::Result<doc::Root, PayloadError>::success(std::move(*erased));
}

bool payload_postcondition_matches(const doc::Root& root, const Payload& payload) noexcept {
    if (const auto* put = std::get_if<PutNode>(&payload)) {
        const auto* current = root.get(put->node.id);
        return current != nullptr && *current == put->node;
    }
    return !root.contains(std::get<EraseNode>(payload).id);
}

} // namespace mediaserver::cmd
