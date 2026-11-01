#include "library.hpp"

#include <ajfx/random.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace ajfx::studio {
namespace {

constexpr float pi = 3.14159265358979F;

float smoothstep(float edge0, float edge1, float x) {
    // Equal edges are a step, not a division by zero (which would fill a
    // vanished flame's rows solid).
    if (edge0 == edge1) {
        return x < edge0 ? 0.0F : 1.0F;
    }
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

// Smooth value noise in [0, 1], a lattice point per unit.
float value_noise(float x, float y, std::uint32_t seed) {
    const float ix = std::floor(x);
    const float iy = std::floor(y);
    const auto corner = [&](float cx, float cy) {
        return hash_unit(hash_mix(hash_mix(seed, static_cast<std::uint32_t>(static_cast<std::int32_t>(cx) + 4096)),
                                  static_cast<std::uint32_t>(static_cast<std::int32_t>(cy) + 4096)));
    };
    const auto smooth = [](float t) { return t * t * (3.0F - 2.0F * t); };
    const float tx = smooth(x - ix);
    const float ty = smooth(y - iy);
    const float top = corner(ix, iy) + (corner(ix + 1.0F, iy) - corner(ix, iy)) * tx;
    const float bottom = corner(ix, iy + 1.0F) + (corner(ix + 1.0F, iy + 1.0F) - corner(ix, iy + 1.0F)) * tx;
    return top + (bottom - top) * ty;
}

// Four octaves, in [0, 1].
float fbm(float x, float y, std::uint32_t seed) {
    float sum = 0.0F;
    float amplitude = 0.5F;
    float total = 0.0F;
    for (std::uint32_t octave = 0U; octave < 4U; ++octave) {
        sum += amplitude * value_noise(x, y, seed + octave * 101U);
        total += amplitude;
        x *= 2.03F;
        y *= 2.03F;
        amplitude *= 0.5F;
    }
    return sum / total;
}

struct Pixel final {
    float shade{1.0F};
    float alpha{0.0F};
};

// A texture of `columns` x `rows` cells of `cell` texels, each drawn by
// `draw(u, v, frame, frames)` with u and v in [-1, 1] (v down).
template <class Draw>
TextureData generate(std::uint32_t cell, std::uint32_t columns, std::uint32_t rows, Draw&& draw) {
    const std::uint32_t width = cell * columns;
    const std::uint32_t height = cell * rows;
    const std::uint32_t frames = columns * rows;
    std::vector<std::uint8_t> pixels(std::size_t{width} * height * 4U);
    for (std::uint32_t frame = 0U; frame < frames; ++frame) {
        const std::uint32_t ox = (frame % columns) * cell;
        const std::uint32_t oy = (frame / columns) * cell;
        for (std::uint32_t y = 0U; y < cell; ++y) {
            for (std::uint32_t x = 0U; x < cell; ++x) {
                const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(cell) * 2.0F - 1.0F;
                const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(cell) * 2.0F - 1.0F;
                const Pixel p = draw(u, v, frame, frames);
                std::uint8_t* texel = pixels.data() + (std::size_t{oy + y} * width + ox + x) * 4U;
                const auto byte = [](float value) {
                    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
                };
                texel[0] = texel[1] = texel[2] = byte(p.shade);
                texel[3] = byte(p.alpha);
            }
        }
    }
    Result<TextureData> texture = make_texture(width, height, pixels);
    return texture ? std::move(texture).value() : TextureData{};
}

// A hot centre in a long soft falloff: glowing embers, magic, lens-like light.
TextureData glow() {
    return generate(64U, 1U, 1U, [](float u, float v, std::uint32_t, std::uint32_t) {
        const float r = std::sqrt(u * u + v * v);
        const float halo = std::pow(std::clamp(1.0F - r, 0.0F, 1.0F), 2.6F);
        const float core = std::exp(-r * r * 60.0F);
        return Pixel{1.0F, std::clamp(halo * 0.75F + core * 0.5F, 0.0F, 1.0F)};
    });
}

// A bright line along v that tapers at both ends: a streak when stretched.
TextureData spark() {
    return generate(64U, 1U, 1U, [](float u, float v, std::uint32_t, std::uint32_t) {
        const float across = std::exp(-(u * u) / (2.0F * 0.16F * 0.16F));
        const float along = std::pow(std::clamp(1.0F - v * v, 0.0F, 1.0F), 1.5F);
        const float core = std::exp(-(u * u) / (2.0F * 0.05F * 0.05F)) * along;
        return Pixel{1.0F, std::clamp(across * along * 0.8F + core * 0.4F, 0.0F, 1.0F)};
    });
}

// Sixteen frames of a flame tongue: whole and licking at first, lifting,
// thinning and breaking into wisps as it burns out.
TextureData flame() {
    return generate(64U, 4U, 4U, [](float u, float v, std::uint32_t frame, std::uint32_t frames) {
        const float t = static_cast<float>(frame) / static_cast<float>(frames - 1U);
        const float h = (1.0F - v) * 0.5F - 0.12F * t; // 0 at the bottom, 1 at the top, rising as it ages
        const float wobble = (fbm(u * 1.6F + 3.0F, h * 2.4F - t * 2.5F, 17U) - 0.5F) * 0.9F * std::max(h, 0.0F);
        const float x = std::fabs(u + wobble);
        // Wide low down, narrowing to a point.
        const float width = 0.62F * std::pow(std::clamp(1.0F - h, 0.0F, 1.0F), 0.9F) *
                            smoothstep(-0.05F, 0.18F, h) * (1.0F - 0.35F * t);
        const float body = width > 1.0e-4F ? smoothstep(width, width * 0.25F, x) : 0.0F;
        const float breakup = fbm(u * 3.0F, h * 4.0F - t * 3.0F, 41U);
        const float erode = smoothstep(0.18F + 0.55F * t, 0.42F + 0.55F * t, breakup + 0.25F * (1.0F - h));
        const float top_fade = 1.0F - smoothstep(0.62F - 0.25F * t, 1.0F, h);
        const float density = body * erode * top_fade * (1.0F - 0.55F * t);
        const float edge = std::clamp(1.0F - std::max(std::fabs(u), std::fabs(v)), 0.0F, 1.0F);
        return Pixel{0.62F + 0.38F * body, std::clamp(density * 1.25F, 0.0F, 1.0F) * smoothstep(0.0F, 0.08F, edge)};
    });
}

// Sixteen frames of a billowing puff, lit from above, spreading and thinning.
TextureData smoke() {
    return generate(64U, 4U, 4U, [](float u, float v, std::uint32_t frame, std::uint32_t frames) {
        const float t = static_cast<float>(frame) / static_cast<float>(frames - 1U);
        const float r = std::sqrt(u * u + v * v);
        const float lumps = fbm(u * 2.2F + t * 0.8F, v * 2.2F - t * 0.5F, 7U);
        const float detail = fbm(u * 6.0F - t, v * 6.0F + t * 0.7F, 29U);
        const float boundary = (0.48F + 0.32F * lumps) * (0.82F + 0.18F * t);
        const float body = smoothstep(boundary, boundary - 0.38F, r);
        const float density = body * (0.55F + 0.45F * detail) * (1.0F - 0.45F * t);
        // Light from above: the top of the puff is brighter than its belly.
        const float light = 0.62F + 0.3F * std::clamp(-v * 0.8F + (detail - 0.5F) * 0.6F + 0.3F, 0.0F, 1.0F);
        return Pixel{light, std::clamp(density * 1.1F, 0.0F, 1.0F)};
    });
}

// A soft streaky blob, for magic, dust and embers.
TextureData wisp() {
    return generate(64U, 1U, 1U, [](float u, float v, std::uint32_t, std::uint32_t) {
        const float r = std::sqrt(u * u + v * v);
        const float streaks = fbm(u * 2.0F, v * 7.0F, 53U);
        const float body = std::pow(std::clamp(1.0F - r, 0.0F, 1.0F), 1.8F);
        return Pixel{0.85F + 0.15F * streaks, std::clamp(body * (0.35F + 0.9F * streaks), 0.0F, 1.0F)};
    });
}

// Four points of light around a hot centre: glints and magic sparkles.
TextureData star() {
    return generate(64U, 1U, 1U, [](float u, float v, std::uint32_t, std::uint32_t) {
        const float au = std::fabs(u);
        const float av = std::fabs(v);
        const float rays = std::max(std::exp(-au * 14.0F) * std::exp(-av * 2.6F), std::exp(-av * 14.0F) * std::exp(-au * 2.6F));
        const float r = std::sqrt(u * u + v * v);
        const float fade = std::clamp(1.0F - r, 0.0F, 1.0F);
        const float core = std::exp(-r * r * 40.0F);
        return Pixel{1.0F, std::clamp((rays * 0.9F + core * 0.6F) * fade, 0.0F, 1.0F)};
    });
}

// A soft ring: shockwaves, ripples, portals.
TextureData ring() {
    return generate(64U, 1U, 1U, [](float u, float v, std::uint32_t, std::uint32_t) {
        const float r = std::sqrt(u * u + v * v);
        const float band = std::exp(-std::pow((r - 0.72F) / 0.11F, 2.0F));
        return Pixel{1.0F, std::clamp(band * smoothstep(1.0F, 0.9F, r), 0.0F, 1.0F)};
    });
}

const std::array<LibraryTexture, 8> textures = {{
    {"dot", "", 1U, 1U},
    {"glow", "glow", 1U, 1U},
    {"spark", "spark", 1U, 1U},
    {"flame", "flame", 4U, 4U},
    {"smoke", "smoke", 4U, 4U},
    {"wisp", "wisp", 1U, 1U},
    {"star", "star", 1U, 1U},
    {"ring", "ring", 1U, 1U},
}};

// A flat-shaded mesh from a triangle list, each triangle turned to face away
// from the origin, texture coordinates projected from above.
MeshData faceted(const std::vector<std::array<Vec3, 3>>& triangles) {
    MeshData mesh;
    for (std::array<Vec3, 3> tri : triangles) {
        Vec3 normal = cross(tri[1] - tri[0], tri[2] - tri[0]);
        const Vec3 centre = (tri[0] + tri[1] + tri[2]) * (1.0F / 3.0F);
        if (dot(normal, centre) < 0.0F) {
            std::swap(tri[1], tri[2]);
            normal = normal * -1.0F;
        }
        const float length = std::sqrt(dot(normal, normal));
        normal = length > 0.0F ? normal * (1.0F / length) : Vec3{0.0F, 1.0F, 0.0F};
        for (const Vec3& p : tri) {
            mesh.indices.push_back(static_cast<std::uint32_t>(mesh.vertices.size()));
            mesh.vertices.push_back(MeshVertex{p, normal, p.x + 0.5F, p.z + 0.5F});
        }
    }
    return mesh;
}

MeshData chip() {
    MeshData mesh = cube_mesh();
    for (MeshVertex& vertex : mesh.vertices) {
        vertex.position = Vec3{vertex.position.x, vertex.position.y * 0.5F, vertex.position.z * 0.8F};
    }
    return mesh;
}

// A flattened, lopsided double pyramid: a sliver of glass or stone.
MeshData shard() {
    const Vec3 ends[2] = {{0.5F, 0.02F, 0.05F}, {-0.42F, -0.03F, -0.04F}};
    const Vec3 ring[4] = {{0.05F, 0.16F, 0.0F}, {-0.02F, 0.0F, 0.3F}, {0.0F, -0.14F, 0.02F}, {0.04F, 0.0F, -0.26F}};
    std::vector<std::array<Vec3, 3>> triangles;
    for (int i = 0; i < 4; ++i) {
        triangles.push_back({ends[0], ring[i], ring[(i + 1) % 4]});
        triangles.push_back({ends[1], ring[(i + 1) % 4], ring[i]});
    }
    return faceted(triangles);
}

// An icosahedron with its corners pushed in and out: a pebble.
MeshData rock() {
    const float g = (1.0F + std::sqrt(5.0F)) * 0.5F;
    std::array<Vec3, 12> v = {{{-1, g, 0}, {1, g, 0}, {-1, -g, 0}, {1, -g, 0},
                               {0, -1, g}, {0, 1, g}, {0, -1, -g}, {0, 1, -g},
                               {g, 0, -1}, {g, 0, 1}, {-g, 0, -1}, {-g, 0, 1}}};
    for (std::size_t i = 0U; i < v.size(); ++i) {
        const float length = std::sqrt(dot(v[i], v[i]));
        const float jitter = 0.78F + 0.32F * hash_unit(hash_mix(99U, static_cast<std::uint32_t>(i)));
        v[i] = v[i] * (0.5F * jitter / length);
        v[i].y *= 0.75F;
    }
    const int faces[20][3] = {{0, 11, 5}, {0, 5, 1},  {0, 1, 7},   {0, 7, 10}, {0, 10, 11},
                              {1, 5, 9},  {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
                              {3, 9, 4},  {3, 4, 2},  {3, 2, 6},   {3, 6, 8},  {3, 8, 9},
                              {4, 9, 5},  {2, 4, 11}, {6, 2, 10},  {8, 6, 7},  {9, 8, 1}};
    std::vector<std::array<Vec3, 3>> triangles;
    for (const auto& face : faces) {
        triangles.push_back({v[static_cast<std::size_t>(face[0])], v[static_cast<std::size_t>(face[1])],
                             v[static_cast<std::size_t>(face[2])]});
    }
    return faceted(triangles);
}

struct Generated final {
    std::map<std::string, TextureData, std::less<>> textures;
    std::map<std::string, MeshData, std::less<>> meshes;
};

const Generated& generated() {
    static const Generated library = [] {
        Generated g;
        g.textures.emplace("glow", glow());
        g.textures.emplace("spark", spark());
        g.textures.emplace("flame", flame());
        g.textures.emplace("smoke", smoke());
        g.textures.emplace("wisp", wisp());
        g.textures.emplace("star", star());
        g.textures.emplace("ring", ring());
        g.meshes.emplace("chip", chip());
        g.meshes.emplace("shard", shard());
        g.meshes.emplace("rock", rock());
        g.meshes.emplace("cube", cube_mesh());
        return g;
    }();
    return library;
}

} // namespace

const LibraryTexture* library_texture(std::string_view id) {
    for (const LibraryTexture& texture : textures) {
        if (texture.id == id) {
            return &texture;
        }
    }
    return nullptr;
}

const TextureData* library_texture_data(std::string_view entry) {
    const auto& set = generated().textures;
    const auto found = set.find(entry);
    return found != set.end() ? &found->second : nullptr;
}

const MeshData* library_mesh(std::string_view name) {
    const auto& set = generated().meshes;
    const auto found = set.find(name);
    return found != set.end() ? &found->second : nullptr;
}

} // namespace ajfx::studio
