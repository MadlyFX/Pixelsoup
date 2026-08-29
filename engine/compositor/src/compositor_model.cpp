#include <mediaserver/compositor/compositor_model.hpp>

#include <mediaserver/compositor/compositor_schema.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace mediaserver::compositor {
namespace {

[[nodiscard]] const doc::PropertyValue* find(const doc::PropertyMap& properties,
                                             std::string_view name) noexcept {
    const auto found = properties.find(name);
    return found == properties.end() ? nullptr : &found->second;
}

class Reader final {
  public:
    explicit Reader(const doc::PropertyMap& properties) noexcept
        : properties_(&properties) {}

    [[nodiscard]] float number(std::string_view name, float fallback) noexcept {
        const doc::PropertyValue* const value = find(*properties_, name);
        if (value == nullptr) {
            return fallback;
        }
        double wide{};
        if (const auto* const real = std::get_if<double>(value)) {
            wide = *real;
        } else if (const auto* const whole = std::get_if<std::int64_t>(value)) {
            wide = static_cast<double>(*whole);
        } else {
            ok_ = false;
            return fallback;
        }
        const float narrowed = static_cast<float>(wide);
        if (!std::isfinite(narrowed)) {
            ok_ = false;
            return fallback;
        }
        return narrowed;
    }

    [[nodiscard]] std::int64_t integer(std::string_view name,
                                       std::int64_t fallback) noexcept {
        const doc::PropertyValue* const value = find(*properties_, name);
        if (value == nullptr) {
            return fallback;
        }
        if (const auto* const whole = std::get_if<std::int64_t>(value)) {
            return *whole;
        }
        ok_ = false;
        return fallback;
    }

    [[nodiscard]] std::string text(std::string_view name,
                                   std::string_view fallback) noexcept {
        const doc::PropertyValue* const value = find(*properties_, name);
        if (value == nullptr) {
            return std::string{fallback};
        }
        if (const auto* const string = std::get_if<std::string>(value)) {
            return *string;
        }
        ok_ = false;
        return std::string{fallback};
    }

    [[nodiscard]] bool boolean(std::string_view name, bool fallback) noexcept {
        const doc::PropertyValue* const value = find(*properties_, name);
        if (value == nullptr) {
            return fallback;
        }
        if (const auto* const flag = std::get_if<bool>(value)) {
            return *flag;
        }
        ok_ = false;
        return fallback;
    }

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    void fail() noexcept { ok_ = false; }

  private:
    const doc::PropertyMap* properties_{};
    bool ok_{true};
};

[[nodiscard]] std::optional<LevelsParameters>
read_levels(Reader& reader) noexcept {
    LevelsParameters value{};
    const auto read_channel = [&reader](scene::LevelsChannelParameters& channel,
                                        std::string_view input_black,
                                        std::string_view input_white,
                                        std::string_view gamma,
                                        std::string_view output_black,
                                        std::string_view output_white) noexcept {
        channel.input_black = reader.number(input_black, 0.0F);
        channel.input_white = reader.number(input_white, 1.0F);
        channel.gamma = reader.number(gamma, 1.0F);
        channel.output_black = reader.number(output_black, 0.0F);
        channel.output_white = reader.number(output_white, 1.0F);
    };
    read_channel(value.master, schema::levels_master_input_black,
                 schema::levels_master_input_white,
                 schema::levels_master_gamma,
                 schema::levels_master_output_black,
                 schema::levels_master_output_white);
    read_channel(value.red, schema::levels_red_input_black,
                 schema::levels_red_input_white, schema::levels_red_gamma,
                 schema::levels_red_output_black,
                 schema::levels_red_output_white);
    read_channel(value.green, schema::levels_green_input_black,
                 schema::levels_green_input_white, schema::levels_green_gamma,
                 schema::levels_green_output_black,
                 schema::levels_green_output_white);
    read_channel(value.blue, schema::levels_blue_input_black,
                 schema::levels_blue_input_white, schema::levels_blue_gamma,
                 schema::levels_blue_output_black,
                 schema::levels_blue_output_white);
    return reader.ok() ? scene::normalized_levels(value) : std::nullopt;
}

[[nodiscard]] std::optional<LiftGammaGainParameters>
read_lift_gamma_gain(Reader& reader) noexcept {
    LiftGammaGainParameters value{};
    value.lift = scene::Vec3{
        reader.number(schema::lift_r, 0.0F),
        reader.number(schema::lift_g, 0.0F),
        reader.number(schema::lift_b, 0.0F)};
    value.gamma = scene::Vec3{
        reader.number(schema::gamma_r, 1.0F),
        reader.number(schema::gamma_g, 1.0F),
        reader.number(schema::gamma_b, 1.0F)};
    value.gain = scene::Vec3{
        reader.number(schema::gain_r, 1.0F),
        reader.number(schema::gain_g, 1.0F),
        reader.number(schema::gain_b, 1.0F)};
    return reader.ok() ? scene::normalized_lift_gamma_gain(value)
                       : std::nullopt;
}

[[nodiscard]] std::optional<ContrastPivotParameters>
read_contrast_pivot(Reader& reader) noexcept {
    ContrastPivotParameters value{};
    value.contrast = reader.number(schema::contrast, value.contrast);
    value.pivot = reader.number(schema::pivot, value.pivot);
    return reader.ok() ? scene::normalized_contrast_pivot(value)
                       : std::nullopt;
}

[[nodiscard]] std::optional<ColorMatrixParameters>
read_color_matrix(Reader& reader) noexcept {
    ColorMatrixParameters value{};
    const std::array<std::string_view, 9U> properties{
        schema::m00, schema::m01, schema::m02,
        schema::m10, schema::m11, schema::m12,
        schema::m20, schema::m21, schema::m22};
    for (std::size_t index = 0U; index < properties.size(); ++index) {
        value.matrix[index] =
            reader.number(properties[index], value.matrix[index]);
    }
    value.offset = scene::Vec3{
        reader.number(schema::offset_r, value.offset.x),
        reader.number(schema::offset_g, value.offset.y),
        reader.number(schema::offset_b, value.offset.z)};
    return reader.ok() ? scene::normalized_color_matrix(value)
                       : std::nullopt;
}

[[nodiscard]] std::optional<WhiteBalanceParameters>
read_white_balance(Reader& reader) noexcept {
    WhiteBalanceParameters value{};
    value.temperature_kelvin = reader.number(
        schema::temperature_kelvin, value.temperature_kelvin);
    value.tint = reader.number(schema::tint, value.tint);
    return reader.ok() ? scene::normalized_white_balance(value)
                       : std::nullopt;
}

[[nodiscard]] std::optional<scene::CurveChannelParameters> read_curve_channel(
    Reader& reader, const doc::PropertyMap& properties,
    std::string_view prefix, bool required) {
    scene::CurveChannelParameters channel{};
    channel.points.clear();
    bool gap{};
    for (core::u32 index = 0U;
         index <= scene::max_curve_control_points; ++index) {
        const std::string base = std::string{prefix} + std::to_string(index);
        const std::string x_name = base + std::string{schema::curve_x_suffix};
        const std::string y_name = base + std::string{schema::curve_y_suffix};
        const bool has_x = find(properties, x_name) != nullptr;
        const bool has_y = find(properties, y_name) != nullptr;
        if (!has_x && !has_y) {
            gap = true;
            continue;
        }
        if (!has_x || !has_y || gap
            || index == scene::max_curve_control_points) {
            reader.fail();
            return std::nullopt;
        }
        channel.points.push_back(scene::CurveControlPoint{
            reader.number(x_name, 0.0F), reader.number(y_name, 0.0F)});
    }
    if (channel.points.empty()) {
        return required
            ? std::optional<scene::CurveChannelParameters>{
                  scene::CurveChannelParameters{}}
            : std::nullopt;
    }
    return channel;
}

[[nodiscard]] std::optional<CurvesParameters> read_curves(
    Reader& reader, const doc::PropertyMap& properties) {
    CurvesParameters value{};
    const auto master = read_curve_channel(
        reader, properties, schema::curve_master_prefix, true);
    if (!master.has_value()) {
        reader.fail();
        return std::nullopt;
    }
    value.master = *master;
    value.red = read_curve_channel(
        reader, properties, schema::curve_red_prefix, false);
    value.green = read_curve_channel(
        reader, properties, schema::curve_green_prefix, false);
    value.blue = read_curve_channel(
        reader, properties, schema::curve_blue_prefix, false);
    return reader.ok() ? scene::normalized_curves(value) : std::nullopt;
}

[[nodiscard]] bool as_node(std::int64_t value, doc::NodeId& output) noexcept {
    if (value <= 0) {
        return false;
    }
    output = doc::NodeId{static_cast<core::u64>(value)};
    return true;
}

[[nodiscard]] bool as_port(std::int64_t value, core::u8& output) noexcept {
    if (value < 0 || value > static_cast<std::int64_t>(
                                 std::numeric_limits<core::u8>::max())) {
        return false;
    }
    output = static_cast<core::u8>(value);
    return true;
}

[[nodiscard]] bool as_u32(std::int64_t value, core::u32& output) noexcept {
    if (value < 0 || value > static_cast<std::int64_t>(
                                 std::numeric_limits<core::u32>::max())) {
        return false;
    }
    output = static_cast<core::u32>(value);
    return true;
}

[[nodiscard]] bool as_i32(std::int64_t value, core::i32& output) noexcept {
    if (value < static_cast<std::int64_t>(
                    std::numeric_limits<core::i32>::min()) ||
        value > static_cast<std::int64_t>(
                    std::numeric_limits<core::i32>::max())) {
        return false;
    }
    output = static_cast<core::i32>(value);
    return true;
}

[[nodiscard]] std::optional<FilmGrainParameters>
read_film_grain(Reader& reader) noexcept {
    FilmGrainParameters value{};
    value.amount = reader.number(schema::amount, value.amount);
    value.pixel_scale =
        reader.number(schema::pixel_scale, value.pixel_scale);
    value.chroma_ratio =
        reader.number(schema::chroma_ratio, value.chroma_ratio);
    if (!as_u32(reader.integer(schema::seed, 0), value.seed)) {
        reader.fail();
    }
    const std::string mode =
        reader.text(schema::mode, schema::monochrome_mode);
    if (mode == schema::monochrome_mode) {
        value.mode = scene::FilmGrainMode::Monochrome;
    } else if (mode == schema::rgb_mode) {
        value.mode = scene::FilmGrainMode::RGB;
    } else {
        reader.fail();
    }
    return reader.ok() ? scene::normalized_film_grain(value)
                       : std::nullopt;
}

[[nodiscard]] std::optional<ChromaKeyParameters>
read_chroma_key(Reader& reader) noexcept {
    ChromaKeyParameters value{};
    value.mix = reader.number(schema::chroma_key_mix, value.mix);
    if (!reader.ok() || value.mix == 0.0F) {
        return reader.ok() ? scene::normalized_chroma_key(value)
                           : std::nullopt;
    }
    value.key_color = scene::Vec3{
        reader.number(schema::chroma_key_color_r, value.key_color.x),
        reader.number(schema::chroma_key_color_g, value.key_color.y),
        reader.number(schema::chroma_key_color_b, value.key_color.z)};
    value.tolerance =
        reader.number(schema::chroma_key_tolerance, value.tolerance);
    value.softness =
        reader.number(schema::chroma_key_softness, value.softness);
    if (!as_i32(reader.integer(schema::chroma_key_morphology_px, 0),
                value.morphology_px)) {
        reader.fail();
    }
    value.spill_suppression = reader.number(
        schema::chroma_key_spill_suppression, value.spill_suppression);
    return reader.ok() ? scene::normalized_chroma_key(value) : std::nullopt;
}

[[nodiscard]] std::optional<LumaKeyParameters>
read_luma_key(Reader& reader) noexcept {
    LumaKeyParameters value{};
    value.mix = reader.number(schema::luma_key_mix, value.mix);
    if (!reader.ok() || value.mix == 0.0F) {
        return reader.ok() ? scene::normalized_luma_key(value)
                           : std::nullopt;
    }
    value.low_threshold =
        reader.number(schema::luma_key_low_threshold, value.low_threshold);
    value.high_threshold = reader.number(
        schema::luma_key_high_threshold, value.high_threshold);
    value.low_softness =
        reader.number(schema::luma_key_low_softness, value.low_softness);
    value.high_softness =
        reader.number(schema::luma_key_high_softness, value.high_softness);
    value.invert = reader.boolean(schema::luma_key_invert, value.invert);
    return reader.ok() ? scene::normalized_luma_key(value) : std::nullopt;
}

[[nodiscard]] std::optional<NodeKind>
kind_for_type(std::string_view type) noexcept {
    if (type == schema::scene_layer_type) {
        return NodeKind::SceneLayer;
    }
    if (type == schema::brightness_type) {
        return NodeKind::Brightness;
    }
    if (type == schema::saturation_type) {
        return NodeKind::Saturation;
    }
    if (type == schema::blur_type) {
        return NodeKind::Blur;
    }
    if (type == schema::levels_type) {
        return NodeKind::Levels;
    }
    if (type == schema::lift_gamma_gain_type) {
        return NodeKind::LiftGammaGain;
    }
    if (type == schema::contrast_pivot_type) {
        return NodeKind::ContrastPivot;
    }
    if (type == schema::color_matrix_type) {
        return NodeKind::ColorMatrix;
    }
    if (type == schema::white_balance_type) {
        return NodeKind::WhiteBalance;
    }
    if (type == schema::curves_type) {
        return NodeKind::Curves;
    }
    if (type == schema::film_grain_type) {
        return NodeKind::FilmGrain;
    }
    if (type == schema::box_blur_type) {
        return NodeKind::BoxBlur;
    }
    if (type == schema::directional_blur_type) {
        return NodeKind::DirectionalBlur;
    }
    if (type == schema::chroma_key_type) {
        return NodeKind::ChromaKey;
    }
    if (type == schema::luma_key_type) {
        return NodeKind::LumaKey;
    }
    if (type == schema::matte_type) {
        return NodeKind::Matte;
    }
    if (type == schema::radial_array_type) {
        return NodeKind::RadialArray;
    }
    if (type == schema::composite_type) {
        return NodeKind::Composite;
    }
    if (type == schema::output_type) {
        return NodeKind::Output;
    }
    return std::nullopt;
}

[[nodiscard]] bool is_compositor_type(std::string_view type) noexcept {
    return type == schema::edge_type || kind_for_type(type).has_value();
}

[[nodiscard]] bool in_extent(float value) noexcept {
    return value >= -max_spatial_extent && value <= max_spatial_extent;
}

[[nodiscard]] GraphNode parse_node(const doc::Node& node, NodeKind kind,
                                   Reader& reader) {
    GraphNode parsed{};
    parsed.node = node.id;
    parsed.kind = kind;
    // Zero stays zero: an absent owner is legacy content, resolved against the
    // default scene when the graph is evaluated rather than guessed here.
    static_cast<void>(as_node(reader.integer(schema::scene_id, 0),
                              parsed.scene));
    parsed.label = reader.text(schema::label, node_kind_name(kind));
    parsed.editor_x = reader.number(schema::editor_x, 0.0F);
    parsed.editor_y = reader.number(schema::editor_y, 0.0F);
    if (!in_extent(parsed.editor_x) || !in_extent(parsed.editor_y)) {
        reader.fail();
    }

    switch (kind) {
    case NodeKind::SceneLayer: {
        SceneLayerParameters parameters{};
        if (!as_node(reader.integer(schema::scene_node, 0),
                     parameters.scene_node)) {
            reader.fail();
        }
        parsed.parameters = parameters;
        break;
    }
    case NodeKind::Brightness: {
        const float amount = reader.number(schema::amount, 1.0F);
        if (amount < 0.0F || amount > max_brightness) {
            reader.fail();
        }
        parsed.parameters = BrightnessParameters{amount};
        break;
    }
    case NodeKind::Saturation: {
        const float amount = reader.number(schema::amount, 1.0F);
        if (amount < 0.0F || amount > max_saturation) {
            reader.fail();
        }
        parsed.parameters = SaturationParameters{amount};
        break;
    }
    case NodeKind::Blur: {
        const float radius = reader.number(schema::radius, 0.0F);
        if (radius < 0.0F || radius > max_blur_radius) {
            reader.fail();
        }
        parsed.parameters = BlurParameters{radius};
        break;
    }
    case NodeKind::Levels: {
        const auto levels = read_levels(reader);
        if (!levels.has_value()) {
            reader.fail();
        } else {
            parsed.parameters = *levels;
        }
        break;
    }
    case NodeKind::LiftGammaGain: {
        const auto parameters = read_lift_gamma_gain(reader);
        if (!parameters.has_value()) {
            reader.fail();
        } else {
            parsed.parameters = *parameters;
        }
        break;
    }
    case NodeKind::ContrastPivot: {
        const auto parameters = read_contrast_pivot(reader);
        if (!parameters.has_value()) {
            reader.fail();
        } else {
            parsed.parameters = *parameters;
        }
        break;
    }
    case NodeKind::ColorMatrix: {
        const auto parameters = read_color_matrix(reader);
        if (!parameters.has_value()) {
            reader.fail();
        } else {
            parsed.parameters = *parameters;
        }
        break;
    }
    case NodeKind::WhiteBalance: {
        const auto parameters = read_white_balance(reader);
        if (!parameters.has_value()) {
            reader.fail();
        } else {
            parsed.parameters = *parameters;
        }
        break;
    }
    case NodeKind::Curves: {
        const auto parameters = read_curves(reader, node.properties);
        if (!parameters.has_value()) {
            reader.fail();
        } else {
            parsed.parameters = *parameters;
        }
        break;
    }
    case NodeKind::FilmGrain: {
        const auto parameters = read_film_grain(reader);
        if (!parameters.has_value()) {
            reader.fail();
        } else {
            parsed.parameters = *parameters;
        }
        break;
    }
    case NodeKind::BoxBlur: {
        core::u32 radius{};
        core::u32 passes{};
        if (!as_u32(reader.integer(schema::box_blur_radius, 0), radius) ||
            !as_u32(reader.integer(schema::box_blur_passes, 1), passes)) {
            reader.fail();
        }
        const auto parameters = scene::normalized_box_blur(
            BoxBlurParameters{radius, passes});
        if (!parameters.has_value()) {
            reader.fail();
        } else {
            parsed.parameters = *parameters;
        }
        break;
    }
    case NodeKind::DirectionalBlur: {
        const auto parameters = scene::normalized_directional_blur(
            DirectionalBlurParameters{
                reader.number(schema::directional_blur_length, 0.0F),
                reader.number(schema::directional_blur_angle_turns, 0.0F)});
        if (!parameters.has_value()) {
            reader.fail();
        } else {
            parsed.parameters = *parameters;
        }
        break;
    }
    case NodeKind::ChromaKey: {
        const auto parameters = read_chroma_key(reader);
        if (!parameters.has_value()) {
            reader.fail();
        } else {
            parsed.parameters = *parameters;
        }
        break;
    }
    case NodeKind::LumaKey: {
        const auto parameters = read_luma_key(reader);
        if (!parameters.has_value()) {
            reader.fail();
        } else {
            parsed.parameters = *parameters;
        }
        break;
    }
    case NodeKind::Matte: {
        MatteParameters parameters{};
        const std::string mode = reader.text(
            schema::matte_mode, schema::matte_alpha_mode);
        if (mode == schema::matte_alpha_mode) {
            parameters.mode = MatteMode::Alpha;
        } else if (mode == schema::matte_luma_mode) {
            parameters.mode = MatteMode::Luma;
        } else {
            reader.fail();
        }
        parameters.invert =
            reader.boolean(schema::matte_invert, parameters.invert);
        parameters.clamp_input = reader.boolean(
            schema::matte_clamp_input, parameters.clamp_input);
        parsed.parameters = parameters;
        break;
    }
    case NodeKind::RadialArray: {
        RadialArrayParameters parameters{};
        if (!as_u32(reader.integer(schema::count, 9), parameters.count) ||
            parameters.count == 0U || parameters.count > max_radial_instances) {
            reader.fail();
        }
        parameters.center = scene::Vec3{
            reader.number(schema::center_x, 0.0F),
            reader.number(schema::center_y, 0.0F),
            reader.number(schema::center_z, 0.0F),
        };
        parameters.radius = reader.number(schema::radius, 1.0F);
        parameters.start_angle = reader.number(schema::start_angle_turns, 0.0F);
        if (!in_extent(parameters.center.x) ||
            !in_extent(parameters.center.y) ||
            !in_extent(parameters.center.z) || parameters.radius < 0.0F ||
            parameters.radius > max_spatial_extent) {
            reader.fail();
        }
        parsed.parameters = parameters;
        break;
    }
    case NodeKind::Composite:
        parsed.parameters = CompositeParameters{};
        break;
    case NodeKind::Output:
        parsed.parameters =
            OutputParameters{reader.boolean(schema::replace_scene, false)};
        break;
    }
    return parsed;
}

[[nodiscard]] PlanConnection connection_from(const GraphEdge& edge,
                                             PortValueType type) noexcept {
    return PlanConnection{edge.node,    edge.from.node, edge.from.port,
                          edge.to.node, edge.to.port,   edge.order,
                          type};
}

[[nodiscard]] bool diagnostic_less(const GraphDiagnostic& left,
                                   const GraphDiagnostic& right) noexcept {
    return left.sort_key() < right.sort_key();
}

[[nodiscard]] GraphExecutionDomain
execution_domain(NodeKind kind) noexcept {
    switch (kind) {
    case NodeKind::Brightness:
    case NodeKind::Saturation:
    case NodeKind::Levels:
    case NodeKind::LiftGammaGain:
    case NodeKind::ContrastPivot:
    case NodeKind::ColorMatrix:
    case NodeKind::WhiteBalance:
    case NodeKind::Curves:
    case NodeKind::FilmGrain:
        return GraphExecutionDomain::Inline;
    case NodeKind::RadialArray:
        return GraphExecutionDomain::Geometry;
    case NodeKind::Blur:
    case NodeKind::BoxBlur:
    case NodeKind::DirectionalBlur:
    case NodeKind::ChromaKey:
    case NodeKind::LumaKey:
    case NodeKind::Matte:
        return GraphExecutionDomain::IsolatedLayer;
    case NodeKind::SceneLayer:
    case NodeKind::Composite:
    case NodeKind::Output:
        return GraphExecutionDomain::Structural;
    }
    return GraphExecutionDomain::Structural;
}

[[nodiscard]] bool is_effect_node(NodeKind kind) noexcept {
    return kind != NodeKind::SceneLayer && kind != NodeKind::Composite &&
        kind != NodeKind::Output && kind != NodeKind::Matte;
}

[[nodiscard]] scene::EffectBoundsExpansion
node_bounds_expansion(const GraphNode& node) noexcept {
    if (effect_node_is_identity(node)) {
        return {};
    }
    if (const auto* const blur =
            std::get_if<BlurParameters>(&node.parameters)) {
        return scene::EffectBoundsExpansion{blur->radius, blur->radius};
    }
    if (const auto* const box =
            std::get_if<BoxBlurParameters>(&node.parameters)) {
        return scene::box_blur_bounds(*box);
    }
    if (const auto* const directional =
            std::get_if<DirectionalBlurParameters>(&node.parameters)) {
        return scene::directional_blur_bounds(*directional);
    }
    return {};
}

[[nodiscard]] bool input_connection_less(const PlanConnection& left,
                                         const PlanConnection& right) noexcept {
    return std::tuple{left.target_port, left.order, left.edge} <
           std::tuple{right.target_port, right.order, right.edge};
}

[[nodiscard]] bool
consumer_connection_less(const PlanConnection& left,
                         const PlanConnection& right) noexcept {
    return std::tuple{left.source_port, left.target, left.target_port,
                      left.order, left.edge} <
           std::tuple{right.source_port, right.target, right.target_port,
                      right.order, right.edge};
}

[[nodiscard]] std::vector<RadialInstance>
radial_instances(const RadialArrayParameters& parameters) {
    std::vector<RadialInstance> result;
    result.reserve(parameters.count);
    for (core::u32 index = 0U; index < parameters.count; ++index) {
        const double fraction =
            static_cast<double>(index) / static_cast<double>(parameters.count);
        const scene::Turns angle = scene::wrap_turns(
            static_cast<double>(parameters.start_angle) + fraction);
        const float sine = scene::deterministic::sin_turns(angle);
        const float cosine = scene::deterministic::cos_turns(angle);
        const scene::Vec3 position{
            parameters.center.x + sine * parameters.radius,
            parameters.center.y,
            parameters.center.z + cosine * parameters.radius,
        };
        // The local +Z face points toward the array centre. At angle zero the
        // plate is on +Z and yaws half a turn, which is a useful, stable
        // convention for nine-angle vehicle capture stages.
        const scene::Turns inward_yaw =
            scene::wrap_turns(static_cast<double>(angle) + 0.5);
        const scene::Mat4 world =
            scene::multiply(scene::translation(position),
                            scene::rotation(inward_yaw, 0.0F, 0.0F));
        result.push_back(RadialInstance{index, angle, world});
    }
    return result;
}

} // namespace

core::u32 CompositorCounters::nodes_over_capacity() const noexcept {
    return nodes > max_graph_nodes ? nodes - max_graph_nodes : 0U;
}

core::u32 CompositorCounters::edges_over_capacity() const noexcept {
    return edges > max_graph_edges ? edges - max_graph_edges : 0U;
}

void CompositorModel::forget(doc::NodeId id) noexcept {
    nodes_.erase(id);
    edges_.erase(id);
    document_types_.erase(id);
}

void CompositorModel::put(const doc::Node& node) {
    forget(node.id);
    document_types_.insert_or_assign(node.id, node.type);

    if (const auto kind = kind_for_type(node.type); kind.has_value()) {
        Reader reader{node.properties};
        GraphNode parsed = parse_node(node, *kind, reader);
        if (!reader.ok()) {
            ++counters_.rejected;
            return;
        }
        nodes_.insert_or_assign(node.id, std::move(parsed));
        return;
    }

    if (node.type != schema::edge_type) {
        return;
    }

    Reader reader{node.properties};
    GraphEdge edge{};
    edge.node = node.id;
    if (!as_node(reader.integer(schema::from_node, 0), edge.from.node) ||
        !as_port(reader.integer(schema::from_port, 0), edge.from.port) ||
        !as_node(reader.integer(schema::to_node, 0), edge.to.node) ||
        !as_port(reader.integer(schema::to_port, 0), edge.to.port)) {
        reader.fail();
    }
    edge.order = reader.integer(schema::order, 0);
    if (!reader.ok()) {
        ++counters_.rejected;
        return;
    }
    edges_.insert_or_assign(node.id, edge);
}

void CompositorModel::recount() noexcept {
    counters_.nodes = static_cast<core::u32>(std::min<std::size_t>(
        nodes_.size(),
        static_cast<std::size_t>(std::numeric_limits<core::u32>::max())));
    counters_.edges = static_cast<core::u32>(std::min<std::size_t>(
        edges_.size(),
        static_cast<std::size_t>(std::numeric_limits<core::u32>::max())));
}

void CompositorModel::reset(const doc::Root& root) {
    nodes_.clear();
    edges_.clear();
    document_types_.clear();
    counters_ = CompositorCounters{};
    resource_scene_model_.reset(root);
    const doc::Delta delta = diff(doc::Root{}, root);
    for (const doc::NodeChange& change : delta.changes) {
        if (change.after) {
            put(*change.after);
        }
    }
    recount();
}

void CompositorModel::apply(const doc::Delta& delta) {
    resource_scene_model_.apply(delta);
    for (const doc::NodeChange& change : delta.changes) {
        switch (change.kind) {
        case doc::ChangeKind::Removed:
            forget(change.id);
            break;
        case doc::ChangeKind::Added:
        case doc::ChangeKind::Modified:
            if (change.after) {
                put(*change.after);
            }
            break;
        }
    }
    recount();
}

CompositorGraph CompositorModel::evaluate() const {
    return build(nullptr, doc::NodeId{});
}

CompositorGraph CompositorModel::evaluate(doc::NodeId scene,
                                          doc::NodeId legacy_owner) const {
    return build(&scene, legacy_owner);
}

CompositorGraph CompositorModel::build(const doc::NodeId* const scoped_scene,
                                       doc::NodeId legacy_owner) const {
    const auto in_scope = [&](doc::NodeId owner) noexcept {
        if (scoped_scene == nullptr) {
            return true;
        }
        return (owner == doc::NodeId{} ? legacy_owner : owner) ==
            *scoped_scene;
    };

    CompositorGraph result{};
    result.nodes.reserve(std::min<std::size_t>(nodes_.size(), max_graph_nodes));
    for (const auto& [id, node] : nodes_) {
        static_cast<void>(id);
        if (result.nodes.size() >= max_graph_nodes) {
            break;
        }
        if (!in_scope(node.scene)) {
            continue;
        }
        result.nodes.push_back(node);
    }
    result.edges.reserve(std::min<std::size_t>(edges_.size(), max_graph_edges));
    for (const auto& [id, edge] : edges_) {
        static_cast<void>(id);
        if (result.edges.size() >= max_graph_edges) {
            break;
        }
        // An edge has no owner of its own; it is in scope when both of the
        // nodes it joins are. An edge left dangling by scoping is simply not
        // this scene's edge, so it is dropped rather than diagnosed.
        if (scoped_scene != nullptr &&
            (result.node(edge.from.node) == nullptr ||
             result.node(edge.to.node) == nullptr)) {
            continue;
        }
        result.edges.push_back(edge);
    }

    if (counters_.nodes_over_capacity() != 0U) {
        result.diagnostics.push_back(
            {DiagnosticCode::NodesOverCapacity, {}, {}, 0U});
    }
    if (counters_.edges_over_capacity() != 0U) {
        result.diagnostics.push_back(
            {DiagnosticCode::EdgesOverCapacity, {}, {}, 0U});
    }

    for (const GraphNode& node : result.nodes) {
        if (node.kind != NodeKind::SceneLayer) {
            continue;
        }
        const auto& parameters =
            std::get<SceneLayerParameters>(node.parameters);
        const auto found = document_types_.find(parameters.scene_node);
        if (found == document_types_.end() ||
            is_compositor_type(found->second)) {
            result.diagnostics.push_back(
                {DiagnosticCode::DanglingSceneReference, node.node,
                 parameters.scene_node, 0U});
        }
    }

    struct AcceptedEdge final {
        const GraphEdge* edge{};
        PortValueType type{PortValueType::Image};
    };
    std::vector<AcceptedEdge> accepted;
    accepted.reserve(result.edges.size());
    std::set<std::pair<doc::NodeId, core::u8>> occupied_inputs;
    std::map<std::pair<doc::NodeId, core::u8>, core::u32> output_uses;

    for (const GraphEdge& edge : result.edges) {
        const GraphNode* const from = result.node(edge.from.node);
        const GraphNode* const to = result.node(edge.to.node);
        if (from == nullptr || to == nullptr) {
            result.diagnostics.push_back(
                {DiagnosticCode::DanglingNode, edge.node,
                 from == nullptr ? edge.from.node : edge.to.node,
                 from == nullptr ? edge.from.port : edge.to.port});
            continue;
        }
        const PortDescriptor* const output =
            find_output_port(from->kind, edge.from.port);
        const PortDescriptor* const input =
            find_input_port(to->kind, edge.to.port);
        if (output == nullptr || input == nullptr) {
            result.diagnostics.push_back(
                {DiagnosticCode::UnknownPort, edge.node,
                 output == nullptr ? from->node : to->node,
                 output == nullptr ? edge.from.port : edge.to.port});
            continue;
        }
        if (output->type != input->type) {
            result.diagnostics.push_back({DiagnosticCode::IncompatiblePorts,
                                          edge.node, to->node, edge.to.port});
            continue;
        }
        const auto input_key = std::pair{to->node, edge.to.port};
        if (!input->multiple && !occupied_inputs.insert(input_key).second) {
            result.diagnostics.push_back({DiagnosticCode::DuplicateInput,
                                          edge.node, to->node, edge.to.port});
            continue;
        }
        const auto output_key = std::pair{from->node, edge.from.port};
        core::u32& uses = output_uses[output_key];
        if (!output->multiple && uses != 0U) {
            result.diagnostics.push_back({DiagnosticCode::DuplicateInput,
                                          edge.node, from->node,
                                          edge.from.port});
            continue;
        }
        ++uses;
        accepted.push_back(AcceptedEdge{&edge, output->type});
    }

    for (const GraphNode& node : result.nodes) {
        for (const PortDescriptor& port : input_ports(node.kind)) {
            if (!port.required) {
                continue;
            }
            const bool connected =
                std::any_of(accepted.begin(), accepted.end(),
                            [&node, &port](const AcceptedEdge& value) noexcept {
                                return value.edge->to.node == node.node &&
                                       value.edge->to.port == port.index;
                            });
            if (!connected) {
                result.diagnostics.push_back(
                    {DiagnosticCode::MissingRequiredInput,
                     node.node,
                     {},
                     port.index});
            }
        }
    }

    // The current executor has two effect phases: pointwise inline operations
    // happen while the drawable is shaded, then spatial filters consume the isolated
    // layer. Preserve that truthful order in the authored graph. A connection
    // after Blur cannot be moved ahead of it silently merely because its node
    // kind is inline, so retain the graph and diagnose the offending input.
    for (const GraphNode& node : result.nodes) {
        if (node.kind != NodeKind::SceneLayer) {
            continue;
        }
        std::vector<const AcceptedEdge*> effects;
        for (const AcceptedEdge& value : accepted) {
            if (value.type == PortValueType::Effect &&
                value.edge->to.node == node.node &&
                value.edge->to.port == 0U) {
                effects.push_back(&value);
            }
        }
        std::sort(effects.begin(), effects.end(),
                  [](const AcceptedEdge* left,
                     const AcceptedEdge* right) noexcept {
                      return std::tuple{left->edge->order, left->edge->node} <
                          std::tuple{right->edge->order, right->edge->node};
                  });
        bool spatial_seen = false;
        for (const AcceptedEdge* value : effects) {
            const GraphNode* const effect = result.node(value->edge->from.node);
            if (effect == nullptr) {
                continue;
            }
            // Identity nodes are authored and editable but semantically inert:
            // they neither establish nor violate an execution-domain order.
            if (effect_node_is_identity(*effect)) {
                continue;
            }
            if (effect->kind == NodeKind::Blur ||
                effect->kind == NodeKind::BoxBlur ||
                effect->kind == NodeKind::DirectionalBlur ||
                effect->kind == NodeKind::ChromaKey ||
                effect->kind == NodeKind::LumaKey) {
                spatial_seen = true;
                continue;
            }
            if (spatial_seen &&
                (effect->kind == NodeKind::Brightness ||
                 effect->kind == NodeKind::Saturation ||
                 effect->kind == NodeKind::Levels ||
                 effect->kind == NodeKind::LiftGammaGain ||
                 effect->kind == NodeKind::ContrastPivot ||
                 effect->kind == NodeKind::ColorMatrix ||
                 effect->kind == NodeKind::WhiteBalance ||
                 effect->kind == NodeKind::Curves ||
                 effect->kind == NodeKind::FilmGrain)) {
                result.diagnostics.push_back(
                    {DiagnosticCode::UnsupportedEffectOrder, node.node,
                     effect->node, 0U});
            }
        }
    }
    const auto first_output =
        std::find_if(result.nodes.begin(), result.nodes.end(),
                     [](const GraphNode& node) noexcept {
                         return node.kind == NodeKind::Output;
                     });
    if (!result.nodes.empty() && first_output == result.nodes.end()) {
        result.diagnostics.push_back(
            {DiagnosticCode::MissingOutput, {}, {}, 0U});
    } else if (first_output != result.nodes.end()) {
        for (auto output = std::next(first_output);
             output != result.nodes.end(); ++output) {
            if (output->kind == NodeKind::Output) {
                result.diagnostics.push_back({DiagnosticCode::DuplicateOutput,
                                              output->node, first_output->node,
                                              0U});
            }
        }
    }

    // Reverse reachability is shared by dead-branch diagnostics, canonical
    // operator paths, and authored resource estimates. All accepted port types
    // participate: an Effect or TransformSet feeding a live SceneLayer is just
    // as Output-reachable as the layer's Image connection.
    struct ForwardHop final {
        doc::NodeId target{};
        doc::NodeId edge{};
    };
    std::map<doc::NodeId, std::vector<ForwardHop>> path_adjacency;
    std::map<doc::NodeId, std::vector<doc::NodeId>> reverse_adjacency;
    for (const GraphNode& node : result.nodes) {
        path_adjacency.emplace(node.node, std::vector<ForwardHop>{});
        reverse_adjacency.emplace(node.node, std::vector<doc::NodeId>{});
    }
    for (const AcceptedEdge& value : accepted) {
        path_adjacency[value.edge->from.node].push_back(
            ForwardHop{value.edge->to.node, value.edge->node});
        reverse_adjacency[value.edge->to.node].push_back(
            value.edge->from.node);
    }
    for (auto& [node, hops] : path_adjacency) {
        static_cast<void>(node);
        std::sort(hops.begin(), hops.end(),
                  [](const ForwardHop& left,
                     const ForwardHop& right) noexcept {
                      return std::tuple{left.target, left.edge} <
                          std::tuple{right.target, right.edge};
                  });
    }
    for (auto& [node, upstream] : reverse_adjacency) {
        static_cast<void>(node);
        std::sort(upstream.begin(), upstream.end());
        upstream.erase(std::unique(upstream.begin(), upstream.end()),
                       upstream.end());
    }

    std::map<doc::NodeId, core::u32> output_distance;
    std::vector<doc::NodeId> reachability_queue;
    for (const GraphNode& node : result.nodes) {
        if (node.kind == NodeKind::Output) {
            output_distance.emplace(node.node, 0U);
            reachability_queue.push_back(node.node);
        }
    }
    for (std::size_t cursor = 0U; cursor < reachability_queue.size();
         ++cursor) {
        const doc::NodeId current = reachability_queue[cursor];
        const core::u32 next_distance = output_distance[current] + 1U;
        for (const doc::NodeId upstream : reverse_adjacency[current]) {
            if (output_distance.emplace(upstream, next_distance).second) {
                reachability_queue.push_back(upstream);
            }
        }
    }

    // Matte is a binary image processor, not a unary SceneLayer effect. The
    // initial executable topology consumes two direct SceneLayer branches so
    // each can be rendered once in authored-canvas isolation and the matte
    // branch cannot accidentally become visible colour.
    for (const GraphNode& node : result.nodes) {
        if (node.kind != NodeKind::Matte ||
            !output_distance.contains(node.node)) {
            continue;
        }
        std::array<const GraphNode*, 2U> branches{};
        bool topology_valid = true;
        for (const AcceptedEdge& value : accepted) {
            if (value.type != PortValueType::Image ||
                value.edge->to.node != node.node) {
                continue;
            }
            if (value.edge->to.port >= branches.size() ||
                branches[value.edge->to.port] != nullptr) {
                topology_valid = false;
                continue;
            }
            branches[value.edge->to.port] =
                result.node(value.edge->from.node);
        }
        for (const GraphNode* const branch : branches) {
            topology_valid = topology_valid && branch != nullptr &&
                branch->kind == NodeKind::SceneLayer;
        }
        if (!topology_valid) {
            result.diagnostics.push_back(
                {DiagnosticCode::UnsupportedMatteTopology, node.node, {},
                 0U});
            continue;
        }
        const auto& source_parameters =
            std::get<SceneLayerParameters>(branches[0]->parameters);
        const auto& matte_parameters =
            std::get<SceneLayerParameters>(branches[1]->parameters);
        if (branches[0]->node == branches[1]->node ||
            source_parameters.scene_node == matte_parameters.scene_node) {
            result.diagnostics.push_back(
                {DiagnosticCode::DuplicateSceneLayerReference,
                 branches[1]->node, branches[0]->node, 1U});
        }
        for (const GraphNode* const branch : branches) {
            const doc::NodeId source_id =
                std::get<SceneLayerParameters>(branch->parameters).scene_node;
            const auto found = document_types_.find(source_id);
            if (found != document_types_.end() &&
                found->second == scene::schema::group_type) {
                result.diagnostics.push_back(
                    {DiagnosticCode::UnsupportedMatteGroupSource,
                     branch->node, source_id, 0U});
            }
        }
        // Port 1 is consumed coverage. Any second Output-reachable consumer
        // would also composite it as colour (or feed another visible image
        // operation), violating the one-render/one-consumer contract.
        for (const AcceptedEdge& value : accepted) {
            if (value.type == PortValueType::Image &&
                value.edge->from.node == branches[1]->node &&
                value.edge->to.node != node.node &&
                output_distance.contains(value.edge->to.node)) {
                result.diagnostics.push_back(
                    {DiagnosticCode::MatteBranchAlsoOutputReachable,
                     branches[1]->node, value.edge->to.node,
                     value.edge->to.port});
            }
        }
    }

    result.node_metadata.reserve(result.nodes.size());
    for (const GraphNode& node : result.nodes) {
        GraphNodeMetadata metadata{};
        metadata.node = node.node;
        metadata.execution_domain = execution_domain(node.kind);
        metadata.identity = !is_effect_node(node.kind)
            ? EffectIdentityState::NotEffect
            : (effect_node_is_identity(node) ? EffectIdentityState::Identity
                                             : EffectIdentityState::Active);
        metadata.bounds_expansion = node_bounds_expansion(node);
        const auto reachable = output_distance.find(node.node);
        if (reachable != output_distance.end()) {
            doc::NodeId current = node.node;
            metadata.path_to_output.push_back(current);
            while (output_distance[current] != 0U) {
                const ForwardHop* selected = nullptr;
                for (const ForwardHop& hop : path_adjacency[current]) {
                    const auto target = output_distance.find(hop.target);
                    if (target == output_distance.end() ||
                        target->second + 1U != output_distance[current]) {
                        continue;
                    }
                    if (selected == nullptr ||
                        std::tuple{hop.target, hop.edge} <
                            std::tuple{selected->target, selected->edge}) {
                        selected = &hop;
                    }
                }
                if (selected == nullptr) {
                    // Defensive only: reverse BFS assigned a distance through
                    // one of these exact accepted hops.
                    metadata.path_to_output.clear();
                    metadata.edges_to_output.clear();
                    break;
                }
                current = selected->target;
                metadata.edges_to_output.push_back(selected->edge);
                metadata.path_to_output.push_back(current);
            }
        }
        if (!reachability_queue.empty() && metadata.path_to_output.empty()) {
            result.diagnostics.push_back(
                {DiagnosticCode::DeadBranch, node.node, {}, 0U,
                 DiagnosticSeverity::Warning});
        }
        result.node_metadata.push_back(std::move(metadata));
    }

    // A scoped projection has one exact authored stage. The whole-document
    // view is estimable only when that document also contains exactly one
    // stage; otherwise reporting a local output size would be misleading.
    const std::vector<scene::SceneInfo> authored_scenes =
        resource_scene_model_.scenes();
    const scene::SceneInfo* authored_scene = nullptr;
    if (scoped_scene != nullptr) {
        const auto found = std::find_if(
            authored_scenes.begin(), authored_scenes.end(),
            [scoped_scene](const scene::SceneInfo& value) noexcept {
                return value.id == *scoped_scene;
            });
        if (found != authored_scenes.end()) {
            authored_scene = &*found;
        }
    } else if (authored_scenes.size() == 1U) {
        authored_scene = &authored_scenes.front();
    }
    if (authored_scene != nullptr) {
        result.resources.available = true;
        result.resources.canvas = authored_scene->canvas;
        result.resources.effect_settings =
            resource_scene_model_.effect_settings();
        core::u64 spatial_render_passes{};
        core::u64 spatial_taps_per_pixel{};
        for (const GraphNode& node : result.nodes) {
            if (node.kind == NodeKind::Matte &&
                output_distance.contains(node.node)) {
                ++result.resources.matte_nodes;
            }
        }
        for (const GraphNode& node : result.nodes) {
            if (node.kind != NodeKind::SceneLayer ||
                !output_distance.contains(node.node)) {
                continue;
            }
            ++result.resources.active_scene_layers;
            bool gaussian{};
            bool box{};
            bool directional{};
            bool chroma_key{};
            bool luma_key{};
            scene::EffectBoundsExpansion layer_bounds{};
            for (const AcceptedEdge& value : accepted) {
                if (value.type != PortValueType::Effect ||
                    value.edge->to.node != node.node ||
                    value.edge->to.port != 0U) {
                    continue;
                }
                const GraphNode* const effect =
                    result.node(value.edge->from.node);
                if (effect == nullptr || effect_node_is_identity(*effect)) {
                    continue;
                }
                if (effect->kind == NodeKind::Blur) {
                    gaussian = true;
                    const float radius =
                        std::get<BlurParameters>(effect->parameters).radius;
                    const core::u32 passes =
                        result.resources.effect_settings.blur_passes;
                    spatial_render_passes += 2ULL * passes;
                    spatial_taps_per_pixel +=
                        2ULL * passes *
                        result.resources.effect_settings.blur_taps;
                    const float reach = radius * static_cast<float>(passes);
                    layer_bounds.x += reach;
                    layer_bounds.y += reach;
                } else if (effect->kind == NodeKind::BoxBlur) {
                    box = true;
                    const BoxBlurParameters parameters =
                        std::get<BoxBlurParameters>(effect->parameters);
                    spatial_render_passes += 2ULL * parameters.passes;
                    spatial_taps_per_pixel +=
                        2ULL * parameters.passes *
                        (2ULL * parameters.radius_px + 1ULL);
                    const scene::EffectBoundsExpansion bounds =
                        scene::box_blur_bounds(parameters);
                    layer_bounds.x += bounds.x;
                    layer_bounds.y += bounds.y;
                } else if (effect->kind == NodeKind::DirectionalBlur) {
                    directional = true;
                    const DirectionalBlurParameters parameters =
                        std::get<DirectionalBlurParameters>(
                            effect->parameters);
                    ++spatial_render_passes;
                    spatial_taps_per_pixel +=
                        result.resources.effect_settings.directional_blur_taps;
                    const scene::EffectBoundsExpansion bounds =
                        scene::directional_blur_bounds(parameters);
                    layer_bounds.x += bounds.x;
                    layer_bounds.y += bounds.y;
                } else if (effect->kind == NodeKind::ChromaKey) {
                    chroma_key = true;
                    const ChromaKeyParameters parameters =
                        std::get<ChromaKeyParameters>(effect->parameters);
                    spatial_render_passes +=
                        parameters.morphology_px == 0 ? 2ULL : 4ULL;
                } else if (effect->kind == NodeKind::LumaKey) {
                    luma_key = true;
                    spatial_render_passes += 2ULL;
                }
            }
            if (gaussian) {
                ++result.resources.blurred_scene_layers;
            }
            if (box) {
                ++result.resources.box_blurred_scene_layers;
            }
            if (directional) {
                ++result.resources.directional_blurred_scene_layers;
            }
            if (chroma_key) {
                ++result.resources.chroma_keyed_scene_layers;
            }
            if (luma_key) {
                ++result.resources.luma_keyed_scene_layers;
            }
            result.resources.max_layer_bounds_expansion.x = std::max(
                result.resources.max_layer_bounds_expansion.x,
                layer_bounds.x);
            result.resources.max_layer_bounds_expansion.y = std::max(
                result.resources.max_layer_bounds_expansion.y,
                layer_bounds.y);
        }
        if (result.resources.active_scene_layers != 0U) {
            // Budget in authored full-canvas equivalents: one RGBA8 and one
            // D32 target per active layer, two RGBA8 composite ping-pong
            // targets, and two reusable RGBA8 Gaussian targets whenever any
            // live layer is blurred. Hardware-map slice sizes are deliberately
            // absent from this project-level estimate.
            result.resources.offscreen_textures =
                result.resources.active_scene_layers * 2U + 2U +
                (result.resources.blurred_scene_layers == 0U ? 0U : 2U) +
                (result.resources.box_blurred_scene_layers == 0U &&
                         result.resources.directional_blurred_scene_layers ==
                             0U
                     ? 0U
                     : 2U) +
                (result.resources.chroma_keyed_scene_layers == 0U &&
                         result.resources.luma_keyed_scene_layers == 0U
                     ? 0U
                     : 4U) +
                (result.resources.matte_nodes == 0U ? 0U : 2U);
            const core::u64 bytes_per_texture =
                static_cast<core::u64>(result.resources.canvas.width) *
                static_cast<core::u64>(result.resources.canvas.height) * 4ULL;
            result.resources.bytes_per_texture = bytes_per_texture;
            result.resources.offscreen_texture_bytes =
                bytes_per_texture * result.resources.offscreen_textures;
            result.resources.render_passes =
                result.resources.active_scene_layers * 2U +
                static_cast<core::u32>(spatial_render_passes);
            result.resources.blur_taps_per_pixel = spatial_taps_per_pixel;
        }
        const doc::NodeId budget_subject = first_output == result.nodes.end()
            ? doc::NodeId{}
            : first_output->node;
        if (result.resources.render_passes > max_graph_render_passes) {
            result.diagnostics.push_back(
                {DiagnosticCode::PassBudgetExceeded, budget_subject, {}, 0U});
        }
        if (result.resources.offscreen_texture_bytes >
            max_graph_offscreen_texture_bytes) {
            result.diagnostics.push_back({DiagnosticCode::TextureBudgetExceeded,
                                          budget_subject, {}, 0U});
        }
        if (result.resources.blur_taps_per_pixel >
            max_graph_blur_taps_per_pixel) {
            result.diagnostics.push_back(
                {DiagnosticCode::TapBudgetExceeded, budget_subject, {}, 0U});
        }
    }

    if (first_output != result.nodes.end()) {
        // compositor::apply currently adjusts drawables by scene-node id. Two
        // Output-reachable SceneLayer branches for the same id would therefore
        // union their modifiers and render that union in both branches. Keep
        // disconnected alternatives editable, but reject the executable
        // ambiguity visibly until adjusted scenes become branch-local.
        std::set<doc::NodeId> visited;
        std::set<doc::NodeId> visited_layers;
        std::vector<doc::NodeId> pending{first_output->node};
        std::map<doc::NodeId, doc::NodeId> layer_for_source;
        while (!pending.empty()) {
            const doc::NodeId current = pending.back();
            pending.pop_back();
            if (!visited.insert(current).second) {
                continue;
            }
            for (const AcceptedEdge& value : accepted) {
                if (value.type != PortValueType::Image ||
                    value.edge->to.node != current) {
                    continue;
                }
                const GraphNode* const upstream =
                    result.node(value.edge->from.node);
                if (upstream == nullptr) {
                    continue;
                }
                if (upstream->kind != NodeKind::SceneLayer) {
                    pending.push_back(upstream->node);
                    continue;
                }
                if (!visited_layers.insert(upstream->node).second) {
                    continue;
                }
                const doc::NodeId source =
                    std::get<SceneLayerParameters>(upstream->parameters)
                        .scene_node;
                const auto [found, inserted] =
                    layer_for_source.try_emplace(source, upstream->node);
                if (!inserted) {
                    result.diagnostics.push_back(
                        {DiagnosticCode::DuplicateSceneLayerReference,
                         upstream->node, found->second, 0U});
                }
            }
        }
    }

    std::map<doc::NodeId, core::u32> indegree;
    std::map<doc::NodeId, std::vector<doc::NodeId>> adjacency;
    for (const GraphNode& node : result.nodes) {
        indegree.emplace(node.node, 0U);
        adjacency.emplace(node.node, std::vector<doc::NodeId>{});
    }
    for (const AcceptedEdge& value : accepted) {
        ++indegree[value.edge->to.node];
        adjacency[value.edge->from.node].push_back(value.edge->to.node);
    }
    for (auto& [id, neighbours] : adjacency) {
        static_cast<void>(id);
        std::sort(neighbours.begin(), neighbours.end());
    }

    std::set<doc::NodeId> ready;
    for (const auto& [id, count] : indegree) {
        if (count == 0U) {
            ready.insert(id);
        }
    }
    std::vector<doc::NodeId> topological;
    topological.reserve(result.nodes.size());
    while (!ready.empty()) {
        const doc::NodeId id = *ready.begin();
        ready.erase(ready.begin());
        topological.push_back(id);
        for (const doc::NodeId target : adjacency[id]) {
            core::u32& count = indegree[target];
            --count;
            if (count == 0U) {
                ready.insert(target);
            }
        }
    }

    if (topological.size() != result.nodes.size()) {
        std::map<doc::NodeId, core::u8> state;
        std::vector<doc::NodeId> stack;
        std::set<doc::NodeId> cycle_nodes;
        const std::function<void(doc::NodeId)> visit = [&](doc::NodeId id) {
            state[id] = 1U;
            stack.push_back(id);
            for (const doc::NodeId target : adjacency[id]) {
                if (state[target] == 0U) {
                    visit(target);
                } else if (state[target] == 1U) {
                    const auto first =
                        std::find(stack.begin(), stack.end(), target);
                    cycle_nodes.insert(first, stack.end());
                }
            }
            stack.pop_back();
            state[id] = 2U;
        };
        for (const GraphNode& node : result.nodes) {
            if (state[node.node] == 0U) {
                visit(node.node);
            }
        }
        for (const doc::NodeId id : cycle_nodes) {
            result.diagnostics.push_back({DiagnosticCode::Cycle, id, {}, 0U});
        }
    }

    std::sort(result.diagnostics.begin(), result.diagnostics.end(),
              diagnostic_less);
    result.diagnostics.erase(
        std::unique(result.diagnostics.begin(), result.diagnostics.end()),
        result.diagnostics.end());
    if (!result.valid()) {
        return result;
    }

    result.plan.steps.reserve(topological.size());
    for (const doc::NodeId id : topological) {
        const GraphNode* const node = result.node(id);
        if (node == nullptr) {
            continue;
        }
        PlanStep step{};
        step.node = node->node;
        step.kind = node->kind;
        step.requires_offscreen =
            ((node->kind == NodeKind::Blur ||
              node->kind == NodeKind::BoxBlur ||
              node->kind == NodeKind::DirectionalBlur ||
              node->kind == NodeKind::ChromaKey ||
              node->kind == NodeKind::LumaKey) &&
             !effect_node_is_identity(*node)) ||
            node->kind == NodeKind::Matte;
        for (const AcceptedEdge& value : accepted) {
            const PlanConnection connection =
                connection_from(*value.edge, value.type);
            if (value.edge->to.node == id) {
                step.inputs.push_back(connection);
            }
            if (value.edge->from.node == id) {
                step.consumers.push_back(connection);
            }
        }
        std::sort(step.inputs.begin(), step.inputs.end(),
                  input_connection_less);
        std::sort(step.consumers.begin(), step.consumers.end(),
                  consumer_connection_less);
        if (node->kind == NodeKind::RadialArray) {
            step.radial_instances = radial_instances(
                std::get<RadialArrayParameters>(node->parameters));
        }
        result.plan.steps.push_back(std::move(step));
    }
    return result;
}

namespace {

[[nodiscard]] doc::NodeId owner_of(const doc::Node& node,
                                   doc::NodeId legacy_owner) noexcept {
    const doc::PropertyValue* const value =
        find(node.properties, schema::scene_id);
    if (value == nullptr) {
        return legacy_owner;
    }
    const auto* const whole = std::get_if<std::int64_t>(value);
    if (whole == nullptr || *whole <= 0) {
        return legacy_owner;
    }
    return doc::NodeId{static_cast<core::u64>(*whole)};
}

[[nodiscard]] std::optional<doc::NodeId> reference(
    const doc::Node& node, std::string_view property) noexcept {
    const doc::PropertyValue* const value = find(node.properties, property);
    if (value == nullptr) {
        return std::nullopt;
    }
    const auto* const whole = std::get_if<std::int64_t>(value);
    if (whole == nullptr || *whole <= 0) {
        return std::nullopt;
    }
    return doc::NodeId{static_cast<core::u64>(*whole)};
}

void write_reference(doc::PropertyMap& properties, std::string_view name,
                     doc::NodeId id) {
    properties.insert_or_assign(std::string{name},
                                static_cast<std::int64_t>(id.value()));
}

} // namespace

core::Result<GraphClonePlan, GraphCloneError> plan_clone_graph(
    const doc::Root& root,
    doc::NodeId source,
    doc::NodeId destination,
    doc::NodeId legacy_owner,
    std::span<const std::pair<doc::NodeId, doc::NodeId>> element_remap,
    doc::NodeId first_id) {
    using Result = core::Result<GraphClonePlan, GraphCloneError>;
    constexpr core::u64 max_reference_id =
        static_cast<core::u64>(std::numeric_limits<std::int64_t>::max());
    if (first_id.value() == 0U || first_id.value() > max_reference_id) {
        return Result::failure(GraphCloneError::InvalidFirstId);
    }

    try {
        std::map<doc::NodeId, doc::Node> all;
        for (const doc::NodeChange& change :
             doc::diff(doc::Root{}, root).changes) {
            if (change.after) {
                all.emplace(change.id, *change.after);
            }
        }

        // Nodes first, so the edge pass can ask whether an endpoint is one of
        // the graph objects this scene owns.
        std::set<doc::NodeId> selected;
        for (const auto& [id, node] : all) {
            if (!kind_for_type(node.type).has_value() ||
                owner_of(node, legacy_owner) != source) {
                continue;
            }
            selected.insert(id);
        }
        for (const auto& [id, node] : all) {
            if (node.type != schema::edge_type) {
                continue;
            }
            const auto from = reference(node, schema::from_node);
            const auto to = reference(node, schema::to_node);
            if (from.has_value() && to.has_value() &&
                selected.contains(*from) && selected.contains(*to)) {
                selected.insert(id);
            }
        }

        GraphClonePlan plan{};
        plan.puts.reserve(selected.size());
        plan.remap.reserve(selected.size());
        core::u64 candidate = first_id.value();
        for (const doc::NodeId original : selected) {
            while (root.contains(doc::NodeId{candidate})) {
                if (candidate == max_reference_id) {
                    return Result::failure(GraphCloneError::IdExhausted);
                }
                ++candidate;
            }
            plan.remap.emplace_back(original, doc::NodeId{candidate});
            if (candidate != max_reference_id) {
                ++candidate;
            } else if (plan.remap.size() != selected.size()) {
                return Result::failure(GraphCloneError::IdExhausted);
            }
        }

        const auto cloned = [&](doc::NodeId original)
            -> std::optional<doc::NodeId> {
            const auto found = std::lower_bound(
                plan.remap.begin(), plan.remap.end(), original,
                [](const auto& mapping, doc::NodeId id) noexcept {
                    return mapping.first < id;
                });
            if (found == plan.remap.end() || found->first != original) {
                return std::nullopt;
            }
            return found->second;
        };
        const auto cloned_element = [&](doc::NodeId original)
            -> std::optional<doc::NodeId> {
            for (const auto& [from, to] : element_remap) {
                if (from == original) {
                    return to;
                }
            }
            return std::nullopt;
        };

        for (const auto& [original, clone] : plan.remap) {
            doc::Node node = all.at(original);
            node.id = clone;
            if (node.type == schema::edge_type) {
                const auto from = cloned(*reference(node, schema::from_node));
                const auto to = cloned(*reference(node, schema::to_node));
                write_reference(node.properties, schema::from_node, *from);
                write_reference(node.properties, schema::to_node, *to);
                plan.puts.push_back(std::move(node));
                continue;
            }
            write_reference(node.properties, schema::scene_id, destination);
            if (node.type == schema::scene_layer_type) {
                const auto layered = reference(node, schema::scene_node);
                if (layered.has_value()) {
                    if (const auto copy = cloned_element(*layered);
                        copy.has_value()) {
                        write_reference(node.properties, schema::scene_node,
                                        *copy);
                    }
                }
            }
            plan.puts.push_back(std::move(node));
        }
        return Result::success(std::move(plan));
    } catch (const std::bad_alloc&) {
        return Result::failure(GraphCloneError::AllocationFailed);
    }
}

} // namespace mediaserver::compositor
