#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// CRC-32C (Castagnoli, reflected polynomial 0x82f63b78), the check value on
// every persisted record: project files, settings, the release ledger and the
// sealed frame log.
namespace mediaserver::core {

namespace detail {
[[nodiscard]] constexpr std::array<std::uint32_t, 256U> make_crc32c_table() noexcept {
    std::array<std::uint32_t, 256U> table{};
    for (std::size_t index = 0U; index < table.size(); ++index) {
        auto value = static_cast<std::uint32_t>(index);
        for (std::uint32_t bit = 0U; bit < 8U; ++bit) {
            value = (value >> 1U) ^ ((value & 1U) != 0U ? 0x82f63b78U : 0U);
        }
        table[index] = value;
    }
    return table;
}

inline constexpr auto crc32c_table = make_crc32c_table();
} // namespace detail

[[nodiscard]] inline std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const auto byte : bytes) {
        const auto lookup = static_cast<std::uint8_t>(
            (crc ^ std::to_integer<std::uint8_t>(byte)) & 0xffU);
        crc = detail::crc32c_table[lookup] ^ (crc >> 8U);
    }
    return ~crc;
}

} // namespace mediaserver::core
