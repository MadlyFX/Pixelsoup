#include <mediaserver/scene/geometry.hpp>

#include <algorithm>
#include <array>

namespace mediaserver::scene {
namespace {

constexpr float half_extent = 0.5F;

void push_quad(
    MeshData& into,
    core::u32 a,
    core::u32 b,
    core::u32 c,
    core::u32 d) {
    into.indices.push_back(a);
    into.indices.push_back(b);
    into.indices.push_back(c);
    into.indices.push_back(a);
    into.indices.push_back(c);
    into.indices.push_back(d);
}

// A grid of (segments + 1) squared vertices spanning the unit square, mapped
// into the plane described by `origin`, `across`, and `down`. Used for the
// plane and for each face of the box, so the two cannot tessellate
// differently.
void build_grid(
    MeshData& into,
    core::u32 segments,
    Vec3 origin,
    Vec3 across,
    Vec3 down,
    Vec3 normal) {
    const auto first = static_cast<core::u32>(into.vertices.size());
    const auto steps = static_cast<float>(segments);
    const auto stride = segments + 1U;

    for (core::u32 row = 0U; row <= segments; ++row) {
        const auto v = static_cast<float>(row) / steps;
        for (core::u32 column = 0U; column <= segments; ++column) {
            const auto u = static_cast<float>(column) / steps;
            const auto position = origin + across * u + down * v;
            into.vertices.push_back(MeshVertex{position, normal, u, v});
        }
    }

    for (core::u32 row = 0U; row < segments; ++row) {
        for (core::u32 column = 0U; column < segments; ++column) {
            const auto top_left = first + row * stride + column;
            const auto top_right = top_left + 1U;
            const auto bottom_left = top_left + stride;
            const auto bottom_right = bottom_left + 1U;
            push_quad(into, top_left, bottom_left, bottom_right, top_right);
        }
    }
}

void build_plane(MeshData& into, core::u32 segments) {
    build_grid(
        into,
        segments,
        Vec3{-half_extent, 0.0F, -half_extent},
        Vec3{1.0F, 0.0F, 0.0F},
        Vec3{0.0F, 0.0F, 1.0F},
        Vec3{0.0F, 1.0F, 0.0F});
}

void build_box(MeshData& into, core::u32 segments) {
    // Six independent grids rather than eight shared corners: a cube needs
    // three different normals and three different texture coordinates at every
    // corner, so sharing vertices would mean sharing the wrong ones.
    struct Face final {
        Vec3 origin;
        Vec3 across;
        Vec3 down;
        Vec3 normal;
    };

    constexpr float e = half_extent;
    const std::array<Face, 6U> faces{{
        // +X
        {Vec3{e, e, -e}, Vec3{0.0F, 0.0F, 2.0F * e}, Vec3{0.0F, -2.0F * e, 0.0F},
         Vec3{1.0F, 0.0F, 0.0F}},
        // -X
        {Vec3{-e, e, e}, Vec3{0.0F, 0.0F, -2.0F * e}, Vec3{0.0F, -2.0F * e, 0.0F},
         Vec3{-1.0F, 0.0F, 0.0F}},
        // +Y
        {Vec3{-e, e, -e}, Vec3{2.0F * e, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, 2.0F * e},
         Vec3{0.0F, 1.0F, 0.0F}},
        // -Y
        {Vec3{-e, -e, e}, Vec3{2.0F * e, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, -2.0F * e},
         Vec3{0.0F, -1.0F, 0.0F}},
        // +Z
        {Vec3{-e, e, e}, Vec3{2.0F * e, 0.0F, 0.0F}, Vec3{0.0F, -2.0F * e, 0.0F},
         Vec3{0.0F, 0.0F, 1.0F}},
        // -Z
        {Vec3{e, e, -e}, Vec3{-2.0F * e, 0.0F, 0.0F}, Vec3{0.0F, -2.0F * e, 0.0F},
         Vec3{0.0F, 0.0F, -1.0F}},
    }};

    for (const auto& face : faces) {
        build_grid(
            into, segments, face.origin, face.across, face.down, face.normal);
    }
}

void build_sphere(MeshData& into, core::u32 segments) {
    // Rings run pole to pole; sectors run around. Twice as many sectors as
    // rings keeps the quads roughly square at the equator.
    const auto rings = std::max(segments, 2U);
    const auto sectors = std::max(segments * 2U, 3U);
    const auto ring_steps = static_cast<float>(rings);
    const auto sector_steps = static_cast<float>(sectors);
    const auto stride = sectors + 1U;

    for (core::u32 ring = 0U; ring <= rings; ++ring) {
        const auto v = static_cast<float>(ring) / ring_steps;
        // Half a turn from the north pole to the south.
        const auto polar = v * 0.5F;
        const auto y = deterministic::cos_turns(polar);
        const auto ring_radius = deterministic::sin_turns(polar);

        for (core::u32 sector = 0U; sector <= sectors; ++sector) {
            const auto u = static_cast<float>(sector) / sector_steps;
            const auto x = ring_radius * deterministic::cos_turns(u);
            const auto z = ring_radius * deterministic::sin_turns(u);
            // The unit-radius point is the normal; halving it gives the
            // position on a sphere of diameter one.
            const Vec3 normal{x, y, z};
            into.vertices.push_back(
                MeshVertex{normal * half_extent, normal, u, v});
        }
    }

    for (core::u32 ring = 0U; ring < rings; ++ring) {
        for (core::u32 sector = 0U; sector < sectors; ++sector) {
            const auto top_left = ring * stride + sector;
            const auto top_right = top_left + 1U;
            const auto bottom_left = top_left + stride;
            const auto bottom_right = bottom_left + 1U;
            push_quad(into, top_left, bottom_left, bottom_right, top_right);
        }
    }
}

void build_cylinder(MeshData& into, core::u32 segments) {
    const auto sectors = std::max(segments, 3U);
    const auto sector_steps = static_cast<float>(sectors);

    // Side wall. Duplicated seam vertex at u = 1 so the texture coordinate can
    // wrap without the last quad running backwards across the whole map.
    const auto side_first = static_cast<core::u32>(into.vertices.size());
    for (core::u32 sector = 0U; sector <= sectors; ++sector) {
        const auto u = static_cast<float>(sector) / sector_steps;
        const auto x = deterministic::cos_turns(u);
        const auto z = deterministic::sin_turns(u);
        const Vec3 normal{x, 0.0F, z};
        into.vertices.push_back(MeshVertex{
            Vec3{x * half_extent, half_extent, z * half_extent}, normal, u, 0.0F});
        into.vertices.push_back(MeshVertex{
            Vec3{x * half_extent, -half_extent, z * half_extent},
            normal,
            u,
            1.0F});
    }
    for (core::u32 sector = 0U; sector < sectors; ++sector) {
        const auto top = side_first + sector * 2U;
        const auto bottom = top + 1U;
        const auto next_top = top + 2U;
        const auto next_bottom = top + 3U;
        push_quad(into, top, bottom, next_bottom, next_top);
    }

    // Caps. A centre vertex and a fan, with the cap normal on every vertex so
    // the rim stays a hard edge rather than smearing into the wall.
    const auto build_cap = [&](float y, Vec3 normal, bool reverse) {
        const auto centre = static_cast<core::u32>(into.vertices.size());
        into.vertices.push_back(
            MeshVertex{Vec3{0.0F, y, 0.0F}, normal, 0.5F, 0.5F});
        for (core::u32 sector = 0U; sector <= sectors; ++sector) {
            const auto u = static_cast<float>(sector) / sector_steps;
            const auto x = deterministic::cos_turns(u);
            const auto z = deterministic::sin_turns(u);
            into.vertices.push_back(MeshVertex{
                Vec3{x * half_extent, y, z * half_extent},
                normal,
                x * half_extent + 0.5F,
                z * half_extent + 0.5F});
        }
        for (core::u32 sector = 0U; sector < sectors; ++sector) {
            const auto rim = centre + 1U + sector;
            into.indices.push_back(centre);
            into.indices.push_back(reverse ? rim + 1U : rim);
            into.indices.push_back(reverse ? rim : rim + 1U);
        }
    };
    build_cap(half_extent, Vec3{0.0F, 1.0F, 0.0F}, false);
    build_cap(-half_extent, Vec3{0.0F, -1.0F, 0.0F}, true);
}

} // namespace

MeshKey normalized(MeshKey key) noexcept {
    key.segments =
        std::clamp(key.segments, min_mesh_segments, max_mesh_segments);
    return key;
}

bool build_mesh(MeshKey key, MeshData& into) {
    const auto normal_key = normalized(key);
    into.vertices.clear();
    into.indices.clear();

    switch (normal_key.primitive) {
    case Primitive::Plane:
        build_plane(into, normal_key.segments);
        return true;
    case Primitive::Box:
        build_box(into, normal_key.segments);
        return true;
    case Primitive::Sphere:
        build_sphere(into, normal_key.segments);
        return true;
    case Primitive::Cylinder:
        build_cylinder(into, normal_key.segments);
        return true;
    }
    return false;
}

std::string_view primitive_name(Primitive primitive) noexcept {
    switch (primitive) {
    case Primitive::Plane:
        return "plane";
    case Primitive::Box:
        break;
    case Primitive::Sphere:
        return "sphere";
    case Primitive::Cylinder:
        return "cylinder";
    }
    return "box";
}

bool parse_primitive(std::string_view name, Primitive& into) noexcept {
    if (name == "plane") {
        into = Primitive::Plane;
        return true;
    }
    if (name == "box") {
        into = Primitive::Box;
        return true;
    }
    if (name == "sphere") {
        into = Primitive::Sphere;
        return true;
    }
    if (name == "cylinder") {
        into = Primitive::Cylinder;
        return true;
    }
    return false;
}

} // namespace mediaserver::scene
