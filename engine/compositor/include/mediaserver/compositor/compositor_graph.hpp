#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/scene/scene.hpp>

#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

namespace mediaserver::compositor {

enum class NodeKind : core::u8 {
    SceneLayer,
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
    Matte,
    RadialArray,
    LedCarProcess,
    Composite,
    Output,
    // Lays images over a base with one blend mode and opacity, or - with
    // nothing wired to LAYERS - blends the base against a constant.
    Merge,
    // A generated image: a solid colour or a gradient across the canvas.
    Background,
};

// One past the last kind, for tables indexed by kind. Output is not last.
inline constexpr std::size_t node_kind_count =
    static_cast<std::size_t>(NodeKind::Background) + 1U;

[[nodiscard]] constexpr std::string_view
node_kind_name(NodeKind kind) noexcept {
    switch (kind) {
    case NodeKind::SceneLayer:
        return "Scene Layer";
    case NodeKind::Brightness:
        return "Brightness";
    case NodeKind::Saturation:
        return "Saturation";
    case NodeKind::Blur:
        return "Gaussian Blur";
    case NodeKind::Levels:
        return "Levels";
    case NodeKind::LiftGammaGain:
        return "Lift/Gamma/Gain";
    case NodeKind::ContrastPivot:
        return "Contrast/Pivot";
    case NodeKind::ColorMatrix:
        return "Colour Matrix";
    case NodeKind::WhiteBalance:
        return "White Balance";
    case NodeKind::Curves:
        return "Curves";
    case NodeKind::FilmGrain:
        return "Film Grain / Noise";
    case NodeKind::BoxBlur:
        return "Box Blur";
    case NodeKind::DirectionalBlur:
        return "Directional Blur";
    case NodeKind::ChromaKey:
        return "Chroma Key";
    case NodeKind::LumaKey:
        return "Luma Key";
    case NodeKind::Matte:
        return "Matte";
    case NodeKind::RadialArray:
        return "Radial Array";
    case NodeKind::LedCarProcess:
        return "LED Car Process";
    case NodeKind::Composite:
        return "Composite";
    case NodeKind::Output:
        return "Output";
    case NodeKind::Merge:
        return "Merge";
    case NodeKind::Background:
        return "Background";
    }
    return "Node";
}

// Effects come in two kinds, carried by different wires. An effect (Effect)
// - colour, grain - is applied pointwise while a drawable is shaded; a
// spatial filter (Filter) - a blur or a key - reads the finished, isolated
// layer. Every effect therefore runs before every filter, and the separate
// value types make that order part of how a graph is wired.
enum class PortValueType : core::u8 {
    Image,
    Effect,
    TransformSet,
    Filter,
};

enum class PortDirection : core::u8 {
    Input,
    Output,
};

struct PortDescriptor final {
    core::u8 index{};
    std::string_view name;
    PortValueType type{PortValueType::Image};
    PortDirection direction{PortDirection::Input};
    // Inputs may accept more than one edge. Outputs may fan out when true.
    bool multiple{};
    bool required{};

    bool operator==(const PortDescriptor&) const = default;
};

[[nodiscard]] std::span<const PortDescriptor>
input_ports(NodeKind kind) noexcept;
[[nodiscard]] std::span<const PortDescriptor>
output_ports(NodeKind kind) noexcept;
[[nodiscard]] const PortDescriptor* find_input_port(NodeKind kind,
                                                    core::u8 index) noexcept;
[[nodiscard]] const PortDescriptor* find_output_port(NodeKind kind,
                                                     core::u8 index) noexcept;

struct SceneLayerParameters final {
    doc::NodeId scene_node{};
    bool operator==(const SceneLayerParameters&) const = default;
};

struct BrightnessParameters final {
    float amount{1.0F};
    bool operator==(const BrightnessParameters&) const = default;
};

struct SaturationParameters final {
    float amount{1.0F};
    bool operator==(const SaturationParameters&) const = default;
};

// One canonical Levels payload is shared with first-class scene effects; the
// compositor wrapper contributes graph ports and editor position, not another
// normalization contract.
using LevelsParameters = scene::LevelsParameters;
using LiftGammaGainParameters = scene::LiftGammaGainParameters;
using ContrastPivotParameters = scene::ContrastPivotParameters;
using ColorMatrixParameters = scene::ColorMatrixParameters;
using WhiteBalanceParameters = scene::WhiteBalanceParameters;
using CurvesParameters = scene::CurvesParameters;
using FilmGrainParameters = scene::FilmGrainParameters;
using BoxBlurParameters = scene::BoxBlurParameters;
using DirectionalBlurParameters = scene::DirectionalBlurParameters;
using ChromaKeyParameters = scene::ChromaKeyParameters;
using LumaKeyParameters = scene::LumaKeyParameters;

enum class MatteMode : core::u8 {
    Alpha = 0U,
    Luma = 1U,
};

struct MatteParameters final {
    MatteMode mode{MatteMode::Alpha};
    bool invert{};
    // Clamp recovered straight matte RGB before the Rec.709 luma dot. This is
    // useful for alpha-bearing HDR/out-of-gamut inputs; alpha mode ignores it.
    bool clamp_input{true};

    bool operator==(const MatteParameters&) const = default;
};

struct BlurParameters final {
    float radius{};
    bool operator==(const BlurParameters&) const = default;
};

struct RadialArrayParameters final {
    core::u32 count{9U};
    scene::Vec3 center{};
    float radius{1.0F};
    scene::Turns start_angle{};
    bool operator==(const RadialArrayParameters&) const = default;
};

// A purpose-built nine-plate volume for virtual car process work. The first
// eight image inputs are arranged clockwise around the vehicle and the ninth
// is the ceiling/reflection plate. Each slot transform is composed *before*
// the connected SceneLayer's evaluated world transform, so that layer's own
// transform remains a local correction rather than being replaced.
struct LedCarProcessParameters final {
    scene::Vec3 position{};
    scene::Turns yaw{};
    scene::Turns pitch{};
    scene::Turns roll{};
    // Wall plate size and radius are independent from the overhead plate so
    // reflection coverage can be tuned without disturbing the surround.
    float plate_scale{1.0F};
    float array_scale{4.0F};
    float reflection_height{4.0F};
    float reflection_scale{1.0F};
    // Normalized width feather applied independently at both horizontal
    // edges of the eight wall plates. The ceiling plate is deliberately
    // excluded. [0, 0.5], where zero is a hard edge.
    float horizontal_blend{0.05F};

    bool operator==(const LedCarProcessParameters&) const = default;
};

struct CompositeParameters final {
    bool operator==(const CompositeParameters&) const = default;
};

// How one image lands on another. Normal, Add, Multiply and Screen share
// scene::BlendMode's values; the rest exist only on a Merge.
enum class MergeBlendMode : core::u8 {
    Normal = 0U,
    Add = 1U,
    Multiply = 2U,
    Screen = 3U,
    Subtract = 4U,
    Overlay = 5U,
    Lighten = 6U,
    Darken = 7U,
    Difference = 8U,
};

inline constexpr core::u8 merge_blend_mode_count = 9U;

[[nodiscard]] constexpr std::string_view
merge_blend_mode_name(MergeBlendMode mode) noexcept {
    switch (mode) {
    case MergeBlendMode::Normal:
        return "Normal";
    case MergeBlendMode::Add:
        return "Add";
    case MergeBlendMode::Multiply:
        return "Multiply";
    case MergeBlendMode::Screen:
        return "Screen";
    case MergeBlendMode::Subtract:
        return "Subtract";
    case MergeBlendMode::Overlay:
        return "Overlay";
    case MergeBlendMode::Lighten:
        return "Lighten";
    case MergeBlendMode::Darken:
        return "Darken";
    case MergeBlendMode::Difference:
        return "Difference";
    }
    return "Normal";
}

inline constexpr float max_merge_value = 16.0F;

struct MergeParameters final {
    MergeBlendMode blend{MergeBlendMode::Normal};
    // [0, 1]: how much of each landed layer, or of the constant, shows.
    float opacity{1.0F};
    // What LAYERS presents while nothing is wired to it: BASE is blended
    // against this constant, so Multiply scales its brightness and Add lifts
    // it. Normal has no constant to lay over and leaves BASE unchanged.
    // [0, max_merge_value].
    float value{1.0F};

    bool operator==(const MergeParameters&) const = default;
};

// True when a Merge with nothing wired to LAYERS leaves BASE exactly as it
// is: nothing to blend against, or a constant that changes nothing.
[[nodiscard]] constexpr bool
merge_constant_is_identity(const MergeParameters& parameters) noexcept {
    if (parameters.opacity <= 0.0F) {
        return true;
    }
    switch (parameters.blend) {
    case MergeBlendMode::Normal:
        return true;
    case MergeBlendMode::Multiply:
        return parameters.value == 1.0F;
    case MergeBlendMode::Add:
    case MergeBlendMode::Subtract:
    case MergeBlendMode::Lighten:
        return parameters.value == 0.0F;
    case MergeBlendMode::Screen:
    case MergeBlendMode::Overlay:
    case MergeBlendMode::Darken:
    case MergeBlendMode::Difference:
        return false;
    }
    return false;
}

// A Background is the scene's colour fill, so the node and a Background
// surface on Stage are one vocabulary edited with the same controls. Here its
// positions are fractions of the authored canvas, so every node of a wall
// computes the same colour for the same canvas pixel.
using BackgroundMode = scene::FillMode;
using BackgroundColor = scene::FillColor;
using BackgroundParameters = scene::Fill;

inline constexpr float max_background_gradient_size = scene::max_fill_size;
inline constexpr float max_background_center = scene::max_fill_center;

struct OutputParameters final {
    // False is the non-destructive editor default: unconnected scene drawables
    // pass through below the graph-managed layers. True opts into the legacy
    // full-replacement/transparent-background result.
    bool replace_scene{};
    bool operator==(const OutputParameters&) const = default;
};

using NodeParameters =
    std::variant<SceneLayerParameters, BrightnessParameters,
                  SaturationParameters, BlurParameters, LevelsParameters,
                  LiftGammaGainParameters, ContrastPivotParameters,
                  ColorMatrixParameters, WhiteBalanceParameters,
                  CurvesParameters, FilmGrainParameters,
                  BoxBlurParameters, DirectionalBlurParameters,
                  ChromaKeyParameters, LumaKeyParameters,
                  MatteParameters,
                  RadialArrayParameters,
                  LedCarProcessParameters,
                  CompositeParameters,
                 OutputParameters,
                 MergeParameters,
                 BackgroundParameters>;

struct GraphNode final {
    doc::NodeId node{};
    // The scene that owns this graph object, exactly as authored. Zero is no
    // owner.
    doc::NodeId scene{};
    NodeKind kind{NodeKind::SceneLayer};
    std::string label;
    float editor_x{};
    float editor_y{};
    // Editor layout: drawn as its title and connected ports. Not evaluated,
    // planned or digested.
    bool collapsed{};
    // Passed through: the node renders exactly as its identity. Only effect
    // and Radial Array nodes can be bypassed; on any other kind the authored
    // flag reads as false. Render-affecting, so it is digested.
    bool bypassed{};
    NodeParameters parameters{SceneLayerParameters{}};

    bool operator==(const GraphNode&) const = default;
};

// Effect nodes, Radial Array and Merge can be bypassed: each has an identity
// to pass through as (a Merge passes its BASE). Scene Layer, Background,
// Matte, LED Car Process, Composite and Output shape the image path itself
// and cannot.
[[nodiscard]] constexpr bool node_kind_bypassable(NodeKind kind) noexcept {
    return kind != NodeKind::SceneLayer && kind != NodeKind::Matte &&
        kind != NodeKind::LedCarProcess && kind != NodeKind::Composite &&
        kind != NodeKind::Output && kind != NodeKind::Background;
}

// Returns true only for pointwise/spatial effect nodes whose validated
// parameters request their documented identity operation, and for any
// bypassed node. Structural nodes are never identities here. Keeping this
// decision in the graph model makes order validation and scene application
// agree exactly.
[[nodiscard]] bool effect_node_is_identity(const GraphNode& node) noexcept;

struct EdgeEndpoint final {
    doc::NodeId node{};
    core::u8 port{};
    bool operator==(const EdgeEndpoint&) const = default;
};

// A reroute point a wire passes through, in graph units.
struct WirePoint final {
    float x{};
    float y{};
    bool operator==(const WirePoint&) const = default;
};

// A comment box drawn around part of a graph. Layout only: never part of
// evaluation, planning, or the digests.
struct GraphComment final {
    doc::NodeId node{};
    doc::NodeId scene{};
    std::string title;
    float x{};
    float y{};
    float width{};
    float height{};
    // An index into the editor's comment colours.
    core::u8 color{};
    bool operator==(const GraphComment&) const = default;
};

// Comment colours the editor offers, and the smallest box it draws.
inline constexpr core::u8 comment_color_count = 6U;
inline constexpr float min_comment_extent = 48.0F;

// More points than any readable wire needs; a list beyond it is refused.
inline constexpr std::size_t max_wire_points = 16U;

struct GraphEdge final {
    doc::NodeId node{};
    EdgeEndpoint from;
    EdgeEndpoint to;
    core::i64 order{};
    // Editor layout only, in order from the source: where the operator has
    // routed the wire. Never read by evaluation, planning or the digests.
    std::vector<WirePoint> points;
    bool operator==(const GraphEdge&) const = default;
};

enum class DiagnosticCode : core::u8 {
    NodesOverCapacity = 0U,
    EdgesOverCapacity = 1U,
    DanglingNode = 2U,
    DanglingSceneReference = 3U,
    UnknownPort = 4U,
    IncompatiblePorts = 5U,
    DuplicateInput = 6U,
    // 7 was MissingRequiredInput, before a node waiting for a required input
    // was disabled rather than an error (see GraphNodeMetadata::disabled).
    MissingOutput = 8U,
    DuplicateOutput = 9U,
    // The current executor adjusts by referenced scene-node identity, so two
    // active branches for the same drawable cannot yet carry branch-local
    // effects without leaking them into one another.
    DuplicateSceneLayerReference = 10U,
    // 11 was UnsupportedEffectOrder, before effects and filters had
    // inputs of their own.
    Cycle = 12U,
    // A retained editor branch that cannot contribute to any Output. This is
    // informational: operators may keep alternatives parked in the graph.
    DeadBranch = 13U,
    // Deterministic project budgets derived from authored topology, canvas,
    // and effect settings. They never consult adapter limits or available
    // machine memory.
    PassBudgetExceeded = 14U,
    TextureBudgetExceeded = 15U,
    TapBudgetExceeded = 16U,
    // The first executable Matte topology is deliberately exact: each live
    // Matte consumes two direct, distinct SceneLayer image branches.
    UnsupportedMatteTopology = 17U,
    // Groups need one retained AuthoredCanvas composite. Treating a group id
    // as a drawable SceneLayer would clone/fake those semantics.
    UnsupportedMatteGroupSource = 18U,
    // A consumed matte image must not also reach Output through another image
    // path, which would render it once as coverage and again as visible colour.
    MatteBranchAlsoOutputReachable = 19U,
    // Merges, and Composites feeding another image node, each render into an
    // intermediate image of their own. More than max_merge_nesting of them
    // stacked inside one another is refused rather than flattened.
    MergeNestingTooDeep = 20U,
};

enum class DiagnosticSeverity : core::u8 {
    Error = 0U,
    Warning = 1U,
};

[[nodiscard]] constexpr std::string_view
diagnostic_severity_name(DiagnosticSeverity severity) noexcept {
    return severity == DiagnosticSeverity::Error ? "Error" : "Warning";
}

using GraphDiagnosticSortKey =
    std::tuple<DiagnosticSeverity, doc::NodeId, DiagnosticCode, doc::NodeId,
               core::u8>;

struct GraphDiagnostic final {
    DiagnosticCode code{DiagnosticCode::DanglingNode};
    doc::NodeId subject{};
    doc::NodeId related{};
    core::u8 port{};
    // Trailing so existing aggregate initializers remain source-compatible;
    // every pre-severity diagnostic is an Error by definition.
    DiagnosticSeverity severity{DiagnosticSeverity::Error};

    [[nodiscard]] GraphDiagnosticSortKey sort_key() const noexcept {
        return {severity, subject, code, related, port};
    }
    bool operator==(const GraphDiagnostic&) const = default;
};

enum class GraphExecutionDomain : core::u8 {
    Structural,
    Inline,
    Geometry,
    IsolatedLayer,
    FullScene,
};

enum class EffectIdentityState : core::u8 {
    NotEffect,
    Identity,
    Active,
};

// Stable operator-facing facts for one authored node. The canonical path
// includes `node` and the Output at its ends. It is empty for a dead branch;
// among multiple paths the shortest is chosen, then the lexicographically
// lowest node/edge sequence.
struct GraphNodeMetadata final {
    doc::NodeId node{};
    GraphExecutionDomain execution_domain{GraphExecutionDomain::Structural};
    EffectIdentityState identity{EffectIdentityState::NotEffect};
    std::vector<doc::NodeId> path_to_output;
    // One fewer entry than path_to_output: the exact authored edge selected
    // between each pair of nodes, so parallel wires remain distinguishable.
    std::vector<doc::NodeId> edges_to_output;
    // Symmetric canvas-pixel reach of this one node's kernel. Inline and
    // structural nodes are zero. Sequential layer support is the sum of these
    // values in authored effect-edge order.
    scene::EffectBoundsExpansion bounds_expansion{};
    // A required input is open, or fed only by disabled nodes: the node takes
    // no part in the picture, and its wires carry nothing, until it is wired.
    // A disabled Output means the graph composites nothing.
    bool disabled{};

    bool operator==(const GraphNodeMetadata&) const = default;
};

struct GraphResourceEstimate final {
    // False when the current projection does not identify one authored stage
    // canvas. Counts remain zero rather than borrowing a local output size.
    // Estimates cover Output-reachable graph layers and graph spatial-filter
    // nodes; they do not guess at frame-varying first-class scene effects.
    bool available{};
    scene::Canvas canvas{};
    scene::EffectSettings effect_settings{};
    core::u32 active_scene_layers{};
    core::u32 blurred_scene_layers{};
    core::u32 box_blurred_scene_layers{};
    core::u32 directional_blurred_scene_layers{};
    core::u32 chroma_keyed_scene_layers{};
    core::u32 luma_keyed_scene_layers{};
    core::u32 matte_nodes{};
    core::u32 merge_nodes{};
    core::u32 background_nodes{};
    // The deepest stack of intermediate images on the way to Output: each
    // Merge, and each Composite that feeds another image node, adds one.
    core::u32 merge_nesting{};
    core::u32 offscreen_textures{};
    core::u64 bytes_per_texture{};
    // Sum of the full-canvas color/depth targets above. This is an authored
    // workload estimate, not a claim about physical residency on one adapter.
    core::u64 offscreen_texture_bytes{};
    core::u32 render_passes{};
    core::u64 blur_taps_per_pixel{};
    // Maximum summed symmetric support of any Output-reachable SceneLayer.
    scene::EffectBoundsExpansion max_layer_bounds_expansion{};

    bool operator==(const GraphResourceEstimate&) const = default;
};

struct PlanConnection final {
    doc::NodeId edge{};
    doc::NodeId source{};
    core::u8 source_port{};
    doc::NodeId target{};
    core::u8 target_port{};
    core::i64 order{};
    PortValueType type{PortValueType::Image};
    bool operator==(const PlanConnection&) const = default;
};

struct RadialInstance final {
    core::u32 index{};
    scene::Turns angle{};
    scene::Mat4 world{};
    bool operator==(const RadialInstance&) const = default;
};

struct PlanStep final {
    doc::NodeId node{};
    NodeKind kind{NodeKind::SceneLayer};
    std::vector<PlanConnection> inputs;
    std::vector<PlanConnection> consumers;
    std::vector<RadialInstance> radial_instances;
    // Every active spatial filter remains a real pass in the plan. The GPU
    // executor must allocate an offscreen source and destination; this model
    // never substitutes a pointwise approximation for a blur.
    bool requires_offscreen{};
    bool operator==(const PlanStep&) const = default;
};

struct CompositorPlan final {
    std::vector<PlanStep> steps;
    bool operator==(const CompositorPlan&) const = default;
};

inline constexpr core::u32 max_graph_nodes = 1024U;
inline constexpr core::u32 max_graph_edges = 4096U;
inline constexpr core::u32 max_radial_instances = 256U;
inline constexpr float max_brightness = 16.0F;
inline constexpr float max_saturation = scene::max_saturation;
inline constexpr float max_blur_radius = 256.0F;
inline constexpr core::u32 max_box_blur_radius =
    scene::max_box_blur_radius_px;
inline constexpr core::u32 max_box_blur_passes =
    scene::max_box_blur_passes;
inline constexpr float max_directional_blur_length =
    scene::max_directional_blur_length_px;
inline constexpr float max_spatial_extent = 1'000'000.0F;
inline constexpr float max_led_car_horizontal_blend = 0.5F;
// Intermediate images one Output path may stack. Each level holds a pair of
// full-canvas targets while it is open.
inline constexpr core::u32 max_merge_nesting = 4U;
// Facility-independent graph budgets. They intentionally describe authored
// work rather than querying one adapter's VRAM or driver limits.
inline constexpr core::u32 max_graph_render_passes = 128U;
inline constexpr core::u64 max_graph_blur_taps_per_pixel = 2'048ULL;
// The offscreen texture budget is authored with the project's effect settings
// rather than fixed here, so a facility sizes it to its own render nodes.
// Decimal gigabytes, converted in double so every machine gets the same bytes.
[[nodiscard]] inline core::u64 graph_texture_budget_bytes(
    const scene::EffectSettings& settings) noexcept {
    return static_cast<core::u64>(
        static_cast<double>(settings.graph_texture_budget_gb) * 1.0e9);
}

struct CompositorGraph final {
    // Authored values are sorted by their document ids. An invalid graph is
    // retained for editor display, but produces no executable plan.
    std::vector<GraphNode> nodes;
    std::vector<GraphEdge> edges;
    std::vector<GraphDiagnostic> diagnostics;
    // Ascending by document id, like nodes and edges.
    std::vector<GraphComment> comments;
    CompositorPlan plan;
    std::vector<GraphNodeMetadata> node_metadata;
    GraphResourceEstimate resources;

    // Warnings remain visible and digestible but do not suppress execution.
    [[nodiscard]] bool has_errors() const noexcept;
    [[nodiscard]] bool has_warnings() const noexcept;
    [[nodiscard]] bool valid() const noexcept { return !has_errors(); }
    [[nodiscard]] const GraphNode* node(doc::NodeId id) const noexcept;
    [[nodiscard]] const GraphEdge* edge(doc::NodeId id) const noexcept;
    [[nodiscard]] const PlanStep* step(doc::NodeId id) const noexcept;
    [[nodiscard]] const GraphNodeMetadata*
    metadata(doc::NodeId id) const noexcept;

    bool operator==(const CompositorGraph&) const = default;
};

// FNV-1a over the authored graph, diagnostics, and evaluated pass plan.
[[nodiscard]] core::u64 digest(const CompositorGraph& graph) noexcept;

// Applies Output-reachable scene-layer modifiers to an already evaluated
// scene. Brightness/saturation/blur fold into each referenced drawable's
// LayerStyle and a RadialArray replaces that drawable with its deterministic
// transform set.
// Disconnected nodes, and invalid/incomplete graphs retained by the editor,
// deliberately leave rendering unchanged.
[[nodiscard]] scene::Scene apply(const CompositorGraph& graph,
                                 const scene::Scene& source);

} // namespace mediaserver::compositor
