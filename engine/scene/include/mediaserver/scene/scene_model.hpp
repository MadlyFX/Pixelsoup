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
    core::u32 lanes{};
    core::u32 groups{};
    core::u32 models{};
    core::u32 directional_lights{};
    core::u32 point_lights{};
    core::u32 slices{};
    core::u32 channels{};
    core::u32 effects{};
    core::u32 masks{};
    core::u32 mask_points{};
    // Cameras beyond the lowest-id camera in each scene. Recomputed after a
    // delta because adding/removing the lowest stage can re-home legacy
    // unscoped cameras without touching the camera nodes themselves.
    core::u32 duplicate_cameras{};

    bool operator==(const SceneCounters&) const = default;

    // Camera nodes past the first in each scene. Stages are first-class scenes,
    // not competing singleton definitions. The lowest camera node id wins in
    // each scene, independent of the order changes arrived in.
    [[nodiscard]] core::u32 duplicate_singletons() const noexcept;

    // Drawables, masks/points, lights, and slices held beyond their bounded
    // projection capacity. Non-zero means evaluation truncates
    // deterministically and emits semantic diagnostics where the carrier
    // exposes them.
    [[nodiscard]] core::u32 over_capacity() const noexcept;
};

// One authored clip placement, for the editor's timeline.
//
// Independent of the playhead, which is the whole point: an evaluated Scene
// deliberately omits a clip that is off air, and a video timeline has to keep
// showing, selecting, and dragging exactly those clips. This is the authored
// truth - what the document holds - not what this frame draws.
struct ClipPlacement final {
    doc::NodeId node{};
    SceneId scene{};
    std::string source;
    core::u32 sequence_start{};
    core::u32 sequence_count{1U};
    ClipTiming timing{};
    doc::NodeId lane{};
    core::i64 order{};
    bool locked{};
    std::optional<MediaFrameRate> frame_rate;
    std::optional<doc::NodeId> parent_group;
    core::i64 group_order{};

    bool operator==(const ClipPlacement&) const = default;
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
    //
    // `loop` is the show's repeat range. It is read only to aim the decode
    // warm-up in Scene::media_prewarm across the wrap; every drawn value is
    // identical with or without it, so a caller that has no transport may omit
    // it and lose nothing but a head start.
    [[nodiscard]] Scene evaluate(SceneTime time, ShowLoop loop = {}) const;
    [[nodiscard]] Scene evaluate(
        SceneId scene, SceneTime time, ShowLoop loop = {}) const;

    // Valid authored scenes, in stage-node-id order. Documents with no stages
    // retain an implicit legacy evaluation scene but report no authored scene
    // metadata here.
    [[nodiscard]] std::vector<SceneInfo> scenes() const;

    // The key frames each animated node holds, for the editor's timeline. One
    // entry per target that has channels, frames deduped and ascending. Not on
    // the per-frame path - the Director queries it for the editor only.
    [[nodiscard]] std::vector<ChannelKeyframes> channel_keyframes() const;

    // The project-wide effect settings, already clamped. The built-in default
    // when the document carries no settings node.
    [[nodiscard]] EffectSettings effect_settings() const;

    // The node the settings above came from, or zero when the document has
    // none. The authoring boundary needs this to decide between editing an
    // existing node and creating one, exactly as the sync policy does.
    [[nodiscard]] doc::NodeId effect_settings_node() const noexcept;

    // Every clip placement in one scene, in document node order. Editor-facing
    // and off the per-frame render path, like channel_keyframes() and
    // effects().
    [[nodiscard]] std::vector<ClipPlacement> clips(SceneId scene) const;

    // Authored lanes in deterministic (order,id) order. If a scene has any
    // unlaned placements, exactly one id-zero implicit lane is included; this
    // query never mutates the document.
    [[nodiscard]] std::vector<TimelineLane> lanes(SceneId scene) const;

    // Valid authored groups in parent-before-child hierarchy order. The
    // transform is the unanimated/base projection; evaluate() carries the
    // frame-specific form when group channels are present.
    [[nodiscard]] std::vector<SceneGroup> groups(SceneId scene) const;

    // Semantic hierarchy errors for groups and grouped drawables, sorted by
    // (subject, kind, related). Malformed node properties remain parse
    // rejections and are counted in SceneCounters instead.
    [[nodiscard]] std::vector<GroupDiagnostic> group_diagnostics(
        SceneId scene) const;

    // First-class effect nodes in deterministic target/order/id order, carrying
    // their authored base values before channel evaluation. This is
    // editor-facing and not on the per-frame render path.
    [[nodiscard]] std::vector<EffectInstance> effects() const;

    // Parsed masks in deterministic target/order/id order. Disabled and
    // semantically malformed masks remain visible here so a later Mask panel
    // can repair them; only valid active masks enter evaluate().mask_programs.
    [[nodiscard]] std::vector<MaskInstance> masks(SceneId scene) const;

    // Stable semantic mask errors sorted by (subject, kind, related). Scalar
    // type/non-finite errors are parse rejections in SceneCounters; path,
    // ownership, ordering, and capacity errors are represented here.
    [[nodiscard]] std::vector<MaskDiagnostic> mask_diagnostics(
        SceneId scene) const;

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
        std::optional<doc::NodeId> parent_group;
        core::i64 group_order{};
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
        bool locked{};
        bool visible{true};
    };

    struct LightEntry final {
        std::optional<SceneId> scene;
        bool directional{true};
        Vec3 vector{};
        Vec3 color{1.0F, 1.0F, 1.0F};
        float intensity{1.0F};
        float range{10.0F};
        bool locked{};
        bool visible{true};
    };

    struct MediaEntry final {
        std::optional<SceneId> scene;
        std::optional<doc::NodeId> parent_group;
        core::i64 group_order{};
        MeshKey mesh{Primitive::Quad, 1U};
        Vec3 position{};
        Turns yaw{};
        Turns pitch{};
        Turns roll{};
        Vec3 scale{1.0F, 1.0F, 1.0F};
        std::string source;
        doc::NodeId asset{};
        core::u32 sequence_start{};
        core::u32 sequence_count{1U};
        std::optional<MediaFrameRate> frame_rate;
        ClipTiming timing{};
        std::optional<doc::NodeId> lane;
        core::i64 clip_order{};
        LayerStyle style{};
        bool locked{};
        bool visible{true};
    };

    struct LaneEntry final {
        std::optional<SceneId> scene;
        core::i64 order{};
        std::string name{"Track 1"};
        bool muted{};
        bool locked{};
        BlendMode compositing{BlendMode::Normal};
        // The screen the track owns. Authored exactly like a drawable's, which
        // is why the fields below are the same ones ObjectEntry carries.
        Vec3 position{};
        Turns yaw{};
        Turns pitch{};
        Turns roll{};
        Vec3 scale{1.0F, 1.0F, 1.0F};
        LayerStyle style{};
        core::u32 raster_width{default_raster_width};
        core::u32 raster_height{default_raster_height};
        bool aspect_locked{true};
    };

    struct GroupEntry final {
        std::optional<SceneId> scene;
        std::optional<doc::NodeId> parent_group;
        core::i64 group_order{};
        std::string name{"Group"};
        Vec3 position{};
        Turns yaw{};
        Turns pitch{};
        Turns roll{};
        Vec3 scale{1.0F, 1.0F, 1.0F};
        bool locked{};
        bool visible{true};
    };

    struct SettingsEntry final {
        EffectSettings settings{};
    };

    struct ModelEntry final {
        std::optional<SceneId> scene;
        std::optional<doc::NodeId> parent_group;
        core::i64 group_order{};
        Vec3 position{};
        Turns yaw{};
        Turns pitch{};
        Turns roll{};
        Vec3 scale{1.0F, 1.0F, 1.0F};
        std::string source;
        Material material{Vec3{0.72F, 0.72F, 0.74F}, 0.6F, 0.0F,
                          Vec3{0.0F, 0.0F, 0.0F}};
        LayerStyle style{};
        bool locked{};
        bool visible{true};
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
        LevelsParameters levels{};
        LiftGammaGainParameters lift_gamma_gain{};
        ContrastPivotParameters contrast_pivot{};
        ColorMatrixParameters color_matrix{};
        WhiteBalanceParameters white_balance{};
        CurvesParameters curves{};
        FilmGrainParameters film_grain{};
        BoxBlurParameters box_blur{};
        DirectionalBlurParameters directional_blur{};
        ChromaKeyParameters chroma_key{};
        LumaKeyParameters luma_key{};
    };

    struct MaskEntry final {
        std::optional<SceneId> scene;
        std::string name{"Mask"};
        doc::NodeId target{};
        MaskKind kind{MaskKind::Rectangle};
        MaskCombine combine{MaskCombine::Add};
        core::i64 order{};
        bool enabled{true};
        float feather_canvas_px{};
        float expansion_canvas_px{};
        float opacity{1.0F};
        bool invert{};
        MaskBounds bounds{};
    };

    struct MaskPointEntry final {
        std::optional<SceneId> scene;
        doc::NodeId mask{};
        core::i64 order{};
        float x{};
        float y{};
        float in_x{};
        float in_y{};
        float out_x{};
        float out_y{};
    };

    // How a key reaches the next one. Hold steps; Linear interpolates; Cubic
    // uses the canonical tangent carrier below.
    enum class ChannelInterp : core::u8 { Hold, Linear, Cubic };

    struct ChannelKey final {
        core::i64 frame{};
        double value{};
        ChannelInterp interp{ChannelInterp::Linear};
        KeyTangentMode tangent_mode{KeyTangentMode::Auto};
        double in_slope{};
        double out_slope{};
        std::optional<double> in_weight;
        std::optional<double> out_weight;
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
        LaneEntry,
        GroupEntry,
        MediaEntry,
        ModelEntry,
        LightEntry,
        SliceEntry,
        EffectEntry,
        MaskEntry,
        MaskPointEntry,
        ChannelEntry,
        SettingsEntry>;

    // A channel's value at a frame, evaluated in f64 for bit-identical results
    // across nodes; clamped before the first key and after the last. The keys
    // must be non-empty and frame-sorted, as `put` leaves them.
    [[nodiscard]] static double sample_channel(
        const ChannelEntry& channel, core::i64 frame) noexcept;
    [[nodiscard]] static double auto_tangent(
        const ChannelEntry& channel, std::size_t key_index) noexcept;
    [[nodiscard]] static double tangent_slope(
        const ChannelEntry& channel, std::size_t key_index,
        bool incoming) noexcept;
    [[nodiscard]] static bool valid_channel(
        const ChannelEntry& channel) noexcept;

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
    // Whether a node is something an effect may target: a drawable, or a
    // track, whose screen carries a look for whatever is playing on it.
    [[nodiscard]] bool effect_target(doc::NodeId id) const noexcept;
    struct GroupResolution final {
        Mat4 world{};
        bool locked{};
        bool visible{true};
    };
    struct GroupLocal final {
        Vec3 position{};
        Turns yaw{};
        Turns pitch{};
        Turns roll{};
        Vec3 scale{1.0F, 1.0F, 1.0F};
    };
    struct GroupProjection final {
        std::map<doc::NodeId, GroupResolution> resolved;
        std::vector<SceneGroup> groups;
        std::vector<GroupDiagnostic> diagnostics;
    };
    struct MaskProjection final {
        std::vector<MaskInstance> masks;
        std::vector<MaskProgram> programs;
        std::vector<MaskDiagnostic> diagnostics;
    };
    [[nodiscard]] GroupProjection resolve_groups(
        SceneId scene,
        const std::map<doc::NodeId, GroupLocal>* local_overrides = nullptr)
        const;
    [[nodiscard]] MaskProjection resolve_masks(
        SceneId scene, const GroupProjection& groups) const;
    void refresh_duplicate_cameras() noexcept;
};

} // namespace mediaserver::scene
