#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/gui/dock.hpp>
#include <mediaserver/editor/preview_resolution.hpp>

#include <cstddef>
#include <span>
#include <vector>

// Versioned wire form of the operator's machine-local dock arrangement.
// Keeping the codec beside the editor (rather than in persist) leaves the
// settings layer ignorant of GUI types and lets the dock model remain a pure
// in-memory model.
namespace mediaserver::editor {

inline constexpr std::size_t max_workspace_layout_bytes = 256U * 1'024U;

enum class WorkspaceLayoutError {
  InvalidFormat,
  UnsupportedVersion,
  SizeLimitExceeded,
  AllocationFailed,
};

using WorkspaceLayoutBytes = std::vector<std::byte>;

// The dock tree and the top-level window extent are one machine-local UI
// workspace. Keeping them in one record means a resize follows the same
// settle-and-save path as a dock gesture and cannot drift out of sync with it.
// A zero extent is reserved for legacy records that predate window sizing.
struct WorkspaceLayoutState final {
  gui::DockLayout::State docks;
  core::u32 window_width{};
  core::u32 window_height{};
  PreviewResolution stage_resolution{PreviewResolution::Full};
  PreviewResolution program_resolution{PreviewResolution::Full};
  // The operator's interface zoom on top of the display's own scale, in
  // percent. 100 is "as the display asks"; records older than the field read
  // as 100. Machine-local like the rest of the record: a 4K operator desk and
  // a laptop want different answers for the same project.
  core::u16 ui_zoom_percent{100U};
  // The Compositor's Parameters panel width in UI units, as the operator
  // last dragged it. Zero, and records older than the field, mean the
  // panel's default width.
  float compositor_parameter_width{};
  // Whether the Compositor shows its minimap. Records older than the field
  // read as hidden, the panel's default.
  bool compositor_minimap{};
  // Whether the show bar's night dim is on. Machine-local: it suits the room
  // the desk is in, not the show.
  bool night_dim{};
  // Whether the History strip is put away. Records older than the field read
  // as shown.
  bool history_hidden{};
  // Each view's media rendition. Records older than the field read as Proxy.
  MediaRendition stage_media{MediaRendition::Proxy};
  MediaRendition program_media{MediaRendition::Proxy};
};

// The zoom range the editor offers, in percent.
inline constexpr core::u16 min_ui_zoom_percent = 50U;
inline constexpr core::u16 max_ui_zoom_percent = 300U;
// Wider than any panel the editor draws; the record refuses anything past it.
inline constexpr float max_compositor_parameter_width = 4'096.0F;

using WorkspaceLayoutEncodeResult =
    core::Result<WorkspaceLayoutBytes, WorkspaceLayoutError>;
using WorkspaceLayoutDecodeResult =
    core::Result<WorkspaceLayoutState, WorkspaceLayoutError>;

[[nodiscard]] WorkspaceLayoutEncodeResult
encode_workspace_layout(const WorkspaceLayoutState &state) noexcept;

[[nodiscard]] WorkspaceLayoutDecodeResult
decode_workspace_layout(std::span<const std::byte> bytes) noexcept;

} // namespace mediaserver::editor
