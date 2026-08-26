#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>

#include <span>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The authored hardware topology: configured machines, processors, walls, and
// the cables between their typed ports.
//
// This is deliberately not a live-device registry. A configured device may be
// offline, and runtime health belongs in a separate transient snapshot. Keeping
// those domains apart prevents a saved project from claiming a machine is live
// merely because its icon exists.
namespace mediaserver::topology {

enum class HardwareDeviceKind : core::u8 {
  RenderServer,
  OperatorMachine,
  LedProcessor,
  LedWall,
};

[[nodiscard]] constexpr std::string_view
device_kind_name(HardwareDeviceKind kind) noexcept {
  switch (kind) {
  case HardwareDeviceKind::RenderServer:
    return "Render Server";
  case HardwareDeviceKind::OperatorMachine:
    return "Operator Machine";
  case HardwareDeviceKind::LedProcessor:
    return "LED Processor";
  case HardwareDeviceKind::LedWall:
    return "LED Wall";
  }
  return "Device";
}

enum class HardwareSignal : core::u8 {
  Network,
  Video,
};

enum class HardwarePortDirection : core::u8 {
  Input,
  Output,
};

enum class HardwareWindowMode : core::u8 {
  // Legacy projects and command-line development launches keep their current
  // presenter settings until an operator explicitly authors a window mode.
  Inherit,
  Windowed,
  Borderless,
  Fullscreen,
};

struct HardwarePort final {
  core::u8 index{};
  std::string_view name;
  HardwareSignal signal{HardwareSignal::Video};
  HardwarePortDirection direction{HardwarePortDirection::Input};

  bool operator==(const HardwarePort &) const = default;
};

// A Render Server keeps a bounded set of authored-compatible logical video
// slots. Runtime discovery decides which slots are currently presented, but it
// never changes whether a saved cable remains structurally valid.
inline constexpr core::u8 max_hardware_video_outputs = 32U;
inline constexpr core::u8 first_render_server_video_port = 1U;
inline constexpr core::u8 last_render_server_video_port =
    max_hardware_video_outputs;

// These are the baseline ports shown without runtime observations. Render
// Servers expose CONTROL IN plus legacy VIDEO OUT 1 here; find_port also
// describes logical video outputs 2..32 so saved links remain valid offline.
[[nodiscard]] std::span<const HardwarePort>
ports_for(HardwareDeviceKind kind) noexcept;

[[nodiscard]] const HardwarePort *find_port(HardwareDeviceKind kind,
                                            core::u8 index) noexcept;

struct HardwareDevice final {
  // Document provenance, needed by selection and edits. Runtime identities
  // are separate and may be rebound without changing this authored object.
  doc::NodeId node{};
  HardwareDeviceKind kind{HardwareDeviceKind::RenderServer};
  std::string name;
  core::i32 grid_x{};
  core::i32 grid_y{};
  // Numeric IPv4/IPv6 host address for lifecycle control; blank means local
  // loopback. Hostname resolution is deliberately not implied by this field.
  // Runtime inventory may report a separate human-facing machine name.
  std::string address;
  std::string lifecycle_endpoint;
  // Runtime output identity. This is the exact value passed to the managed
  // render process as --node-index; it is never inferred from map ordering.
  core::u32 node_index{};
  core::u32 width{1920U};
  core::u32 height{1080U};
  core::u32 refresh_numerator{60U};
  core::u32 refresh_denominator{1U};
  HardwareWindowMode window_mode{HardwareWindowMode::Inherit};
  // Signed desktop coordinates support monitors arranged left/above the
  // primary display. They are applied only when window_mode is not Inherit.
  core::i32 desktop_x{};
  core::i32 desktop_y{};

  bool operator==(const HardwareDevice &) const = default;
};

inline constexpr core::u32 default_hardware_control_base_port = 47'840U;
inline constexpr core::u32 max_hardware_node_index = 4'095U;
inline constexpr core::u32 max_hardware_control_base_port =
    65'535U - max_hardware_node_index;
// Live presenter limits, used by launch resolution and new-value authoring.
// They are deliberately not document parse limits: an older project may hold
// larger dimensions and must stay evaluable so the operator can repair it.
inline constexpr core::u32 max_hardware_output_width = 7'680U;
inline constexpr core::u32 max_hardware_output_height = 4'320U;

struct HardwareControlSettings final {
  // Zero/authored=false means an older project has no settings node. Its
  // hidden per-device lifecycle_endpoint remains the compatibility fallback.
  doc::NodeId node{};
  core::u32 base_port{default_hardware_control_base_port};
  bool authored{};

  bool operator==(const HardwareControlSettings &) const = default;
};

struct HardwareEndpoint final {
  doc::NodeId device{};
  core::u8 port{};

  bool operator==(const HardwareEndpoint &) const = default;
};

struct HardwareLink final {
  doc::NodeId node{};
  HardwareEndpoint from;
  HardwareEndpoint to;

  bool operator==(const HardwareLink &) const = default;
};

inline constexpr core::u32 max_hardware_devices = 256U;
inline constexpr core::u32 max_hardware_links = 512U;

struct HardwareMap final {
  // Both arrays are sorted by document node id. Invalid/dangling links never
  // enter this evaluated value; the model reports them in its counters.
  std::vector<HardwareDevice> devices;
  std::vector<HardwareLink> links;
  HardwareControlSettings control;

  [[nodiscard]] const HardwareDevice *device(doc::NodeId id) const noexcept;
  [[nodiscard]] const HardwareLink *link(doc::NodeId id) const noexcept;
};

// Resolves the listener without storing redundant per-node endpoints. New
// projects use Address (blank means loopback) and base_port + node_index. An
// old lifecycle_endpoint is honored only while no settings node exists.
[[nodiscard]] std::optional<std::string>
lifecycle_endpoint_for(const HardwareMap &map,
                       const HardwareDevice &device);

// True only for a typed output-to-input cable between two different devices.
[[nodiscard]] bool compatible(const HardwareDevice &from_device,
                              core::u8 from_port,
                              const HardwareDevice &to_device,
                              core::u8 to_port) noexcept;

// FNV-1a over the evaluated authored topology in its deterministic order.
[[nodiscard]] core::u64 digest(const HardwareMap &map) noexcept;

} // namespace mediaserver::topology
