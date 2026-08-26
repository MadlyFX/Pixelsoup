#include <mediaserver/compositor/compositor_model.hpp>

#include <mediaserver/compositor/compositor_schema.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

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

[[nodiscard]] std::optional<NodeKind>
kind_for_type(std::string_view type) noexcept {
    if (type == schema::scene_layer_type) {
        return NodeKind::SceneLayer;
    }
    if (type == schema::brightness_type) {
        return NodeKind::Brightness;
    }
    if (type == schema::blur_type) {
        return NodeKind::Blur;
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
    case NodeKind::Blur: {
        const float radius = reader.number(schema::radius, 0.0F);
        if (radius < 0.0F || radius > max_blur_radius) {
            reader.fail();
        }
        parsed.parameters = BlurParameters{radius};
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
    return std::tuple{left.subject, static_cast<core::u8>(left.code),
                      left.related, left.port} <
           std::tuple{right.subject, static_cast<core::u8>(right.code),
                      right.related, right.port};
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
    apply(diff(doc::Root{}, root));
}

void CompositorModel::apply(const doc::Delta& delta) {
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
    CompositorGraph result{};
    result.nodes.reserve(std::min<std::size_t>(nodes_.size(), max_graph_nodes));
    for (const auto& [id, node] : nodes_) {
        static_cast<void>(id);
        if (result.nodes.size() >= max_graph_nodes) {
            break;
        }
        result.nodes.push_back(node);
    }
    result.edges.reserve(std::min<std::size_t>(edges_.size(), max_graph_edges));
    for (const auto& [id, edge] : edges_) {
        static_cast<void>(id);
        if (result.edges.size() >= max_graph_edges) {
            break;
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
        step.requires_offscreen = node->kind == NodeKind::Blur;
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

} // namespace mediaserver::compositor
