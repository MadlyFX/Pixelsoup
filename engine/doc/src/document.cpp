#include <mediaserver/doc/document.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace mediaserver::doc::detail {

struct TrieNode final {
    std::array<std::shared_ptr<const TrieNode>, 16U> children{};
    NodeRef value;
};

} // namespace mediaserver::doc::detail

namespace mediaserver::doc {
namespace {

constexpr std::size_t radix_bits = 4U;
constexpr std::size_t radix_levels = 64U / radix_bits;
constexpr std::uint64_t radix_mask = 0x0fU;

[[nodiscard]] std::size_t child_index(NodeId id, std::size_t depth) noexcept {
    const auto shift = static_cast<unsigned>((radix_levels - depth - 1U) * radix_bits);
    return static_cast<std::size_t>((id.value() >> shift) & radix_mask);
}

[[nodiscard]] NodeRef find_ref(
    const std::shared_ptr<const detail::TrieNode>& node,
    NodeId id,
    std::size_t depth = 0U) noexcept {
    if (!node) {
        return {};
    }
    if (depth == radix_levels) {
        return node->value;
    }
    return find_ref(node->children[child_index(id, depth)], id, depth + 1U);
}

[[nodiscard]] std::shared_ptr<const detail::TrieNode> set_ref(
    const std::shared_ptr<const detail::TrieNode>& node,
    NodeId id,
    NodeRef value,
    std::size_t depth = 0U) {
    auto updated = node ? std::make_shared<detail::TrieNode>(*node)
                        : std::make_shared<detail::TrieNode>();
    if (depth == radix_levels) {
        updated->value = std::move(value);
        return updated;
    }

    const auto index = child_index(id, depth);
    updated->children[index] = set_ref(updated->children[index], id, std::move(value), depth + 1U);
    return updated;
}

[[nodiscard]] bool is_empty(const detail::TrieNode& node) noexcept {
    if (node.value) {
        return false;
    }
    for (const auto& child : node.children) {
        if (child) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::shared_ptr<const detail::TrieNode> erase_ref(
    const std::shared_ptr<const detail::TrieNode>& node,
    NodeId id,
    std::size_t depth = 0U) {
    auto updated = std::make_shared<detail::TrieNode>(*node);
    if (depth == radix_levels) {
        updated->value.reset();
    } else {
        const auto index = child_index(id, depth);
        updated->children[index] = erase_ref(updated->children[index], id, depth + 1U);
    }
    return is_empty(*updated) ? std::shared_ptr<const detail::TrieNode>{} : std::move(updated);
}

[[nodiscard]] Version next_version(Version current) noexcept {
    return Version{current.value() + 1U};
}

void collect_changes(
    const std::shared_ptr<const detail::TrieNode>& before,
    const std::shared_ptr<const detail::TrieNode>& after,
    std::size_t depth,
    std::vector<NodeChange>& changes) {
    if (before == after) {
        return;
    }

    if (depth == radix_levels) {
        const auto before_value = before ? before->value : NodeRef{};
        const auto after_value = after ? after->value : NodeRef{};
        if (before_value && after_value && *before_value == *after_value) {
            return;
        }
        if (!before_value && !after_value) {
            return;
        }

        const auto id = before_value ? before_value->id : after_value->id;
        const auto kind = !before_value ? ChangeKind::Added
                         : !after_value ? ChangeKind::Removed
                                        : ChangeKind::Modified;
        changes.push_back(NodeChange{id, kind, before_value, after_value});
        return;
    }

    for (std::size_t index = 0U; index < 16U; ++index) {
        const auto& before_child = before ? before->children[index]
                                          : std::shared_ptr<const detail::TrieNode>{};
        const auto& after_child = after ? after->children[index]
                                        : std::shared_ptr<const detail::TrieNode>{};
        collect_changes(before_child, after_child, depth + 1U, changes);
    }
}

} // namespace

Root::Root() noexcept = default;

Root::Root(
    std::shared_ptr<const detail::TrieNode> trie,
    Version version,
    std::size_t size) noexcept
    : trie_(std::move(trie)), version_(version), size_(size) {}

const Node* Root::get(NodeId id) const noexcept {
    const auto ref = find_ref(trie_, id);
    return ref.get();
}

bool Root::contains(NodeId id) const noexcept {
    return get(id) != nullptr;
}

Root Root::set(Node node) const {
    const bool inserting = !contains(node.id);
    const auto id = node.id;
    auto value = std::make_shared<const Node>(std::move(node));
    auto updated = set_ref(trie_, id, std::move(value));
    return Root(std::move(updated), next_version(version_), size_ + (inserting ? 1U : 0U));
}

std::optional<Root> Root::erase(NodeId id) const {
    if (!contains(id)) {
        return std::nullopt;
    }
    auto updated = erase_ref(trie_, id);
    return Root(std::move(updated), next_version(version_), size_ - 1U);
}

std::optional<Root> Root::restore(std::span<const Node> nodes, Version version) {
    if (version.value() < static_cast<core::u64>(nodes.size())) {
        return std::nullopt;
    }

    Root restored;
    for (const auto& node : nodes) {
        if (restored.contains(node.id)) {
            return std::nullopt;
        }
        restored = restored.set(node);
    }
    return Root(restored.trie_, version, restored.size_);
}

Delta diff(const Root& from, const Root& to) {
    Delta delta{from.version(), to.version(), {}};
    collect_changes(from.trie_, to.trie_, 0U, delta.changes);
    return delta;
}

} // namespace mediaserver::doc
