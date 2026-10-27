#include <mediaserver/editor/workspace_layout.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <utility>

namespace mediaserver::editor {
namespace {

constexpr std::array<std::byte, 8U> magic{
    std::byte{'M'}, std::byte{'S'}, std::byte{'D'}, std::byte{'O'},
    std::byte{'C'}, std::byte{'K'}, std::byte{'0'}, std::byte{'1'}};
constexpr std::uint16_t legacy_version = 1U;
constexpr std::uint16_t window_size_version = 2U;
constexpr std::uint16_t resolution_version = 3U;
// Version 4 spends version 3's reserved header word on the interface zoom.
constexpr std::uint16_t zoom_version = 4U;
// Version 5 appends the Compositor's Parameters width to the header.
constexpr std::uint16_t parameter_width_version = 5U;
// Version 6 appends a word of Compositor view flags; bit 0 is the minimap.
constexpr std::uint16_t current_version = 6U;
constexpr std::uint16_t legacy_header_size = 28U;
constexpr std::uint16_t window_size_header_size = 36U;
constexpr std::uint16_t resolution_header_size = 40U;
constexpr std::uint16_t parameter_width_header_size = 44U;
constexpr std::uint16_t header_size = 48U;
constexpr std::uint32_t minimap_flag = 1U;
constexpr std::size_t fixed_node_size = 28U;
constexpr std::size_t floating_size = 20U;
constexpr std::size_t closed_size = 40U;
constexpr std::size_t maximum_entries = 4'096U;

void append_u8(WorkspaceLayoutBytes &out, std::uint8_t value) {
  out.push_back(static_cast<std::byte>(value));
}

void append_u16(WorkspaceLayoutBytes &out, std::uint16_t value) {
  append_u8(out, static_cast<std::uint8_t>(value & 0xffU));
  append_u8(out, static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}

void append_u32(WorkspaceLayoutBytes &out, std::uint32_t value) {
  for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
    append_u8(out, static_cast<std::uint8_t>((value >> shift) & 0xffU));
  }
}

void append_u64(WorkspaceLayoutBytes &out, std::uint64_t value) {
  for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
    append_u8(out, static_cast<std::uint8_t>((value >> shift) & 0xffU));
  }
}

void append_float(WorkspaceLayoutBytes &out, float value) {
  append_u32(out, std::bit_cast<std::uint32_t>(value));
}

class Reader final {
public:
  explicit Reader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

  [[nodiscard]] bool u8(std::uint8_t &value) noexcept {
    if (remaining() < 1U) {
      return false;
    }
    value = std::to_integer<std::uint8_t>(bytes_[cursor_++]);
    return true;
  }

  [[nodiscard]] bool u16(std::uint16_t &value) noexcept {
    std::uint8_t low{};
    std::uint8_t high{};
    if (!u8(low) || !u8(high)) {
      return false;
    }
    value = static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(low) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(high) << 8U));
    return true;
  }

  [[nodiscard]] bool u32(std::uint32_t &value) noexcept {
    value = 0U;
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
      std::uint8_t byte{};
      if (!u8(byte)) {
        return false;
      }
      value |= static_cast<std::uint32_t>(byte) << shift;
    }
    return true;
  }

  [[nodiscard]] bool u64(std::uint64_t &value) noexcept {
    value = 0U;
    for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
      std::uint8_t byte{};
      if (!u8(byte)) {
        return false;
      }
      value |= static_cast<std::uint64_t>(byte) << shift;
    }
    return true;
  }

  [[nodiscard]] bool floating(float &value) noexcept {
    std::uint32_t bits{};
    if (!u32(bits)) {
      return false;
    }
    value = std::bit_cast<float>(bits);
    return true;
  }

  [[nodiscard]] std::size_t remaining() const noexcept {
    return bytes_.size() - cursor_;
  }

private:
  std::span<const std::byte> bytes_;
  std::size_t cursor_{};
};

[[nodiscard]] WorkspaceLayoutEncodeResult encode_failure(
    WorkspaceLayoutError error) {
  return WorkspaceLayoutEncodeResult::failure(error);
}

[[nodiscard]] bool valid_parameter_width(float width) noexcept {
  return std::isfinite(width) && width >= 0.0F &&
         width <= max_compositor_parameter_width;
}

[[nodiscard]] WorkspaceLayoutDecodeResult decode_failure(
    WorkspaceLayoutError error) {
  return WorkspaceLayoutDecodeResult::failure(error);
}

} // namespace

WorkspaceLayoutEncodeResult
encode_workspace_layout(const WorkspaceLayoutState &workspace) noexcept {
  try {
    const gui::DockLayout::State &state = workspace.docks;
    if (!valid(workspace.stage_resolution) || !valid(workspace.program_resolution) ||
        workspace.ui_zoom_percent < min_ui_zoom_percent ||
        workspace.ui_zoom_percent > max_ui_zoom_percent ||
        !valid_parameter_width(workspace.compositor_parameter_width)) {
      return encode_failure(WorkspaceLayoutError::InvalidFormat);
    }
    if (state.nodes.size() > maximum_entries ||
        state.floating.size() > maximum_entries ||
        state.closed.size() > maximum_entries) {
      return encode_failure(WorkspaceLayoutError::SizeLimitExceeded);
    }
    std::size_t tab_count{};
    for (const gui::DockLayout::State::Node &node : state.nodes) {
      if (node.tabs.size() > maximum_entries - tab_count) {
        return encode_failure(WorkspaceLayoutError::SizeLimitExceeded);
      }
      tab_count += node.tabs.size();
    }
    const std::size_t size = static_cast<std::size_t>(header_size) +
        state.nodes.size() * fixed_node_size + tab_count * sizeof(gui::PanelId) +
        state.floating.size() * floating_size +
        state.closed.size() * closed_size;
    if (size > max_workspace_layout_bytes) {
      return encode_failure(WorkspaceLayoutError::SizeLimitExceeded);
    }

    WorkspaceLayoutBytes bytes;
    bytes.reserve(size);
    bytes.insert(bytes.end(), magic.begin(), magic.end());
    append_u16(bytes, current_version);
    append_u16(bytes, header_size);
    append_u32(bytes, static_cast<std::uint32_t>(state.nodes.size()));
    append_u32(bytes, static_cast<std::uint32_t>(state.floating.size()));
    append_u32(bytes, static_cast<std::uint32_t>(state.closed.size()));
    append_u32(bytes, state.root);
    append_u32(bytes, workspace.window_width);
    append_u32(bytes, workspace.window_height);
    append_u8(bytes, static_cast<core::u8>(workspace.stage_resolution));
    append_u8(bytes, static_cast<core::u8>(workspace.program_resolution));
    append_u16(bytes, workspace.ui_zoom_percent);
    append_float(bytes, workspace.compositor_parameter_width);
    append_u32(bytes, workspace.compositor_minimap ? minimap_flag : 0U);

    for (const gui::DockLayout::State::Node &node : state.nodes) {
      append_u8(bytes, node.split ? 1U : 0U);
      append_u8(bytes, static_cast<std::uint8_t>(node.axis));
      append_u16(bytes, 0U);
      append_float(bytes, node.fraction);
      append_u32(bytes, node.first);
      append_u32(bytes, node.second);
      append_u64(bytes, node.active);
      append_u32(bytes, static_cast<std::uint32_t>(node.tabs.size()));
      for (const gui::PanelId panel : node.tabs) {
        append_u64(bytes, panel);
      }
    }
    for (const gui::DockLayout::State::Floating &frame : state.floating) {
      append_u32(bytes, frame.node);
      append_float(bytes, frame.frame.x);
      append_float(bytes, frame.frame.y);
      append_float(bytes, frame.frame.width);
      append_float(bytes, frame.frame.height);
    }
    for (const gui::DockLayout::State::Closed &closed : state.closed) {
      append_u64(bytes, closed.panel);
      append_u8(bytes, static_cast<std::uint8_t>(
                           (closed.has_anchor ? 1U : 0U) |
                           (closed.floating ? 2U : 0U)));
      append_u8(bytes, static_cast<std::uint8_t>(closed.side));
      append_u16(bytes, 0U);
      append_u64(bytes, closed.anchor);
      append_float(bytes, closed.fraction);
      append_float(bytes, closed.frame.x);
      append_float(bytes, closed.frame.y);
      append_float(bytes, closed.frame.width);
      append_float(bytes, closed.frame.height);
    }
    if (bytes.size() != size) {
      return encode_failure(WorkspaceLayoutError::InvalidFormat);
    }
    return WorkspaceLayoutEncodeResult::success(std::move(bytes));
  } catch (const std::bad_alloc &) {
    return encode_failure(WorkspaceLayoutError::AllocationFailed);
  } catch (const std::length_error &) {
    return encode_failure(WorkspaceLayoutError::SizeLimitExceeded);
  }
}

WorkspaceLayoutDecodeResult
decode_workspace_layout(std::span<const std::byte> bytes) noexcept {
  try {
    if (bytes.size() > max_workspace_layout_bytes) {
      return decode_failure(WorkspaceLayoutError::SizeLimitExceeded);
    }
    if (bytes.size() < legacy_header_size ||
        !std::equal(magic.begin(), magic.end(), bytes.begin())) {
      return decode_failure(WorkspaceLayoutError::InvalidFormat);
    }
    Reader reader{bytes.subspan(magic.size())};
    std::uint16_t version{};
    std::uint16_t encoded_header_size{};
    std::uint32_t node_count{};
    std::uint32_t floating_count{};
    std::uint32_t closed_count{};
    std::uint32_t root{};
    std::uint32_t window_width{};
    std::uint32_t window_height{};
    if (!reader.u16(version) || !reader.u16(encoded_header_size) ||
        !reader.u32(node_count) || !reader.u32(floating_count) ||
        !reader.u32(closed_count) || !reader.u32(root)) {
      return decode_failure(WorkspaceLayoutError::InvalidFormat);
    }
    if (version != legacy_version && version != window_size_version &&
        version != resolution_version && version != zoom_version &&
        version != parameter_width_version && version != current_version) {
      return decode_failure(WorkspaceLayoutError::UnsupportedVersion);
    }
    const std::uint16_t expected_header_size =
        version == legacy_version        ? legacy_header_size
        : version == window_size_version ? window_size_header_size
        : version == parameter_width_version ? parameter_width_header_size
        : version == current_version         ? header_size
                                             : resolution_header_size;
    if (encoded_header_size != expected_header_size ||
        (version >= window_size_version &&
         (!reader.u32(window_width) || !reader.u32(window_height))) ||
        node_count > maximum_entries ||
        floating_count > maximum_entries || closed_count > maximum_entries) {
      return decode_failure(WorkspaceLayoutError::InvalidFormat);
    }

    // Both zero is the legacy/no-size sentinel. A half-present size cannot be
    // meaningfully restored, and the upper bound keeps corrupt records from
    // asking SDL or the renderers for absurd allocations at startup.
    constexpr std::uint32_t maximum_window_extent = 32'768U;
    if ((window_width == 0U) != (window_height == 0U) ||
        window_width > maximum_window_extent ||
        window_height > maximum_window_extent) {
      return decode_failure(WorkspaceLayoutError::InvalidFormat);
    }

    WorkspaceLayoutState workspace{};
    if (version >= resolution_version) {
      core::u8 stage_resolution{};
      core::u8 program_resolution{};
      std::uint16_t zoom{};
      if (!reader.u8(stage_resolution) || !reader.u8(program_resolution) ||
          !reader.u16(zoom) ||
          !valid(static_cast<PreviewResolution>(stage_resolution)) ||
          !valid(static_cast<PreviewResolution>(program_resolution))) {
        return decode_failure(WorkspaceLayoutError::InvalidFormat);
      }
      if (version == resolution_version) {
        if (zoom != 0U) {
          return decode_failure(WorkspaceLayoutError::InvalidFormat);
        }
      } else if (zoom < min_ui_zoom_percent || zoom > max_ui_zoom_percent) {
        return decode_failure(WorkspaceLayoutError::InvalidFormat);
      } else {
        workspace.ui_zoom_percent = zoom;
      }
      workspace.stage_resolution = static_cast<PreviewResolution>(stage_resolution);
      workspace.program_resolution = static_cast<PreviewResolution>(program_resolution);
    }
    if (version >= parameter_width_version &&
        (!reader.floating(workspace.compositor_parameter_width) ||
         !valid_parameter_width(workspace.compositor_parameter_width))) {
      return decode_failure(WorkspaceLayoutError::InvalidFormat);
    }
    if (version >= current_version) {
      std::uint32_t flags{};
      // Unknown flags are refused rather than silently dropped on resave.
      if (!reader.u32(flags) || (flags & ~minimap_flag) != 0U) {
        return decode_failure(WorkspaceLayoutError::InvalidFormat);
      }
      workspace.compositor_minimap = (flags & minimap_flag) != 0U;
    }
    workspace.window_width = window_width;
    workspace.window_height = window_height;
    gui::DockLayout::State &state = workspace.docks;
    state.root = root;
    state.nodes.reserve(node_count);
    for (std::uint32_t index = 0U; index < node_count; ++index) {
      std::uint8_t split{};
      std::uint8_t axis{};
      std::uint16_t reserved{};
      std::uint32_t first{};
      std::uint32_t second{};
      std::uint64_t active{};
      std::uint32_t tab_count{};
      gui::DockLayout::State::Node node;
      if (!reader.u8(split) || !reader.u8(axis) || !reader.u16(reserved) ||
          !reader.floating(node.fraction) || !reader.u32(first) ||
          !reader.u32(second) || !reader.u64(active) ||
          !reader.u32(tab_count) || split > 1U || reserved != 0U ||
          axis > static_cast<std::uint8_t>(gui::Axis::Column) ||
          tab_count > maximum_entries) {
        return decode_failure(WorkspaceLayoutError::InvalidFormat);
      }
      node.split = split != 0U;
      node.axis = static_cast<gui::Axis>(axis);
      node.first = first;
      node.second = second;
      node.active = active;
      node.tabs.reserve(tab_count);
      for (std::uint32_t tab = 0U; tab < tab_count; ++tab) {
        std::uint64_t panel{};
        if (!reader.u64(panel)) {
          return decode_failure(WorkspaceLayoutError::InvalidFormat);
        }
        node.tabs.push_back(panel);
      }
      state.nodes.push_back(std::move(node));
    }

    state.floating.reserve(floating_count);
    for (std::uint32_t index = 0U; index < floating_count; ++index) {
      gui::DockLayout::State::Floating frame;
      if (!reader.u32(frame.node) || !reader.floating(frame.frame.x) ||
          !reader.floating(frame.frame.y) ||
          !reader.floating(frame.frame.width) ||
          !reader.floating(frame.frame.height)) {
        return decode_failure(WorkspaceLayoutError::InvalidFormat);
      }
      state.floating.push_back(frame);
    }

    state.closed.reserve(closed_count);
    for (std::uint32_t index = 0U; index < closed_count; ++index) {
      gui::DockLayout::State::Closed closed;
      std::uint8_t flags{};
      std::uint8_t side{};
      std::uint16_t reserved{};
      std::uint64_t panel{};
      std::uint64_t anchor{};
      if (!reader.u64(panel) || !reader.u8(flags) || !reader.u8(side) ||
          !reader.u16(reserved) || !reader.u64(anchor) ||
          !reader.floating(closed.fraction) ||
          !reader.floating(closed.frame.x) ||
          !reader.floating(closed.frame.y) ||
          !reader.floating(closed.frame.width) ||
          !reader.floating(closed.frame.height) || flags > 3U ||
          side > static_cast<std::uint8_t>(gui::DockSide::Center) ||
          reserved != 0U) {
        return decode_failure(WorkspaceLayoutError::InvalidFormat);
      }
      closed.panel = panel;
      closed.has_anchor = (flags & 1U) != 0U;
      closed.floating = (flags & 2U) != 0U;
      closed.side = static_cast<gui::DockSide>(side);
      closed.anchor = anchor;
      state.closed.push_back(closed);
    }
    if (reader.remaining() != 0U) {
      return decode_failure(WorkspaceLayoutError::InvalidFormat);
    }
    return WorkspaceLayoutDecodeResult::success(std::move(workspace));
  } catch (const std::bad_alloc &) {
    return decode_failure(WorkspaceLayoutError::AllocationFailed);
  } catch (const std::length_error &) {
    return decode_failure(WorkspaceLayoutError::SizeLimitExceeded);
  }
}

} // namespace mediaserver::editor
