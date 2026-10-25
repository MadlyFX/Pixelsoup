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
    // Zero preserves the legacy desktop/current-mode behavior. A physical
    // binding supplies an exact rational rate and requires the presenter to
    // select that native fullscreen mode rather than silently accepting the
    // closest OS choice.
    core::u32 refresh_numerator{};
    core::u32 refresh_denominator{};
    bool require_exact_display_mode{};
    core::u32 output_bit_depth{8U};
    // The desktop rectangle of the display this binding names, resolved from
    // the authored target identity rather than from where the window happens
    // to land. Width zero means no identified display, which is the legacy
    // launch contract: the presenter then keeps its position-based behavior.
    //
    // Position alone cannot select a display. Two active targets can share a
    // desktop origin - a mirrored pair does exactly that - and asking the
    // toolkit which display contains a point then answers with whichever one
    // it enumerated first. The authored binding already knows which physical
    // output it means, so the extent travels with the origin and the
    // presenter matches a display instead of guessing one.
    core::i32 display_bounds_x{};
    core::i32 display_bounds_y{};
    core::u32 display_bounds_width{};
    core::u32 display_bounds_height{};

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
