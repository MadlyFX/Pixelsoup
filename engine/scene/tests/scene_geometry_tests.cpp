#include <mediaserver/scene/geometry.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

namespace core = mediaserver::core;
namespace scene = mediaserver::scene;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define GEOMETRY_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

constexpr std::array<scene::Primitive, 4U> all_primitives{{
    scene::Primitive::Plane,
    scene::Primitive::Box,
    scene::Primitive::Sphere,
    scene::Primitive::Cylinder,
}};

void every_primitive_produces_a_usable_mesh() {
    scene::MeshData mesh;
    for (const auto primitive : all_primitives) {
        for (const core::u32 segments : {1U, 2U, 7U, 32U}) {
            GEOMETRY_CHECK(scene::build_mesh(scene::MeshKey{primitive, segments}, mesh));
            GEOMETRY_CHECK(!mesh.vertices.empty());
            GEOMETRY_CHECK(!mesh.indices.empty());
            // Triangles, so the index count is a multiple of three.
            GEOMETRY_CHECK(mesh.indices.size() % 3U == 0U);

            for (const auto index : mesh.indices) {
                GEOMETRY_CHECK(index < mesh.vertices.size());
            }

            for (const auto& vertex : mesh.vertices) {
                // Every primitive is built at unit size, so nothing may leave
                // the unit box. Size is the instance transform's job.
                GEOMETRY_CHECK(std::abs(vertex.position.x) <= 0.5F + 1.0e-5F);
                GEOMETRY_CHECK(std::abs(vertex.position.y) <= 0.5F + 1.0e-5F);
                GEOMETRY_CHECK(std::abs(vertex.position.z) <= 0.5F + 1.0e-5F);
                // A normal that is not unit length would make the shading
                // depend on the tessellation.
                GEOMETRY_CHECK(
                    std::abs(scene::length(vertex.normal) - 1.0F) <= 1.0e-5F);
                GEOMETRY_CHECK(vertex.u >= -1.0e-5F && vertex.u <= 1.0F + 1.0e-5F);
                GEOMETRY_CHECK(vertex.v >= -1.0e-5F && vertex.v <= 1.0F + 1.0e-5F);
            }
        }
    }
}

void a_sphere_is_actually_round() {
    scene::MeshData mesh;
    GEOMETRY_CHECK(
        scene::build_mesh(scene::MeshKey{scene::Primitive::Sphere, 24U}, mesh));
    for (const auto& vertex : mesh.vertices) {
        GEOMETRY_CHECK(std::abs(scene::length(vertex.position) - 0.5F) <= 1.0e-5F);
        // On a sphere centred at the origin the outward normal is the
        // direction of the position, which is worth checking because it is the
        // assumption the generator is built on.
        const auto radial = scene::normalize(vertex.position);
        GEOMETRY_CHECK(scene::dot(radial, vertex.normal) >= 1.0F - 1.0e-4F);
    }
}

void a_box_has_six_flat_faces() {
    scene::MeshData mesh;
    GEOMETRY_CHECK(
        scene::build_mesh(scene::MeshKey{scene::Primitive::Box, 1U}, mesh));
    // Six faces, four vertices each at the lowest tessellation, and two
    // triangles per face. Corners are not shared, because the three faces
    // meeting at one need three different normals.
    GEOMETRY_CHECK(mesh.vertices.size() == 24U);
    GEOMETRY_CHECK(mesh.indices.size() == 36U);

    core::u32 axis_aligned = 0U;
    for (const auto& vertex : mesh.vertices) {
        const auto sum = std::abs(vertex.normal.x) + std::abs(vertex.normal.y)
            + std::abs(vertex.normal.z);
        if (std::abs(sum - 1.0F) <= 1.0e-5F) {
            ++axis_aligned;
        }
    }
    GEOMETRY_CHECK(axis_aligned == 24U);
}

void generation_repeats_itself_byte_for_byte() {
    // The whole content-distribution argument for procedural primitives rests
    // on this: a node generates rather than receives, so two nodes must
    // generate the same bytes.
    for (const auto primitive : all_primitives) {
        scene::MeshData first;
        scene::MeshData second;
        const scene::MeshKey key{primitive, 13U};
        GEOMETRY_CHECK(scene::build_mesh(key, first));
        GEOMETRY_CHECK(scene::build_mesh(key, second));
        GEOMETRY_CHECK(first.vertices == second.vertices);
        GEOMETRY_CHECK(first.indices == second.indices);
    }

    // And building into a reused buffer leaves no trace of what was there.
    scene::MeshData reused;
    GEOMETRY_CHECK(
        scene::build_mesh(scene::MeshKey{scene::Primitive::Sphere, 20U}, reused));
    const auto sphere_vertices = reused.vertices.size();
    GEOMETRY_CHECK(
        scene::build_mesh(scene::MeshKey{scene::Primitive::Box, 1U}, reused));
    GEOMETRY_CHECK(reused.vertices.size() == 24U);
    GEOMETRY_CHECK(sphere_vertices != 24U);
}

void a_key_is_normalised_before_it_reaches_a_cache() {
    // Two documents asking for tessellation 0 and tessellation 1 want the same
    // mesh and should share one cache entry rather than generating twice.
    GEOMETRY_CHECK(
        scene::normalized(scene::MeshKey{scene::Primitive::Box, 0U})
        == (scene::MeshKey{scene::Primitive::Box, scene::min_mesh_segments}));
    GEOMETRY_CHECK(
        scene::normalized(scene::MeshKey{scene::Primitive::Box, 100'000U})
        == (scene::MeshKey{scene::Primitive::Box, scene::max_mesh_segments}));
    // An in-range key is left alone.
    const scene::MeshKey ordinary{scene::Primitive::Sphere, 16U};
    GEOMETRY_CHECK(scene::normalized(ordinary) == ordinary);
}

void every_primitive_has_a_name_that_round_trips() {
    for (const auto primitive : all_primitives) {
        scene::Primitive parsed{};
        GEOMETRY_CHECK(scene::parse_primitive(scene::primitive_name(primitive), parsed));
        GEOMETRY_CHECK(parsed == primitive);
    }
    scene::Primitive ignored{};
    GEOMETRY_CHECK(!scene::parse_primitive("teapot", ignored));
    GEOMETRY_CHECK(!scene::parse_primitive("", ignored));
    GEOMETRY_CHECK(!scene::parse_primitive("Box", ignored));
}

// See the equivalent constant in scene_math_tests.cpp: this pins the generated
// geometry itself, so a toolchain that produced different vertices would be
// caught here rather than on a wall.
constexpr core::u64 golden_geometry_digest = 0xebccc6981a1ecba0ULL;

void the_generated_geometry_matches_its_recorded_digest() {
    auto state = 14'695'981'039'346'656'037ULL;
    const auto mix = [&state](core::u32 bits) noexcept {
        for (core::u32 shift = 0U; shift < 32U; shift += 8U) {
            state ^= (bits >> shift) & 0xffU;
            state *= 1'099'511'628'211ULL;
        }
    };
    const auto mix_float = [&mix](float value) noexcept {
        core::u32 bits{};
        std::memcpy(&bits, &value, sizeof(bits));
        mix(bits == 0x8000'0000U ? 0U : bits);
    };

    scene::MeshData mesh;
    for (const auto primitive : all_primitives) {
        for (const core::u32 segments : {1U, 3U, 16U}) {
            check(
                scene::build_mesh(scene::MeshKey{primitive, segments}, mesh),
                "build_mesh",
                __LINE__);
            mix(static_cast<core::u32>(mesh.vertices.size()));
            mix(static_cast<core::u32>(mesh.indices.size()));
            for (const auto& vertex : mesh.vertices) {
                mix_float(vertex.position.x);
                mix_float(vertex.position.y);
                mix_float(vertex.position.z);
                mix_float(vertex.normal.x);
                mix_float(vertex.normal.y);
                mix_float(vertex.normal.z);
                mix_float(vertex.u);
                mix_float(vertex.v);
            }
            for (const auto index : mesh.indices) {
                mix(index);
            }
        }
    }

    if (state != golden_geometry_digest) {
        std::cout << "  computed 0x" << std::hex << state << std::dec << '\n';
    }
    GEOMETRY_CHECK(state == golden_geometry_digest);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 7U> cases{{
    {"every primitive produces a usable mesh",
     &every_primitive_produces_a_usable_mesh},
    {"a sphere is actually round", &a_sphere_is_actually_round},
    {"a box has six flat faces", &a_box_has_six_flat_faces},
    {"generation repeats itself byte for byte",
     &generation_repeats_itself_byte_for_byte},
    {"a key is normalised before it reaches a cache",
     &a_key_is_normalised_before_it_reaches_a_cache},
    {"every primitive has a name that round trips",
     &every_primitive_has_a_name_that_round_trips},
    {"the generated geometry matches its recorded digest",
     &the_generated_geometry_matches_its_recorded_digest},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const auto& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
