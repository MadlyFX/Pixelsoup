#include "allocation_guard.hpp"

#include <mediaserver/show/test_pattern.hpp>

#include "show_test_support.hpp"

#include <array>
#include <span>
#include <string_view>
#include <vector>

namespace {

namespace show = mediaserver::show;
namespace cmd = mediaserver::cmd;
namespace core = mediaserver::core;

using mediaserver::show::test::AllocationGuard;

constexpr core::u32 surface_width = 96U;
constexpr core::u32 surface_height = 72U;
constexpr core::u32 padded_stride = surface_width * 4U + 16U;
constexpr core::u8 canary = 0xa5U;

struct Surface final {
    std::vector<core::u8> storage;
    show::FrameBufferView view{};
};

[[nodiscard]] Surface make_surface(
    core::u32 width = surface_width,
    core::u32 height = surface_height,
    core::u32 stride = padded_stride,
    show::PixelFormat format = show::PixelFormat::Rgba8888) {
    Surface surface{};
    const auto required = static_cast<std::size_t>(stride)
        * static_cast<std::size_t>(height);
    // A trailing guard region catches any write past the declared surface.
    surface.storage.assign(required + 64U, canary);
    surface.view = show::FrameBufferView{
        std::span{surface.storage}.first(required),
        width,
        height,
        stride,
        format};
    return surface;
}

[[nodiscard]] bool guard_region_intact(const Surface& surface) {
    const auto required = static_cast<std::size_t>(surface.view.stride)
        * static_cast<std::size_t>(surface.view.height);
    for (auto index = required; index < surface.storage.size(); ++index) {
        if (surface.storage[index] != canary) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] show::TestPatternInput pattern_input(core::u64 frame) {
    show::TestPatternInput input{};
    input.show_frame = show::ShowFrame{frame};
    input.control_frame = cmd::FrameNumber{frame};
    input.rate = show::FrameRate{50U, 1U};
    input.state = show::TransportState::Playing;
    return input;
}

void malformed_surfaces_are_refused() {
    std::vector<core::u8> storage(1024U, 0U);

    const std::array<show::FrameBufferView, 4U> rejected{
        // Zero extent.
        show::FrameBufferView{storage, 0U, 4U, 16U},
        show::FrameBufferView{storage, 4U, 0U, 16U},
        // Stride narrower than one row of pixels.
        show::FrameBufferView{storage, 8U, 4U, 16U},
        // Buffer shorter than stride times height.
        show::FrameBufferView{
            std::span{storage}.first(32U), 4U, 4U, 16U},
    };

    for (const auto& view : rejected) {
        SHOW_CHECK(!show::valid_frame_buffer(view));
        SHOW_CHECK(!show::render_test_pattern(view, pattern_input(1U)));
        SHOW_CHECK(show::frame_buffer_digest(view) == 0U);
    }

    const show::FrameBufferView accepted{
        std::span{storage}.first(64U), 4U, 4U, 16U};
    SHOW_CHECK(show::valid_frame_buffer(accepted));
    SHOW_CHECK(show::render_test_pattern(accepted, pattern_input(1U)));
}

void rendering_stays_inside_the_declared_surface() {
    auto surface = make_surface();
    SHOW_CHECK(show::render_test_pattern(surface.view, pattern_input(12'345U)));
    SHOW_CHECK(guard_region_intact(surface));

    // Stride padding is not part of the surface and must be untouched.
    for (core::u32 y = 0U; y < surface.view.height; ++y) {
        const auto row = static_cast<std::size_t>(y)
            * static_cast<std::size_t>(surface.view.stride);
        for (auto index = static_cast<std::size_t>(surface.view.width) * 4U;
             index < surface.view.stride;
             ++index) {
            SHOW_CHECK(surface.storage[row + index] == canary);
        }
    }

    // Text pushed well outside the surface clips instead of corrupting memory.
    show::draw_text(surface.view, -400, -400, 3U, 0xffffffffU, "CLIPPED");
    show::draw_text(surface.view, 4'000, 4'000, 3U, 0xffffffffU, "CLIPPED");
    show::fill_rect(surface.view, -50, -50, 10'000, 10'000, 0x102030ffU);
    SHOW_CHECK(guard_region_intact(surface));
}

void the_pattern_is_deterministic_for_a_given_frame() {
    auto first = make_surface();
    auto second = make_surface();

    SHOW_CHECK(show::render_test_pattern(first.view, pattern_input(777U)));
    SHOW_CHECK(show::render_test_pattern(second.view, pattern_input(777U)));
    SHOW_CHECK(
        show::frame_buffer_digest(first.view)
        == show::frame_buffer_digest(second.view));

    // Re-rendering over an existing surface produces the same result, so a
    // presented frame never depends on what was there before.
    SHOW_CHECK(show::render_test_pattern(first.view, pattern_input(4U)));
    SHOW_CHECK(show::render_test_pattern(first.view, pattern_input(777U)));
    SHOW_CHECK(
        show::frame_buffer_digest(first.view)
        == show::frame_buffer_digest(second.view));
}

void adjacent_frames_are_visibly_different() {
    auto surface = make_surface();
    std::array<core::u64, 8U> digests{};

    for (core::u64 offset = 0U; offset < digests.size(); ++offset) {
        SHOW_CHECK(
            show::render_test_pattern(surface.view, pattern_input(100U + offset)));
        digests[static_cast<std::size_t>(offset)] =
            show::frame_buffer_digest(surface.view);
    }

    for (std::size_t left = 0U; left < digests.size(); ++left) {
        for (auto right = left + 1U; right < digests.size(); ++right) {
            SHOW_CHECK(digests[left] != digests[right]);
        }
    }
}

void node_identity_and_status_change_the_surface() {
    auto surface = make_surface();

    auto input = pattern_input(500U);
    SHOW_CHECK(show::render_test_pattern(surface.view, input));
    const auto baseline = show::frame_buffer_digest(surface.view);

    input.node_index = 1U;
    SHOW_CHECK(show::render_test_pattern(surface.view, input));
    const auto other_node = show::frame_buffer_digest(surface.view);
    SHOW_CHECK(other_node != baseline);

    input.node_index = 0U;
    input.held = true;
    SHOW_CHECK(show::render_test_pattern(surface.view, input));
    const auto held = show::frame_buffer_digest(surface.view);
    SHOW_CHECK(held != baseline);

    input.held = false;
    input.needs_resync = true;
    SHOW_CHECK(show::render_test_pattern(surface.view, input));
    const auto resync = show::frame_buffer_digest(surface.view);
    SHOW_CHECK(resync != baseline);
    SHOW_CHECK(resync != held);

    input.needs_resync = false;
    input.state = show::TransportState::Paused;
    SHOW_CHECK(show::render_test_pattern(surface.view, input));
    SHOW_CHECK(show::frame_buffer_digest(surface.view) != baseline);
}

void pixel_order_follows_the_declared_format() {
    auto rgba = make_surface(4U, 1U, 16U, show::PixelFormat::Rgba8888);
    auto bgra = make_surface(4U, 1U, 16U, show::PixelFormat::Bgra8888);

    show::fill_rect(rgba.view, 0, 0, 4, 1, 0x11223344U);
    show::fill_rect(bgra.view, 0, 0, 4, 1, 0x11223344U);

    SHOW_CHECK(rgba.storage[0] == 0x11U);
    SHOW_CHECK(rgba.storage[1] == 0x22U);
    SHOW_CHECK(rgba.storage[2] == 0x33U);
    SHOW_CHECK(rgba.storage[3] == 0x44U);

    SHOW_CHECK(bgra.storage[0] == 0x33U);
    SHOW_CHECK(bgra.storage[1] == 0x22U);
    SHOW_CHECK(bgra.storage[2] == 0x11U);
    SHOW_CHECK(bgra.storage[3] == 0x44U);
}

void numbers_and_timecode_format_into_borrowed_storage() {
    std::array<char, 24U> buffer{};

    SHOW_CHECK(show::format_unsigned(std::span{buffer}, 0U) == "0");
    SHOW_CHECK(show::format_unsigned(std::span{buffer}, 1'234'567U) == "1234567");
    SHOW_CHECK(show::format_unsigned(std::span{buffer}, 7U, 4U) == "0007");
    SHOW_CHECK(
        show::format_unsigned(std::span{buffer}, 18'446'744'073'709'551'615ULL)
        == "18446744073709551615");

    // A buffer too small yields nothing rather than a truncated number.
    std::array<char, 2U> tiny{};
    SHOW_CHECK(show::format_unsigned(std::span{tiny}, 1'000U).empty());

    const show::Timecode code{1U, 2U, 3U, 4U};
    SHOW_CHECK(show::format_timecode(std::span{buffer}, code) == "01:02:03:04");

    std::array<char, 10U> short_buffer{};
    SHOW_CHECK(show::format_timecode(std::span{short_buffer}, code).empty());
}

void rendering_a_frame_never_allocates() {
    auto surface = make_surface();
    // Render once outside the guard so any lazy first-touch work is done.
    SHOW_CHECK(show::render_test_pattern(surface.view, pattern_input(1U)));

    {
        const AllocationGuard guard{};
        const auto rendered =
            show::render_test_pattern(surface.view, pattern_input(2U));
        const auto digest = show::frame_buffer_digest(surface.view);
        if (!rendered || digest == 0U) {
            throw std::runtime_error{"the pattern failed to render"};
        }
        SHOW_CHECK(guard.quiet());
    }
    SHOW_CHECK(guard_region_intact(surface));
}

void a_tiny_surface_still_renders() {
    // Output windows are resizable, so the layout has to survive a surface far
    // smaller than any glyph it wants to draw.
    for (const core::u32 extent : {1U, 2U, 7U, 15U, 33U}) {
        auto surface = make_surface(extent, extent, extent * 4U);
        SHOW_CHECK(show::render_test_pattern(surface.view, pattern_input(9U)));
        SHOW_CHECK(guard_region_intact(surface));
    }
}

constexpr std::array<mediaserver::show::test::Case, 9U> cases{{
    {"malformed surfaces", &malformed_surfaces_are_refused},
    {"writes stay in bounds", &rendering_stays_inside_the_declared_surface},
    {"deterministic", &the_pattern_is_deterministic_for_a_given_frame},
    {"adjacent frames differ", &adjacent_frames_are_visibly_different},
    {"node and status differ", &node_identity_and_status_change_the_surface},
    {"pixel order", &pixel_order_follows_the_declared_format},
    {"number formatting", &numbers_and_timecode_format_into_borrowed_storage},
    {"render is allocation-free", &rendering_a_frame_never_allocates},
    {"tiny surfaces", &a_tiny_surface_still_renders},
}};

} // namespace

int main() {
    return mediaserver::show::test::run(cases);
}
