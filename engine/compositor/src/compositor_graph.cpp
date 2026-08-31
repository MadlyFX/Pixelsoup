#include <mediaserver/compositor/compositor_graph.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
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
constexpr std::array<PortDescriptor, 1U> saturation_outputs{{
    {0U, "EFFECT", PortValueType::Effect, PortDirection::Output, true, false},
}};
constexpr std::array<PortDescriptor, 1U> blur_outputs{{
    {0U, "EFFECT", PortValueType::Effect, PortDirection::Output, true, false},
}};
constexpr std::array<PortDescriptor, 1U> levels_outputs{{
    {0U, "EFFECT", PortValueType::Effect, PortDirection::Output, true, false},
}};
constexpr std::array<PortDescriptor, 1U> lift_gamma_gain_outputs{{
    {0U, "EFFECT", PortValueType::Effect, PortDirection::Output, true, false},
}};
constexpr std::array<PortDescriptor, 1U> contrast_pivot_outputs{{
    {0U, "EFFECT", PortValueType::Effect, PortDirection::Output, true, false},
}};
constexpr std::array<PortDescriptor, 1U> color_matrix_outputs{{
    {0U, "EFFECT", PortValueType::Effect, PortDirection::Output, true, false},
}};
constexpr std::array<PortDescriptor, 1U> white_balance_outputs{{
    {0U, "EFFECT", PortValueType::Effect, PortDirection::Output, true, false},
}};
constexpr std::array<PortDescriptor, 1U> curves_outputs{{
    {0U, "EFFECT", PortValueType::Effect, PortDirection::Output, true, false},
}};
constexpr std::array<PortDescriptor, 1U> film_grain_outputs{{
    {0U, "EFFECT", PortValueType::Effect, PortDirection::Output, true, false},
}};
constexpr std::array<PortDescriptor, 1U> radial_outputs{{
    {0U, "INSTANCES", PortValueType::TransformSet, PortDirection::Output, true,
     false},
}};
constexpr std::array<PortDescriptor, 10U> led_car_process_inputs{{
    {0U, "FRONT", PortValueType::Image, PortDirection::Input, false, false},
    {1U, "FRONT RIGHT 3/4", PortValueType::Image, PortDirection::Input, false,
     false},
    {2U, "RIGHT", PortValueType::Image, PortDirection::Input, false, false},
    {3U, "BACK RIGHT 3/4", PortValueType::Image, PortDirection::Input, false,
     false},
    {4U, "BACK", PortValueType::Image, PortDirection::Input, false, false},
    {5U, "BACK LEFT 3/4", PortValueType::Image, PortDirection::Input, false,
     false},
    {6U, "LEFT", PortValueType::Image, PortDirection::Input, false, false},
    {7U, "FRONT LEFT 3/4", PortValueType::Image, PortDirection::Input, false,
     false},
    {8U, "CEILING REFLECTION", PortValueType::Image, PortDirection::Input,
     false, false},
    {9U, "SETUP EFFECTS", PortValueType::Effect, PortDirection::Input, true,
     false},
}};
constexpr std::array<PortDescriptor, 1U> led_car_process_outputs{{
    {0U, "IMAGE", PortValueType::Image, PortDirection::Output, true, false},
}};
constexpr std::array<PortDescriptor, 2U> matte_inputs{{
    {0U, "Source Image", PortValueType::Image, PortDirection::Input, false,
     true},
    {1U, "Matte Image", PortValueType::Image, PortDirection::Input, false,
     true},
}};
constexpr std::array<PortDescriptor, 1U> matte_outputs{{
    {0U, "Image", PortValueType::Image, PortDirection::Output, true, false},
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

void hash_levels_channel(core::u64& hash,
                         const scene::LevelsChannelParameters& value) noexcept {
    hash_float(hash, value.input_black);
    hash_float(hash, value.input_white);
    hash_float(hash, value.gamma);
    hash_float(hash, value.output_black);
    hash_float(hash, value.output_white);
}

void hash_levels(core::u64& hash,
                 const LevelsParameters& value) noexcept {
    hash_levels_channel(hash, value.master);
    hash_levels_channel(hash, value.red);
    hash_levels_channel(hash, value.green);
    hash_levels_channel(hash, value.blue);
}

void hash_lift_gamma_gain(
    core::u64& hash, const LiftGammaGainParameters& value) noexcept {
    for (const scene::Vec3 vector : {value.lift, value.gamma, value.gain}) {
        hash_float(hash, vector.x);
        hash_float(hash, vector.y);
        hash_float(hash, vector.z);
    }
}

void hash_contrast_pivot(
    core::u64& hash, const ContrastPivotParameters& value) noexcept {
    hash_float(hash, value.contrast);
    hash_float(hash, value.pivot);
}

void hash_color_matrix(
    core::u64& hash, const ColorMatrixParameters& value) noexcept {
    for (const float coefficient : value.matrix) {
        hash_float(hash, coefficient);
    }
    hash_float(hash, value.offset.x);
    hash_float(hash, value.offset.y);
    hash_float(hash, value.offset.z);
}

void hash_white_balance(
    core::u64& hash, const WhiteBalanceParameters& value) noexcept {
    hash_float(hash, value.temperature_kelvin);
    hash_float(hash, value.tint);
    for (const float coefficient : value.matrix) {
        hash_float(hash, coefficient);
    }
}

void hash_curves(core::u64& hash, const CurvesParameters& value) noexcept {
    const auto hash_channel = [&hash](
                                  const scene::CurveChannelParameters& channel) {
        hash_u64(hash, static_cast<core::u64>(channel.points.size()));
        for (const scene::CurveControlPoint point : channel.points) {
            hash_float(hash, point.x);
            hash_float(hash, point.y);
        }
    };
    hash_channel(value.master);
    for (const auto* const channel : {&value.red, &value.green, &value.blue}) {
        hash_byte(hash, channel->has_value() ? 1U : 0U);
        if (channel->has_value()) {
            hash_channel(**channel);
        }
    }
}

void hash_film_grain(
    core::u64& hash, const FilmGrainParameters& value) noexcept {
    hash_float(hash, value.amount);
    hash_float(hash, value.pixel_scale);
    hash_float(hash, value.chroma_ratio);
    hash_u32(hash, value.seed);
    hash_byte(hash, static_cast<core::u8>(value.mode));
}

void hash_chroma_key(core::u64& hash,
                     const ChromaKeyParameters& value) noexcept {
    hash_float(hash, value.key_color.x);
    hash_float(hash, value.key_color.y);
    hash_float(hash, value.key_color.z);
    hash_float(hash, value.tolerance);
    hash_float(hash, value.softness);
    hash_u32(hash, static_cast<core::u32>(value.morphology_px));
    hash_float(hash, value.spill_suppression);
    hash_float(hash, value.mix);
}

void hash_luma_key(core::u64& hash,
                   const LumaKeyParameters& value) noexcept {
    hash_float(hash, value.low_threshold);
    hash_float(hash, value.high_threshold);
    hash_float(hash, value.low_softness);
    hash_float(hash, value.high_softness);
    hash_byte(hash, value.invert ? 1U : 0U);
    hash_float(hash, value.mix);
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
    case NodeKind::Matte:
        return matte_inputs;
    case NodeKind::Composite:
        return composite_inputs;
    case NodeKind::LedCarProcess:
        return led_car_process_inputs;
    case NodeKind::Output:
        return output_inputs;
    case NodeKind::Brightness:
    case NodeKind::Saturation:
    case NodeKind::Blur:
    case NodeKind::Levels:
    case NodeKind::LiftGammaGain:
    case NodeKind::ContrastPivot:
    case NodeKind::ColorMatrix:
    case NodeKind::WhiteBalance:
    case NodeKind::Curves:
    case NodeKind::FilmGrain:
    case NodeKind::BoxBlur:
    case NodeKind::DirectionalBlur:
    case NodeKind::ChromaKey:
    case NodeKind::LumaKey:
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
    case NodeKind::Saturation:
        return saturation_outputs;
    case NodeKind::Blur:
        return blur_outputs;
    case NodeKind::Levels:
        return levels_outputs;
    case NodeKind::LiftGammaGain:
        return lift_gamma_gain_outputs;
    case NodeKind::ContrastPivot:
        return contrast_pivot_outputs;
    case NodeKind::ColorMatrix:
        return color_matrix_outputs;
    case NodeKind::WhiteBalance:
        return white_balance_outputs;
    case NodeKind::Curves:
        return curves_outputs;
    case NodeKind::FilmGrain:
        return film_grain_outputs;
    case NodeKind::BoxBlur:
    case NodeKind::DirectionalBlur:
    case NodeKind::ChromaKey:
    case NodeKind::LumaKey:
        return blur_outputs;
    case NodeKind::Matte:
        return matte_outputs;
    case NodeKind::RadialArray:
        return radial_outputs;
    case NodeKind::LedCarProcess:
        return led_car_process_outputs;
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

bool effect_node_is_identity(const GraphNode& node) noexcept {
    switch (node.kind) {
    case NodeKind::Brightness: {
        const auto* const parameters =
            std::get_if<BrightnessParameters>(&node.parameters);
        return parameters != nullptr && parameters->amount == 1.0F;
    }
    case NodeKind::Saturation: {
        const auto* const parameters =
            std::get_if<SaturationParameters>(&node.parameters);
        return parameters != nullptr && parameters->amount == 1.0F;
    }
    case NodeKind::Blur: {
        const auto* const parameters =
            std::get_if<BlurParameters>(&node.parameters);
        return parameters != nullptr && parameters->radius <= 0.0F;
    }
    case NodeKind::Levels: {
        const auto* const parameters =
            std::get_if<LevelsParameters>(&node.parameters);
        return parameters != nullptr && scene::levels_is_identity(*parameters);
    }
    case NodeKind::LiftGammaGain: {
        const auto* const parameters =
            std::get_if<LiftGammaGainParameters>(&node.parameters);
        return parameters != nullptr
            && scene::lift_gamma_gain_is_identity(*parameters);
    }
    case NodeKind::ContrastPivot: {
        const auto* const parameters =
            std::get_if<ContrastPivotParameters>(&node.parameters);
        return parameters != nullptr
            && scene::contrast_pivot_is_identity(*parameters);
    }
    case NodeKind::ColorMatrix: {
        const auto* const parameters =
            std::get_if<ColorMatrixParameters>(&node.parameters);
        return parameters != nullptr
            && scene::color_matrix_is_identity(*parameters);
    }
    case NodeKind::WhiteBalance: {
        const auto* const parameters =
            std::get_if<WhiteBalanceParameters>(&node.parameters);
        return parameters != nullptr
            && scene::white_balance_is_identity(*parameters);
    }
    case NodeKind::Curves: {
        const auto* const parameters =
            std::get_if<CurvesParameters>(&node.parameters);
        return parameters != nullptr && scene::curves_is_identity(*parameters);
    }
    case NodeKind::FilmGrain: {
        const auto* const parameters =
            std::get_if<FilmGrainParameters>(&node.parameters);
        return parameters != nullptr
            && scene::film_grain_is_identity(*parameters);
    }
    case NodeKind::BoxBlur: {
        const auto* const parameters =
            std::get_if<BoxBlurParameters>(&node.parameters);
        return parameters != nullptr &&
            scene::box_blur_is_identity(*parameters);
    }
    case NodeKind::DirectionalBlur: {
        const auto* const parameters =
            std::get_if<DirectionalBlurParameters>(&node.parameters);
        return parameters != nullptr &&
            scene::directional_blur_is_identity(*parameters);
    }
    case NodeKind::ChromaKey: {
        const auto* const parameters =
            std::get_if<ChromaKeyParameters>(&node.parameters);
        return parameters != nullptr &&
            scene::chroma_key_is_identity(*parameters);
    }
    case NodeKind::LumaKey: {
        const auto* const parameters =
            std::get_if<LumaKeyParameters>(&node.parameters);
        return parameters != nullptr &&
            scene::luma_key_is_identity(*parameters);
    }
    case NodeKind::Matte:
    case NodeKind::SceneLayer:
    case NodeKind::RadialArray:
    case NodeKind::LedCarProcess:
    case NodeKind::Composite:
    case NodeKind::Output:
        return false;
    }
    return false;
}

bool CompositorGraph::has_errors() const noexcept {
    return std::any_of(
        diagnostics.begin(), diagnostics.end(),
        [](const GraphDiagnostic& diagnostic) noexcept {
            return diagnostic.severity == DiagnosticSeverity::Error;
        });
}

bool CompositorGraph::has_warnings() const noexcept {
    return std::any_of(
        diagnostics.begin(), diagnostics.end(),
        [](const GraphDiagnostic& diagnostic) noexcept {
            return diagnostic.severity == DiagnosticSeverity::Warning;
        });
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

const GraphNodeMetadata*
CompositorGraph::metadata(doc::NodeId id) const noexcept {
    const auto found = std::lower_bound(
        node_metadata.begin(), node_metadata.end(), id,
        [](const GraphNodeMetadata& value, doc::NodeId wanted) noexcept {
            return value.node < wanted;
        });
    return found != node_metadata.end() && found->node == id ? &*found
                                                             : nullptr;
}

core::u64 digest(const CompositorGraph& graph) noexcept {
    core::u64 hash = fnv_offset;
    hash_u64(hash, static_cast<core::u64>(graph.nodes.size()));
    for (const GraphNode& node : graph.nodes) {
        hash_u64(hash, node.node.value());
        hash_u64(hash, node.scene.value());
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
                                                    SaturationParameters>) {
                    hash_float(hash, parameters.amount);
                } else if constexpr (std::is_same_v<Parameters,
                                                    BlurParameters>) {
                    hash_float(hash, parameters.radius);
                } else if constexpr (std::is_same_v<Parameters,
                                                     LevelsParameters>) {
                    hash_levels(hash, parameters);
                } else if constexpr (std::is_same_v<
                                         Parameters,
                                         LiftGammaGainParameters>) {
                    hash_lift_gamma_gain(hash, parameters);
                } else if constexpr (std::is_same_v<
                                         Parameters,
                                         ContrastPivotParameters>) {
                    hash_contrast_pivot(hash, parameters);
                } else if constexpr (std::is_same_v<
                                         Parameters,
                                         ColorMatrixParameters>) {
                    hash_color_matrix(hash, parameters);
                } else if constexpr (std::is_same_v<
                                         Parameters,
                                         WhiteBalanceParameters>) {
                    hash_white_balance(hash, parameters);
                } else if constexpr (std::is_same_v<Parameters,
                                                    CurvesParameters>) {
                    hash_curves(hash, parameters);
                } else if constexpr (std::is_same_v<Parameters,
                                                    FilmGrainParameters>) {
                    hash_film_grain(hash, parameters);
                } else if constexpr (std::is_same_v<Parameters,
                                                    BoxBlurParameters>) {
                    hash_u32(hash, parameters.radius_px);
                    hash_u32(hash, parameters.passes);
                } else if constexpr (std::is_same_v<
                                         Parameters,
                                         DirectionalBlurParameters>) {
                    hash_float(hash, parameters.length_px);
                    hash_float(hash, parameters.angle_turns);
                } else if constexpr (std::is_same_v<Parameters,
                                                    ChromaKeyParameters>) {
                    hash_chroma_key(hash, parameters);
                } else if constexpr (std::is_same_v<Parameters,
                                                    LumaKeyParameters>) {
                    hash_luma_key(hash, parameters);
                } else if constexpr (std::is_same_v<Parameters,
                                                    MatteParameters>) {
                    hash_byte(hash, static_cast<core::u8>(parameters.mode));
                    hash_byte(hash, parameters.invert ? 1U : 0U);
                    hash_byte(hash, parameters.clamp_input ? 1U : 0U);
                } else if constexpr (std::is_same_v<Parameters,
                                                    RadialArrayParameters>) {
                    hash_u32(hash, parameters.count);
                    hash_float(hash, parameters.center.x);
                    hash_float(hash, parameters.center.y);
                    hash_float(hash, parameters.center.z);
                    hash_float(hash, parameters.radius);
                    hash_float(hash, parameters.start_angle);
                } else if constexpr (std::is_same_v<
                                         Parameters,
                                         LedCarProcessParameters>) {
                    hash_float(hash, parameters.position.x);
                    hash_float(hash, parameters.position.y);
                    hash_float(hash, parameters.position.z);
                    hash_float(hash, parameters.yaw);
                    hash_float(hash, parameters.pitch);
                    hash_float(hash, parameters.roll);
                    hash_float(hash, parameters.plate_scale);
                    hash_float(hash, parameters.array_scale);
                    hash_float(hash, parameters.reflection_height);
                    hash_float(hash, parameters.reflection_scale);
                    hash_float(hash, parameters.horizontal_blend);
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
        hash_byte(hash, static_cast<core::u8>(diagnostic.severity));
    }

    hash_u64(hash, static_cast<core::u64>(graph.node_metadata.size()));
    for (const GraphNodeMetadata& metadata : graph.node_metadata) {
        hash_u64(hash, metadata.node.value());
        hash_byte(hash, static_cast<core::u8>(metadata.execution_domain));
        hash_byte(hash, static_cast<core::u8>(metadata.identity));
        hash_u64(hash,
                 static_cast<core::u64>(metadata.path_to_output.size()));
        for (const doc::NodeId node : metadata.path_to_output) {
            hash_u64(hash, node.value());
        }
        hash_u64(hash,
                 static_cast<core::u64>(metadata.edges_to_output.size()));
        for (const doc::NodeId edge : metadata.edges_to_output) {
            hash_u64(hash, edge.value());
        }
        hash_float(hash, metadata.bounds_expansion.x);
        hash_float(hash, metadata.bounds_expansion.y);
    }

    hash_byte(hash, graph.resources.available ? 1U : 0U);
    hash_u32(hash, graph.resources.canvas.width);
    hash_u32(hash, graph.resources.canvas.height);
    hash_u32(hash, graph.resources.effect_settings.blur_taps);
    hash_float(hash, graph.resources.effect_settings.blur_sigma_ratio);
    hash_u32(hash, graph.resources.effect_settings.blur_passes);
    hash_u32(hash,
             graph.resources.effect_settings.directional_blur_taps);
    hash_byte(hash, static_cast<core::u8>(
                        graph.resources.effect_settings.brightness_curve));
    hash_float(hash, graph.resources.effect_settings.brightness_gamma);
    hash_u32(hash, graph.resources.active_scene_layers);
    hash_u32(hash, graph.resources.blurred_scene_layers);
    hash_u32(hash, graph.resources.box_blurred_scene_layers);
    hash_u32(hash, graph.resources.directional_blurred_scene_layers);
    hash_u32(hash, graph.resources.chroma_keyed_scene_layers);
    hash_u32(hash, graph.resources.luma_keyed_scene_layers);
    hash_u32(hash, graph.resources.matte_nodes);
    hash_u32(hash, graph.resources.offscreen_textures);
    hash_u64(hash, graph.resources.bytes_per_texture);
    hash_u64(hash, graph.resources.offscreen_texture_bytes);
    hash_u32(hash, graph.resources.render_passes);
    hash_u64(hash, graph.resources.blur_taps_per_pixel);
    hash_float(hash, graph.resources.max_layer_bounds_expansion.x);
    hash_float(hash, graph.resources.max_layer_bounds_expansion.y);

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
            } else if (upstream->kind == NodeKind::Composite ||
                       upstream->kind == NodeKind::Matte ||
                       upstream->kind == NodeKind::LedCarProcess) {
                pending.push_back(upstream->node);
            }
        }
    }

    struct Adjustment final {
        bool brightness_enabled{};
        double brightness{1.0};
        bool saturation_enabled{};
        double saturation{1.0};
        bool blur_enabled{};
        float blur_radius{};
        std::vector<scene::InlineEffectOperation> inline_operations;
        std::vector<scene::SpatialEffectOperation> spatial_operations;
        const PlanStep* radial{};
        std::optional<scene::Mat4> led_car_transform;
        float horizontal_blend{};
    };
    std::map<doc::NodeId, Adjustment> adjustments;
    const auto append_modifier = [&](Adjustment& adjustment,
                                     const GraphNode& modifier) {
        if (effect_node_is_identity(modifier)) {
            return;
        }
        if (modifier.kind == NodeKind::Brightness) {
            const float amount =
                std::get<BrightnessParameters>(modifier.parameters).amount;
            adjustment.brightness_enabled = true;
            adjustment.brightness = scene::composed_brightness_product(
                adjustment.brightness, static_cast<double>(amount));
            adjustment.inline_operations.push_back(
                scene::InlineEffectOperation{
                    scene::EffectKind::Brightness, amount, {}});
        } else if (modifier.kind == NodeKind::Saturation) {
            const float amount =
                std::get<SaturationParameters>(modifier.parameters).amount;
            if (amount != 1.0F) {
                adjustment.saturation_enabled = true;
                adjustment.saturation = scene::composed_saturation_product(
                    adjustment.saturation, static_cast<double>(amount));
                adjustment.inline_operations.push_back(
                    scene::InlineEffectOperation{
                        scene::EffectKind::Saturation, amount, {}});
            }
        } else if (modifier.kind == NodeKind::Blur) {
            const float radius =
                std::get<BlurParameters>(modifier.parameters).radius;
            adjustment.blur_enabled = true;
            adjustment.blur_radius = std::clamp(
                adjustment.blur_radius + radius, 0.0F, max_blur_radius);
            adjustment.spatial_operations.push_back(
                scene::SpatialEffectOperation{
                    scene::EffectKind::Blur, radius, {}, {}});
        } else if (modifier.kind == NodeKind::BoxBlur) {
            adjustment.spatial_operations.push_back(
                scene::SpatialEffectOperation{
                    scene::EffectKind::BoxBlur, 0.0F,
                    std::get<BoxBlurParameters>(modifier.parameters), {}});
        } else if (modifier.kind == NodeKind::DirectionalBlur) {
            adjustment.spatial_operations.push_back(
                scene::SpatialEffectOperation{
                    scene::EffectKind::DirectionalBlur, 0.0F, {},
                    std::get<DirectionalBlurParameters>(modifier.parameters)});
        } else if (modifier.kind == NodeKind::ChromaKey) {
            scene::SpatialEffectOperation operation{};
            operation.kind = scene::EffectKind::ChromaKey;
            operation.chroma_key =
                std::get<ChromaKeyParameters>(modifier.parameters);
            adjustment.spatial_operations.push_back(std::move(operation));
        } else if (modifier.kind == NodeKind::LumaKey) {
            scene::SpatialEffectOperation operation{};
            operation.kind = scene::EffectKind::LumaKey;
            operation.luma_key =
                std::get<LumaKeyParameters>(modifier.parameters);
            adjustment.spatial_operations.push_back(std::move(operation));
        } else if (modifier.kind == NodeKind::Levels) {
            adjustment.inline_operations.push_back(
                scene::InlineEffectOperation{
                    scene::EffectKind::Levels, 1.0F,
                    std::get<LevelsParameters>(modifier.parameters)});
        } else if (modifier.kind == NodeKind::LiftGammaGain) {
            adjustment.inline_operations.push_back(
                scene::InlineEffectOperation{
                    scene::EffectKind::LiftGammaGain, 1.0F, {},
                    std::get<LiftGammaGainParameters>(modifier.parameters)});
        } else if (modifier.kind == NodeKind::ContrastPivot) {
            adjustment.inline_operations.push_back(
                scene::InlineEffectOperation{
                    scene::EffectKind::ContrastPivot, 1.0F, {}, {},
                    std::get<ContrastPivotParameters>(modifier.parameters)});
        } else if (modifier.kind == NodeKind::ColorMatrix) {
            adjustment.inline_operations.push_back(
                scene::InlineEffectOperation{
                    scene::EffectKind::ColorMatrix, 1.0F, {}, {}, {},
                    std::get<ColorMatrixParameters>(modifier.parameters)});
        } else if (modifier.kind == NodeKind::WhiteBalance) {
            adjustment.inline_operations.push_back(
                scene::InlineEffectOperation{
                    scene::EffectKind::WhiteBalance, 1.0F, {}, {}, {}, {},
                    std::get<WhiteBalanceParameters>(modifier.parameters)});
        } else if (modifier.kind == NodeKind::Curves) {
            adjustment.inline_operations.push_back(
                scene::InlineEffectOperation{
                    scene::EffectKind::Curves, 1.0F, {}, {}, {}, {}, {},
                    std::get<CurvesParameters>(modifier.parameters)});
        } else if (modifier.kind == NodeKind::FilmGrain) {
            scene::InlineEffectOperation operation{};
            operation.kind = scene::EffectKind::FilmGrain;
            operation.film_grain =
                std::get<FilmGrainParameters>(modifier.parameters);
            adjustment.inline_operations.push_back(std::move(operation));
        } else if (modifier.kind == NodeKind::RadialArray &&
                   adjustment.radial == nullptr) {
            adjustment.radial = graph.step(modifier.node);
        }
    };
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
            append_modifier(adjustment, *modifier);
        }
    }

    // The setup node is downstream of each SceneLayer, so its effects append
    // after layer-authored effects and its slot matrix becomes a parent of the
    // layer's evaluated world. That preserves local corrections, masks and
    // per-layer effects while letting the whole rig move as one object.
    for (const GraphNode& setup : graph.nodes) {
        if (setup.kind != NodeKind::LedCarProcess ||
            !visited.contains(setup.node)) {
            continue;
        }
        const auto& parameters =
            std::get<LedCarProcessParameters>(setup.parameters);
        const PlanStep* const setup_step = graph.step(setup.node);
        if (setup_step == nullptr) {
            continue;
        }
        const scene::Mat4 setup_world = scene::multiply(
            scene::translation(parameters.position),
            scene::rotation(parameters.yaw, parameters.pitch,
                            parameters.roll));
        for (const PlanConnection& image : setup_step->inputs) {
            if (image.type != PortValueType::Image || image.target_port > 8U) {
                continue;
            }
            const GraphNode* const layer = graph.node(image.source);
            if (layer == nullptr || layer->kind != NodeKind::SceneLayer) {
                continue;
            }
            const doc::NodeId source_node =
                std::get<SceneLayerParameters>(layer->parameters).scene_node;
            Adjustment& adjustment = adjustments[source_node];
            scene::Mat4 slot{};
            if (image.target_port < 8U) {
                const scene::Turns angle = static_cast<float>(
                    static_cast<double>(image.target_port) / 8.0);
                const scene::Vec3 position{
                    scene::deterministic::sin_turns(angle) *
                        parameters.array_scale,
                    0.0F,
                    scene::deterministic::cos_turns(angle) *
                        parameters.array_scale};
                slot = scene::multiply(
                    scene::translation(position),
                    scene::multiply(
                        scene::rotation(scene::wrap_turns(
                                            static_cast<double>(angle) + 0.5),
                                        0.0F, 0.0F),
                        scene::scaling(scene::Vec3{parameters.plate_scale,
                                                   parameters.plate_scale,
                                                   parameters.plate_scale})));
                adjustment.horizontal_blend = parameters.horizontal_blend;
            } else {
                slot = scene::multiply(
                    scene::translation(
                        scene::Vec3{0.0F, parameters.reflection_height, 0.0F}),
                    scene::multiply(
                        scene::rotation(0.0F, 0.25F, 0.0F),
                        scene::scaling(scene::Vec3{
                            parameters.reflection_scale,
                            parameters.reflection_scale,
                            parameters.reflection_scale})));
            }
            adjustment.led_car_transform =
                scene::multiply(setup_world, slot);
            for (const PlanConnection& effect : setup_step->inputs) {
                if (effect.type != PortValueType::Effect ||
                    effect.target_port != 9U) {
                    continue;
                }
                const GraphNode* const modifier = graph.node(effect.source);
                if (modifier != nullptr) {
                    append_modifier(adjustment, *modifier);
                }
            }
        }
    }

    scene::Scene result = source;
    const float brightness_gamma =
        scene::clamped(source.effect_settings).brightness_gamma;
    const auto fold_style = [&](doc::NodeId id,
                                 scene::LayerStyle& style) {
        const auto found = adjustments.find(id);
        if (found == adjustments.end()) {
            return;
        }
        const Adjustment& adjustment = found->second;
        if (style.effects.inline_operations.empty()) {
            if (style.effects.brightness_enabled
                && std::isfinite(style.effects.brightness)
                && style.effects.brightness != 1.0F) {
                style.effects.inline_operations.push_back(
                    scene::InlineEffectOperation{
                        scene::EffectKind::Brightness,
                        style.effects.brightness, {}});
            }
            const float saturation =
                scene::normalized_saturation(style.effects.saturation);
            if (style.effects.saturation_enabled && saturation != 1.0F) {
              style.effects.inline_operations.push_back(
                  scene::InlineEffectOperation{
                      scene::EffectKind::Saturation, saturation, {}});
            }
        }
        // Synthesize the legacy Gaussian carrier only when a real graph
        // spatial operation must be appended after it. An identity or purely
        // inline graph must leave the evaluated EffectStack byte-for-byte
        // unchanged; the executor can still derive the Gaussian operation
        // from blur_enabled/blur_radius at its compatibility boundary.
        if (!adjustment.spatial_operations.empty() &&
            style.effects.spatial_operations.empty() &&
            style.effects.blur_enabled &&
            std::isfinite(style.effects.blur_radius) &&
            style.effects.blur_radius > 0.0F) {
          style.effects.spatial_operations.push_back(
              scene::SpatialEffectOperation{
                  scene::EffectKind::Blur, style.effects.blur_radius, {}, {}});
        }
        style.effects.inline_operations.insert(
            style.effects.inline_operations.end(),
            adjustment.inline_operations.begin(),
            adjustment.inline_operations.end());
        style.effects.spatial_operations.insert(
            style.effects.spatial_operations.end(),
            adjustment.spatial_operations.begin(),
            adjustment.spatial_operations.end());
        if (adjustment.brightness_enabled) {
            const double base = style.effects.brightness_enabled
                ? (style.effects.brightness_product_valid
                       ? scene::composed_brightness_product(
                             style.effects.brightness_product, 1.0)
                       : (std::isfinite(style.effects.brightness)
                              ? static_cast<double>(std::clamp(
                                    style.effects.brightness, 0.0F,
                                    max_brightness))
                              : 1.0))
                : 1.0;
            const double brightness = scene::composed_brightness_product(
                base, adjustment.brightness);
            style.effects.brightness_enabled = true;
            style.effects.brightness_product_valid = true;
            style.effects.brightness_product = brightness;
            style.effects.brightness = scene::brightness_from_product(
                brightness, brightness_gamma);
        }
        if (adjustment.saturation_enabled) {
            const double base_saturation =
                style.effects.saturation_enabled
                ? (style.effects.saturation_product_valid
                       ? scene::composed_saturation_product(
                             style.effects.saturation_product, 1.0)
                       : static_cast<double>(scene::normalized_saturation(
                             style.effects.saturation)))
                : 1.0;
            style.effects.saturation_enabled = true;
            const double saturation = scene::composed_saturation_product(
                base_saturation, adjustment.saturation);
            style.effects.saturation_product_valid = true;
            style.effects.saturation_product = saturation;
            style.effects.saturation =
                scene::saturation_from_product(saturation);
        }
        if (adjustment.blur_enabled) {
            const float base = style.effects.blur_enabled
                    && std::isfinite(style.effects.blur_radius)
                    && style.effects.blur_radius > 0.0F
                ? std::clamp(style.effects.blur_radius, 0.0F,
                             max_blur_radius)
                : 0.0F;
            style.effects.blur_enabled = true;
            style.effects.blur_radius =
                std::clamp(base + adjustment.blur_radius, 0.0F,
                           max_blur_radius);
        }
        style.horizontal_blend = std::max(
            style.horizontal_blend, adjustment.horizontal_blend);
    };
    for (scene::MeshInstance& instance : result.instances) {
        fold_style(instance.source, instance.style);
    }
    for (scene::MediaInstance& instance : result.media) {
        fold_style(instance.source_node, instance.style);
        // A compositor Scene Layer may name the Track screen rather than one
        // placement. Track adjustments run after the clip's own graph branch,
        // matching the authored scene fold where a Track grade follows the
        // currently playing clip.
        if (instance.timeline_lane != doc::NodeId{} &&
            instance.timeline_lane != instance.source_node) {
            fold_style(instance.timeline_lane, instance.style);
        }
    }
    for (scene::ModelInstance& instance : result.models) {
        fold_style(instance.source_node, instance.style);
    }
    for (scene::SceneScreen& screen : result.screens) {
        fold_style(screen.node, screen.style);
    }

    const auto setup_transform = [&adjustments](doc::NodeId id)
        -> const scene::Mat4* {
        const auto found = adjustments.find(id);
        return found != adjustments.end() &&
                found->second.led_car_transform.has_value()
            ? std::addressof(*found->second.led_car_transform)
            : nullptr;
    };
    const auto apply_setup_transform = [&](auto& value, doc::NodeId id) {
        const scene::Mat4* const setup = setup_transform(id);
        if (setup == nullptr) {
            return;
        }
        value.world = scene::multiply(*setup, value.world);
        value.normal = scene::normal_basis(value.world);
    };
    for (scene::MeshInstance& instance : result.instances) {
        apply_setup_transform(instance, instance.source);
    }
    for (scene::MediaInstance& instance : result.media) {
        const doc::NodeId id = setup_transform(instance.source_node) != nullptr
            ? instance.source_node
            : instance.timeline_lane;
        apply_setup_transform(instance, id);
    }
    for (scene::ModelInstance& instance : result.models) {
        apply_setup_transform(instance, instance.source_node);
    }
    for (scene::SceneScreen& screen : result.screens) {
        if (const scene::Mat4* const setup = setup_transform(screen.node);
            setup != nullptr) {
            screen.world = scene::multiply(*setup, screen.world);
        }
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
           [&adjustments](const scene::MediaInstance& value) noexcept {
               const auto clip = adjustments.find(value.source_node);
               if (clip != adjustments.end() && clip->second.radial != nullptr) {
                   return value.source_node;
               }
               return value.timeline_lane != doc::NodeId{}
                   ? value.timeline_lane
                   : value.source_node;
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
