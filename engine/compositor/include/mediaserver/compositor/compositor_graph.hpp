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
};

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
    }
    return "Node";
}

enum class PortValueType : core::u8 {
    Image,
    Effect,
    TransformSet,
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
                 OutputParameters>;

struct GraphNode final {
    doc::NodeId node{};
    // The scene that owns this graph object, exactly as authored. Zero is
    // legacy content with no owner, which belongs to the default scene.
    doc::NodeId scene{};
    NodeKind kind{NodeKind::SceneLayer};
    std::string label;
    float editor_x{};
    float editor_y{};
    NodeParameters parameters{SceneLayerParameters{}};

    bool operator==(const GraphNode&) const = default;
};

// Returns true only for pointwise/spatial effect nodes whose validated
// parameters request their documented identity operation. Structural nodes
// are never identities here. Keeping this decision in the graph model makes
// order validation and scene application agree exactly.
[[nodiscard]] bool effect_node_is_identity(const GraphNode& node) noexcept;

struct EdgeEndpoint final {
    doc::NodeId node{};
    core::u8 port{};
    bool operator==(const EdgeEndpoint&) const = default;
};

struct GraphEdge final {
    doc::NodeId node{};
    EdgeEndpoint from;
    EdgeEndpoint to;
    core::i64 order{};
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
    MissingRequiredInput = 7U,
    MissingOutput = 8U,
    DuplicateOutput = 9U,
    // The current executor adjusts by referenced scene-node identity, so two
    // active branches for the same drawable cannot yet carry branch-local
    // effects without leaking them into one another.
    DuplicateSceneLayerReference = 10U,
    // SceneLayer effects execute inline before spatial isolation. An inline
    // effect connected after Blur would therefore claim an order the executor
    // cannot preserve, so the authored graph remains visible but unplannable.
    UnsupportedEffectOrder = 11U,
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
// Facility-independent graph budgets. They intentionally describe authored
// work rather than querying one adapter's VRAM or driver limits.
inline constexpr core::u32 max_graph_render_passes = 128U;
inline constexpr core::u64 max_graph_offscreen_texture_bytes =
    1'000'000'000ULL;
inline constexpr core::u64 max_graph_blur_taps_per_pixel = 2'048ULL;

struct CompositorGraph final {
    // Authored values are sorted by their document ids. An invalid graph is
    // retained for editor display, but produces no executable plan.
    std::vector<GraphNode> nodes;
    std::vector<GraphEdge> edges;
    std::vector<GraphDiagnostic> diagnostics;
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
