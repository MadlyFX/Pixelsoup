#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/geometry.hpp>
#include <mediaserver/scene/math.hpp>

#include <array>
#include <optional>
#include <string>
#include <vector>

// The evaluated scene: what the renderer draws, and the last place the answer
// is still a number rather than a pixel.
//
// A Scene is a value, not a graph. It has no parents, no children, no
// invalidation, and nothing lazy in it. Every transform is already composed to
// world space and every animated quantity is already resolved at a show frame,
// because the thing this type exists to make possible is a single comparison:
// two nodes hold equal Scenes, or the show is broken.
//
// That comparison is `digest()`. The frame digest the test pattern uses cannot
// survive this phase - two nodes rendering different slices of one canvas
// produce deliberately different pixels, so equal output is no longer the
// signal. The scene digest replaces it and is strictly better: it compares the
// replicated state itself rather than a rendering of it, so it catches
// divergence one frame after it happens instead of whenever it becomes
// visible, and it is meaningful between nodes with different slices, different
// output resolutions, and different GPUs.
namespace mediaserver::scene {

// A scene is identified by the document node that defines its stage. Zero
// denotes the implicit compatibility scene when a document has no stage nodes
// (and remains an ordinary authored identity if a stage node itself is id 0).
using SceneId = doc::NodeId;
inline constexpr SceneId legacy_scene_id{};

// Show position, expressed without depending on the transport.
//
// The frame ordinal and the rate are carried rather than a time in seconds
// because that is what is exactly replicated: every node in the cohort agrees
// on the integer frame, and any two of them derive the same instant from it by
// exact arithmetic. A seconds value computed upstream and distributed would
// have been rounded once already.
struct SceneTime final {
    core::u64 frame{};
    core::u32 rate_numerator{60U};
    core::u32 rate_denominator{1U};

    bool operator==(const SceneTime&) const = default;

    // Seconds since frame zero. Exact for any frame count a show can reach:
    // the integer converts to binary64 without loss and the two operations
    // that follow are correctly rounded, so every node computes the same bits.
    [[nodiscard]] double seconds() const noexcept;
};

// A media source's native frame cadence. Video placements persist this exact
// rational so every render node maps replicated show time to the same source
// ordinal without consulting local decoder metadata. Image sequences omit it
// and retain their authored one-file-per-show-frame behaviour.
struct MediaFrameRate final {
    core::u32 numerator{};
    core::u32 denominator{};

    bool operator==(const MediaFrameRate&) const = default;
};

// Where a clip sits on the show timeline and which part of its source it
// plays. This is the value a video editor manipulates: the same clip trimmed
// twice and placed twice is two placements of one source, not two copies of a
// file.
//
// Everything here is an exact frame count, never a duration in seconds, for
// the same reason SceneTime carries an ordinal: every node has to agree on the
// frame a cut happens on, and a seconds value would have been rounded already.
//
// A duration of zero is the unplaced form and means "always on, looping from
// show frame zero" - the behaviour every clip had before placement existed, so
// a project authored without it keeps playing exactly as it did.
struct ClipTiming final {
    // The show frame the clip becomes visible on.
    core::i64 start{};
    // How many show frames it occupies. Zero is the unplaced legacy form.
    core::i64 duration{};
    // Trim: which source frame the clip's first show frame shows.
    core::i64 source_in{};
    // Opacity ramps at the head and tail, in show frames. A fade to nothing
    // over the scene's black surround is the fade to black an operator means;
    // two clips whose ramps overlap are a cross dissolve.
    core::i64 fade_in{};
    core::i64 fade_out{};

    [[nodiscard]] bool placed() const noexcept { return duration > 0; }
    [[nodiscard]] core::i64 last_frame() const noexcept {
        return start + duration - 1;
    }

    bool operator==(const ClipTiming&) const = default;
};

// The show's own repeat: the inclusive frame range the transport plays and
// whether it wraps at the end of it.
//
// Evaluation reads this for one purpose - deciding what to warm. A clip the
// show is about to come back round onto needs its frames decoded before the
// wrap for the same reason a clip the playhead is walking towards does, and a
// clip that stays on air across the wrap needs them most of all: its source is
// rarely the length of the show, so the wrap moves it to an unrelated part of
// its own file with no cut to announce it.
//
// It never decides what is drawn. An absent or disabled loop simply means
// nothing is warmed across a wrap, and the range travels with the transport
// anchor, so every node warms the same clips on the same frame.
struct ShowLoop final {
    core::u64 first{};
    core::u64 last{};
    bool enabled{};

    bool operator==(const ShowLoop&) const = default;
};

// Whether a placed clip is on air at `frame`. An unplaced clip is always on.
[[nodiscard]] bool clip_visible(const ClipTiming& timing,
                                core::i64 frame) noexcept;

// Whether a placed clip's first frame is within `lead` frames ahead of `frame`,
// so a decoder can be walked to it before the cut rather than after it. False
// for a clip already on air and for one that is never placed.
[[nodiscard]] bool clip_starts_within(const ClipTiming& timing, core::i64 frame,
                                      core::i64 lead) noexcept;

// The opacity multiplier the head and tail ramps impose at `frame`, in [0, 1].
// One outside the ramps, and one everywhere for an unplaced clip.
//
// Exact: the ramp is a ratio of two frame counts, so every node computes the
// same bits from the same integers rather than accumulating a per-frame step.
[[nodiscard]] float clip_fade(const ClipTiming& timing,
                              core::i64 frame) noexcept;

// Lightweight project metadata for scene choosers. The full environment is
// carried by an evaluated Scene; callers listing scenes only need identity,
// display name, and canvas geometry.
struct SceneInfo final {
    SceneId id{};
    std::string name;
    Canvas canvas{};

    bool operator==(const SceneInfo&) const = default;
};

// Surface response, in the vocabulary a physically based shader wants. The
// current shader uses base colour, roughness, and emission; metallic is
// carried because the material a document author writes should not have to
// change when the shading model grows into it.
struct Material final {
    Vec3 base_color{0.8F, 0.8F, 0.8F};
    float roughness{0.5F};
    float metallic{0.0F};
    Vec3 emissive{0.0F, 0.0F, 0.0F};

    bool operator==(const Material&) const = default;
};

// How a drawable is composited over the layers behind it. Kept in the
// evaluated scene rather than in the renderer so every render node makes the
// same choice from replicated state.
enum class BlendMode : core::u8 {
    Normal,
    Add,
    Multiply,
    Screen,
};

// The format a screen is born with when nothing authored one. 16:9 at HD is
// what almost every plate on a volume already is, so the common case needs no
// decision from the operator at all.
inline constexpr core::u32 default_raster_width = 1'920U;
inline constexpr core::u32 default_raster_height = 1'080U;
// A raster is a picture size, not an arbitrary integer. The ceiling is far
// above any real wall and exists so the aspect division below can never be
// handed something absurd.
inline constexpr core::u32 max_raster_extent = 65'536U;

// A track and the screen it owns, projected for editor and renderer use.
//
// A track is not a row that clips happen to be filed under - it is a plane in
// the scene, and the clips in it are what is playing on that plane. So the
// transform here is the one their pictures are drawn with, and a clip's own
// transform is not read at all while it is on a screen.
//
// Node id zero is reserved for the implicit compatibility lane used by
// unlaned placements; it is never inserted into the document by evaluation,
// and it owns no screen - its clips are ordinary scene elements standing on
// their own transforms.
struct TimelineLane final {
    doc::NodeId id{};
    SceneId scene{};
    core::i64 order{};
    std::string name{"Track 1"};
    bool muted{};
    bool locked{};
    BlendMode compositing{BlendMode::Normal};
    bool implicit{};
    // The screen's placement. `scale` is the plane's extent in world units;
    // the quad it is applied to spans one unit, so a scale of (16, 9, 1) is a
    // sixteen-by-nine screen.
    Vec3 position{};
    Turns yaw{};
    Turns pitch{};
    Turns roll{};
    Vec3 scale{1.0F, 1.0F, 1.0F};
    // Composited over whatever is behind the screen, after every clip on it
    // has been composited onto the screen. See `opacity` on the evaluated
    // clips for the one case where those two orders differ.
    float opacity{1.0F};
    core::u32 raster_width{default_raster_width};
    core::u32 raster_height{default_raster_height};
    // When set, `scale` above already has the derived height in it: this
    // projection reports what renders, not what the document happens to hold,
    // so an Inspector showing these values is not showing a stale number.
    bool aspect_locked{true};

    bool operator==(const TimelineLane&) const = default;
};

// The plane extent a screen actually renders with. Identity when the lock is
// open; otherwise the height follows the width and the raster, and a raster
// with a zero extent falls back to the authored height rather than dividing
// by it.
[[nodiscard]] Vec3 screen_extent(const Vec3& scale, core::u32 raster_width,
                                 core::u32 raster_height,
                                 bool aspect_locked) noexcept;

// Deterministic hierarchy validation. Invalid groups and every drawable below
// them are omitted from evaluation, while these plain-data records remain
// available to the Director for a truthful hierarchy/error presentation.
enum class GroupDiagnosticKind : core::u8 {
    MissingParent,
    CrossSceneParent,
    Cycle,
    InvalidAncestor,
};

struct GroupDiagnostic final {
    GroupDiagnosticKind kind{GroupDiagnosticKind::MissingParent};
    doc::NodeId subject{};
    doc::NodeId related{};

    bool operator==(const GroupDiagnostic&) const = default;
};

// One valid authored group projected for the renderer-independent hierarchy.
// position/rotation/scale are local values at the evaluated frame; world is
// the recursively composed transform. `locked` and `visible` are authored on
// this group, while the effective fields include every ancestor.
struct SceneGroup final {
    doc::NodeId node{};
    SceneId scene{};
    std::string name{"Group"};
    std::optional<doc::NodeId> parent_group;
    core::i64 group_order{};
    Vec3 position{};
    Turns yaw{};
    Turns pitch{};
    Turns roll{};
    Vec3 scale{1.0F, 1.0F, 1.0F};
    Mat4 world{};
    bool locked{};
    bool visible{true};
    bool effective_locked{};
    bool effective_visible{true};

    bool operator==(const SceneGroup&) const = default;
};

// First-class coverage masks. A drawable mask is authored in normalized
// source-layer coordinates. A group has no source plane of its own, so a mask
// targeting a group is instead authored in normalized project-canvas
// coordinates and remains a group-target program for the later isolated group
// composite; it is never silently cloned into descendant layer space.
enum class MaskKind : core::u8 {
    Rectangle,
    Ellipse,
    // The final control point connects back to the first; open paths are not
    // part of the mask contract.
    Bezier,
};

enum class MaskCombine : core::u8 {
    Add,
    Subtract,
    Intersect,
    Difference,
};

enum class MaskCoordinateSpace : core::u8 {
    SourceLayer,
    AuthoredCanvas,
};

// Rectangle and Ellipse use explicit normalized bounds before their optional
// rotation is applied about the bounds centre. Equal or reversed bounds are
// malformed rather than repaired by swapping endpoints.
struct MaskBounds final {
    float left{};
    float top{};
    float right{1.0F};
    float bottom{1.0F};

    bool operator==(const MaskBounds&) const = default;
};

// One child scene.mask_point node. Handles are absolute control positions in
// the same normalized coordinate space as position (not relative offsets), so
// a future rasterizer has no convention to infer from editor state.
struct MaskPoint final {
    doc::NodeId source{};
    SceneId scene{};
    doc::NodeId mask{};
    core::i64 order{};
    float x{};
    float y{};
    float in_x{};
    float in_y{};
    float out_x{};
    float out_y{};

    bool operator==(const MaskPoint&) const = default;
};

// One parsed mask, including disabled and semantically malformed masks. The
// latter remain visible to diagnostics/editor queries but never enter an
// executable MaskProgram. Bezier points are sorted by (order, child node id).
struct MaskInstance final {
    doc::NodeId source{};
    SceneId scene{};
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
    // Clockwise turns in target pixel space. Keeping rotation separate from
    // the bounds leaves exact numeric crops available while the viewport can
    // expose the familiar transform ring.
    float rotation_turns{};
    std::vector<MaskPoint> points;

    bool operator==(const MaskInstance&) const = default;
};

// Active, valid operations for exactly one authored target. Operations are
// strict (order, mask node id) order. The first operation is always Add and
// seeds coverage; subsequent coverage b folds into a as Add=max(a,b),
// Subtract=a*(1-b), Intersect=min(a,b), Difference=abs(a-b). An absent program
// is the identity. Group lock/visibility do not alter this replicated program.
struct MaskProgram final {
    doc::NodeId target{};
    MaskCoordinateSpace coordinate_space{MaskCoordinateSpace::SourceLayer};
    std::vector<MaskInstance> operations;

    bool operator==(const MaskProgram&) const = default;
};

enum class MaskDiagnosticKind : core::u8 {
    MissingSceneOwner,
    InvalidSceneOwner,
    MissingTarget,
    MissingTargetSceneOwner,
    CrossSceneTarget,
    InvalidTarget,
    MissingMask,
    CrossSceneMask,
    MalformedPath,
    DuplicatePointOrder,
    InvalidFirstCombine,
    MaskLimitExceeded,
    PointLimitExceeded,
};

// `subject` is the node to repair; `related` is its target, parent mask, or
// colliding point as appropriate. Records are sorted by (subject, kind,
// related), independent of checkpoint-versus-streamed construction order.
struct MaskDiagnostic final {
    MaskDiagnosticKind kind{MaskDiagnosticKind::MissingTarget};
    doc::NodeId subject{};
    doc::NodeId related{};

    bool operator==(const MaskDiagnostic&) const = default;
};

// Exact binary32 fold for every operation after the first Add seed. Inputs are
// coverage values in [0,1], already including the operation's invert/opacity.
[[nodiscard]] float fold_mask_coverage(float accumulated, float coverage,
                                       MaskCombine combine) noexcept;

enum class EffectKind : core::u8 {
    Brightness,
    Saturation,
    Blur,
    Levels,
    LiftGammaGain,
    ContrastPivot,
    ColorMatrix,
    WhiteBalance,
    Curves,
    FilmGrain,
    BoxBlur,
    DirectionalBlur,
    ChromaKey,
    LumaKey,
    // Sentinel used by the render effect registry. New kinds belong before
    // Count; doing so changes the registry's required size and makes a missing
    // execution contract a compile-time error.
    Count,
};

inline constexpr core::u32 max_box_blur_radius_px = 256U;
inline constexpr core::u32 max_box_blur_passes = 8U;
inline constexpr float max_directional_blur_length_px = 512.0F;
inline constexpr core::u32 min_directional_blur_taps = 3U;
inline constexpr core::u32 max_directional_blur_taps = 65U;
inline constexpr core::i32 max_key_morphology_radius_px = 64;

// Box radius is an exact integer canvas-pixel distance. One pass means a
// horizontal and vertical pair; repeated pairs expand support by
// radius*passes on both axes.
struct BoxBlurParameters final {
    core::u32 radius_px{};
    core::u32 passes{1U};

    bool operator==(const BoxBlurParameters&) const = default;
};

// The full centred gather line is `length_px` canvas pixels long. Angle zero
// is the canvas +X axis and one quarter turn is +Y. Tap count is project-wide
// EffectSettings state, not a per-node quality knob.
struct DirectionalBlurParameters final {
    float length_px{};
    Turns angle_turns{};

    bool operator==(const DirectionalBlurParameters&) const = default;
};

// Chroma distance is Euclidean in Rec.709 Cb/Cr space. The key colour is
// straight RGB while layer inputs/results are premultiplied. Positive signed
// morphology dilates foreground coverage and negative values erode it, in
// exact authored-canvas pixels. Final coverage remains gated by source alpha,
// so even positive morphology does not expand layer bounds.
struct ChromaKeyParameters final {
    Vec3 key_color{0.0F, 1.0F, 0.0F};
    float tolerance{0.1F};
    float softness{0.1F};
    core::i32 morphology_px{};
    float spill_suppression{};
    float mix{1.0F};

    bool operator==(const ChromaKeyParameters&) const = default;
};

// Coverage is one through [low_threshold, high_threshold]. The independent
// softness values extend their respective threshold outward; invert flips the
// combined matte. As with Chroma Key, mix zero is an exact identity.
struct LumaKeyParameters final {
    float low_threshold{};
    float high_threshold{1.0F};
    float low_softness{};
    float high_softness{};
    bool invert{};
    float mix{1.0F};

    bool operator==(const LumaKeyParameters&) const = default;
};

struct EffectBoundsExpansion final {
    float x{};
    float y{};

    bool operator==(const EffectBoundsExpansion&) const = default;
};

// Malformed spatial payloads fail closed at document/compositor boundaries.
// Directional angles are wrapped to [0,1); finite lengths remain otherwise
// exact. Bounds describe the symmetric kernel reach in canvas pixels.
[[nodiscard]] std::optional<BoxBlurParameters>
normalized_box_blur(const BoxBlurParameters& value) noexcept;
[[nodiscard]] bool box_blur_is_identity(
    const BoxBlurParameters& value) noexcept;
[[nodiscard]] EffectBoundsExpansion box_blur_bounds(
    const BoxBlurParameters& value) noexcept;
[[nodiscard]] std::optional<DirectionalBlurParameters>
normalized_directional_blur(
    const DirectionalBlurParameters& value) noexcept;
[[nodiscard]] bool directional_blur_is_identity(
    const DirectionalBlurParameters& value) noexcept;
[[nodiscard]] EffectBoundsExpansion directional_blur_bounds(
    const DirectionalBlurParameters& value) noexcept;
[[nodiscard]] std::optional<ChromaKeyParameters>
normalized_chroma_key(const ChromaKeyParameters& value) noexcept;
[[nodiscard]] bool chroma_key_is_identity(
    const ChromaKeyParameters& value) noexcept;
[[nodiscard]] constexpr EffectBoundsExpansion chroma_key_bounds(
    const ChromaKeyParameters&) noexcept {
    return {};
}
[[nodiscard]] std::optional<LumaKeyParameters>
normalized_luma_key(const LumaKeyParameters& value) noexcept;
[[nodiscard]] bool luma_key_is_identity(
    const LumaKeyParameters& value) noexcept;
[[nodiscard]] constexpr EffectBoundsExpansion luma_key_bounds(
    const LumaKeyParameters&) noexcept {
    return {};
}

inline constexpr float min_levels_gamma = 0.01F;
inline constexpr float max_levels_gamma = 10.0F;

// One Levels transfer curve. The four channels in LevelsParameters are stored
// master first and then R/G/B, which is also their exact evaluation order.
struct LevelsChannelParameters final {
    float input_black{};
    float input_white{1.0F};
    float gamma{1.0F};
    float output_black{};
    float output_white{1.0F};

    bool operator==(const LevelsChannelParameters&) const = default;
};

struct LevelsParameters final {
    LevelsChannelParameters master{};
    LevelsChannelParameters red{};
    LevelsChannelParameters green{};
    LevelsChannelParameters blue{};

    bool operator==(const LevelsParameters&) const = default;
};

// Finite endpoints are clamped to 0..1 and gamma to 0.01..10. Equal or
// reversed input endpoints are invalid and return nullopt rather than leaving
// a divide-by-zero decision to a shader. This is the canonical model boundary
// used by both scene and compositor document projections.
[[nodiscard]] std::optional<LevelsParameters>
normalized_levels(const LevelsParameters& value) noexcept;
[[nodiscard]] bool levels_is_identity(const LevelsParameters& value) noexcept;

inline constexpr float min_lift_gamma_gain_lift = -4.0F;
inline constexpr float max_lift_gamma_gain_lift = 4.0F;
inline constexpr float min_lift_gamma_gain_gamma = 0.01F;
inline constexpr float max_lift_gamma_gain_gamma = 10.0F;
inline constexpr float min_lift_gamma_gain_gain = 0.0F;
inline constexpr float max_lift_gamma_gain_gain = 16.0F;

// A three-channel grading operation evaluated component-wise as
// pow(max(rgb + lift, 0), 1/gamma) * gain. Finite authored values are clamped
// to the bounded processor range at the model boundary; non-finite payloads
// are invalid rather than delegated to GPU-specific NaN behaviour.
struct LiftGammaGainParameters final {
    Vec3 lift{};
    Vec3 gamma{1.0F, 1.0F, 1.0F};
    Vec3 gain{1.0F, 1.0F, 1.0F};

    bool operator==(const LiftGammaGainParameters&) const = default;
};

[[nodiscard]] std::optional<LiftGammaGainParameters>
normalized_lift_gamma_gain(const LiftGammaGainParameters& value) noexcept;
[[nodiscard]] bool lift_gamma_gain_is_identity(
    const LiftGammaGainParameters& value) noexcept;

inline constexpr float min_contrast_pivot_contrast = 0.0F;
inline constexpr float max_contrast_pivot_contrast = 4.0F;
inline constexpr float min_contrast_pivot_pivot = 0.0F;
inline constexpr float max_contrast_pivot_pivot = 1.0F;

// A two-scalar contrast operation evaluated as
// (rgb - pivot) * contrast + pivot without an intermediate clamp. Pivot is
// still canonical authored state when contrast is one, even though it cannot
// affect pixels in that identity case.
struct ContrastPivotParameters final {
    float contrast{1.0F};
    float pivot{0.5F};

    bool operator==(const ContrastPivotParameters&) const = default;
};

[[nodiscard]] std::optional<ContrastPivotParameters>
normalized_contrast_pivot(const ContrastPivotParameters& value) noexcept;
[[nodiscard]] bool contrast_pivot_is_identity(
    const ContrastPivotParameters& value) noexcept;

// Both matrix coefficients and RGB offsets use this finite canonical range.
// It is deliberately broad enough for channel extraction and colour-space
// transforms while bounding all replicated and GPU-visible values.
inline constexpr float min_color_matrix_coefficient = -16.0F;
inline constexpr float max_color_matrix_coefficient = 16.0F;

// Row-major 3x3 matrix followed by an RGB offset. Evaluation is exactly
// M * rgb + offset; rows are packed to the GPU as float4(row, offset channel).
struct ColorMatrixParameters final {
    std::array<float, 9U> matrix{
        1.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 1.0F};
    Vec3 offset{};

    bool operator==(const ColorMatrixParameters&) const = default;
};

[[nodiscard]] std::optional<ColorMatrixParameters>
normalized_color_matrix(const ColorMatrixParameters& value) noexcept;
[[nodiscard]] bool color_matrix_is_identity(
    const ColorMatrixParameters& value) noexcept;

inline constexpr float min_white_balance_temperature_kelvin = 2'000.0F;
inline constexpr float max_white_balance_temperature_kelvin = 12'000.0F;
inline constexpr float d65_white_balance_temperature_kelvin = 6'500.0F;
inline constexpr float min_white_balance_tint = -1.0F;
inline constexpr float max_white_balance_tint = 1.0F;
// Tint is a fixed displacement along CIE 1960 UCS v. Positive authored tint
// moves toward magenta, so it subtracts this displacement from v.
inline constexpr float white_balance_tint_uv_scale = 0.008F;

// Canonical authored controls plus their derived row-major linear-sRGB
// adaptation matrix. Model normalization derives the matrix from D65 to the
// authored white point using the documented Bradford implementation in
// scene.cpp; render code never invents a second temperature conversion.
struct WhiteBalanceParameters final {
    float temperature_kelvin{d65_white_balance_temperature_kelvin};
    float tint{};
    std::array<float, 9U> matrix{
        1.0F, 0.0F, 0.0F,
        0.0F, 1.0F, 0.0F,
        0.0F, 0.0F, 1.0F};

    bool operator==(const WhiteBalanceParameters&) const = default;
};

[[nodiscard]] std::optional<WhiteBalanceParameters>
normalized_white_balance(const WhiteBalanceParameters& value) noexcept;
[[nodiscard]] bool white_balance_is_identity(
    const WhiteBalanceParameters& value) noexcept;

// Curves use a fixed-size sampled contract on every render node. Authored
// control points stay compact and editable; model normalization derives these
// 1024 binary32 RGBA-lane samples once at the document boundary. Each sample
// stores master, red, green, and blue respectively (there is no alpha curve).
inline constexpr core::u32 curve_lut_entries = 1'024U;
inline constexpr core::u32 min_curve_control_points = 2U;
inline constexpr core::u32 max_curve_control_points = 32U;

struct CurveControlPoint final {
    float x{};
    float y{};

    bool operator==(const CurveControlPoint&) const = default;
};

struct CurveChannelParameters final {
    std::vector<CurveControlPoint> points{
        CurveControlPoint{0.0F, 0.0F},
        CurveControlPoint{1.0F, 1.0F}};

    bool operator==(const CurveChannelParameters&) const = default;
};

struct CurvesParameters final {
    CurveChannelParameters master{};
    std::optional<CurveChannelParameters> red;
    std::optional<CurveChannelParameters> green;
    std::optional<CurveChannelParameters> blue;
    // Exactly curve_lut_entries values after successful normalization. Vec4
    // lanes are master/R/G/B; optional channels are filled with identity.
    std::vector<Vec4> lut;

    bool operator==(const CurvesParameters&) const = default;
};

// Canonicalizes and samples with the deterministic Fritsch-Carlson/PCHIP
// monotone cubic implementation in scene.cpp. Every present channel must have
// 2..32 finite points, x strictly increasing in [0,1], y in [0,1], and exact
// endpoints (0,0)/(1,1). Malformed curves fail closed.
[[nodiscard]] std::optional<CurvesParameters>
normalized_curves(const CurvesParameters& value) noexcept;
[[nodiscard]] bool curves_is_identity(const CurvesParameters& value) noexcept;
// True only when the authored points are valid and the stored LUT exactly
// matches the deterministic samples they derive. The render frame path uses
// this allocation-free check before uploading the already-evaluated LUT.
[[nodiscard]] bool curves_lut_is_canonical(
    const CurvesParameters& value) noexcept;

enum class FilmGrainMode : core::u8 {
    Monochrome,
    RGB,
};

inline constexpr float min_film_grain_amount = 0.0F;
inline constexpr float max_film_grain_amount = 1.0F;
inline constexpr float min_film_grain_pixel_scale = 1.0F;
inline constexpr float max_film_grain_pixel_scale = 64.0F;
inline constexpr float min_film_grain_chroma_ratio = 0.0F;
inline constexpr float max_film_grain_chroma_ratio = 1.0F;

// Deterministic zero-mean noise. The renderer hashes this replicated seed with
// the exact evaluated show frame, authored canvas-pixel coordinate, and
// channel. Pixel scale controls the size of each canvas-space noise cell; RGB
// mode blends a shared monochrome sample toward independent channel samples by
// chroma_ratio. No local clock or driver random state participates.
struct FilmGrainParameters final {
    float amount{};
    float pixel_scale{1.0F};
    float chroma_ratio{};
    core::u32 seed{};
    FilmGrainMode mode{FilmGrainMode::Monochrome};

    bool operator==(const FilmGrainParameters&) const = default;
};

// Finite scalar controls clamp to their documented bounds. Unknown enum
// values fail closed; the integer seed is already exact canonical state.
[[nodiscard]] std::optional<FilmGrainParameters>
normalized_film_grain(const FilmGrainParameters& value) noexcept;
[[nodiscard]] bool film_grain_is_identity(
    const FilmGrainParameters& value) noexcept;

// An ordered inline operation retained after model evaluation. Brightness and
// Saturation use value; other pointwise colour effects use their typed
// payloads. Spatial effects never enter this list. The renderer compiles these
// records into bounded operation and parameter buffers without changing their
// non-commutative order.
struct InlineEffectOperation final {
    EffectKind kind{EffectKind::Brightness};
    float value{1.0F};
    LevelsParameters levels{};
    LiftGammaGainParameters lift_gamma_gain{};
    ContrastPivotParameters contrast_pivot{};
    ColorMatrixParameters color_matrix{};
    WhiteBalanceParameters white_balance{};
    CurvesParameters curves{};
    FilmGrainParameters film_grain{};

    bool operator==(const InlineEffectOperation&) const = default;
};

// The ordered isolated-layer program. Gaussian Blur uses `value` as its
// canvas-pixel radius; Box and Directional Blur use their typed payloads.
// Keeping this separate from the inline program makes execution scope
// explicit and preserves non-commutative spatial order across scene effects
// and graph modifiers.
struct SpatialEffectOperation final {
    EffectKind kind{EffectKind::Blur};
    float value{};
    BoxBlurParameters box_blur{};
    DirectionalBlurParameters directional_blur{};
    ChromaKeyParameters chroma_key{};
    LumaKeyParameters luma_key{};

    bool operator==(const SpatialEffectOperation&) const = default;
};

// A folded rendering summary of the ordered, first-class effect nodes targeting
// one layer. Effect identity/order remains in Scene::effects; the two operation
// vectors retain the exact inline and isolated-layer programs a draw needs.
struct EffectStack final {
    bool brightness_enabled{};
    float brightness{1.0F};
    // Unshaped product retained so compositor-graph Brightness modifiers join
    // the same stack before its one clamp/gamma response.
    bool brightness_product_valid{};
    double brightness_product{1.0};
    // Colourfulness about Rec.709 luminance in the project's selected effect
    // response domain: linear light for Exposure, encoded display values for
    // Gain. One is identity, zero is monochrome, and values above one increase
    // colourfulness.
    bool saturation_enabled{};
    float saturation{1.0F};
    // Scene effects are evaluated before compositor-graph modifiers, but both
    // belong to one multiplicative program. Preserve the unbounded binary64
    // product across that boundary so only the final consumer clamps to 0..4.
    // Hand-built/older Scene values leave this false and derive the product
    // from `saturation` when a later modifier needs it.
    bool saturation_product_valid{};
    double saturation_product{1.0};
    bool blur_enabled{};
    float blur_radius{}; // canvas pixels
    std::vector<InlineEffectOperation> inline_operations;
    std::vector<SpatialEffectOperation> spatial_operations;

    bool operator==(const EffectStack&) const = default;
};

// What a brightness effect multiplies.
enum class BrightnessCurve : core::u8 {
    // Scene-referred: the multiplier scales radiance before the tone map, so
    // a highlight rolls off instead of clipping. This is what an exposure
    // change physically is, and what the renderer has always done.
    Exposure,
    // Display-referred: the multiplier scales the encoded result. This is what
    // a vision mixer's gain control does - it clips, and it is the behaviour an
    // operator coming from a switcher expects to see.
    Gain,
};

// How the effects an operator drops onto a layer are actually computed.
//
// Project-wide rather than per-effect, and deliberately not in the Inspector.
// These are the numbers a facility sets once for a show and then leaves alone;
// a kernel width beside every blur node would be five controls an operator
// scrolls past and never touches. They live in the document rather than in
// machine-local preferences because they change the image: a node with a
// different kernel would render a different frame, which is exactly the kind
// of divergence the scene digest exists to catch.
struct EffectSettings final {
    // Samples per axis in the separable Gaussian, counting the centre. Odd,
    // and clamped to a range the shader can actually loop over.
    core::u32 blur_taps{9U};
    // The Gaussian's sigma as a fraction of the authored radius. Below about a
    // third the blur reads as a box; above it the kernel's tail is truncated.
    float blur_sigma_ratio{0.34F};
    // How many times the separable pair runs. Repeating a modest kernel is how
    // a very wide blur stays smooth without a very wide tap count.
    core::u32 blur_passes{1U};
    // Samples along the complete centred Directional Blur line. Odd so the
    // authored pixel is always one exact sample; bounded by the shader loop.
    core::u32 directional_blur_taps{17U};
    BrightnessCurve brightness_curve{BrightnessCurve::Exposure};
    // The exponent the folded brightness multiplier is raised to before it is
    // applied. One is the literal multiplier; below one softens the control's
    // response and above one sharpens it.
    float brightness_gamma{1.0F};

    bool operator==(const EffectSettings&) const = default;
};

inline constexpr core::u32 min_blur_taps = 3U;
inline constexpr core::u32 max_blur_taps = 33U;
inline constexpr core::u32 max_blur_passes = 4U;
inline constexpr float min_blur_sigma_ratio = 0.05F;
inline constexpr float max_blur_sigma_ratio = 2.0F;
inline constexpr float min_brightness_gamma = 0.1F;
inline constexpr float max_brightness_gamma = 4.0F;
inline constexpr float max_saturation = 4.0F;

// Saturation is authored in [0, 4]. A non-finite or out-of-contract value is
// identity rather than an accidental colour operation. This single model
// boundary is shared by document evaluation and render-registry tests.
[[nodiscard]] float normalized_saturation(float value) noexcept;

[[nodiscard]] double composed_brightness_product(double left,
                                                 double right) noexcept;
[[nodiscard]] float brightness_from_product(double product,
                                            float response_gamma) noexcept;

// Associative Saturation folding shared by the scene and compositor models.
// Factors are non-negative; zero remains absorbing even after an earlier
// product overflowed. Invalid carrier data composes as identity, while a
// positive overflow resolves to the maximum visible amount.
[[nodiscard]] double composed_saturation_product(double left,
                                                 double right) noexcept;
[[nodiscard]] float saturation_from_product(double product) noexcept;

// The nearest usable settings to `value`: taps forced odd and into range,
// every scalar clamped. Applied at the model boundary so no renderer, and no
// panel, has to guard the same numbers a second time.
[[nodiscard]] EffectSettings clamped(const EffectSettings& value) noexcept;

// One evaluated first-class effect. `source` is its document node and `target`
// is the drawable it processes. Keeping this list preserves identity, order,
// and duplicate effects for the Inspector/compositor even though a drawable
// also carries a compact folded summary for rendering.
struct EffectInstance final {
    doc::NodeId source{};
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

    bool operator==(const EffectInstance&) const = default;
};

struct LayerStyle final {
    float opacity{1.0F};
    BlendMode blend{BlendMode::Normal};
    EffectStack effects{};

    bool operator==(const LayerStyle&) const = default;
};

// One track's screen, resolved at the evaluated frame.
//
// It is in the evaluated Scene for the same reason groups are: a muted track
// still has a plane an operator has to be able to find, select, and move, and
// a value the editor can only get by re-deriving it from the document is a
// value the editor and the renderer can disagree about. Screens draw nothing
// themselves - the clips on them do - so this is carrier data for the gizmo,
// the digest, and anything that later wants to outline an empty one.
struct SceneScreen final {
    doc::NodeId node{};
    SceneId scene{};
    std::string name;
    core::i64 order{};
    // Already carries the aspect lock and any channel driving the track.
    Mat4 world{};
    core::u32 raster_width{default_raster_width};
    core::u32 raster_height{default_raster_height};
    bool aspect_locked{true};
    // Folded exactly as a clip's is, so what the screen contributes to every
    // clip on it is visible here rather than only inside the clips.
    LayerStyle style{};
    bool muted{};
    bool locked{};

    bool operator==(const SceneScreen&) const = default;
};

// One drawable. The mesh is a key rather than geometry: the renderer resolves
// it through its cache, and two instances of the same key share one upload.
struct MeshInstance final {
    MeshKey mesh{};
    Mat4 world{};
    // The inverse transpose of the world basis. Precomputed here rather than
    // in the shader because it is per instance, not per vertex, and because
    // the CPU is where the singular case can be handled honestly.
    Mat4 normal{};
    Material material{};
    // Which document node produced this. Carried for diagnostics and for the
    // selection path that will need it; the renderer ignores it.
    doc::NodeId source{};
    LayerStyle style{};
    // Editor-only interaction state. It does not alter rendering.
    bool locked{};
    // Authored hierarchy identity. `world` above is already parent-composed;
    // these fields let editor/diagnostic consumers relate the evaluated draw
    // back to its canonical sibling position.
    std::optional<doc::NodeId> parent_group;
    core::i64 group_order{};

    bool operator==(const MeshInstance&) const = default;
};

// A clip that has not started yet and the frame it will open on: a decode hint
// the renderer hands to its media source, never a draw. Carries no transform or
// style. Its placement identity names the same decoder continuity the live clip
// will use, so warming one of two cuts from the same file cannot displace the
// other cut's forward-walking decoder.
struct MediaPrewarm final {
    std::string source;
    core::u32 frame_index{};
    doc::NodeId source_node{};

    bool operator==(const MediaPrewarm&) const = default;
};

// A clip textured onto a screen-facing quad, resolved at a show frame.
//
// The scene layer is headless, so this carries a *reference* - the sequence's
// path template and the file number to show at this frame - not pixels. The
// renderer resolves that reference to a texture through its own media cache.
// Two nodes evaluating the same show frame compute the same `frame_index`, so
// they display the same file, frame-exact, without any pixel replication.
struct MediaInstance final {
    // The geometry the clip is drawn on. A screen-facing quad by default.
    MeshKey mesh{Primitive::Quad, 1U};
    Mat4 world{};
    Mat4 normal{};
    // The printf-style sequence template and the file number to load now.
    std::string source;
    core::u32 frame_index{};
    // The half-open range of file numbers this clip cycles over, which
    // `frame_index` is always inside. A renderer reads ahead within it, so the
    // frame after the clip's last is its first rather than one past the end of
    // the file. A zero count is "not stated", and read-ahead stays linear.
    //
    // Deliberately outside scene_hash: this names what will be wanted next, not
    // what is displayed now, and two nodes already agree on the frame shown.
    core::u32 loop_first{};
    core::u32 loop_count{};
    // Stable persistent Library identity authored on the placement. This is
    // deliberately separate from `source_node` (the placement itself): a
    // machine-local derivative may change what is decoded without changing
    // either document identity.
    doc::NodeId asset{};
    // Which document node produced this, for the selection/manipulation path.
    doc::NodeId source_node{};
    // Zero for a loose scene layer; otherwise the explicit track whose screen
    // supplies this clip's transform. Render ordering uses group_order for the
    // former and the timeline compositing order for the latter.
    doc::NodeId timeline_lane{};
    // Dense order assigned after lane/clip sorting. It is the deterministic
    // over/under order for media placements and is independent of NodeId.
    core::u64 compositing_order{};
    LayerStyle style{};
    bool locked{};
    std::optional<doc::NodeId> parent_group;
    core::i64 group_order{};

    bool operator==(const MediaInstance&) const = default;
};

// An imported 3D model placed in the scene. The scene layer is headless, so
// this carries only a reference (the file path) and the transform; the renderer
// loads the geometry and material behind its own cache, exactly as media carries
// a path the renderer resolves to a texture. A node without the file draws
// nothing rather than failing.
struct ModelInstance final {
    Mat4 world{};
    Mat4 normal{};
    std::string source;
    doc::NodeId source_node{};
    // Imported material factors are used by the renderer when available;
    // these authored values provide a deterministic fallback and an Inspector
    // override path.
    Material material{Vec3{0.72F, 0.72F, 0.74F}, 0.6F, 0.0F,
                      Vec3{0.0F, 0.0F, 0.0F}};
    LayerStyle style{};
    bool locked{};
    std::optional<doc::NodeId> parent_group;
    core::i64 group_order{};

    bool operator==(const ModelInstance&) const = default;
};

struct DirectionalLight final {
    // The direction the light travels, not the direction towards it.
    Vec3 direction{0.0F, -1.0F, 0.0F};
    Vec3 color{1.0F, 1.0F, 1.0F};
    float intensity{1.0F};
    // Which document node produced this, for the editor's selection path; the
    // renderer ignores it.
    doc::NodeId source{};
    bool locked{};

    bool operator==(const DirectionalLight&) const = default;
};

struct PointLight final {
    Vec3 position{};
    Vec3 color{1.0F, 1.0F, 1.0F};
    float intensity{1.0F};
    // Distance at which the light contributes nothing. A hard cutoff rather
    // than a physical inverse square that never quite reaches zero, because a
    // bounded radius is what lets the renderer cull and what an operator can
    // reason about.
    float range{10.0F};
    // Which document node produced this, for the editor's selection path.
    doc::NodeId source{};
    bool locked{};

    bool operator==(const PointLight&) const = default;
};

// Ambient surround, standing in for the image-based lighting that replaces it.
// Two colours and a strength is enough to keep unlit sides from going black
// and to give the frame a horizon.
struct Environment final {
    Vec3 sky_color{0.05F, 0.07F, 0.10F};
    Vec3 ground_color{0.02F, 0.02F, 0.02F};
    float ambient{1.0F};

    bool operator==(const Environment&) const = default;
};

// Which node renders which rectangle of the canvas.
//
// This lives in the scene, and therefore in the document, because it has to be
// replicated: a node that disagreed with the Director about its own slice
// would render a correct image of the wrong region, which is the hardest kind
// of fault to see. A node selects its binding by its own index and never
// invents one.
struct SliceBinding final {
    core::u32 node_index{};
    CanvasRect rect{};

    bool operator==(const SliceBinding&) const = default;
};

// Caps, so the renderer sizes its buffers once and a runaway document is
// refused rather than absorbed. Generous against what a single node can draw
// at rate today, and far below what would make the upload interesting.
inline constexpr core::u32 max_scene_instances = 4'096U;
inline constexpr core::u32 max_scene_directional_lights = 4U;
inline constexpr core::u32 max_scene_point_lights = 64U;
inline constexpr core::u32 max_scene_slices = 256U;
inline constexpr core::u32 max_scene_media = 64U;
// How far ahead of a cut a clip's first frame is named for decoding, in show
// frames. Long enough to cover a seek and a keyframe walk on a long-GOP file;
// short enough that a timeline of short clips does not ask for a dozen files at
// once. Replicated state like everything else here: every node warms the same
// clips at the same frame, so a cut is no softer on one node than another.
inline constexpr core::i64 media_prewarm_lead = 12;
inline constexpr core::u32 max_scene_models = 256U;
inline constexpr core::u32 max_scene_effects = 512U;
inline constexpr core::u32 max_scene_groups = 1'024U;
inline constexpr core::u32 max_scene_lanes = 256U;
inline constexpr core::u32 max_scene_masks = 512U;
inline constexpr core::u32 max_mask_points_per_mask = 256U;
inline constexpr core::u32 max_scene_mask_points = 16'384U;
inline constexpr core::u32 max_mask_name_bytes = 128U;
inline constexpr float max_mask_feather_canvas_px = 16'384.0F;
inline constexpr float max_mask_expansion_canvas_px = 16'384.0F;
// Keyframe channels the model applies per frame, and keys one channel holds.
// Channels are not drawn, so these bound parse and per-frame work rather than a
// GPU buffer; past the cap the model truncates deterministically in node-id
// order, exactly as the drawable caps above do.
inline constexpr core::u32 max_scene_channels = 256U;
inline constexpr core::u32 max_channel_keys = 64U;

// A transform component a keyframe channel can drive. The order is the array
// index the model uses when applying overrides, and the value a channel node's
// component names; the editor uses it to place a channel on the right timeline
// row. Position/Rotation/Scale, X/Y/Z (rotation as yaw/pitch/roll).
enum class PropertyChannel : core::u8 {
    PositionX,
    PositionY,
    PositionZ,
    RotationYaw,
    RotationPitch,
    RotationRoll,
    ScaleX,
    ScaleY,
    ScaleZ,
    Opacity,
    Brightness,
    Saturation,
    BlurRadius,
    FilmGrainAmount,
    BoxBlurRadius,
    DirectionalBlurLength,
    ChromaKeyMix,
    LumaKeyMix,
    Count,
};

// Source compatibility for the first editor implementation. New code should
// use PropertyChannel: channels now animate compositing and effect properties
// as well as transforms.
using TransformChannel = PropertyChannel;

// How a timeline key reaches the next one. Kept public alongside the keyframe
// view so an editor can draw the same curve the evaluator actually samples.
enum class KeyInterpolation : core::u8 {
    Hold,
    Linear,
    Cubic,
};

enum class KeyTangentMode : core::u8 {
    Auto,
    Aligned,
    Broken,
    Flat,
};

// Canonical tangent projection for one Cubic key. Slopes are value per show
// frame. A missing weight means the evaluator uses one third of the adjacent
// interval; authored weights are positive show-frame distances. Auto exposes
// its derived PCHIP slope but never an authored weight. Aligned exposes one
// shared slope for both directions and retains its independent weights.
struct KeyTangent final {
    KeyTangentMode mode{KeyTangentMode::Auto};
    double in_slope{};
    double out_slope{};
    std::optional<double> in_weight;
    std::optional<double> out_weight;

    bool operator==(const KeyTangent&) const = default;
};

// Weighted Cubic sampling always executes exactly this many x(t) bisections.
// There is deliberately no convergence exit, so all replicated nodes perform
// the same binary64 operations even when their libm implementations differ.
inline constexpr core::u32 key_bezier_bisection_iterations = 48U;

// The frames one node/component keyframe channel holds, for the editor's
// timeline. Not part of the drawable scene - the channels already show in the
// transforms - so this is queried from the model on demand rather than carried
// on every Scene. One entry per (target, component) that has keys.
struct ChannelKeyframes final {
    doc::NodeId target{};
    PropertyChannel component{};
    std::vector<core::i64> frames; // deduped, ascending
    // Parallel to frames. Older/headless callers that only need diamonds may
    // leave these empty; SceneModel always fills both.
    std::vector<double> values;
    std::vector<KeyInterpolation> interpolations;
    // Parallel to frames. Hold and Linear entries are nullopt: they have no
    // dormant tangent state. Every Cubic entry has one canonical projection,
    // including legacy Cubic keys which project as Auto.
    std::vector<std::optional<KeyTangent>> tangents;

    bool operator==(const ChannelKeyframes&) const = default;
};

struct Scene final {
    // The stage node this value was evaluated from. The implicit legacy scene
    // uses legacy_scene_id when the document contains no stage nodes.
    SceneId id{};
    // Exact replicated show ordinal used by deterministic temporal effects.
    // It is kept as an integer all the way to the shader (split into two u32
    // lanes), so long-running shows never lose frame identity to float
    // conversion.
    core::u64 show_frame{};
    Canvas canvas{1'920U, 1'080U};
    Camera camera{};
    Environment environment{};
    // Project-wide, not scene-scoped: every scene of one show renders its
    // effects the same way. Carried on the evaluated value rather than read
    // from the document by the renderer, so it is replicated and digested like
    // everything else the picture depends on.
    EffectSettings effect_settings{};
    // Valid groups in deterministic parent-before-child hierarchy order.
    // Invisible groups remain here so an editor can select and reveal them;
    // their drawables are omitted from the vectors below.
    std::vector<SceneGroup> groups;
    // Track screens in (order, node) order. Muted tracks are present and
    // marked rather than omitted, so an operator can still find the plane
    // they switched off.
    std::vector<SceneScreen> screens;
    // Invalid hierarchy edges are plain replicated carrier data. Rendering
    // ignores them, while the digest and editor can distinguish an empty
    // scene from one whose content was omitted for a repairable group error.
    std::vector<GroupDiagnostic> group_diagnostics;
    // All parsed masks (including disabled/malformed state), their executable
    // active subset grouped by exact target, and stable semantic diagnostics.
    // Keeping all three in the evaluated value makes authored identity and
    // invalid state participate in the cross-node scene digest.
    std::vector<MaskInstance> masks;
    std::vector<MaskProgram> mask_programs;
    std::vector<MaskDiagnostic> mask_diagnostics;
    // Sorted by mesh key and then by source node id. Sorted rather than merely
    // grouped because the renderer draws one batch per distinct mesh and the
    // order of those batches is part of what two nodes have to agree on.
    std::vector<MeshInstance> instances;
    // Clips, kept separate from procedural instances because the renderer draws
    // them in their own textured pass.
    std::vector<MediaInstance> media;
    // Clips whose first frame is a few frames away, and the frame each will
    // open on. Nothing here is drawn: it exists so a decoder can be walked to a
    // cut before the cut, because reaching an arbitrary frame of a long-GOP
    // file costs a seek and several frames of decode, and a clip first asked
    // for on the frame it goes on air cannot answer in time.
    //
    // Deliberately outside scene_hash, like MediaInstance's loop window: it
    // names what will be wanted, never what is shown.
    std::vector<MediaPrewarm> media_prewarm;
    // Imported models, drawn in the lit pass with geometry the renderer loads.
    std::vector<ModelInstance> models;
    // Ordered by target, authored order, then effect node id.
    std::vector<EffectInstance> effects;
    std::vector<DirectionalLight> directional_lights;
    std::vector<PointLight> point_lights;
    std::vector<SliceBinding> slices;

    // The rectangle bound to this node, or null when the document binds none.
    // A node with no binding renders nothing rather than guessing at the whole
    // canvas: an unmapped output should be black and obviously unmapped.
    [[nodiscard]] const CanvasRect* slice_for(core::u32 node_index) const noexcept;
};

// FNV-1a over every value in the scene, in declaration order, reading floats
// as their bit patterns rather than their decimal forms.
//
// Bit patterns because that is the question being asked. Two nodes that agree
// to seven decimal places and differ in the last bit will render a visible
// seam, so a digest that called them equal would be worse than no digest.
//
// Padding is never hashed - each field is fed in by name - so the value is
// stable across compilers and does not depend on struct layout.
[[nodiscard]] core::u64 digest(const Scene& scene) noexcept;

// Stable digest of the editor-facing channel carrier, including canonical
// Cubic tangent mode, slopes, optional weights, and absence on Hold/Linear.
[[nodiscard]] core::u64 digest(const ChannelKeyframes& channel) noexcept;

} // namespace mediaserver::scene
