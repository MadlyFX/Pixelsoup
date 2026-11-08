#include <mediaserver/topology/hardware_map.hpp>

#include <mediaserver/core/fnv1a.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace mediaserver::topology {
namespace {

constexpr std::array<HardwarePort, 2U> render_server_ports{{
    {0U, "CONTROL IN", HardwareSignal::Network, HardwarePortDirection::Input},
    {1U, "HDMI OUT", HardwareSignal::Video, HardwarePortDirection::Output},
}};

constexpr auto additional_render_server_ports = [] {
  std::array<HardwarePort,
             static_cast<std::size_t>(max_hardware_video_outputs - 1U)>
      ports{};
  for (std::size_t index = 0U; index < ports.size(); ++index) {
    ports[index] =
        HardwarePort{static_cast<core::u8>(index + 2U), "VIDEO OUT",
                     HardwareSignal::Video, HardwarePortDirection::Output};
  }
  return ports;
}();

constexpr std::array<HardwarePort, 1U> operator_ports{{
    {0U, "CONTROL OUT", HardwareSignal::Network, HardwarePortDirection::Output},
}};

constexpr std::array<HardwarePort, 2U> processor_ports{{
    {0U, "VIDEO IN 1", HardwareSignal::Video, HardwarePortDirection::Input},
    {processor_led_output_port, "LED OUT", HardwareSignal::Video,
     HardwarePortDirection::Output},
}};

constexpr std::array<std::string_view,
                     static_cast<std::size_t>(max_processor_video_inputs - 1U)>
    processor_input_names{{
        "VIDEO IN 2", "VIDEO IN 3", "VIDEO IN 4", "VIDEO IN 5",
        "VIDEO IN 6", "VIDEO IN 7", "VIDEO IN 8", "VIDEO IN 9",
        "VIDEO IN 10", "VIDEO IN 11", "VIDEO IN 12", "VIDEO IN 13",
        "VIDEO IN 14", "VIDEO IN 15", "VIDEO IN 16",
    }};

constexpr auto additional_processor_ports = [] {
  std::array<HardwarePort, processor_input_names.size()> ports{};
  for (std::size_t index = 0U; index < ports.size(); ++index) {
    ports[index] = HardwarePort{static_cast<core::u8>(index + 2U),
                                processor_input_names[index],
                                HardwareSignal::Video,
                                HardwarePortDirection::Input};
  }
  return ports;
}();

constexpr std::array<HardwarePort, 1U> wall_ports{{
    {0U, "LED IN", HardwareSignal::Video, HardwarePortDirection::Input},
}};

void hash_u32(core::u64 &hash, core::u32 value) noexcept {
  core::fnv1a_u64(hash, static_cast<core::u64>(value));
}

void hash_i32(core::u64 &hash, core::i32 value) noexcept {
  hash_u32(hash, static_cast<core::u32>(value));
}

} // namespace

std::span<const HardwarePort> ports_for(HardwareDeviceKind kind) noexcept {
  switch (kind) {
  case HardwareDeviceKind::RenderServer:
    return render_server_ports;
  case HardwareDeviceKind::OperatorMachine:
    return operator_ports;
  case HardwareDeviceKind::LedProcessor:
    return processor_ports;
  case HardwareDeviceKind::LedWall:
    return wall_ports;
  }
  return {};
}

const HardwarePort *find_port(HardwareDeviceKind kind,
                              core::u8 index) noexcept {
  const auto ports = ports_for(kind);
  const auto found = std::find_if(ports.begin(), ports.end(),
                                  [index](const HardwarePort &port) noexcept {
                                    return port.index == index;
                                  });
  if (found != ports.end()) {
    return &*found;
  }
  if (kind == HardwareDeviceKind::RenderServer && index >= 2U &&
      index <= last_render_server_video_port) {
    return &additional_render_server_ports[static_cast<std::size_t>(index -
                                                                    2U)];
  }
  if (kind == HardwareDeviceKind::LedProcessor && index >= 2U &&
      index <= max_processor_video_inputs) {
    return &additional_processor_ports[static_cast<std::size_t>(index - 2U)];
  }
  return nullptr;
}

const HardwareDevice *HardwareMap::device(doc::NodeId id) const noexcept {
  const auto found = std::lower_bound(
      devices.begin(), devices.end(), id,
      [](const HardwareDevice &device, doc::NodeId wanted) noexcept {
        return device.node < wanted;
      });
  return found != devices.end() && found->node == id ? &*found : nullptr;
}

const HardwareLink *HardwareMap::link(doc::NodeId id) const noexcept {
  const auto found = std::lower_bound(
      links.begin(), links.end(), id,
      [](const HardwareLink &link, doc::NodeId wanted) noexcept {
        return link.node < wanted;
      });
  return found != links.end() && found->node == id ? &*found : nullptr;
}

const HardwareOutputBinding *
HardwareMap::output_binding(doc::NodeId id) const noexcept {
  const auto found = std::lower_bound(
      output_bindings.begin(), output_bindings.end(), id,
      [](const HardwareOutputBinding &binding, doc::NodeId wanted) noexcept {
        return binding.node < wanted;
      });
  return found != output_bindings.end() && found->node == id ? &*found
                                                              : nullptr;
}

const HardwareOutputBinding *HardwareMap::output_binding_for(
    doc::NodeId render_server, core::u8 logical_port) const noexcept {
  const auto found = std::find_if(
      output_bindings.begin(), output_bindings.end(),
      [render_server, logical_port](const HardwareOutputBinding &binding) {
        return binding.render_server == render_server &&
               binding.logical_port == logical_port;
      });
  return found == output_bindings.end() ? nullptr : &*found;
}

std::string format_display_adapter_id(core::u64 adapter_id) {
  constexpr std::string_view digits{"0123456789abcdef"};
  std::string result(16U, '0');
  for (std::size_t index = 0U; index < result.size(); ++index) {
    const auto shift = static_cast<core::u32>((15U - index) * 4U);
    result[index] = digits[static_cast<std::size_t>(
        (adapter_id >> shift) & 0x0fU)];
  }
  return result;
}

std::optional<core::u64>
parse_display_adapter_id(std::string_view encoded) noexcept {
  if (encoded.size() != 16U) {
    return std::nullopt;
  }
  core::u64 result{};
  for (const char character : encoded) {
    core::u8 nibble{};
    if (character >= '0' && character <= '9') {
      nibble = static_cast<core::u8>(character - '0');
    } else if (character >= 'a' && character <= 'f') {
      nibble = static_cast<core::u8>(character - 'a' + 10);
    } else {
      return std::nullopt;
    }
    result = (result << 4U) | nibble;
  }
  return result == 0U ? std::nullopt : std::optional<core::u64>{result};
}

std::optional<std::string>
lifecycle_endpoint_for(const HardwareMap &map,
                       const HardwareDevice &device) {
  if (device.kind != HardwareDeviceKind::RenderServer ||
      device.node_index > max_hardware_node_index) {
    return std::nullopt;
  }
  if (!map.control.authored && !device.lifecycle_endpoint.empty()) {
    return device.lifecycle_endpoint;
  }
  if (map.control.base_port == 0U ||
      map.control.base_port > max_hardware_control_base_port) {
    return std::nullopt;
  }
  const core::u32 port = map.control.base_port + device.node_index;
  std::string host = device.address.empty() ? "127.0.0.1" : device.address;
  if (host.empty()) {
    return std::nullopt;
  }
  if (host.find(':') != std::string::npos &&
      !(host.size() >= 2U && host.front() == '[' && host.back() == ']')) {
    host = '[' + host + ']';
  }
  return host + ':' + std::to_string(port);
}

bool compatible(const HardwareDevice &from_device, core::u8 from_port,
                const HardwareDevice &to_device, core::u8 to_port) noexcept {
  if (from_device.node == to_device.node) {
    return false;
  }
  const HardwarePort *const from = find_port(from_device.kind, from_port);
  const HardwarePort *const to = find_port(to_device.kind, to_port);
  return from != nullptr && to != nullptr &&
         from->direction == HardwarePortDirection::Output &&
         to->direction == HardwarePortDirection::Input &&
         from->signal == to->signal;
}

bool render_server_active(const HardwareMap &map,
                          const HardwareDevice &device) noexcept {
  if (device.kind != HardwareDeviceKind::RenderServer) {
    return false;
  }
  return std::any_of(map.links.begin(), map.links.end(),
                     [&](const HardwareLink &link) noexcept {
                       return link.from.device == device.node ||
                              link.to.device == device.node;
                     }) ||
         std::any_of(map.output_bindings.begin(), map.output_bindings.end(),
                     [&](const HardwareOutputBinding &binding) noexcept {
                       return binding.render_server == device.node;
                     });
}

RenderServerLookup render_server_for_node(const HardwareMap &map,
                                          core::u32 node_index) noexcept {
  RenderServerLookup result{};
  for (const HardwareDevice &device : map.devices) {
    if (device.kind != HardwareDeviceKind::RenderServer ||
        device.node_index != node_index ||
        !render_server_active(map, device)) {
      continue;
    }
    if (result.device != nullptr) {
      result.duplicate = true;
      break;
    }
    result.device = &device;
  }
  return result;
}

std::optional<core::u32> main_render_node(const HardwareMap &map) noexcept {
  const HardwareDevice *main{};
  for (const HardwareDevice &device : map.devices) {
    if (device.kind == HardwareDeviceKind::RenderServer && device.main_node &&
        render_server_active(map, device) &&
        (main == nullptr || device.node < main->node)) {
      main = &device;
    }
  }
  return main != nullptr ? std::optional<core::u32>{main->node_index}
                         : std::nullopt;
}

std::vector<const HardwareDevice *>
cluster_render_servers(const HardwareMap &map) {
  std::vector<const HardwareDevice *> members;
  for (const HardwareDevice &device : map.devices) {
    if (!render_server_active(map, device) ||
        std::any_of(members.begin(), members.end(),
                    [&](const HardwareDevice *member) noexcept {
                      return member->node_index == device.node_index;
                    })) {
      continue;
    }
    members.push_back(&device);
  }
  return members;
}

core::u32 next_free_node_index(const HardwareMap &map) noexcept {
  for (core::u32 index = 0U; index < max_hardware_node_index; ++index) {
    const bool used = std::any_of(
        map.devices.begin(), map.devices.end(),
        [index](const HardwareDevice &device) noexcept {
          return device.kind == HardwareDeviceKind::RenderServer &&
                 device.node_index == index;
        });
    if (!used) {
      return index;
    }
  }
  return max_hardware_node_index;
}

core::u64 digest(const HardwareMap &map) noexcept {
  core::u64 hash = core::fnv1a_offset_short;
  core::fnv1a_u64(hash, static_cast<core::u64>(map.devices.size()));
  for (const HardwareDevice &device : map.devices) {
    core::fnv1a_u64(hash, device.node.value());
    core::fnv1a_byte(hash, static_cast<core::u8>(device.kind));
    core::fnv1a_text(hash, device.name);
    hash_i32(hash, device.grid_x);
    hash_i32(hash, device.grid_y);
    core::fnv1a_text(hash, device.address);
    core::fnv1a_text(hash, device.lifecycle_endpoint);
    hash_u32(hash, device.node_index);
    hash_u32(hash, device.width);
    hash_u32(hash, device.height);
    hash_u32(hash, device.refresh_numerator);
    hash_u32(hash, device.refresh_denominator);
    core::fnv1a_byte(hash, static_cast<core::u8>(device.window_mode));
    hash_i32(hash, device.desktop_x);
    hash_i32(hash, device.desktop_y);
    core::fnv1a_byte(hash, device.main_node ? 1U : 0U);
    // Only a vendor protocol is hashed, so maps authored before protocols
    // existed keep their digest.
    if (device.protocol != HardwareProcessorProtocol::Generic) {
      hash_u32(hash, 0x50524f54U);
      core::fnv1a_byte(hash, static_cast<core::u8>(device.protocol));
    }
  }
  core::fnv1a_u64(hash, static_cast<core::u64>(map.links.size()));
  for (const HardwareLink &link : map.links) {
    core::fnv1a_u64(hash, link.node.value());
    core::fnv1a_u64(hash, link.from.device.value());
    core::fnv1a_byte(hash, link.from.port);
    core::fnv1a_u64(hash, link.to.device.value());
    core::fnv1a_byte(hash, link.to.port);
    core::fnv1a_u64(hash, link.region.x);
    core::fnv1a_u64(hash, link.region.y);
    core::fnv1a_u64(hash, link.region.width);
    core::fnv1a_u64(hash, link.region.height);
  }
  core::fnv1a_u64(hash, static_cast<core::u64>(map.output_bindings.size()));
  for (const HardwareOutputBinding &binding : map.output_bindings) {
    core::fnv1a_u64(hash, binding.node.value());
    core::fnv1a_u64(hash, binding.render_server.value());
    core::fnv1a_byte(hash, binding.logical_port);
    hash_u32(hash, binding.feed_index);
    core::fnv1a_u64(hash, binding.target.adapter_id);
    hash_u32(hash, binding.target.target_id);
    hash_u32(hash, binding.target.connector_instance);
    hash_u32(hash, binding.width);
    hash_u32(hash, binding.height);
    hash_u32(hash, binding.refresh_numerator);
    hash_u32(hash, binding.refresh_denominator);
    if (binding.output_bit_depth != 8U) { hash_u32(hash, 0x42495453U); hash_u32(hash, binding.output_bit_depth); }
    core::fnv1a_byte(hash, static_cast<core::u8>(binding.window_mode));
  }
  core::fnv1a_byte(hash, map.control.authored ? 1U : 0U);
  core::fnv1a_u64(hash, map.control.node.value());
  hash_u32(hash, map.control.base_port);
  return hash;
}

} // namespace mediaserver::topology
