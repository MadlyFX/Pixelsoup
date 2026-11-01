#pragma once

#include <ajfx/mesh.hpp>
#include <ajfx/texture.hpp>

#include <cstdint>
#include <string_view>

// Studio's built-in pictures and chunks. They are generated, not loaded, so a
// package always carries the same texels whoever exported it. A texture that
// animates is a flipbook the runtime plays across each particle's life.
namespace ajfx::studio {

struct LibraryTexture final {
    std::string_view id;        // as the Appearance block names it
    std::string_view entry;     // the package's texture name; empty draws the runtime's soft disc
    std::uint32_t columns{1U};
    std::uint32_t rows{1U};
};

// The texture a choice id names, or nothing for an unknown id.
[[nodiscard]] const LibraryTexture* library_texture(std::string_view id);
// The generated texels for a package entry name. Built once, then shared.
[[nodiscard]] const TextureData* library_texture_data(std::string_view entry);

// The generated mesh for a chunk name ("chip", "shard", "rock", "cube").
[[nodiscard]] const MeshData* library_mesh(std::string_view name);

} // namespace ajfx::studio
