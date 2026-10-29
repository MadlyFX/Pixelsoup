#pragma once

#include <ajfx/effect.hpp>
#include <ajfx/result.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The .ajfx effect package.
//
// A package is a small container of named entries. `effect.json` describes
// the effect; later versions add textures, meshes and precompiled kernels as
// further entries. Layout, all integers little-endian:
//
//     "AJFX"                 4 bytes
//     container version      u32
//     entry count            u32
//     per entry, sorted by name:
//         name length        u32
//         name               UTF-8
//         data length        u64
//         data
//     FNV-1a 64 of every byte above   u64
//
// Writing is deterministic: the same effect always produces the same bytes,
// so a package's content hash identifies the effect.
namespace ajfx {

inline constexpr std::uint32_t package_container_version = 1U;
// Version of the effect.json schema this library reads and writes.
inline constexpr std::uint32_t effect_format_version = 1U;

struct PackageEntry final {
    std::string name;
    std::vector<std::uint8_t> data;
    friend bool operator==(const PackageEntry&, const PackageEntry&) = default;
};

// The effect as effect.json text.
[[nodiscard]] std::string effect_to_json(const Effect& effect);
// Parses and validates effect.json text.
[[nodiscard]] Result<Effect> effect_from_json(std::string_view json);

// A complete package for the effect. Fails if the effect does not validate.
[[nodiscard]] Result<std::vector<std::uint8_t>> write_package(const Effect& effect);
// Reads, checks and validates a package.
[[nodiscard]] Result<Effect> read_package(std::span<const std::uint8_t> bytes);

// The container layer on its own, for tools that add or inspect entries.
[[nodiscard]] std::vector<std::uint8_t> write_container(std::vector<PackageEntry> entries);
[[nodiscard]] Result<std::vector<PackageEntry>> read_container(std::span<const std::uint8_t> bytes);

} // namespace ajfx
