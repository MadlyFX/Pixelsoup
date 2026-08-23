#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/show/show_clock.hpp>

namespace mediaserver::show {

enum class PixelFormat : core::u8 {
    Rgba8888,
    Bgra8888,
};

// A borrowed, caller-owned 32-bit colour surface. Nothing here allocates: the
// pattern is rasterised straight into a presentation buffer so it is legal to
// call from the playback loop.
struct FrameBufferView final {
    std::span<core::u8> pixels;
    core::u32 width{};
    core::u32 height{};
    core::u32 stride{}; // bytes per row
    PixelFormat format{PixelFormat::Rgba8888};
};

// What a node is currently presenting. This is deliberately everything a human
// needs to compare two output windows at a glance and see divergence: the show
// frame, the control revision behind it, and whether this frame was newly
// approved or held.
struct TestPatternInput final {
    ShowFrame show_frame{};
    cmd::FrameNumber control_frame{};
    FrameRate rate{FrameRate{60U, 1U}};
    TransportState state{TransportState::Stopped};
    core::u32 node_index{};
    // The Director had not approved a newer frame at the last boundary, so the
    // previous frame stands.
    bool held{};
    // The node has latched a resync fault: what is on screen is no longer
    // known to match the Director.
    bool needs_resync{};
};

[[nodiscard]] bool valid_frame_buffer(const FrameBufferView& target) noexcept;

// Deterministic for a given input and surface geometry, which is what makes it
// usable as a cross-process equality check: two nodes presenting the same show
// frame produce byte-identical surfaces apart from their node tint.
[[nodiscard]] bool render_test_pattern(
    const FrameBufferView& target,
    const TestPatternInput& input) noexcept;

// FNV-1a over the visible pixels, skipping stride padding, so the digest is
// independent of buffer padding and stable across runs.
[[nodiscard]] core::u64 frame_buffer_digest(
    const FrameBufferView& target) noexcept;

// 5x7 bitmap text, drawn at an integer scale. Exposed because the operator and
// node shells need the same allocation-free label rendering the pattern uses.
void draw_text(
    const FrameBufferView& target,
    core::i32 origin_x,
    core::i32 origin_y,
    core::u32 scale,
    core::u32 rgba,
    std::string_view text) noexcept;

void fill_rect(
    const FrameBufferView& target,
    core::i32 origin_x,
    core::i32 origin_y,
    core::i32 width,
    core::i32 height,
    core::u32 rgba) noexcept;

// Decimal rendering into a caller-owned buffer. Returns the written prefix, or
// an empty view when the buffer is too small.
[[nodiscard]] std::string_view format_unsigned(
    std::span<char> buffer,
    core::u64 value,
    core::u32 min_digits = 1U) noexcept;

// "HH:MM:SS:FF", non-drop-frame. Requires at least 11 characters.
[[nodiscard]] std::string_view format_timecode(
    std::span<char> buffer,
    Timecode value) noexcept;

} // namespace mediaserver::show
