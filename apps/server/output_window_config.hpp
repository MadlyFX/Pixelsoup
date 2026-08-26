#pragma once

#include "playback.hpp"

#include <mediaserver/topology/hardware_map.hpp>

#include <optional>

namespace mediaserver::app {

// The effective SDL window contract for one render node. Kept independent of
// presenter lifetime so replicated project state can be resolved and tested
// without opening a display.
struct PresenterWindowSettings final {
    core::u32 width{};
    core::u32 height{};
    core::i32 position_x{};
    core::i32 position_y{};
    bool position_explicit{};
    bool windowed{};
    bool fullscreen{};
    bool borderless{};

    bool operator==(const PresenterWindowSettings&) const = default;
};

// Resolves the unique RenderServer matching options.node_index. Missing,
// duplicate, inherited, or invalid authored configurations fail closed to the
// launch options instead of moving a live output to an arbitrary display.
[[nodiscard]] PresenterWindowSettings presenter_window_settings(
    const PlaybackOptions& options,
    const topology::HardwareMap& hardware) noexcept;

// Resolves one replicated revision into either a safe desired configuration or
// no change. Rejected document nodes matter only when they make this node's
// identity ambiguous: a unique valid match remains usable, while a missing
// match plus rejected data cannot be distinguished from a malformed target.
[[nodiscard]] std::optional<PresenterWindowSettings>
presenter_window_update(
    const PlaybackOptions& options,
    const topology::HardwareMap& hardware,
    core::u64 rejected_nodes) noexcept;

[[nodiscard]] PlaybackOptions presenter_options(
    const PlaybackOptions& base,
    const PresenterWindowSettings& window);

} // namespace mediaserver::app
