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

// ---------------------------------------------------------------------------
// Backend-neutral description of the same pattern.
//
// The CPU rasteriser above writes pixels. A GPU path cannot use it, and a
// second copy of the layout would drift from it within a release. So the
// layout is also available as a list of quads: axis-aligned rectangles with a
// colour and a region of a coverage atlas. A solid rectangle points at a fully
// covered texel; a glyph cell points at its bitmap. Both paths then draw the
// same frame by construction, and the GPU path is one instanced draw.
//
// Nothing here mentions a graphics API. The rhi seam consumes these; so could
// anything else.

// Coverage atlas geometry. One row of glyph cells, each `glyph_width` by
// `glyph_height` texels, preceded by a single fully covered texel that solid
// rectangles sample.
inline constexpr core::u32 glyph_width = 5U;
inline constexpr core::u32 glyph_height = 7U;
// Cell pitch used when laying text out, in unscaled texels.
inline constexpr core::u32 glyph_advance = glyph_width + 1U;
inline constexpr std::size_t glyph_count = 41U;

// The solid texel occupies the first cell so that every region is cell
// aligned, which keeps the uv arithmetic uniform.
inline constexpr core::u32 atlas_width = glyph_width * (glyph_count + 1U);
inline constexpr core::u32 atlas_height = glyph_height;
inline constexpr core::u32 atlas_rgba_bytes = atlas_width * atlas_height * 4U;

// Writes the coverage atlas as RGBA8. Coverage is carried in every channel so
// the same atlas works whichever channel a backend samples. Returns false when
// the span is not exactly `atlas_rgba_bytes`.
[[nodiscard]] bool write_glyph_atlas(std::span<core::u8> rgba) noexcept;

struct QuadUv final {
    float u0{};
    float v0{};
    float u1{};
    float v1{};

    bool operator==(const QuadUv&) const = default;
};

// Atlas region for one character, and for the fully covered texel.
[[nodiscard]] QuadUv glyph_uv(char character) noexcept;
[[nodiscard]] QuadUv solid_uv() noexcept;

// One instanced quad. Bounds are in clip space so the vertex stage needs no
// target size, and colour is straight alpha in linear 0..1.
struct PatternQuad final {
    float x0{};
    float y0{};
    float x1{};
    float y1{};
    float r{};
    float g{};
    float b{};
    float a{};
    QuadUv uv{};

    bool operator==(const PatternQuad&) const = default;
};

// Upper bound on the quads one frame can need: the background, border edges,
// inner field, sweep bar, and every glyph cell of every label. Fixed so a
// caller can own the storage and the frame path never allocates.
inline constexpr std::size_t max_pattern_quads = 256U;

struct PatternQuadReport final {
    std::size_t count{};
    // The layout needed more room than the caller supplied, so what was
    // written is a prefix rather than the frame. Reported rather than silently
    // truncated: a partial frame on one output and a whole one on another is
    // exactly the divergence this pattern exists to reveal.
    bool truncated{};
};

// Describes the frame `render_test_pattern` would rasterise, as quads. The
// surface size is passed explicitly because a GPU target need not be the size
// of any CPU surface.
[[nodiscard]] PatternQuadReport build_pattern_quads(
    const TestPatternInput& input,
    core::u32 width,
    core::u32 height,
    std::span<PatternQuad> quads) noexcept;

} // namespace mediaserver::show
