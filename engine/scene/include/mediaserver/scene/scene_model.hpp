#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/scene/scene.hpp>

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// The bridge from document state to a drawable scene.
//
// This is the seam §6.4 of the implementation plan calls the critical one, and
// the rule it states is the rule here: the renderer never reads the document
// and the document never knows a GPU exists. Everything crossing between them
// crosses through this type.
//
// It is split in two on purpose, and the split is where the frame budget goes:
//
//  * `apply` parses document nodes into a private, already-validated form. It
//    allocates, it does string lookups, and it runs only when the document
//    changes - which during playback is never (§0.1). A delta touching three
//    nodes costs three parses, not a rebuild.
//  * `evaluate` turns that form into a `Scene` at a show frame. It runs every
//    frame on every node, does no parsing and no string work, and is a pure
//    function of (parsed state, frame ordinal). Being pure is what makes the
//    scene digest a real cross-node check rather than a hopeful one: there is
//    nothing for two nodes to disagree about except their inputs.
//
// `evaluate` still allocates its output vectors. That is honest rather than
// ideal - the frame path elsewhere in this codebase allocates nothing - and it
// is the first thing to fix when the scene gets big enough to matter, by
// evaluating into a caller-owned arena. It is left alone for now because the
// shape of the output is still moving and a premature arena would freeze it.
namespace mediaserver::scene {

// What the model is holding, and what it refused.
//
// Reported rather than silently absorbed: a scene that renders nothing because
// every object failed to parse looks exactly like a scene that is empty, and
// an operator has to be able to tell those apart at a glance.
//
// The population figures are counts of what is held, not of what happened, so
// they do not depend on whether this model was built by streaming deltas or by
// parsing a checkpoint in one go. `rejected` is the one running total, because
// a parse failure is an event and there is nothing left afterwards to count.
struct SceneCounters final {
    // Cumulative since construction or the last `reset`: nodes carrying a
    // scene type whose properties did not validate.
    core::u64 rejected{};
    core::u32 stages{};
    core::u32 cameras{};
    core::u32 objects{};
    core::u32 media{};
    core::u32 models{};
    core::u32 directional_lights{};
    core::u32 point_lights{};
    core::u32 slices{};
    core::u32 channels{};
    core::u32 effects{};
    // Cameras beyond the lowest-id camera in each scene. Recomputed after a
    // delta because adding/removing the lowest stage can re-home legacy
    // unscoped cameras without touching the camera nodes themselves.
    core::u32 duplicate_cameras{};

    bool operator==(const SceneCounters&) const = default;

    // Camera nodes past the first in each scene. Stages are first-class scenes,
    // not competing singleton definitions. The lowest camera node id wins in
    // each scene, independent of the order changes arrived in.
    [[nodiscard]] core::u32 duplicate_singletons() const noexcept;

    // Objects, lights, and slices the document holds beyond what one frame
    // will draw. Non-zero means `evaluate` is truncating - deterministically,
    // in node id order, so every node truncates identically.
    [[nodiscard]] core::u32 over_capacity() const noexcept;
};

class SceneModel final {
public:
    SceneModel() = default;

    // Discards everything and parses `root` in full. This is the path a node
    // takes when it receives a checkpoint rather than a delta.
    void reset(const doc::Root& root);

    // Folds one document change in. Cheap in proportion to the delta, which is
    // the whole reason the document is a persistent tree.
    void apply(const doc::Delta& delta);

    // The scene as of `time`. Pure: the same model and the same time produce
    // the same bits on every machine. The compatibility overload chooses the
    // lowest-id stage (or the implicit legacy scene when there are no stages).
    [[nodiscard]] Scene evaluate(SceneTime time) const;
    [[nodiscard]] Scene evaluate(SceneId scene, SceneTime time) const;

    // Valid authored scenes, in stage-node-id order. Documents with no stages
    // retain an implicit legacy evaluation scene but report no authored scene
    // metadata here.
    [[nodiscard]] std::vector<SceneInfo> scenes() const;

    // The key frames each animated node holds, for the editor's timeline. One
    // entry per target that has channels, frames deduped and ascending. Not on
    // the per-frame path - the Director queries it for the editor only.
    [[nodiscard]] std::vector<ChannelKeyframes> channel_keyframes() const;

    // First-class effect nodes in deterministic target/order/id order, carrying
    // their authored base values before channel evaluation. This is
    // editor-facing and not on the per-frame render path.
    [[nodiscard]] std::vector<EffectInstance> effects() const;

    // Scene-relevant document nodes currently held. Excludes nodes whose type
    // is not one of the scene types, which are not the scene's business.
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

    // Cumulative since construction or the last `reset`.
    [[nodiscard]] const SceneCounters& counters() const noexcept {
        return counters_;
    }

private:
    // Parsed forms. These are the document's values after validation and unit
    // conversion, and before animation - so they are what an edit changes and
    // what a frame does not.
    struct StageEntry final {
        std::string name;
        Canvas canvas{};
        Environment environment{};
    };

    struct CameraEntry final {
        std::optional<SceneId> scene;
        Camera camera{};
        float orbit_turns_per_second{};
    };

    struct ObjectEntry final {
        std::optional<SceneId> scene;
        MeshKey mesh{};
        Vec3 position{};
        Turns yaw{};
        Turns pitch{};
        Turns roll{};
        Vec3 scale{1.0F, 1.0F, 1.0F};
        Material material{};
        LayerStyle style{};
        float spin_turns_per_second{};
        float bob_amplitude{};
        float bob_turns_per_second{};
    };

    struct LightEntry final {
        std::optional<SceneId> scene;
        bool directional{true};
        Vec3 vector{};
        Vec3 color{1.0F, 1.0F, 1.0F};
        float intensity{1.0F};
        float range{10.0F};
    };

    struct MediaEntry final {
        std::optional<SceneId> scene;
        MeshKey mesh{Primitive::Quad, 1U};
        Vec3 position{};
        Turns yaw{};
        Turns pitch{};
        Turns roll{};
        Vec3 scale{1.0F, 1.0F, 1.0F};
        std::string source;
        core::u32 sequence_start{};
        core::u32 sequence_count{1U};
        std::optional<MediaFrameRate> frame_rate;
        LayerStyle style{};
    };

    struct ModelEntry final {
        std::optional<SceneId> scene;
        Vec3 position{};
        Turns yaw{};
        Turns pitch{};
        Turns roll{};
        Vec3 scale{1.0F, 1.0F, 1.0F};
        std::string source;
        Material material{Vec3{0.72F, 0.72F, 0.74F}, 0.6F, 0.0F,
                          Vec3{0.0F, 0.0F, 0.0F}};
        LayerStyle style{};
    };

    struct SliceEntry final {
        SliceBinding binding{};
    };

    struct EffectEntry final {
        doc::NodeId target{};
        EffectKind kind{EffectKind::Brightness};
        core::i64 order{};
        bool enabled{true};
        float value{1.0F};
    };

    // How a key reaches the next one. Hold steps; Linear interpolates; Cubic
    // eases with a smoothstep (a basic cubic, no explicit tangents yet).
    enum class ChannelInterp : core::u8 { Hold, Linear, Cubic };

    struct ChannelKey final {
        core::i64 frame{};
        double value{};
        ChannelInterp interp{ChannelInterp::Linear};
    };

    struct ChannelEntry final {
        doc::NodeId target{};
        // The transform component this channel drives; the public enum is what
        // the editor also uses to place the channel on a timeline row.
        PropertyChannel component{};
        // Sorted by frame ascending at parse time, so evaluation is a search
        // rather than a scan and two nodes agree on which keys bracket a frame.
        std::vector<ChannelKey> keys;
    };

    using Entry = std::variant<
        StageEntry,
        CameraEntry,
        ObjectEntry,
        MediaEntry,
        ModelEntry,
        LightEntry,
        SliceEntry,
        EffectEntry,
        ChannelEntry>;

    // A channel's value at a frame, evaluated in f64 for bit-identical results
    // across nodes; clamped before the first key and after the last. The keys
    // must be non-empty and frame-sorted, as `put` leaves them.
    [[nodiscard]] static double sample_channel(
        const ChannelEntry& channel, core::i64 frame) noexcept;

    // Ordered by node id, which makes iteration deterministic without a sort
    // and makes "the lowest id wins" a matter of stopping at the first match.
    std::map<doc::NodeId, Entry> entries_;
    SceneCounters counters_{};

    void put(const doc::Node& node);
    void forget(doc::NodeId id) noexcept;
    // Adjusts the population figures for an entry entering or leaving.
    void note(const Entry& entry, bool added) noexcept;
    [[nodiscard]] SceneId default_scene_id() const noexcept;
    [[nodiscard]] bool valid_scene(SceneId scene) const noexcept;
    [[nodiscard]] std::optional<SceneId> scope_of(
        doc::NodeId id, SceneId default_scene) const noexcept;
    [[nodiscard]] bool drawable(doc::NodeId id) const noexcept;
    void refresh_duplicate_cameras() noexcept;
};

} // namespace mediaserver::scene
