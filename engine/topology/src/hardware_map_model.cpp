#include <mediaserver/topology/hardware_map_model.hpp>

#include <mediaserver/topology/hardware_map_schema.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>

namespace mediaserver::topology {
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
        if (const auto* const text_value = std::get_if<std::string>(value)) {
            return *text_value;
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

[[nodiscard]] bool as_u32(std::int64_t value, core::u32& output) noexcept {
    if (value < 0 || value > static_cast<std::int64_t>(
                                 std::numeric_limits<core::u32>::max())) {
        return false;
    }
    output = static_cast<core::u32>(value);
    return true;
}

[[nodiscard]] bool as_i32(std::int64_t value, core::i32& output) noexcept {
    if (value <
            static_cast<std::int64_t>(std::numeric_limits<core::i32>::min()) ||
        value >
            static_cast<std::int64_t>(std::numeric_limits<core::i32>::max())) {
        return false;
    }
    output = static_cast<core::i32>(value);
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

[[nodiscard]] bool as_node(std::int64_t value, doc::NodeId& output) noexcept {
    if (value <= 0) {
        return false;
    }
    output = doc::NodeId{static_cast<core::u64>(value)};
    return true;
}

[[nodiscard]] std::optional<HardwareDeviceKind>
parse_kind(std::string_view value) noexcept {
    if (value == schema::render_server_kind) {
        return HardwareDeviceKind::RenderServer;
    }
    if (value == schema::operator_machine_kind) {
        return HardwareDeviceKind::OperatorMachine;
    }
    if (value == schema::led_processor_kind) {
        return HardwareDeviceKind::LedProcessor;
    }
    if (value == schema::led_wall_kind) {
        return HardwareDeviceKind::LedWall;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<HardwareProcessorProtocol>
parse_protocol(std::string_view value) noexcept {
    if (value == schema::generic_protocol) {
        return HardwareProcessorProtocol::Generic;
    }
    if (value == schema::novastar_coex_protocol) {
        return HardwareProcessorProtocol::NovastarCoex;
    }
    if (value == schema::brompton_tessera_protocol) {
        return HardwareProcessorProtocol::BromptonTessera;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<HardwareWindowMode>
parse_window_mode(std::string_view value) noexcept {
    if (value == schema::inherit_window_mode) {
        return HardwareWindowMode::Inherit;
    }
    if (value == schema::windowed_window_mode) {
        return HardwareWindowMode::Windowed;
    }
    if (value == schema::borderless_window_mode) {
        return HardwareWindowMode::Borderless;
    }
    if (value == schema::fullscreen_window_mode) {
        return HardwareWindowMode::Fullscreen;
    }
    return std::nullopt;
}

} // namespace

core::u32 HardwareMapCounters::devices_over_capacity() const noexcept {
    return devices > max_hardware_devices ? devices - max_hardware_devices : 0U;
}

core::u32 HardwareMapCounters::links_over_capacity() const noexcept {
    return links > max_hardware_links ? links - max_hardware_links : 0U;
}

core::u32 HardwareMapCounters::output_bindings_over_capacity() const noexcept {
    return output_bindings > max_hardware_output_bindings
        ? output_bindings - max_hardware_output_bindings
        : 0U;
}

void HardwareMapModel::forget(doc::NodeId id) noexcept {
    devices_.erase(id);
    links_.erase(id);
    settings_.erase(id);
    output_bindings_.erase(id);
}

void HardwareMapModel::put(const doc::Node& node) {
    forget(node.id);
    if (node.type == schema::device_type) {
        Reader reader{node.properties};
        const std::string kind_text = reader.text(schema::kind, "");
        const auto parsed_kind = parse_kind(kind_text);
        if (!parsed_kind.has_value()) {
            reader.fail();
        }

        core::i32 x{};
        core::i32 y{};
        core::u32 width{};
        core::u32 height{};
        core::u32 node_index{};
        core::u32 refresh_numerator{};
        core::u32 refresh_denominator{};
        core::i32 desktop_x{};
        core::i32 desktop_y{};
        const auto window_mode = parse_window_mode(
            reader.text(schema::window_mode, schema::inherit_window_mode));
        if (!window_mode.has_value()) {
            reader.fail();
        }
        const auto protocol = parse_protocol(
            reader.text(schema::protocol, schema::generic_protocol));
        if (!protocol.has_value()) {
            reader.fail();
        }
        if (!as_i32(reader.integer(schema::grid_x, 0), x) ||
            !as_i32(reader.integer(schema::grid_y, 0), y) ||
            !as_i32(reader.integer(schema::desktop_x, 0), desktop_x) ||
            !as_i32(reader.integer(schema::desktop_y, 0), desktop_y) ||
            !as_u32(reader.integer(schema::width, 1920), width) ||
            !as_u32(reader.integer(schema::height, 1080), height) ||
            !as_u32(reader.integer(schema::node_index, 0), node_index) ||
            !as_u32(reader.integer(schema::refresh_numerator, 60),
                    refresh_numerator) ||
            !as_u32(reader.integer(schema::refresh_denominator, 1),
                    refresh_denominator) ||
            width == 0U || height == 0U || refresh_numerator == 0U ||
            refresh_denominator == 0U || node_index > max_hardware_node_index) {
            reader.fail();
        }
        const std::string fallback_name =
            parsed_kind.has_value()
                ? std::string{device_kind_name(parsed_kind.value())}
                : std::string{"Device"};
        std::string name = reader.text(schema::name, fallback_name);
        std::string address = reader.text(schema::address, "");
        std::string lifecycle_endpoint =
            reader.text(schema::lifecycle_endpoint, "");
        const bool main_node = reader.boolean(schema::main_node, false);
        if (!reader.ok() || !parsed_kind.has_value()) {
            ++counters_.rejected;
            return;
        }
        devices_.insert_or_assign(
            node.id,
            HardwareDevice{node.id, parsed_kind.value(), std::move(name), x, y,
                           std::move(address), std::move(lifecycle_endpoint),
                           node_index, width, height, refresh_numerator,
                           refresh_denominator,
                           parsed_kind == HardwareDeviceKind::RenderServer
                               ? *window_mode
                               : HardwareWindowMode::Inherit,
                           desktop_x, desktop_y,
                           parsed_kind == HardwareDeviceKind::RenderServer &&
                               main_node,
                           parsed_kind == HardwareDeviceKind::LedProcessor
                               ? *protocol
                               : HardwareProcessorProtocol::Generic});
        return;
    }

    if (node.type == schema::settings_type) {
        Reader reader{node.properties};
        core::u32 base_port{};
        if (!as_u32(reader.integer(
                        schema::control_base_port,
                        default_hardware_control_base_port),
                    base_port) ||
            base_port == 0U || base_port > max_hardware_control_base_port ||
            !reader.ok()) {
            ++counters_.rejected;
            return;
        }
        settings_.insert_or_assign(
            node.id, HardwareControlSettings{node.id, base_port, true});
        return;
    }

    if (node.type == schema::output_binding_type) {
        Reader reader{node.properties};
        doc::NodeId render_server{};
        core::u8 logical_port{};
        core::u32 feed_index{};
        core::u32 target_id{};
        core::u32 connector_instance{};
        core::u32 width{};
        core::u32 height{};
        core::u32 refresh_numerator{};
        core::u32 refresh_denominator{};
        const auto adapter_id = parse_display_adapter_id(
            reader.text(schema::display_adapter_id, ""));
        const auto window_mode = parse_window_mode(reader.text(
            schema::window_mode, schema::fullscreen_window_mode));
        const auto output_bits = reader.integer(schema::output_bit_depth, 8);
        if (output_bits != 8 && output_bits != 10 && output_bits != 16) reader.fail();
        if (!as_node(reader.integer(schema::render_server, 0), render_server) ||
            !as_port(reader.integer(schema::logical_port, 0), logical_port) ||
            logical_port < first_render_server_video_port ||
            logical_port > last_render_server_video_port ||
            !as_u32(reader.integer(schema::feed_index, 0), feed_index) ||
            feed_index > max_hardware_node_index ||
            !as_u32(reader.integer(schema::display_target_id, 0), target_id) ||
            !as_u32(reader.integer(schema::connector_instance, 0),
                    connector_instance) ||
            !as_u32(reader.integer(schema::width, 1920), width) ||
            !as_u32(reader.integer(schema::height, 1080), height) ||
            !as_u32(reader.integer(schema::refresh_numerator, 60),
                    refresh_numerator) ||
            !as_u32(reader.integer(schema::refresh_denominator, 1),
                    refresh_denominator) ||
            !adapter_id.has_value() || !window_mode.has_value() ||
            width == 0U || height == 0U ||
            width > max_hardware_output_width ||
            height > max_hardware_output_height ||
            refresh_numerator == 0U || refresh_denominator == 0U) {
            reader.fail();
        }
        if (!reader.ok() || !adapter_id.has_value() ||
            !window_mode.has_value()) {
            ++counters_.rejected;
            return;
        }
        output_bindings_.insert_or_assign(
            node.id,
            HardwareOutputBinding{
                node.id, render_server, logical_port, feed_index,
                HardwareDisplayTargetKey{*adapter_id, target_id,
                                         connector_instance},
                width, height, refresh_numerator, refresh_denominator,
                *window_mode, static_cast<core::u32>(output_bits)});
        return;
    }

    if (node.type != schema::link_type) {
        return;
    }

    Reader reader{node.properties};
    HardwareEndpoint from{};
    HardwareEndpoint to{};
    if (!as_node(reader.integer(schema::from_device, 0), from.device) ||
        !as_port(reader.integer(schema::from_port, 0), from.port) ||
        !as_node(reader.integer(schema::to_device, 0), to.device) ||
        !as_port(reader.integer(schema::to_port, 0), to.port)) {
        reader.fail();
    }
    HardwareOutputRegion region{};
    if (!as_u32(reader.integer(schema::region_x, 0), region.x) ||
        !as_u32(reader.integer(schema::region_y, 0), region.y) ||
        !as_u32(reader.integer(schema::region_width, 0), region.width) ||
        !as_u32(reader.integer(schema::region_height, 0), region.height)) {
        reader.fail();
    }
    if (!reader.ok()) {
        ++counters_.rejected;
        return;
    }
    if (region.empty()) {
        region = HardwareOutputRegion{};
    }
    links_.insert_or_assign(node.id,
                            HardwareLink{node.id, from, to, region});
}

void HardwareMapModel::recount() noexcept {
    counters_.devices = static_cast<core::u32>(std::min<std::size_t>(
        devices_.size(),
        static_cast<std::size_t>(std::numeric_limits<core::u32>::max())));
    counters_.links = static_cast<core::u32>(std::min<std::size_t>(
        links_.size(),
        static_cast<std::size_t>(std::numeric_limits<core::u32>::max())));
    counters_.output_bindings = static_cast<core::u32>(
        std::min<std::size_t>(
            output_bindings_.size(),
            static_cast<std::size_t>(
                std::numeric_limits<core::u32>::max())));

    core::u32 invalid = 0U;
    std::set<std::pair<doc::NodeId, core::u8>> occupied_inputs;
    for (const auto& [id, link] : links_) {
        static_cast<void>(id);
        const auto from = devices_.find(link.from.device);
        const auto to = devices_.find(link.to.device);
        const auto input = std::pair{link.to.device, link.to.port};
        const bool valid = from != devices_.end() && to != devices_.end() &&
                           compatible(from->second, link.from.port, to->second,
                                      link.to.port) &&
                           occupied_inputs.insert(input).second;
        if (!valid && invalid != std::numeric_limits<core::u32>::max()) {
            ++invalid;
        }
    }
    counters_.invalid_links = invalid;

    core::u32 invalid_bindings = 0U;
    std::set<std::pair<doc::NodeId, core::u8>> occupied_ports;
    std::set<std::pair<doc::NodeId, core::u32>> occupied_feeds;
    std::set<std::pair<doc::NodeId, HardwareDisplayTargetKey>>
        occupied_targets;
    for (const auto& [id, binding] : output_bindings_) {
        static_cast<void>(id);
        const auto server = devices_.find(binding.render_server);
        const bool valid =
            server != devices_.end() &&
            server->second.kind == HardwareDeviceKind::RenderServer &&
            occupied_ports
                .insert({binding.render_server, binding.logical_port})
                .second &&
            occupied_feeds.insert({binding.render_server, binding.feed_index})
                .second &&
            occupied_targets.insert({binding.render_server, binding.target})
                .second;
        if (!valid && invalid_bindings !=
                          std::numeric_limits<core::u32>::max()) {
            ++invalid_bindings;
        }
    }
    counters_.invalid_output_bindings = invalid_bindings;
}

void HardwareMapModel::reset(const doc::Root& root) {
    devices_.clear();
    links_.clear();
    settings_.clear();
    output_bindings_.clear();
    counters_ = HardwareMapCounters{};
    apply(diff(doc::Root{}, root));
}

void HardwareMapModel::apply(const doc::Delta& delta) {
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

HardwareMap HardwareMapModel::evaluate() const {
    HardwareMap result{};
    if (!settings_.empty()) {
        result.control = settings_.begin()->second;
    }
    result.devices.reserve(
        std::min<std::size_t>(devices_.size(), max_hardware_devices));
    for (const auto& [id, device] : devices_) {
        static_cast<void>(id);
        if (result.devices.size() >= max_hardware_devices) {
            break;
        }
        result.devices.push_back(device);
    }

    std::set<std::pair<doc::NodeId, core::u8>> occupied_inputs;
    result.links.reserve(
        std::min<std::size_t>(links_.size(), max_hardware_links));
    for (const auto& [id, link] : links_) {
        static_cast<void>(id);
        if (result.links.size() >= max_hardware_links) {
            break;
        }
        const HardwareDevice* const from = result.device(link.from.device);
        const HardwareDevice* const to = result.device(link.to.device);
        const auto input = std::pair{link.to.device, link.to.port};
        if (from == nullptr || to == nullptr ||
            !compatible(*from, link.from.port, *to, link.to.port) ||
            !occupied_inputs.insert(input).second) {
            continue;
        }
        result.links.push_back(link);
    }

    std::set<std::pair<doc::NodeId, core::u8>> occupied_ports;
    std::set<std::pair<doc::NodeId, core::u32>> occupied_feeds;
    std::set<std::pair<doc::NodeId, HardwareDisplayTargetKey>>
        occupied_targets;
    result.output_bindings.reserve(std::min<std::size_t>(
        output_bindings_.size(), max_hardware_output_bindings));
    for (const auto& [id, binding] : output_bindings_) {
        static_cast<void>(id);
        if (result.output_bindings.size() >=
            max_hardware_output_bindings) {
            break;
        }
        const HardwareDevice* const server =
            result.device(binding.render_server);
        if (server == nullptr ||
            server->kind != HardwareDeviceKind::RenderServer ||
            !occupied_ports
                 .insert({binding.render_server, binding.logical_port})
                 .second ||
            !occupied_feeds
                 .insert({binding.render_server, binding.feed_index})
                 .second ||
            !occupied_targets
                 .insert({binding.render_server, binding.target})
                 .second) {
            continue;
        }
        result.output_bindings.push_back(binding);
    }
    return result;
}

} // namespace mediaserver::topology
