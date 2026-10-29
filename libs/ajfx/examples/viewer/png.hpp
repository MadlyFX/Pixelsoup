#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <string>
#include <vector>

// A minimal PNG writer for RGBA8 captures: uncompressed deflate blocks, so
// no zlib is needed. Files are larger than they could be, and that is fine
// for a sample's screenshots.
namespace viewer {

inline std::uint32_t crc32(std::span<const std::uint8_t> bytes, std::uint32_t crc = 0xffffffffU) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t n = 0U; n < 256U; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1U) != 0U ? 0xedb88320U ^ (c >> 1U) : c >> 1U;
            }
            t[n] = c;
        }
        return t;
    }();
    for (const std::uint8_t byte : bytes) {
        crc = table[(crc ^ byte) & 0xffU] ^ (crc >> 8U);
    }
    return crc;
}

inline void put_be32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

inline void put_chunk(std::vector<std::uint8_t>& out, const char (&type)[5], const std::vector<std::uint8_t>& data) {
    put_be32(out, static_cast<std::uint32_t>(data.size()));
    std::vector<std::uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    out.insert(out.end(), body.begin(), body.end());
    put_be32(out, crc32(body) ^ 0xffffffffU);
}

inline bool write_png(const std::string& path, std::uint32_t width, std::uint32_t height,
                      std::span<const std::uint8_t> rgba) {
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(width * 4U + 1U) * height);
    for (std::uint32_t y = 0U; y < height; ++y) {
        raw.push_back(0U); // no filter
        const auto row = rgba.subspan(static_cast<std::size_t>(y) * width * 4U, static_cast<std::size_t>(width) * 4U);
        raw.insert(raw.end(), row.begin(), row.end());
    }

    std::vector<std::uint8_t> zlib{0x78U, 0x01U};
    for (std::size_t at = 0U; at < raw.size() || at == 0U;) {
        const std::size_t length = std::min<std::size_t>(65535U, raw.size() - at);
        const bool last = at + length == raw.size();
        zlib.push_back(last ? 1U : 0U);
        zlib.push_back(static_cast<std::uint8_t>(length));
        zlib.push_back(static_cast<std::uint8_t>(length >> 8U));
        zlib.push_back(static_cast<std::uint8_t>(~length));
        zlib.push_back(static_cast<std::uint8_t>(~length >> 8U));
        zlib.insert(zlib.end(), raw.begin() + static_cast<std::ptrdiff_t>(at),
                    raw.begin() + static_cast<std::ptrdiff_t>(at + length));
        at += length;
        if (last) {
            break;
        }
    }
    std::uint32_t a = 1U;
    std::uint32_t b = 0U;
    for (const std::uint8_t byte : raw) {
        a = (a + byte) % 65521U;
        b = (b + a) % 65521U;
    }
    put_be32(zlib, (b << 16U) | a);

    std::vector<std::uint8_t> header;
    put_be32(header, width);
    put_be32(header, height);
    header.insert(header.end(), {8U, 6U, 0U, 0U, 0U}); // 8-bit RGBA

    std::vector<std::uint8_t> file{0x89U, 'P', 'N', 'G', '\r', '\n', 0x1aU, '\n'};
    put_chunk(file, "IHDR", header);
    put_chunk(file, "IDAT", zlib);
    put_chunk(file, "IEND", {});
    std::ofstream stream{path, std::ios::binary};
    stream.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    return static_cast<bool>(stream);
}

} // namespace viewer
