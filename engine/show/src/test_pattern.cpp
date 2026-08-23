#include <mediaserver/show/test_pattern.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

namespace mediaserver::show {
namespace {

// Space, punctuation, digits, then A-Z. Five significant bits per row.
constexpr std::array<std::array<core::u8, glyph_height>, glyph_count> font{{
    {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000}, // space
    {0b00000, 0b00000, 0b00000, 0b01110, 0b00000, 0b00000, 0b00000}, // -
    {0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00110, 0b00110}, // .
    {0b00001, 0b00010, 0b00010, 0b00100, 0b01000, 0b01000, 0b10000}, // /
    {0b00000, 0b00110, 0b00110, 0b00000, 0b00110, 0b00110, 0b00000}, // :
    {0b01110, 0b10001, 0b10011, 0b10101, 0b11001, 0b10001, 0b01110}, // 0
    {0b00100, 0b01100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}, // 1
    {0b01110, 0b10001, 0b00001, 0b00010, 0b00100, 0b01000, 0b11111}, // 2
    {0b11111, 0b00010, 0b00100, 0b00010, 0b00001, 0b10001, 0b01110}, // 3
    {0b00010, 0b00110, 0b01010, 0b10010, 0b11111, 0b00010, 0b00010}, // 4
    {0b11111, 0b10000, 0b11110, 0b00001, 0b00001, 0b10001, 0b01110}, // 5
    {0b00110, 0b01000, 0b10000, 0b11110, 0b10001, 0b10001, 0b01110}, // 6
    {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b01000, 0b01000}, // 7
    {0b01110, 0b10001, 0b10001, 0b01110, 0b10001, 0b10001, 0b01110}, // 8
    {0b01110, 0b10001, 0b10001, 0b01111, 0b00001, 0b00010, 0b01100}, // 9
    {0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}, // A
    {0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110}, // B
    {0b01110, 0b10001, 0b10000, 0b10000, 0b10000, 0b10001, 0b01110}, // C
    {0b11110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b11110}, // D
    {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b11111}, // E
    {0b11111, 0b10000, 0b10000, 0b11110, 0b10000, 0b10000, 0b10000}, // F
    {0b01110, 0b10001, 0b10000, 0b10111, 0b10001, 0b10001, 0b01111}, // G
    {0b10001, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001}, // H
    {0b01110, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b01110}, // I
    {0b00111, 0b00010, 0b00010, 0b00010, 0b00010, 0b10010, 0b01100}, // J
    {0b10001, 0b10010, 0b10100, 0b11000, 0b10100, 0b10010, 0b10001}, // K
    {0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b10000, 0b11111}, // L
    {0b10001, 0b11011, 0b10101, 0b10101, 0b10001, 0b10001, 0b10001}, // M
    {0b10001, 0b11001, 0b10101, 0b10011, 0b10001, 0b10001, 0b10001}, // N
    {0b01110, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}, // O
    {0b11110, 0b10001, 0b10001, 0b11110, 0b10000, 0b10000, 0b10000}, // P
    {0b01110, 0b10001, 0b10001, 0b10001, 0b10101, 0b10010, 0b01101}, // Q
    {0b11110, 0b10001, 0b10001, 0b11110, 0b10100, 0b10010, 0b10001}, // R
    {0b01111, 0b10000, 0b10000, 0b01110, 0b00001, 0b00001, 0b11110}, // S
    {0b11111, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100, 0b00100}, // T
    {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01110}, // U
    {0b10001, 0b10001, 0b10001, 0b10001, 0b10001, 0b01010, 0b00100}, // V
    {0b10001, 0b10001, 0b10001, 0b10101, 0b10101, 0b11011, 0b10001}, // W
    {0b10001, 0b10001, 0b01010, 0b00100, 0b01010, 0b10001, 0b10001}, // X
    {0b10001, 0b10001, 0b01010, 0b00100, 0b00100, 0b00100, 0b00100}, // Y
    {0b11111, 0b00001, 0b00010, 0b00100, 0b01000, 0b10000, 0b11111}, // Z
}};

[[nodiscard]] constexpr std::size_t glyph_index(char character) noexcept {
    if (character >= '0' && character <= '9') {
        return 5U + static_cast<std::size_t>(character - '0');
    }
    if (character >= 'A' && character <= 'Z') {
        return 15U + static_cast<std::size_t>(character - 'A');
    }
    if (character >= 'a' && character <= 'z') {
        return 15U + static_cast<std::size_t>(character - 'a');
    }
    switch (character) {
    case '-':
        return 1U;
    case '.':
        return 2U;
    case '/':
        return 3U;
    case ':':
        return 4U;
    default:
        return 0U;
    }
}

// Packed 0xRRGGBBAA. The two supported surface layouts differ only in the
// order the components are stored.
constexpr core::u32 colour_white = 0xffffffffU;
constexpr core::u32 colour_field = 0x0a0d12ffU;
constexpr core::u32 colour_held = 0xffb020ffU;
constexpr core::u32 colour_resync = 0xff3030ffU;
constexpr core::u32 colour_label = 0x8fa3b8ffU;

// Six strongly separated hues. The border steps through them once per frame,
// so two windows a single frame apart are obviously different even to a camera.
constexpr std::array<core::u32, 6U> frame_palette{
    0xff4040ffU,
    0xffa030ffU,
    0xf0e040ffU,
    0x40d060ffU,
    0x4090ffffU,
    0xb060ffffU,
};

constexpr std::array<core::u32, 6U> node_palette{
    0x142033ffU,
    0x1b2a18ffU,
    0x2a1a2affU,
    0x2a2410ffU,
    0x102a2affU,
    0x241020ffU,
};

void write_pixel(
    const FrameBufferView& target,
    core::u32 x,
    core::u32 y,
    core::u32 rgba) noexcept {
    const auto offset = static_cast<std::size_t>(y)
            * static_cast<std::size_t>(target.stride)
        + static_cast<std::size_t>(x) * 4U;
    const auto red = static_cast<core::u8>((rgba >> 24U) & 0xffU);
    const auto green = static_cast<core::u8>((rgba >> 16U) & 0xffU);
    const auto blue = static_cast<core::u8>((rgba >> 8U) & 0xffU);
    const auto alpha = static_cast<core::u8>(rgba & 0xffU);

    if (target.format == PixelFormat::Bgra8888) {
        target.pixels[offset] = blue;
        target.pixels[offset + 1U] = green;
        target.pixels[offset + 2U] = red;
    } else {
        target.pixels[offset] = red;
        target.pixels[offset + 1U] = green;
        target.pixels[offset + 2U] = blue;
    }
    target.pixels[offset + 3U] = alpha;
}

[[nodiscard]] core::u32 text_width(
    std::size_t characters,
    core::u32 scale) noexcept {
    if (characters == 0U) {
        return 0U;
    }
    return static_cast<core::u32>(characters) * glyph_advance * scale - scale;
}

void draw_centred(
    const FrameBufferView& target,
    core::i32 origin_y,
    core::u32 scale,
    core::u32 rgba,
    std::string_view text) noexcept {
    const auto width = text_width(text.size(), scale);
    const auto centred = (static_cast<core::i32>(target.width)
                          - static_cast<core::i32>(width))
        / 2;
    draw_text(target, centred, origin_y, scale, rgba, text);
}

} // namespace

bool valid_frame_buffer(const FrameBufferView& target) noexcept {
    if (target.width == 0U || target.height == 0U) {
        return false;
    }
    const auto row_bytes = static_cast<std::size_t>(target.width) * 4U;
    if (static_cast<std::size_t>(target.stride) < row_bytes) {
        return false;
    }
    const auto required = static_cast<std::size_t>(target.stride)
        * static_cast<std::size_t>(target.height);
    return target.pixels.size() >= required;
}

void fill_rect(
    const FrameBufferView& target,
    core::i32 origin_x,
    core::i32 origin_y,
    core::i32 width,
    core::i32 height,
    core::u32 rgba) noexcept {
    if (!valid_frame_buffer(target) || width <= 0 || height <= 0) {
        return;
    }

    const auto surface_width = static_cast<core::i32>(target.width);
    const auto surface_height = static_cast<core::i32>(target.height);
    const auto left = origin_x < 0 ? 0 : origin_x;
    const auto top = origin_y < 0 ? 0 : origin_y;
    const auto right =
        origin_x > surface_width - width ? surface_width : origin_x + width;
    const auto bottom =
        origin_y > surface_height - height ? surface_height : origin_y + height;

    for (auto y = top; y < bottom; ++y) {
        for (auto x = left; x < right; ++x) {
            write_pixel(
                target,
                static_cast<core::u32>(x),
                static_cast<core::u32>(y),
                rgba);
        }
    }
}

void draw_text(
    const FrameBufferView& target,
    core::i32 origin_x,
    core::i32 origin_y,
    core::u32 scale,
    core::u32 rgba,
    std::string_view text) noexcept {
    if (!valid_frame_buffer(target) || scale == 0U) {
        return;
    }

    auto pen = origin_x;
    const auto step = static_cast<core::i32>(glyph_advance * scale);
    for (const auto character : text) {
        const auto& glyph = font[glyph_index(character)];
        for (core::u32 row = 0U; row < glyph_height; ++row) {
            const auto bits = glyph[row];
            for (core::u32 column = 0U; column < glyph_width; ++column) {
                const auto mask =
                    static_cast<core::u8>(1U << (glyph_width - 1U - column));
                if ((bits & mask) == 0U) {
                    continue;
                }
                fill_rect(
                    target,
                    pen + static_cast<core::i32>(column * scale),
                    origin_y + static_cast<core::i32>(row * scale),
                    static_cast<core::i32>(scale),
                    static_cast<core::i32>(scale),
                    rgba);
            }
        }
        pen += step;
    }
}

std::string_view format_unsigned(
    std::span<char> buffer,
    core::u64 value,
    core::u32 min_digits) noexcept {
    std::array<char, 20U> digits{};
    std::size_t count = 0U;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U && count < digits.size());

    while (count < static_cast<std::size_t>(min_digits)
           && count < digits.size()) {
        digits[count++] = '0';
    }
    if (buffer.size() < count) {
        return {};
    }

    for (std::size_t index = 0U; index < count; ++index) {
        buffer[index] = digits[count - 1U - index];
    }
    return std::string_view{buffer.data(), count};
}

std::string_view format_timecode(
    std::span<char> buffer,
    Timecode value) noexcept {
    constexpr std::size_t required = 11U;
    if (buffer.size() < required) {
        return {};
    }

    const std::array<core::u32, 4U> parts{
        value.hours, value.minutes, value.seconds, value.frames};
    std::size_t cursor = 0U;
    for (std::size_t index = 0U; index < parts.size(); ++index) {
        if (index != 0U) {
            buffer[cursor++] = ':';
        }
        const auto written =
            format_unsigned(buffer.subspan(cursor, 2U), parts[index] % 100U, 2U);
        if (written.size() != 2U) {
            return {};
        }
        cursor += 2U;
    }
    return std::string_view{buffer.data(), cursor};
}

bool render_test_pattern(
    const FrameBufferView& target,
    const TestPatternInput& input) noexcept {
    if (!valid_frame_buffer(target)) {
        return false;
    }

    const auto width = static_cast<core::i32>(target.width);
    const auto height = static_cast<core::i32>(target.height);
    const auto node_tint =
        node_palette[static_cast<std::size_t>(input.node_index)
                     % node_palette.size()];

    fill_rect(target, 0, 0, width, height, node_tint);

    // Frame-cycling border. A one-frame divergence between two output windows
    // shows up as a different border colour, with no reading required.
    const auto border = static_cast<core::i32>(
        target.height / 64U == 0U ? 4U : target.height / 64U);
    const auto border_colour =
        frame_palette[static_cast<std::size_t>(
                          input.show_frame.value() % frame_palette.size())];
    fill_rect(target, 0, 0, width, border, border_colour);
    fill_rect(target, 0, height - border, width, border, border_colour);
    fill_rect(target, 0, 0, border, height, border_colour);
    fill_rect(target, width - border, 0, border, height, border_colour);

    // Inner field, so the text sits on a flat surface rather than the tint.
    fill_rect(
        target,
        border,
        border,
        width - 2 * border,
        height - 2 * border,
        colour_field);

    // Sweep bar. A held or dropped frame is visible as a bar that fails to
    // move, which no digit readout conveys as quickly.
    const auto sweep_width =
        static_cast<core::i32>(target.width / 160U == 0U ? 2U : target.width / 160U);
    const auto sweep_x =
        static_cast<core::i32>(input.show_frame.value() % target.width);
    fill_rect(target, sweep_x, border, sweep_width, height - 2 * border, colour_white);

    const auto large = target.height / 64U == 0U ? 1U : target.height / 64U;
    const auto small = target.height / 128U == 0U ? 1U : target.height / 128U;
    const auto line = static_cast<core::i32>(glyph_height * small + 4U * small);

    std::array<char, 32U> text{};
    auto cursor = height / 2 - static_cast<core::i32>(glyph_height * large);

    const auto code = format_timecode(std::span{text}, timecode(input.show_frame, input.rate));
    draw_centred(target, cursor, large, colour_white, code);
    cursor += static_cast<core::i32>(glyph_height * large + 6U * large);

    draw_centred(target, cursor, small, colour_label, "SHOW FRAME");
    cursor += line;
    const auto show = format_unsigned(std::span{text}, input.show_frame.value());
    draw_centred(target, cursor, small, colour_white, show);
    cursor += line + line / 2;

    draw_centred(target, cursor, small, colour_label, "CONTROL FRAME");
    cursor += line;
    const auto control =
        format_unsigned(std::span{text}, input.control_frame.value());
    draw_centred(target, cursor, small, colour_white, control);

    // Node identity, top left, so a wall of outputs is readable at a distance.
    const auto node = format_unsigned(std::span{text}, input.node_index, 2U);
    draw_text(
        target,
        border + static_cast<core::i32>(2U * small),
        border + static_cast<core::i32>(2U * small),
        small,
        colour_label,
        "NODE");
    draw_text(
        target,
        border + static_cast<core::i32>((4U * glyph_advance + 2U) * small),
        border + static_cast<core::i32>(2U * small),
        small,
        colour_white,
        node);

    const auto state_label = input.state == TransportState::Playing ? "PLAY"
        : input.state == TransportState::Paused                     ? "PAUSE"
                                                                    : "STOP";
    draw_text(
        target,
        border + static_cast<core::i32>(2U * small),
        border + static_cast<core::i32>((glyph_height + 4U) * small),
        small,
        colour_white,
        state_label);

    if (input.needs_resync) {
        draw_centred(
            target,
            height - border - static_cast<core::i32>((glyph_height + 4U) * small),
            small,
            colour_resync,
            "RESYNC REQUIRED");
    } else if (input.held) {
        draw_centred(
            target,
            height - border - static_cast<core::i32>((glyph_height + 4U) * small),
            small,
            colour_held,
            "HELD");
    }

    return true;
}

core::u64 frame_buffer_digest(const FrameBufferView& target) noexcept {
    if (!valid_frame_buffer(target)) {
        return 0U;
    }

    constexpr core::u64 offset_basis = 0xcbf29ce484222325ULL;
    constexpr core::u64 prime = 0x100000001b3ULL;

    auto digest = offset_basis;
    const auto row_bytes = static_cast<std::size_t>(target.width) * 4U;
    for (core::u32 y = 0U; y < target.height; ++y) {
        const auto row = static_cast<std::size_t>(y)
            * static_cast<std::size_t>(target.stride);
        for (std::size_t index = 0U; index < row_bytes; ++index) {
            digest ^= static_cast<core::u64>(target.pixels[row + index]);
            digest *= prime;
        }
    }
    return digest;
}

// ---------------------------------------------------------------------------
// Quad description of the same layout.

namespace {

// The glyph cell index the atlas reserves for a fully covered texel. It sits
// before the glyphs so every region stays cell aligned.
constexpr std::size_t solid_cell = 0U;

[[nodiscard]] constexpr float channel(core::u32 rgba, core::u32 shift) noexcept {
    return static_cast<float>((rgba >> shift) & 0xffU) / 255.0F;
}

// Emits into a caller-owned span, counting past the end rather than writing
// past it, so a caller learns the frame did not fit instead of losing memory.
class QuadSink final {
public:
    QuadSink(std::span<PatternQuad> quads, core::u32 width, core::u32 height) noexcept
        : quads_(quads),
          width_(static_cast<float>(width)),
          height_(static_cast<float>(height)) {}

    void rect(
        core::i32 x,
        core::i32 y,
        core::i32 w,
        core::i32 h,
        core::u32 rgba,
        QuadUv uv) noexcept {
        if (w <= 0 || h <= 0) {
            return;
        }
        if (written_ >= quads_.size()) {
            ++written_;
            return;
        }

        auto& quad = quads_[written_];
        // Pixel edges to clip space. A rectangle covering pixels [x, x+w)
        // becomes the clip-space span of those exact edges, which is what
        // makes the GPU rasterisation land on the same pixels the CPU path
        // fills.
        quad.x0 = static_cast<float>(x) / width_ * 2.0F - 1.0F;
        quad.x1 = static_cast<float>(x + w) / width_ * 2.0F - 1.0F;
        quad.y0 = 1.0F - static_cast<float>(y) / height_ * 2.0F;
        quad.y1 = 1.0F - static_cast<float>(y + h) / height_ * 2.0F;
        quad.r = channel(rgba, 24U);
        quad.g = channel(rgba, 16U);
        quad.b = channel(rgba, 8U);
        quad.a = channel(rgba, 0U);
        quad.uv = uv;
        ++written_;
    }

    void solid(core::i32 x, core::i32 y, core::i32 w, core::i32 h, core::u32 rgba) noexcept {
        rect(x, y, w, h, rgba, solid_uv());
    }

    void text(
        core::i32 x,
        core::i32 y,
        core::u32 scale,
        core::u32 rgba,
        std::string_view content) noexcept {
        const auto step = static_cast<core::i32>(glyph_advance * scale);
        auto cursor = x;
        for (const auto character : content) {
            if (character != ' ') {
                rect(
                    cursor,
                    y,
                    static_cast<core::i32>(glyph_width * scale),
                    static_cast<core::i32>(glyph_height * scale),
                    rgba,
                    glyph_uv(character));
            }
            cursor += step;
        }
    }

    void centred(
        core::i32 y,
        core::u32 scale,
        core::u32 rgba,
        std::string_view content) noexcept {
        const auto span = text_width(content.size(), scale);
        const auto x =
            (static_cast<core::i32>(width_) - static_cast<core::i32>(span)) / 2;
        text(x, y, scale, rgba, content);
    }

    [[nodiscard]] PatternQuadReport report() const noexcept {
        return PatternQuadReport{
            std::min(written_, quads_.size()), written_ > quads_.size()};
    }

private:
    std::span<PatternQuad> quads_;
    float width_{};
    float height_{};
    std::size_t written_{};
};

} // namespace

bool write_glyph_atlas(std::span<core::u8> rgba) noexcept {
    if (rgba.size() != static_cast<std::size_t>(atlas_rgba_bytes)) {
        return false;
    }
    std::fill(rgba.begin(), rgba.end(), core::u8{0});

    const auto write = [&rgba](core::u32 x, core::u32 y, core::u8 coverage) noexcept {
        const auto offset =
            (static_cast<std::size_t>(y) * atlas_width + x) * 4U;
        rgba[offset] = coverage;
        rgba[offset + 1U] = coverage;
        rgba[offset + 2U] = coverage;
        rgba[offset + 3U] = coverage;
    };

    // The solid cell is fully covered everywhere, so a solid rectangle may
    // sample anywhere inside it and still read 1.
    for (core::u32 y = 0U; y < glyph_height; ++y) {
        for (core::u32 x = 0U; x < glyph_width; ++x) {
            write(x, y, 0xffU);
        }
    }

    for (std::size_t cell = 0U; cell < glyph_count; ++cell) {
        const auto origin =
            static_cast<core::u32>((cell + 1U) * glyph_width);
        for (core::u32 row = 0U; row < glyph_height; ++row) {
            const auto bits = font[cell][row];
            for (core::u32 column = 0U; column < glyph_width; ++column) {
                const auto mask =
                    static_cast<core::u8>(1U << (glyph_width - 1U - column));
                if ((bits & mask) != 0U) {
                    write(origin + column, row, 0xffU);
                }
            }
        }
    }
    return true;
}

QuadUv solid_uv() noexcept {
    // Half-texel inset so nearest sampling cannot stray into the neighbouring
    // cell no matter how the rectangle is scaled.
    constexpr auto width = static_cast<float>(atlas_width);
    const auto centre = (static_cast<float>(solid_cell * glyph_width) + 2.5F) / width;
    return QuadUv{centre, 0.5F / static_cast<float>(atlas_height), centre,
                  0.5F / static_cast<float>(atlas_height)};
}

QuadUv glyph_uv(char character) noexcept {
    const auto cell = glyph_index(character) + 1U;
    constexpr auto width = static_cast<float>(atlas_width);
    constexpr auto height = static_cast<float>(atlas_height);
    const auto left = static_cast<float>(cell * glyph_width);
    // Sample at texel centres: the first and last texel centres of the cell,
    // so an integer upscale maps each output pixel onto exactly one texel.
    return QuadUv{
        (left + 0.5F) / width,
        0.5F / height,
        (left + static_cast<float>(glyph_width) - 0.5F) / width,
        (height - 0.5F) / height,
    };
}

PatternQuadReport build_pattern_quads(
    const TestPatternInput& input,
    core::u32 width,
    core::u32 height,
    std::span<PatternQuad> quads) noexcept {
    QuadSink sink{quads, width, height};
    if (width == 0U || height == 0U) {
        return sink.report();
    }

    const auto signed_width = static_cast<core::i32>(width);
    const auto signed_height = static_cast<core::i32>(height);

    const auto node_tint =
        node_palette[static_cast<std::size_t>(input.node_index)
                     % node_palette.size()];
    sink.solid(0, 0, signed_width, signed_height, node_tint);

    const auto border =
        static_cast<core::i32>(height / 64U == 0U ? 4U : height / 64U);
    const auto border_colour =
        frame_palette[static_cast<std::size_t>(
                          input.show_frame.value() % frame_palette.size())];
    sink.solid(0, 0, signed_width, border, border_colour);
    sink.solid(0, signed_height - border, signed_width, border, border_colour);
    sink.solid(0, 0, border, signed_height, border_colour);
    sink.solid(signed_width - border, 0, border, signed_height, border_colour);

    sink.solid(
        border,
        border,
        signed_width - 2 * border,
        signed_height - 2 * border,
        colour_field);

    const auto sweep_width =
        static_cast<core::i32>(width / 160U == 0U ? 2U : width / 160U);
    const auto sweep_x = static_cast<core::i32>(input.show_frame.value() % width);
    sink.solid(
        sweep_x, border, sweep_width, signed_height - 2 * border, colour_white);

    const auto large = height / 64U == 0U ? 1U : height / 64U;
    const auto small = height / 128U == 0U ? 1U : height / 128U;
    const auto line = static_cast<core::i32>(glyph_height * small + 4U * small);

    std::array<char, 32U> text{};
    auto cursor = signed_height / 2 - static_cast<core::i32>(glyph_height * large);

    const auto code =
        format_timecode(std::span{text}, timecode(input.show_frame, input.rate));
    sink.centred(cursor, large, colour_white, code);
    cursor += static_cast<core::i32>(glyph_height * large + 6U * large);

    sink.centred(cursor, small, colour_label, "SHOW FRAME");
    cursor += line;
    const auto show = format_unsigned(std::span{text}, input.show_frame.value());
    sink.centred(cursor, small, colour_white, show);
    cursor += line + line / 2;

    sink.centred(cursor, small, colour_label, "CONTROL FRAME");
    cursor += line;
    const auto control =
        format_unsigned(std::span{text}, input.control_frame.value());
    sink.centred(cursor, small, colour_white, control);

    const auto node = format_unsigned(std::span{text}, input.node_index, 2U);
    sink.text(
        border + static_cast<core::i32>(2U * small),
        border + static_cast<core::i32>(2U * small),
        small,
        colour_label,
        "NODE");
    sink.text(
        border + static_cast<core::i32>((4U * glyph_advance + 2U) * small),
        border + static_cast<core::i32>(2U * small),
        small,
        colour_white,
        node);

    const auto state_label = input.state == TransportState::Playing ? "PLAY"
        : input.state == TransportState::Paused                     ? "PAUSE"
                                                                    : "STOP";
    sink.text(
        border + static_cast<core::i32>(2U * small),
        border + static_cast<core::i32>((glyph_height + 4U) * small),
        small,
        colour_white,
        state_label);

    if (input.needs_resync) {
        sink.centred(
            signed_height - border
                - static_cast<core::i32>((glyph_height + 4U) * small),
            small,
            colour_resync,
            "RESYNC REQUIRED");
    } else if (input.held) {
        sink.centred(
            signed_height - border
                - static_cast<core::i32>((glyph_height + 4U) * small),
            small,
            colour_held,
            "HELD");
    }

    return sink.report();
}

} // namespace mediaserver::show
