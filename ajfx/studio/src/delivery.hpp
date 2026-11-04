#pragma once

#include "bake.hpp"
#include "compile.hpp"
#include "document.hpp"

#include <ajfx/cache.hpp>
#include <ajfx/gpu/kernels.hpp>
#include <ajfx/result.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// Delivering a performed scene: the package and its cache written side by
// side (`<name>.ajfx`, `<name>.ajfxcache`), checked as the host that plays
// them will check them, before anything is written.
//
// The cache carries the instance seed it was baked with and the performance
// (cache.hpp), so a host places the effect anywhere and plays what was
// performed. A scene whose baked layers have no current cache is not
// delivered: a host would simulate them live, from no performance, and play
// something else.
namespace ajfx::studio {

struct Delivery final {
    std::vector<std::uint8_t> package;
    // The cache's bytes, when the scene bakes anything; empty otherwise.
    std::vector<std::uint8_t> cache;
    // The cache as the host opens it: from the package's own bytes, as
    // baked (Cache::open_as_baked).
    std::shared_ptr<const Cache> played;
    // The effect, meshes and fields read back from the package, as the
    // host reads them.
    Effect effect;
    MeshSet meshes;
    FieldSet fields;
};

// The package of `compiled` (the scene compiled for export, its files read
// and its cuts made) with `kernels`, and `bake` beside it. Fails, saying
// why in the artist's words, when the scene bakes a layer and `bake` is
// missing or holds another scene or performance, or when the host's own
// check of the written files would refuse them.
[[nodiscard]] Result<Delivery> deliver(const Document& document, const Compiled& compiled,
                                       const gpu::KernelSet& kernels, const BakeRecord* bake);

// Writes the package at `path` and its cache beside it (removing a stale
// one left there, so a host never pairs this package with an older cache).
bool write_delivery(const Delivery& delivery, const std::filesystem::path& path, std::string& error);

// Where a package's cache goes: beside it, `.ajfxcache` for its extension.
[[nodiscard]] std::filesystem::path cache_path_for(const std::filesystem::path& package);

} // namespace ajfx::studio
