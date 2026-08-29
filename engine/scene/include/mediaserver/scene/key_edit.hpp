#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/scene/scene.hpp>

#include <compare>
#include <cstddef>
#include <span>
#include <vector>

// Pure, deterministic curve/dope-sheet key planning.
//
// Keys remain indexed properties on scene.channel nodes. This layer gives an
// editor a typed, unique address and an exact carrier snapshot without making
// keys pretend to be document nodes. Every mutation returns a complete
// document replacement plus node-id-sorted puts/erases, ready for one undoable
// Director transaction.
namespace mediaserver::scene {

struct KeyAddress final {
    doc::NodeId target{};
    PropertyChannel property{};
    core::i64 frame{};

    auto operator<=>(const KeyAddress&) const = default;
};

enum class KeyEditSkipReason : core::u8 {
    InvalidProperty,
    TargetMissing,
    TargetLocked,
    InvalidTarget,
    ChannelMissing,
    AmbiguousChannel,
    KeyMissing,
    ChannelCapacity,
};

struct KeyEditSkip final {
    KeyAddress address;
    KeyEditSkipReason reason{KeyEditSkipReason::KeyMissing};

    bool operator==(const KeyEditSkip&) const = default;
};

// A command-time key snapshot suitable for both row/box/lasso selection and
// curve hit testing. `value` is the finite numeric projection; `carrier` keeps
// the exact authored integer-vs-real representation. The authored flags retain
// implicit zero/linear properties byte-for-byte when another key on the same
// channel is edited.
struct KeyRecord final {
    doc::NodeId channel{};
    KeyAddress address;
    doc::PropertyValue carrier{0.0};
    double value{};
    bool carrier_authored{};
    KeyInterpolation interpolation{KeyInterpolation::Linear};
    bool interpolation_authored{};

    bool operator==(const KeyRecord&) const = default;
};

struct KeySelection final {
    // Unique by typed address and sorted by (target, property, frame).
    std::vector<KeyRecord> records;
    std::vector<KeyEditSkip> skips;
    std::size_t duplicate_inputs{};

    bool operator==(const KeySelection&) const = default;
};

enum class KeyEditError : core::u8 {
    InvalidScale,
    IntegerOverflow,
    AllocationFailed,
};

// A transformed key displaced another transformed key, or replaced a
// stationary authored destination. For transformed/transformed collisions,
// the winner is the latest (original frame, scene.channel node id).
struct KeyCollision final {
    KeyAddress loser;
    KeyAddress winner;
    doc::NodeId channel{};
    core::i64 destination_frame{};
    bool stationary_loser{};

    bool operator==(const KeyCollision&) const = default;
};

struct KeyEditReport final {
    std::size_t selected{};
    std::size_t applied{};
    std::size_t duplicate_inputs{};
    std::vector<KeyEditSkip> skips;
    std::vector<KeyCollision> collisions;

    bool operator==(const KeyEditReport&) const = default;
};

struct KeyEditPlan final {
    doc::Root after;
    // Complete changed channel nodes and erased empty channel ids. Both are
    // independently node-id sorted and never overlap.
    std::vector<doc::Node> puts;
    std::vector<doc::NodeId> erases;
    // The surviving destination keys, sorted by typed address. Duplicate keeps
    // only the new copies here; move/scale return the moved selection; delete
    // returns an empty selection.
    std::vector<KeyRecord> selection_after;
    KeyEditReport report;
};

enum class KeyTimeAnchor : core::u8 {
    FirstSelected,
    LastSelected,
    Playhead,
    User,
};

struct KeyTimeScale final {
    // Signed scale numerator over a strictly positive denominator. Negative
    // scales mirror around the anchor; zero deliberately collapses to it and
    // therefore exercises the ordinary deterministic collision rule.
    core::i64 numerator{1};
    core::i64 denominator{1};
    KeyTimeAnchor anchor{KeyTimeAnchor::FirstSelected};
    core::i64 playhead{};
    core::i64 user_anchor{};

    bool operator==(const KeyTimeScale&) const = default;
};

using KeySelectionResult = core::Result<KeySelection, KeyEditError>;
using KeyEditResult = core::Result<KeyEditPlan, KeyEditError>;

// Canonicalizes duplicate input addresses, resolves the same lowest-id valid
// channel SceneModel evaluates, snapshots exact carriers/interpolation, and
// reports stale, locked, incompatible, or duplicate-frame channels as skips.
[[nodiscard]] KeySelectionResult inspect_key_selection(
    const doc::Root& document, std::span<const KeyAddress> selection);

[[nodiscard]] KeyEditResult plan_key_move(
    const doc::Root& document, std::span<const KeyAddress> selection,
    core::i64 delta);

[[nodiscard]] KeyEditResult plan_key_duplicate(
    const doc::Root& document, std::span<const KeyAddress> selection,
    core::i64 delta);

[[nodiscard]] KeyEditResult plan_key_delete(
    const doc::Root& document, std::span<const KeyAddress> selection);

// destination = anchor + round_ties_away(
//     (source - anchor) * numerator / denominator)
//
// Full signed-frame inputs are evaluated with portable 128-bit limb
// arithmetic. Any unrepresentable destination rejects the whole plan before a
// document node is changed.
[[nodiscard]] KeyEditResult plan_key_scale(
    const doc::Root& document, std::span<const KeyAddress> selection,
    KeyTimeScale scale);

} // namespace mediaserver::scene
