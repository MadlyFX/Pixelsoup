#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/scene/scene.hpp>

#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mediaserver::compositor {

enum class NodeKind : core::u8 {
    SceneLayer,
    Brightness,
    Blur,
    RadialArray,
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
    case NodeKind::Blur:
        return "Blur";
    case NodeKind::RadialArray:
        return "Radial Array";
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
    std::variant<SceneLayerParameters, BrightnessParameters, BlurParameters,
                 RadialArrayParameters, CompositeParameters, OutputParameters>;

struct GraphNode final {
    doc::NodeId node{};
    NodeKind kind{NodeKind::SceneLayer};
    std::string label;
    float editor_x{};
    float editor_y{};
    NodeParameters parameters{SceneLayerParameters{}};

    bool operator==(const GraphNode&) const = default;
};

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
    NodesOverCapacity,
    EdgesOverCapacity,
    DanglingNode,
    DanglingSceneReference,
    UnknownPort,
    IncompatiblePorts,
    DuplicateInput,
    MissingRequiredInput,
    MissingOutput,
    DuplicateOutput,
    Cycle,
};

struct GraphDiagnostic final {
    DiagnosticCode code{DiagnosticCode::DanglingNode};
    doc::NodeId subject{};
    doc::NodeId related{};
    core::u8 port{};
    bool operator==(const GraphDiagnostic&) const = default;
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
    // A Blur deliberately remains a real spatial pass in the plan. The GPU
    // executor must allocate an offscreen source and destination; this model
    // never pretends a scalar opacity change is a blur.
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
inline constexpr float max_blur_radius = 256.0F;
inline constexpr float max_spatial_extent = 1'000'000.0F;

struct CompositorGraph final {
    // Authored values are sorted by their document ids. An invalid graph is
    // retained for editor display, but produces no executable plan.
    std::vector<GraphNode> nodes;
    std::vector<GraphEdge> edges;
    std::vector<GraphDiagnostic> diagnostics;
    CompositorPlan plan;

    [[nodiscard]] bool valid() const noexcept { return diagnostics.empty(); }
    [[nodiscard]] const GraphNode* node(doc::NodeId id) const noexcept;
    [[nodiscard]] const GraphEdge* edge(doc::NodeId id) const noexcept;
    [[nodiscard]] const PlanStep* step(doc::NodeId id) const noexcept;

    bool operator==(const CompositorGraph&) const = default;
};

// FNV-1a over the authored graph, diagnostics, and evaluated pass plan.
[[nodiscard]] core::u64 digest(const CompositorGraph& graph) noexcept;

// Applies Output-reachable scene-layer modifiers to an already evaluated
// scene. Brightness/blur fold into each referenced drawable's LayerStyle and a
// RadialArray replaces that drawable with its deterministic transform set.
// Disconnected nodes, and invalid/incomplete graphs retained by the editor,
// deliberately leave rendering unchanged.
[[nodiscard]] scene::Scene apply(const CompositorGraph& graph,
                                 const scene::Scene& source);

} // namespace mediaserver::compositor
