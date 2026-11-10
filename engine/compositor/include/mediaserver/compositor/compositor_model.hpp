#pragma once

#include <mediaserver/compositor/compositor_graph.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/scene/scene_model.hpp>

#include <map>
#include <optional>
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
    // Brings the model to `root` and leaves it exactly as reset(root) would,
    // parsing only the nodes that differ from the document it last held. A
    // frame loop calls this when the committed document changes.
    void update(const doc::Root& root);
    // The same, for a caller that already keeps a scene model at `root`: the
    // canvas and effect settings the resource estimates read are taken from
    // it, where update(root) would work them out again in a scene model of
    // this object's own. The result is identical either way; a `scenes` that
    // does not hold exactly `root` is ignored.
    void update(const doc::Root& root, const scene::SceneModel& scenes);

    // The whole authored graph, every scene at once. Useful for tooling and
    // for a document with a single scene; a renderer wants the scoped form.
    [[nodiscard]] CompositorGraph evaluate() const;

    // The graph belonging to one scene: the objects whose `scene_id` names it.
    // Edges follow their endpoints: an edge is in scope only when both of the
    // nodes it joins are.
    [[nodiscard]] CompositorGraph evaluate(doc::NodeId scene) const;
    [[nodiscard]] std::size_t size() const noexcept {
        return nodes_.size() + edges_.size();
    }
    [[nodiscard]] const CompositorCounters& counters() const noexcept {
        return counters_;
    }

  private:
    std::map<doc::NodeId, GraphNode> nodes_;
    std::map<doc::NodeId, GraphEdge> edges_;
    std::map<doc::NodeId, GraphComment> comments_;
    // Every document node is recorded so a SceneLayer reference can be
    // checked without importing or duplicating scene schema knowledge.
    std::map<doc::NodeId, std::string> document_types_;
    // The canonical scene projection supplies only authored canvas and
    // project-effect settings to diagnostic resource estimates. Keeping the
    // same projection avoids a second parser or subtly different clamps.
    scene::SceneModel resource_scene_model_;
    // What evaluation reads of that projection, taken once per document.
    std::vector<scene::SceneInfo> resource_scenes_;
    scene::EffectSettings resource_effect_settings_{};
    CompositorCounters counters_{};
    // The document reset() or update() last brought the model to, kept so the
    // next update() can find what changed. Empty after apply(), which is
    // given a delta and no document.
    std::optional<doc::Root> document_;

    void put(const doc::Node& node);
    void forget(doc::NodeId id) noexcept;
    void recount() noexcept;
    void read_resources(const scene::SceneModel& scenes);
    [[nodiscard]] CompositorGraph build(const doc::NodeId* scene) const;
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
// The graph objects a copy carries: the chosen nodes and comment boxes as
// they are now, and the wires running between two of them. Wires to anything
// outside the set are left behind. Ids that name nothing in the graph are
// ignored.
[[nodiscard]] std::vector<doc::Node>
snapshot_graph_selection(const doc::Root& root,
                         std::span<const doc::NodeId> chosen);

struct GraphPastePlan final {
    // New nodes in ascending original-node-id order; none exist in `root`.
    std::vector<doc::Node> puts;
    // (original, copy), in the same order.
    std::vector<std::pair<doc::NodeId, doc::NodeId>> remap;
    // Copied nodes that could not be placed: Output, of which a graph has
    // one, and a Scene Layer whose source already has a layer in the
    // destination graph, of which a source has one. Their wires go too.
    std::size_t skipped{};
};

// Pastes a snapshot into `destination`'s graph, every position moved by
// (dx, dy) graph units: nodes, comment boxes, and wires' reroute points.
// Ids are allocated as plan_clone_graph does, and each wire is rewritten to
// the copies of its two ends.
[[nodiscard]] core::Result<GraphPastePlan, GraphCloneError> plan_paste_graph(
    const doc::Root& root, std::span<const doc::Node> snapshot,
    doc::NodeId destination, float dx, float dy, doc::NodeId first_id);

[[nodiscard]] core::Result<GraphClonePlan, GraphCloneError> plan_clone_graph(
    const doc::Root& root,
    doc::NodeId source,
    doc::NodeId destination,
    std::span<const std::pair<doc::NodeId, doc::NodeId>> element_remap,
    doc::NodeId first_id);

} // namespace mediaserver::compositor
