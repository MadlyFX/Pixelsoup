#pragma once

#include <mediaserver/compositor/compositor_graph.hpp>
#include <mediaserver/doc/document.hpp>

#include <map>
#include <string>

namespace mediaserver::compositor {

struct CompositorCounters final {
    core::u64 rejected{};
    core::u32 nodes{};
    core::u32 edges{};

    bool operator==(const CompositorCounters&) const = default;

    [[nodiscard]] core::u32 nodes_over_capacity() const noexcept;
    [[nodiscard]] core::u32 edges_over_capacity() const noexcept;
};

class CompositorModel final {
  public:
    CompositorModel() = default;

    void reset(const doc::Root& root);
    void apply(const doc::Delta& delta);

    [[nodiscard]] CompositorGraph evaluate() const;
    [[nodiscard]] std::size_t size() const noexcept {
        return nodes_.size() + edges_.size();
    }
    [[nodiscard]] const CompositorCounters& counters() const noexcept {
        return counters_;
    }

  private:
    std::map<doc::NodeId, GraphNode> nodes_;
    std::map<doc::NodeId, GraphEdge> edges_;
    // Every document node is recorded so a SceneLayer reference can be
    // checked without importing or duplicating scene schema knowledge.
    std::map<doc::NodeId, std::string> document_types_;
    CompositorCounters counters_{};

    void put(const doc::Node& node);
    void forget(doc::NodeId id) noexcept;
    void recount() noexcept;
};

} // namespace mediaserver::compositor
