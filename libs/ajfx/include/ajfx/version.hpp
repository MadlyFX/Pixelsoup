#pragma once

#include <cstdint>

namespace ajfx {

struct Version final {
    std::uint32_t major{};
    std::uint32_t minor{};
    std::uint32_t patch{};

    friend constexpr bool operator==(const Version&, const Version&) = default;
};

// The version of the headers being compiled against.
inline constexpr Version header_version{0U, 1U, 0U};

// The version of the library that was linked. A mismatch with header_version
// means a stale build of the library is on the link line.
[[nodiscard]] Version library_version() noexcept;

} // namespace ajfx
