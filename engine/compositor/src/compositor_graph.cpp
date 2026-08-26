#include <mediaserver/compositor/compositor_graph.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <map>
#include <set>
#include <type_traits>
#include <utility>

namespace mediaserver::compositor {
namespace {

constexpr std::array<PortDescriptor, 2U> scene_layer_inputs{{
    {0U, "EFFECTS", PortValueType::Effect, PortDirection::Input, true, false},
    {1U, "INSTANCES", PortValueType::TransformSet, PortDirection::Input, false,
     false},
}};
constexpr std::array<PortDescriptor, 1U> scene_layer_outputs{{
    {0U, "IMAGE", PortValueType::Image, PortDirection::Output, true, false},
}};
constexpr std::array<PortDescriptor, 1U> brightness_outputs{{
    {0U, "EFFECT", PortValueType::Effect, PortDirection::Output, true, false},
}};
constexpr std::array<PortDescriptor, 1U> blur_outputs{{
    {0U, "EFFECT", PortValueType::Effect, PortDirection::Output, true, false},
}};
constexpr std::array<PortDescriptor, 1U> radial_outputs{{
    {0U, "INSTANCES", PortValueType::TransformSet, PortDirection::Output, true,
     false},
}};
constexpr std::array<PortDescriptor, 1U> composite_inputs{{
    {0U, "LAYERS", PortValueType::Image, PortDirection::Input, true, true},
}};
constexpr std::array<PortDescriptor, 1U> composite_outputs{{
    {0U, "IMAGE", PortValueType::Image, PortDirection::Output, true, false},
}};
constexpr std::array<PortDescriptor, 1U> output_inputs{{
    {0U, "IMAGE", PortValueType::Image, PortDirection::Input, false, true},
}};

constexpr core::u64 fnv_offset = 1469598103934665603ULL;
constexpr core::u64 fnv_prime = 1099511628211ULL;

void hash_byte(core::u64& hash, core::u8 value) noexcept {
    hash ^= value;
    hash *= fnv_prime;
}

void hash_u64(core::u64& hash, core::u64 value) noexcept {
    for (core::u32 shift = 0U; shift < 64U; shift += 8U) {
        hash_byte(hash, static_cast<core::u8>((value >> shift) & 0xFFU));
    }
}

void hash_u32(core::u64& hash, core::u32 value) noexcept {
    hash_u64(hash, static_cast<core::u64>(value));
}

void hash_i64(core::u64& hash, core::i64 value) noexcept {
    hash_u64(hash, static_cast<core::u64>(value));
}

void hash_float(core::u64& hash, float value) noexcept {
    hash_u32(hash, std::bit_cast<core::u32>(value));
}

void hash_text(core::u64& hash, std::string_view value) noexcept {
    hash_u64(hash, static_cast<core::u64>(value.size()));
    for (const char character : value) {
        hash_byte(hash, static_cast<core::u8>(character));
    }
}

void hash_connection(core::u64& hash,
                     const PlanConnection& connection) noexcept {
    hash_u64(hash, connection.edge.value());
    hash_u64(hash, connection.source.value());
    hash_byte(hash, connection.source_port);
    hash_u64(hash, connection.target.value());
    hash_byte(hash, connection.target_port);
    hash_i64(hash, connection.order);
    hash_byte(hash, static_cast<core::u8>(connection.type));
}

} // namespace

std::span<const PortDescriptor> input_ports(NodeKind kind) noexcept {
    switch (kind) {
    case NodeKind::SceneLayer:
        return scene_layer_inputs;
    case NodeKind::Composite:
        return composite_inputs;
    case NodeKind::Output:
        return output_inputs;
    case NodeKind::Brightness:
    case NodeKind::Blur:
    case NodeKind::RadialArray:
        return {};
    }
    return {};
}

std::span<const PortDescriptor> output_ports(NodeKind kind) noexcept {
    switch (kind) {
    case NodeKind::SceneLayer:
        return scene_layer_outputs;
    case NodeKind::Brightness:
        return brightness_outputs;
    case NodeKind::Blur:
        return blur_outputs;
    case NodeKind::RadialArray:
        return radial_outputs;
    case NodeKind::Composite:
        return composite_outputs;
    case NodeKind::Output:
        return {};
    }
    return {};
}

const PortDescriptor* find_input_port(NodeKind kind, core::u8 index) noexcept {
    const auto ports = input_ports(kind);
    const auto found =
        std::find_if(ports.begin(), ports.end(),
                     [index](const PortDescriptor& port) noexcept {
                         return port.index == index;
                     });
    return found == ports.end() ? nullptr : &*found;
}

const PortDescriptor* find_output_port(NodeKind kind, core::u8 index) noexcept {
    const auto ports = output_ports(kind);
    const auto found =
        std::find_if(ports.begin(), ports.end(),
                     [index](const PortDescriptor& port) noexcept {
                         return port.index == index;
                     });
    return found == ports.end() ? nullptr : &*found;
}

const GraphNode* CompositorGraph::node(doc::NodeId id) const noexcept {
    const auto found = std::lower_bound(
        nodes.begin(), nodes.end(), id,
        [](const GraphNode& value, doc::NodeId wanted) noexcept {
            return value.node < wanted;
        });
    return found != nodes.end() && found->node == id ? &*found : nullptr;
}

const GraphEdge* CompositorGraph::edge(doc::NodeId id) const noexcept {
    const auto found = std::lower_bound(
        edges.begin(), edges.end(), id,
        [](const GraphEdge& value, doc::NodeId wanted) noexcept {
            return value.node < wanted;
        });
    return found != edges.end() && found->node == id ? &*found : nullptr;
}

const PlanStep* CompositorGraph::step(doc::NodeId id) const noexcept {
    const auto found = std::find_if(
        plan.steps.begin(), plan.steps.end(),
        [id](const PlanStep& value) noexcept { return value.node == id; });
    return found == plan.steps.end() ? nullptr : &*found;
}

core::u64 digest(const CompositorGraph& graph) noexcept {
    core::u64 hash = fnv_offset;
    hash_u64(hash, static_cast<core::u64>(graph.nodes.size()));
    for (const GraphNode& node : graph.nodes) {
        hash_u64(hash, node.node.value());
        hash_byte(hash, static_cast<core::u8>(node.kind));
        hash_text(hash, node.label);
        hash_float(hash, node.editor_x);
        hash_float(hash, node.editor_y);
        std::visit(
            [&hash](const auto& parameters) noexcept {
                using Parameters = std::decay_t<decltype(parameters)>;
                if constexpr (std::is_same_v<Parameters,
                                             SceneLayerParameters>) {
                    hash_u64(hash, parameters.scene_node.value());
                } else if constexpr (std::is_same_v<Parameters,
                                                    BrightnessParameters>) {
                    hash_float(hash, parameters.amount);
                } else if constexpr (std::is_same_v<Parameters,
                                                    BlurParameters>) {
                    hash_float(hash, parameters.radius);
                } else if constexpr (std::is_same_v<Parameters,
                                                    RadialArrayParameters>) {
                    hash_u32(hash, parameters.count);
                    hash_float(hash, parameters.center.x);
                    hash_float(hash, parameters.center.y);
                    hash_float(hash, parameters.center.z);
                    hash_float(hash, parameters.radius);
                    hash_float(hash, parameters.start_angle);
                } else if constexpr (std::is_same_v<Parameters,
                                                    OutputParameters>) {
                    hash_byte(hash, parameters.replace_scene ? 1U : 0U);
                }
            },
            node.parameters);
    }

    hash_u64(hash, static_cast<core::u64>(graph.edges.size()));
    for (const GraphEdge& edge : graph.edges) {
        hash_u64(hash, edge.node.value());
        hash_u64(hash, edge.from.node.value());
        hash_byte(hash, edge.from.port);
        hash_u64(hash, edge.to.node.value());
        hash_byte(hash, edge.to.port);
        hash_i64(hash, edge.order);
    }

    hash_u64(hash, static_cast<core::u64>(graph.diagnostics.size()));
    for (const GraphDiagnostic& diagnostic : graph.diagnostics) {
        hash_byte(hash, static_cast<core::u8>(diagnostic.code));
        hash_u64(hash, diagnostic.subject.value());
        hash_u64(hash, diagnostic.related.value());
        hash_byte(hash, diagnostic.port);
    }

    hash_u64(hash, static_cast<core::u64>(graph.plan.steps.size()));
    for (const PlanStep& step : graph.plan.steps) {
        hash_u64(hash, step.node.value());
        hash_byte(hash, static_cast<core::u8>(step.kind));
        hash_byte(hash, step.requires_offscreen ? 1U : 0U);
        hash_u64(hash, static_cast<core::u64>(step.inputs.size()));
        for (const PlanConnection& connection : step.inputs) {
            hash_connection(hash, connection);
        }
        hash_u64(hash, static_cast<core::u64>(step.consumers.size()));
        for (const PlanConnection& connection : step.consumers) {
            hash_connection(hash, connection);
        }
        hash_u64(hash, static_cast<core::u64>(step.radial_instances.size()));
        for (const RadialInstance& instance : step.radial_instances) {
            hash_u32(hash, instance.index);
            hash_float(hash, instance.angle);
            for (const float value : instance.world.m) {
                hash_float(hash, value);
            }
        }
    }
    return hash;
}

scene::Scene apply(const CompositorGraph& graph, const scene::Scene& source) {
    if (!graph.valid() || graph.plan.steps.empty()) {
        return source;
    }

    // Only the image path feeding Output is executable. Keeping disconnected
    // SceneLayer nodes editable must not let their effects leak into Program.
    std::set<doc::NodeId> active_layers;
    std::set<doc::NodeId> visited;
    std::vector<doc::NodeId> pending;
    for (const GraphNode& node : graph.nodes) {
        if (node.kind == NodeKind::Output) {
            pending.push_back(node.node);
            break;
        }
    }
    while (!pending.empty()) {
        const doc::NodeId current = pending.back();
        pending.pop_back();
        if (!visited.insert(current).second) {
            continue;
        }
        const PlanStep* const step = graph.step(current);
        if (step == nullptr) {
            continue;
        }
        for (const PlanConnection& input : step->inputs) {
            if (input.type != PortValueType::Image) {
                continue;
            }
            const GraphNode* const upstream = graph.node(input.source);
            if (upstream == nullptr) {
                continue;
            }
            if (upstream->kind == NodeKind::SceneLayer) {
                active_layers.insert(upstream->node);
            } else if (upstream->kind == NodeKind::Composite) {
                pending.push_back(upstream->node);
            }
        }
    }

    struct Adjustment final {
        bool brightness_enabled{};
        float brightness{1.0F};
        bool blur_enabled{};
        float blur_radius{};
        const PlanStep* radial{};
    };
    std::map<doc::NodeId, Adjustment> adjustments;
    for (const GraphNode& layer : graph.nodes) {
        if (layer.kind != NodeKind::SceneLayer ||
            !active_layers.contains(layer.node)) {
            continue;
        }
        const auto& parameters =
            std::get<SceneLayerParameters>(layer.parameters);
        Adjustment& adjustment = adjustments[parameters.scene_node];
        const PlanStep* const layer_step = graph.step(layer.node);
        if (layer_step == nullptr) {
            continue;
        }
        for (const PlanConnection& input : layer_step->inputs) {
            const GraphNode* const modifier = graph.node(input.source);
            if (modifier == nullptr) {
                continue;
            }
            if (modifier->kind == NodeKind::Brightness) {
                const float amount =
                    std::get<BrightnessParameters>(modifier->parameters).amount;
                adjustment.brightness_enabled = true;
                adjustment.brightness = std::clamp(
                    adjustment.brightness * amount, 0.0F, max_brightness);
            } else if (modifier->kind == NodeKind::Blur) {
                const float radius =
                    std::get<BlurParameters>(modifier->parameters).radius;
                adjustment.blur_enabled = true;
                adjustment.blur_radius = std::clamp(
                    adjustment.blur_radius + radius, 0.0F, max_blur_radius);
            } else if (modifier->kind == NodeKind::RadialArray &&
                       adjustment.radial == nullptr) {
                adjustment.radial = graph.step(modifier->node);
            }
        }
    }

    scene::Scene result = source;
    const auto fold_style = [&](doc::NodeId id,
                                scene::LayerStyle& style) noexcept {
        const auto found = adjustments.find(id);
        if (found == adjustments.end()) {
            return;
        }
        const Adjustment& adjustment = found->second;
        if (adjustment.brightness_enabled) {
            style.effects.brightness_enabled = true;
            style.effects.brightness =
                std::clamp(style.effects.brightness * adjustment.brightness,
                           0.0F, max_brightness);
        }
        if (adjustment.blur_enabled) {
            style.effects.blur_enabled = true;
            style.effects.blur_radius =
                std::clamp(style.effects.blur_radius + adjustment.blur_radius,
                           0.0F, max_blur_radius);
        }
    };
    for (scene::MeshInstance& instance : result.instances) {
        fold_style(instance.source, instance.style);
    }
    for (scene::MediaInstance& instance : result.media) {
        fold_style(instance.source_node, instance.style);
    }
    for (scene::ModelInstance& instance : result.models) {
        fold_style(instance.source_node, instance.style);
    }

    const auto expand = [&]<typename Instance, typename IdOf>(
                            std::vector<Instance>& values, std::size_t capacity,
                            IdOf id_of) {
        std::vector<Instance> expanded;
        expanded.reserve(std::min(values.size(), capacity));
        for (const Instance& instance : values) {
            const auto found = adjustments.find(id_of(instance));
            const PlanStep* const radial =
                found == adjustments.end() ? nullptr : found->second.radial;
            if (radial == nullptr || radial->radial_instances.empty()) {
                if (expanded.size() < capacity) {
                    expanded.push_back(instance);
                }
                continue;
            }
            for (const RadialInstance& transform : radial->radial_instances) {
                if (expanded.size() >= capacity) {
                    break;
                }
                Instance copy = instance;
                copy.world = scene::multiply(transform.world, instance.world);
                copy.normal = scene::normal_basis(copy.world);
                expanded.push_back(std::move(copy));
            }
        }
        values = std::move(expanded);
    };
    expand(
        result.instances, scene::max_scene_instances,
        [](const scene::MeshInstance& value) noexcept { return value.source; });
    expand(result.media, scene::max_scene_media,
           [](const scene::MediaInstance& value) noexcept {
               return value.source_node;
           });
    expand(result.models, scene::max_scene_models,
           [](const scene::ModelInstance& value) noexcept {
               return value.source_node;
           });

    std::sort(result.instances.begin(), result.instances.end(),
              [](const scene::MeshInstance& left,
                 const scene::MeshInstance& right) noexcept {
                  return left.mesh != right.mesh ? left.mesh < right.mesh
                                                 : left.source < right.source;
              });
    return result;
}

} // namespace mediaserver::compositor
