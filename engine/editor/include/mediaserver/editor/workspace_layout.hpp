#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/gui/dock.hpp>

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
};

using WorkspaceLayoutEncodeResult =
    core::Result<WorkspaceLayoutBytes, WorkspaceLayoutError>;
using WorkspaceLayoutDecodeResult =
    core::Result<WorkspaceLayoutState, WorkspaceLayoutError>;

[[nodiscard]] WorkspaceLayoutEncodeResult
encode_workspace_layout(const WorkspaceLayoutState &state) noexcept;

[[nodiscard]] WorkspaceLayoutDecodeResult
decode_workspace_layout(std::span<const std::byte> bytes) noexcept;

} // namespace mediaserver::editor
