#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/scene/scene.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <optional>
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
    // A tangent mode was asked of a Hold or Linear key, which has none.
    NotCubic,
    // The change would leave the channel unable to evaluate, typically
    // handle weights longer than the interval they span.
    InvalidTangent,
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
    // The key's tangent properties exactly as authored, one slot per
    // schema::channel_key_tangent_suffixes entry, so moving, duplicating and
    // re-indexing a key carries its handles byte for byte.
    std::array<std::optional<doc::PropertyValue>, 5U> tangent{};

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
    // A tangent with a non-finite slope or a weight that is not a positive
    // finite number of frames.
    InvalidTangent,
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

// What an easing choice writes on each selected key. A key's interpolation
// names how it reaches the next key; its in tangent shapes the segment from
// the previous key, and only once that key is Cubic. Slopes are value per
// show frame. "Weights kept" keeps authored handle lengths of a key that was
// already Cubic and not Auto, and authors none otherwise.
//
//   Linear    linear; every tangent property removed.
//   Hold      hold; every tangent property removed.
//   Auto      cubic; every tangent property removed (Auto is the default).
//   EasyEase  cubic, flat (both slopes zero); weights kept.
//   EaseIn    cubic, broken: in slope 0, out slope the one it evaluated with;
//             weights kept.
//   EaseOut   cubic, broken: out slope 0, in slope the one it evaluated
//             with; weights kept.
//   Aligned   cubic keys only: aligned, both slopes the mean of the two it
//             evaluated with; weights kept.
//   Broken    cubic keys only: broken, each slope the one it evaluated with;
//             weights kept.
//   Flat      cubic keys only: flat; weights kept.
//
// EasyEase and EaseIn also turn an unselected Linear previous key into an
// Auto Cubic one, so the incoming segment is a curve the eased in tangent
// shapes; a Hold previous key stays a hold. Aligned, Broken and Flat skip a
// Hold or Linear key as NotCubic. A channel the change would leave unable to
// evaluate is left as it was and its keys reported InvalidTangent. The
// selection is unchanged; selection_after lists it as the edit leaves it.
enum class KeyEase : core::u8 {
    Linear,
    Hold,
    Auto,
    EasyEase,
    EaseIn,
    EaseOut,
    Aligned,
    Broken,
    Flat,
};

[[nodiscard]] KeyEditResult plan_key_ease(
    const doc::Root& document, std::span<const KeyAddress> selection,
    KeyEase ease);

// One key's handles, as a handle drag leaves them: the key becomes Cubic with
// exactly `tangent`. Auto removes every tangent property; Flat writes the
// mode and any weights; Aligned writes the mean of the two slopes to both;
// Broken writes both slopes; weights are written only when present. A
// non-finite slope or a weight that is not a positive finite number fails
// with InvalidTangent; weights too long for the key's intervals skip the key
// as InvalidTangent and change nothing.
[[nodiscard]] KeyEditResult plan_key_tangent(
    const doc::Root& document, const KeyAddress& address,
    const KeyTangent& tangent);

} // namespace mediaserver::scene
