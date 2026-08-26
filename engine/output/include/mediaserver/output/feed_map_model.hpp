#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/output/feed_map.hpp>

#include <map>

// The bridge from document state to an evaluated feed map.
//
// The same seam `scene::SceneModel` is, for the output stage: the output path
// never reads the document and the document never knows a connector exists, and
// everything between them crosses through this type. It is simpler than the
// scene model in one way that matters - a feed map has no animation, so there
// is no frame ordinal and `evaluate` takes no time. Parsing still happens only
// when the document changes; `evaluate` is a pure function of the parsed state,
// which is what makes the feed-map digest a real cross-node check.
namespace mediaserver::output {

// What the model is holding, and what it refused. Reported rather than absorbed
// for the same reason the scene counters are: a feed map that binds nothing
// because every node failed to parse looks exactly like one that is empty.
struct FeedMapCounters final {
    // Cumulative since construction or the last `reset`: nodes carrying the
    // feed-map type whose properties did not validate.
    core::u64 rejected{};
    // output.map nodes currently held.
    core::u32 maps{};
    // output.surface nodes currently held.
    core::u32 surfaces{};

    bool operator==(const FeedMapCounters&) const = default;

    // Maps held beyond what one output frame will bind. Non-zero means
    // `evaluate` is truncating - deterministically, in node id order, so every
    // node truncates identically.
    [[nodiscard]] core::u32 over_capacity() const noexcept;
    [[nodiscard]] core::u32 surfaces_over_capacity() const noexcept;
};

class FeedMapModel final {
public:
    FeedMapModel() = default;

    // Discards everything and parses `root` in full - the path a node takes on
    // a checkpoint rather than a delta.
    void reset(const doc::Root& root);

    // Folds one document change in, in proportion to the delta.
    void apply(const doc::Delta& delta);

    // The feed map the document currently describes. Pure: the same model
    // produces the same bits on every machine.
    [[nodiscard]] FeedMap evaluate() const;

    // Feed-map document nodes currently held.
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t surface_size() const noexcept {
        return surfaces_.size();
    }

    [[nodiscard]] const FeedMapCounters& counters() const noexcept {
        return counters_;
    }

private:
    // Ordered by node id, so iteration is deterministic without a sort and
    // "the lowest id wins" is a matter of taking the first entry for a node
    // index.
    std::map<doc::NodeId, OutputMap> entries_;
    std::map<doc::NodeId, ProjectionSurface> surfaces_;
    FeedMapCounters counters_{};

    void put(const doc::Node& node);
    void forget(doc::NodeId id) noexcept;
};

} // namespace mediaserver::output
