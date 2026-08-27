#pragma once

#include <mediaserver/compositor/compositor_graph.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/doc/document.hpp>

#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

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

    // The whole authored graph, every scene at once. Useful for tooling and
    // for a document with a single scene; a renderer wants the scoped form.
    [[nodiscard]] CompositorGraph evaluate() const;

    // The graph belonging to one scene. Graph objects carrying no authored
    // owner belong to `legacy_owner`, which is the lowest-id stage, so a
    // project authored before graphs were scene-scoped keeps rendering its
    // composite exactly where it used to. Edges follow their endpoints: an
    // edge is in scope only when both of the nodes it joins are.
    [[nodiscard]] CompositorGraph evaluate(doc::NodeId scene,
                                           doc::NodeId legacy_owner) const;
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
    [[nodiscard]] CompositorGraph build(const doc::NodeId* scene,
                                        doc::NodeId legacy_owner) const;
};

enum class GraphCloneError : core::u8 {
    InvalidFirstId,
    IdExhausted,
    AllocationFailed,
};

// The compositor half of a scene copy. Planning is separate from authoring for
// the same reason it is in the scene layer: the caller wraps the puts in its
// own transaction and decides when to schedule them.
struct GraphClonePlan final {
    // New nodes in ascending original-node-id order; none exist in `root`.
    std::vector<doc::Node> puts;
    // (original, clone), in the same deterministic order as puts.
    std::vector<std::pair<doc::NodeId, doc::NodeId>> remap;
};

// Copies the graph objects owned by `source` so they belong to `destination`.
// `element_remap` is the scene clone's (original, clone) mapping: a SceneLayer
// pointing at a copied drawable is repointed at the copy, and one pointing
// outside the copied scene keeps its reference. Without this a duplicated
// scene would render through no composite at all, so promoting it would change
// the look of Program - the exact thing Preview exists to prevent.
[[nodiscard]] core::Result<GraphClonePlan, GraphCloneError> plan_clone_graph(
    const doc::Root& root,
    doc::NodeId source,
    doc::NodeId destination,
    doc::NodeId legacy_owner,
    std::span<const std::pair<doc::NodeId, doc::NodeId>> element_remap,
    doc::NodeId first_id);

} // namespace mediaserver::compositor
