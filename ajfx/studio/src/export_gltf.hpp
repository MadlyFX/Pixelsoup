#pragma once

#include <ajfx/cache.hpp>
#include <ajfx/effect.hpp>
#include <ajfx/mesh.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// A performance delivered to DCC tools as glTF 2.0, written from its cache
// rather than from a second bake: what the file shows is what the cache
// holds, and so what the wall plays.
//
// - Each body of a cached solid world is a node with an animation: its
//   translation and rotation every simulation step it stands (LINEAR), and a
//   STEP scale that shows it from the step it is born to the step it leaves.
//   Before and after, glTF holds the first and last pose.
// - A body that breaks is a group: the whole body, then the pieces it came
//   apart into, each animated in the same space. A piece is the one born on
//   the step a body left, nearest it.
// - A prop is a node of its own, animated only when it moves.
// - Each body is drawn with its kind's model (the kind's draw mesh, else the
//   world's own), scaled by its size and the kind's scale, tinted with its
//   colour; one with no model is a unit cube.
// - Units are the effect's (metres), Y up, as glTF has them.
//
// Particle systems are not written: a glTF has no particles. Alembic or USD
// would carry them; neither is written yet.
namespace ajfx::studio {

struct GltfOptions final {
    // Steps written: 0 to frames - 1. 0 for every step the cache holds.
    std::uint64_t frames{0U};
    // The name a body's node takes, by its world's place among the effect's
    // emitters and its spawn index; empty for the default
    // ("<emitter> body <spawn index>").
    std::function<std::string(std::size_t, std::uint32_t)> name;
    // The scene's name, for the glTF scene.
    std::string scene{"ajfx performance"};
};

struct GltfFile final {
    // The .gltf JSON, naming `bin_name` as its one buffer.
    std::string json;
    std::vector<std::uint8_t> bin;
    std::string bin_name;
    // What it holds, for the person who asked.
    std::size_t bodies{0U};
    std::size_t pieces{0U};
    std::size_t props{0U};
    std::uint64_t frames{0U};
    std::string error;
};

// The glTF of every cached solid world of `cache` (opened for `effect`),
// drawn with `meshes` (the package's). Fails, in `error`, when the cache
// holds no solid world or no step.
[[nodiscard]] GltfFile gltf_from_cache(const Effect& effect, const Cache& cache, const MeshSet& meshes,
                                       const GltfOptions& options = {}, std::string bin_name = "performance.bin");

// Writes `file` at `path` (a .gltf) and its buffer beside it.
bool write_gltf(const GltfFile& file, const std::filesystem::path& path, std::string& error);

} // namespace ajfx::studio
