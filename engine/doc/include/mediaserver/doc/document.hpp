#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <mediaserver/doc/node.hpp>

namespace mediaserver::doc {

namespace detail {
struct TrieNode;
}

class Root;
struct Delta;
[[nodiscard]] Delta diff(const Root& from, const Root& to);

class Root final {
public:
    Root() noexcept;

    [[nodiscard]] const Node* get(NodeId id) const noexcept;
    [[nodiscard]] bool contains(NodeId id) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }
    [[nodiscard]] Version version() const noexcept { return version_; }
    // Snapshot provenance is pointer identity, not merely a matching version.
    // Persistent copies of the same root share the exact trie allocation.
    [[nodiscard]] bool same_snapshot(const Root& other) const noexcept {
        return trie_ == other.trie_ && version_ == other.version_
            && size_ == other.size_;
    }

    [[nodiscard]] Root set(Node node) const;
    [[nodiscard]] std::optional<Root> erase(NodeId id) const;
    [[nodiscard]] static std::optional<Root> restore(
        std::span<const Node> nodes,
        Version version);

private:
    Root(
        std::shared_ptr<const detail::TrieNode> trie,
        Version version,
        std::size_t size) noexcept;

    std::shared_ptr<const detail::TrieNode> trie_;
    Version version_;
    std::size_t size_{};

    friend Delta diff(const Root& from, const Root& to);
};

enum class ChangeKind {
    Added,
    Removed,
    Modified,
};

struct NodeChange final {
    NodeId id;
    ChangeKind kind;
    NodeRef before;
    NodeRef after;
};

struct Delta final {
    Version from;
    Version to;
    std::vector<NodeChange> changes;
};

} // namespace mediaserver::doc
