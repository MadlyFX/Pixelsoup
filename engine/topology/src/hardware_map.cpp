#include <mediaserver/topology/hardware_map.hpp>

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
    {0U, "HDMI IN", HardwareSignal::Video, HardwarePortDirection::Input},
    {1U, "LED OUT", HardwareSignal::Video, HardwarePortDirection::Output},
}};

constexpr std::array<HardwarePort, 1U> wall_ports{{
    {0U, "LED IN", HardwareSignal::Video, HardwarePortDirection::Input},
}};

constexpr core::u64 fnv_offset = 1469598103934665603ULL;
constexpr core::u64 fnv_prime = 1099511628211ULL;

void hash_byte(core::u64 &hash, core::u8 byte) noexcept {
  hash ^= byte;
  hash *= fnv_prime;
}

void hash_u64(core::u64 &hash, core::u64 value) noexcept {
  for (core::u32 shift = 0U; shift < 64U; shift += 8U) {
    hash_byte(hash, static_cast<core::u8>((value >> shift) & 0xFFU));
  }
}

void hash_u32(core::u64 &hash, core::u32 value) noexcept {
  hash_u64(hash, static_cast<core::u64>(value));
}

void hash_i32(core::u64 &hash, core::i32 value) noexcept {
  hash_u32(hash, static_cast<core::u32>(value));
}

void hash_text(core::u64 &hash, std::string_view value) noexcept {
  hash_u64(hash, static_cast<core::u64>(value.size()));
  for (const char character : value) {
    hash_byte(hash, static_cast<core::u8>(character));
  }
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

core::u64 digest(const HardwareMap &map) noexcept {
  core::u64 hash = fnv_offset;
  hash_u64(hash, static_cast<core::u64>(map.devices.size()));
  for (const HardwareDevice &device : map.devices) {
    hash_u64(hash, device.node.value());
    hash_byte(hash, static_cast<core::u8>(device.kind));
    hash_text(hash, device.name);
    hash_i32(hash, device.grid_x);
    hash_i32(hash, device.grid_y);
    hash_text(hash, device.address);
    hash_text(hash, device.lifecycle_endpoint);
    hash_u32(hash, device.node_index);
    hash_u32(hash, device.width);
    hash_u32(hash, device.height);
    hash_u32(hash, device.refresh_numerator);
    hash_u32(hash, device.refresh_denominator);
    hash_byte(hash, static_cast<core::u8>(device.window_mode));
    hash_i32(hash, device.desktop_x);
    hash_i32(hash, device.desktop_y);
  }
  hash_u64(hash, static_cast<core::u64>(map.links.size()));
  for (const HardwareLink &link : map.links) {
    hash_u64(hash, link.node.value());
    hash_u64(hash, link.from.device.value());
    hash_byte(hash, link.from.port);
    hash_u64(hash, link.to.device.value());
    hash_byte(hash, link.to.port);
  }
  hash_byte(hash, map.control.authored ? 1U : 0U);
  hash_u64(hash, map.control.node.value());
  hash_u32(hash, map.control.base_port);
  return hash;
}

} // namespace mediaserver::topology
