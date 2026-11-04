#include "user_assets.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <tuple>
#include <utility>

namespace ajfx::studio {
namespace {

float length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

Vec3 normalized(Vec3 v, Vec3 fallback) {
    const float l = length(v);
    return l > 1.0e-12F ? v * (1.0F / l) : fallback;
}

Vec3 minimum(Vec3 a, Vec3 b) {
    return Vec3{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}

Vec3 maximum(Vec3 a, Vec3 b) {
    return Vec3{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

float axis(Vec3 v, int a) {
    return a == 0 ? v.x : a == 1 ? v.y : v.z;
}

bool finite(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Triangles whose corners are all in range and finite.
std::vector<std::array<std::uint32_t, 3>> triangles_of(const ImportedMesh& mesh) {
    std::vector<std::array<std::uint32_t, 3>> triangles;
    const std::size_t count = mesh.positions.size();
    for (std::size_t i = 0U; i + 2U < mesh.indices.size(); i += 3U) {
        const std::array<std::uint32_t, 3> t{mesh.indices[i], mesh.indices[i + 1U], mesh.indices[i + 2U]};
        if (t[0] < count && t[1] < count && t[2] < count && finite(mesh.positions[t[0]]) &&
            finite(mesh.positions[t[1]]) && finite(mesh.positions[t[2]])) {
            triangles.push_back(t);
        }
    }
    return triangles;
}

// Coinciding corners as one: each position's index among the distinct ones,
// positions compared to a hundred-thousandth of the model's size.
std::vector<std::uint32_t> welded(const ImportedMesh& mesh) {
    Vec3 low{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Vec3 high = low * -1.0F;
    for (const Vec3& p : mesh.positions) {
        if (finite(p)) {
            low = minimum(low, p);
            high = maximum(high, p);
        }
    }
    const float size = std::max({high.x - low.x, high.y - low.y, high.z - low.z, 1.0e-6F});
    const double quantum = static_cast<double>(size) * 1.0e-5;
    std::map<std::tuple<long long, long long, long long>, std::uint32_t> seen;
    std::vector<std::uint32_t> out(mesh.positions.size(), 0U);
    for (std::size_t i = 0U; i < mesh.positions.size(); ++i) {
        const Vec3 p = mesh.positions[i];
        const auto key = std::make_tuple(std::llround(static_cast<double>(p.x) / quantum),
                                         std::llround(static_cast<double>(p.y) / quantum),
                                         std::llround(static_cast<double>(p.z) / quantum));
        const auto [found, fresh] = seen.emplace(key, static_cast<std::uint32_t>(seen.size()));
        static_cast<void>(fresh);
        out[i] = found->second;
    }
    return out;
}

using Edge = std::pair<std::uint32_t, std::uint32_t>;

Edge edge(std::uint32_t a, std::uint32_t b) {
    return a < b ? Edge{a, b} : Edge{b, a};
}

// The closest point to `p` on triangle abc (Ericson, Real-Time Collision
// Detection 5.1.5), as a squared distance.
float distance_squared(Vec3 p, Vec3 a, Vec3 b, Vec3 c) {
    const Vec3 ab = b - a;
    const Vec3 ac = c - a;
    const Vec3 ap = p - a;
    const float d1 = dot(ab, ap);
    const float d2 = dot(ac, ap);
    Vec3 q{};
    if (d1 <= 0.0F && d2 <= 0.0F) {
        q = a;
    } else {
        const Vec3 bp = p - b;
        const float d3 = dot(ab, bp);
        const float d4 = dot(ac, bp);
        const Vec3 cp = p - c;
        const float d5 = dot(ab, cp);
        const float d6 = dot(ac, cp);
        const float vc = d1 * d4 - d3 * d2;
        const float vb = d5 * d2 - d1 * d6;
        const float va = d3 * d6 - d5 * d4;
        if (d3 >= 0.0F && d4 <= d3) {
            q = b;
        } else if (vc <= 0.0F && d1 >= 0.0F && d3 <= 0.0F) {
            q = a + ab * (d1 / (d1 - d3));
        } else if (d6 >= 0.0F && d5 <= d6) {
            q = c;
        } else if (vb <= 0.0F && d2 >= 0.0F && d6 <= 0.0F) {
            q = a + ac * (d2 / (d2 - d6));
        } else if (va <= 0.0F && (d4 - d3) >= 0.0F && (d5 - d6) >= 0.0F) {
            q = b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
        } else {
            const float denominator = va + vb + vc;
            if (!(std::fabs(denominator) > 0.0F)) {
                // A sliver with no area: the nearest of its edges' ends.
                return std::min({dot(p - a, p - a), dot(p - b, p - b), dot(p - c, p - c)});
            }
            const float v = vb / denominator;
            const float w = vc / denominator;
            q = a + ab * v + ac * w;
        }
    }
    const Vec3 d = p - q;
    return dot(d, d);
}

std::string hex16(std::uint64_t value) {
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx", static_cast<unsigned long long>(value));
    return buffer;
}

} // namespace

std::string user_asset_name(std::string_view kind, std::string_view path, std::string_view options) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    const auto mix = [&hash](std::string_view text) {
        for (const char c : text) {
            hash = (hash ^ static_cast<std::uint8_t>(c)) * 0x100000001b3ULL;
        }
        hash = (hash ^ 0xFFU) * 0x100000001b3ULL;
    };
    mix(kind);
    mix(path);
    mix(options);
    return "user." + hex16(hash);
}

std::optional<TextureData> sprite_texture(const ImportedImage& image, std::string& error) {
    if (image.width == 0U || image.height == 0U ||
        image.rgba.size() != static_cast<std::size_t>(image.width) * image.height * 4U) {
        error = "the picture has no pixels";
        return std::nullopt;
    }
    std::uint32_t width = image.width;
    std::uint32_t height = image.height;
    std::vector<std::uint8_t> pixels = image.rgba;
    // Halved (a 2x2 box, the last row or column held at an odd edge) until it
    // fits: sprites are small on screen, and packages travel to every node.
    while (std::max(width, height) > max_sprite_size) {
        const std::uint32_t w = std::max(width / 2U, 1U);
        const std::uint32_t h = std::max(height / 2U, 1U);
        std::vector<std::uint8_t> half(static_cast<std::size_t>(w) * h * 4U);
        for (std::uint32_t y = 0U; y < h; ++y) {
            for (std::uint32_t x = 0U; x < w; ++x) {
                for (std::uint32_t c = 0U; c < 4U; ++c) {
                    std::uint32_t sum = 0U;
                    for (std::uint32_t dy = 0U; dy < 2U; ++dy) {
                        for (std::uint32_t dx = 0U; dx < 2U; ++dx) {
                            const std::uint32_t sx = std::min(x * 2U + dx, width - 1U);
                            const std::uint32_t sy = std::min(y * 2U + dy, height - 1U);
                            sum += pixels[(static_cast<std::size_t>(sy) * width + sx) * 4U + c];
                        }
                    }
                    half[(static_cast<std::size_t>(y) * w + x) * 4U + c] = static_cast<std::uint8_t>((sum + 2U) / 4U);
                }
            }
        }
        pixels = std::move(half);
        width = w;
        height = h;
    }
    Result<TextureData> texture = make_texture(width, height, pixels, ColorSpace::Srgb);
    if (!texture) {
        error = texture.error().message;
        return std::nullopt;
    }
    return std::move(texture).value();
}

std::optional<MeshData> debris_mesh(const ImportedMesh& mesh, std::string& error) {
    const std::vector<std::array<std::uint32_t, 3>> triangles = triangles_of(mesh);
    if (triangles.empty()) {
        error = "the model has no triangles";
        return std::nullopt;
    }
    Vec3 low{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Vec3 high = low * -1.0F;
    for (const auto& t : triangles) {
        for (const std::uint32_t i : t) {
            low = minimum(low, mesh.positions[i]);
            high = maximum(high, mesh.positions[i]);
        }
    }
    const Vec3 centre = (low + high) * 0.5F;
    const float longest = std::max({high.x - low.x, high.y - low.y, high.z - low.z});
    if (!(longest > 0.0F)) {
        error = "the model has no size";
        return std::nullopt;
    }
    const float fit = 1.0F / longest;
    const bool smooth = mesh.normals.size() == mesh.positions.size();
    const bool mapped = mesh.uvs.size() == mesh.positions.size() * 2U;
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    if (smooth) {
        // The file's own corners and normals, shared between triangles.
        std::vector<std::uint32_t> remap(mesh.positions.size(), std::numeric_limits<std::uint32_t>::max());
        for (const auto& t : triangles) {
            for (const std::uint32_t i : t) {
                if (remap[i] == std::numeric_limits<std::uint32_t>::max()) {
                    remap[i] = static_cast<std::uint32_t>(vertices.size());
                    MeshVertex v;
                    v.position = (mesh.positions[i] - centre) * fit;
                    v.normal = normalized(mesh.normals[i], Vec3{0.0F, 1.0F, 0.0F});
                    if (mapped) {
                        v.u = mesh.uvs[i * 2U];
                        v.v = mesh.uvs[i * 2U + 1U];
                    }
                    vertices.push_back(v);
                }
                indices.push_back(remap[i]);
            }
        }
    } else {
        // Faceted: each triangle its own three corners and its face's normal.
        for (const auto& t : triangles) {
            const Vec3 a = mesh.positions[t[0]];
            const Vec3 b = mesh.positions[t[1]];
            const Vec3 c = mesh.positions[t[2]];
            const Vec3 normal = normalized(cross(b - a, c - a), Vec3{0.0F, 1.0F, 0.0F});
            for (const std::uint32_t i : t) {
                MeshVertex v;
                v.position = (mesh.positions[i] - centre) * fit;
                v.normal = normal;
                if (mapped) {
                    v.u = mesh.uvs[i * 2U];
                    v.v = mesh.uvs[i * 2U + 1U];
                }
                indices.push_back(static_cast<std::uint32_t>(vertices.size()));
                vertices.push_back(v);
            }
        }
    }
    if (vertices.size() > max_mesh_vertices || indices.size() > max_mesh_indices) {
        error = "the model has " + std::to_string(vertices.size()) + " corners; debris takes at most " +
                std::to_string(max_mesh_vertices) + ". Simplify it in your modelling tool";
        return std::nullopt;
    }
    Result<MeshData> made = make_mesh(std::move(vertices), std::move(indices));
    if (!made) {
        error = made.error().message;
        return std::nullopt;
    }
    return std::move(made).value();
}

bool closed_mesh(const ImportedMesh& mesh) {
    const std::vector<std::array<std::uint32_t, 3>> triangles = triangles_of(mesh);
    if (triangles.empty()) {
        return false;
    }
    const std::vector<std::uint32_t> corner = welded(mesh);
    std::map<Edge, std::uint32_t> uses;
    for (const auto& t : triangles) {
        // A triangle with two corners in one place has no area and no edges:
        // the pole of a sphere made of rings has a fan of them.
        if (corner[t[0]] == corner[t[1]] || corner[t[1]] == corner[t[2]] || corner[t[0]] == corner[t[2]]) {
            continue;
        }
        for (std::size_t k = 0U; k < 3U; ++k) {
            const std::uint32_t a = corner[t[k]];
            const std::uint32_t b = corner[t[(k + 1U) % 3U]];
            if (a != b) {
                ++uses[edge(a, b)];
            }
        }
    }
    return std::all_of(uses.begin(), uses.end(), [](const auto& entry) { return entry.second == 2U; });
}

ImportedMesh placed_mesh(const ImportedMesh& mesh, bool fit, Vec3 turn) {
    ImportedMesh out = mesh;
    if (fit) {
        const std::vector<std::array<std::uint32_t, 3>> triangles = triangles_of(mesh);
        Vec3 low{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                 std::numeric_limits<float>::max()};
        Vec3 high = low * -1.0F;
        for (const auto& t : triangles) {
            for (const std::uint32_t i : t) {
                low = minimum(low, mesh.positions[i]);
                high = maximum(high, mesh.positions[i]);
            }
        }
        const float longest = std::max({high.x - low.x, high.y - low.y, high.z - low.z});
        if (longest > 0.0F) {
            const Vec3 centre = (low + high) * 0.5F;
            for (Vec3& p : out.positions) {
                p = (p - centre) * (1.0F / longest);
            }
        }
    }
    if (turn != Vec3{}) {
        const Quat q = prop_orientation(turn);
        for (Vec3& p : out.positions) {
            p = rotate(q, p);
        }
        for (Vec3& n : out.normals) {
            n = rotate(q, n);
        }
    }
    return out;
}

std::optional<ColliderBake> bake_collider(const ImportedMesh& mesh, float scale, std::uint32_t cells,
                                          ColliderSolid solid_mode, std::string& error) {
    const std::vector<std::array<std::uint32_t, 3>> triangles = triangles_of(mesh);
    if (triangles.empty()) {
        error = "the model has no triangles";
        return std::nullopt;
    }
    if (!(scale > 0.0F) || !std::isfinite(scale)) {
        error = "the model's scale must be above 0";
        return std::nullopt;
    }
    std::vector<Vec3> points(mesh.positions.size());
    Vec3 low{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Vec3 high = low * -1.0F;
    for (std::size_t i = 0U; i < points.size(); ++i) {
        points[i] = mesh.positions[i] * scale;
    }
    for (const auto& t : triangles) {
        for (const std::uint32_t i : t) {
            low = minimum(low, points[i]);
            high = maximum(high, points[i]);
        }
    }
    const float longest = std::max({high.x - low.x, high.y - low.y, high.z - low.z});
    if (!(longest > 0.0F)) {
        error = "the model has no size";
        return std::nullopt;
    }

    // The grid: `cells` along the longest side, three spare cells all round,
    // coarsened until it fits the runtime's limits.
    constexpr std::uint32_t margin = 3U;
    cells = std::clamp(cells, 8U, max_field_size - 2U * margin - 1U);
    float spacing = longest / static_cast<float>(cells);
    std::array<std::uint32_t, 3> n{};
    for (int attempt = 0; attempt < 64; ++attempt) {
        std::uint64_t total = 1U;
        bool fits = true;
        for (int a = 0; a < 3; ++a) {
            const float extent = axis(high, a) - axis(low, a);
            const auto count = static_cast<std::uint32_t>(std::ceil(extent / spacing)) + 2U * margin + 1U;
            n[static_cast<std::size_t>(a)] = std::max(count, 2U);
            fits = fits && count <= max_field_size;
            total *= n[static_cast<std::size_t>(a)];
        }
        if (fits && total <= max_field_samples) {
            break;
        }
        spacing *= 1.1F;
    }
    const Vec3 origin = low - Vec3{spacing, spacing, spacing} * static_cast<float>(margin);
    const std::size_t nx = n[0];
    const std::size_t ny = n[1];
    const std::size_t nz = n[2];
    const std::size_t samples = nx * ny * nz;
    const auto at = [&](std::size_t i, std::size_t j, std::size_t k) { return (k * ny + j) * nx + i; };
    const auto position = [&](std::size_t i, std::size_t j, std::size_t k) {
        return origin + Vec3{static_cast<float>(i), static_cast<float>(j), static_cast<float>(k)} * spacing;
    };

    // Exact distances in a band round each triangle, with the triangle that
    // gave them.
    constexpr float far = std::numeric_limits<float>::max();
    std::vector<float> squared(samples, far);
    std::vector<std::uint32_t> nearest(samples, std::numeric_limits<std::uint32_t>::max());
    const auto cell_of = [&](float value, int a, std::size_t limit) {
        const float f = std::floor((value - axis(origin, a)) / spacing);
        return static_cast<std::size_t>(std::clamp(f, 0.0F, static_cast<float>(limit - 1U)));
    };
    const float band = spacing * 1.5F;
    for (std::size_t t = 0U; t < triangles.size(); ++t) {
        const Vec3 a = points[triangles[t][0]];
        const Vec3 b = points[triangles[t][1]];
        const Vec3 c = points[triangles[t][2]];
        const Vec3 lo = minimum(minimum(a, b), c) - Vec3{band, band, band};
        const Vec3 hi = maximum(maximum(a, b), c) + Vec3{band, band, band};
        for (std::size_t k = cell_of(lo.z, 2, nz); k <= cell_of(hi.z, 2, nz) + 1U && k < nz; ++k) {
            for (std::size_t j = cell_of(lo.y, 1, ny); j <= cell_of(hi.y, 1, ny) + 1U && j < ny; ++j) {
                for (std::size_t i = cell_of(lo.x, 0, nx); i <= cell_of(hi.x, 0, nx) + 1U && i < nx; ++i) {
                    const std::size_t s = at(i, j, k);
                    const float d = distance_squared(position(i, j, k), a, b, c);
                    if (d < squared[s]) {
                        squared[s] = d;
                        nearest[s] = static_cast<std::uint32_t>(t);
                    }
                }
            }
        }
    }
    // Outwards from the band: each sample tries its neighbours' nearest
    // triangles, sweeping the grid in all eight diagonal orders, twice.
    const auto relax = [&](std::size_t s, std::size_t from, Vec3 p) {
        const std::uint32_t t = nearest[from];
        if (t == std::numeric_limits<std::uint32_t>::max() || t == nearest[s]) {
            return;
        }
        const float d = distance_squared(p, points[triangles[t][0]], points[triangles[t][1]], points[triangles[t][2]]);
        if (d < squared[s]) {
            squared[s] = d;
            nearest[s] = t;
        }
    };
    for (int pass = 0; pass < 2; ++pass) {
        for (int order = 0; order < 8; ++order) {
            const bool fx = (order & 1) != 0;
            const bool fy = (order & 2) != 0;
            const bool fz = (order & 4) != 0;
            for (std::size_t kk = 0U; kk < nz; ++kk) {
                const std::size_t k = fz ? nz - 1U - kk : kk;
                for (std::size_t jj = 0U; jj < ny; ++jj) {
                    const std::size_t j = fy ? ny - 1U - jj : jj;
                    for (std::size_t ii = 0U; ii < nx; ++ii) {
                        const std::size_t i = fx ? nx - 1U - ii : ii;
                        const std::size_t s = at(i, j, k);
                        const Vec3 p = position(i, j, k);
                        // The neighbours already visited in this order.
                        if (fx ? i + 1U < nx : i > 0U) {
                            relax(s, at(fx ? i + 1U : i - 1U, j, k), p);
                        }
                        if (fy ? j + 1U < ny : j > 0U) {
                            relax(s, at(i, fy ? j + 1U : j - 1U, k), p);
                        }
                        if (fz ? k + 1U < nz : k > 0U) {
                            relax(s, at(i, j, fz ? k + 1U : k - 1U), p);
                        }
                    }
                }
            }
        }
    }

    const bool solid = solid_mode == ColliderSolid::Solid || (solid_mode == ColliderSolid::Auto && closed_mesh(mesh));
    std::vector<std::uint8_t> votes(samples, 0U);
    if (solid) {
        // Inside where a ray along an axis has crossed the surface an odd
        // number of times. Rays run a hair off the grid's lines, so they do
        // not graze edges and corners exactly.
        for (int a = 0; a < 3; ++a) {
            const int u = (a + 1) % 3;
            const int v = (a + 2) % 3;
            const std::size_t na = n[static_cast<std::size_t>(a)];
            const std::size_t nu = n[static_cast<std::size_t>(u)];
            const std::size_t nv = n[static_cast<std::size_t>(v)];
            // Triangles binned by the rows (u, v) their shadow covers.
            std::vector<std::vector<std::uint32_t>> rows(nu * nv);
            for (std::size_t t = 0U; t < triangles.size(); ++t) {
                const Vec3 p0 = points[triangles[t][0]];
                const Vec3 p1 = points[triangles[t][1]];
                const Vec3 p2 = points[triangles[t][2]];
                const float u0 = std::min({axis(p0, u), axis(p1, u), axis(p2, u)});
                const float u1 = std::max({axis(p0, u), axis(p1, u), axis(p2, u)});
                const float v0 = std::min({axis(p0, v), axis(p1, v), axis(p2, v)});
                const float v1 = std::max({axis(p0, v), axis(p1, v), axis(p2, v)});
                for (std::size_t jv = cell_of(v0, v, nv); jv <= cell_of(v1, v, nv) + 1U && jv < nv; ++jv) {
                    for (std::size_t ju = cell_of(u0, u, nu); ju <= cell_of(u1, u, nu) + 1U && ju < nu; ++ju) {
                        rows[jv * nu + ju].push_back(static_cast<std::uint32_t>(t));
                    }
                }
            }
            std::vector<float> crossings;
            for (std::size_t jv = 0U; jv < nv; ++jv) {
                for (std::size_t ju = 0U; ju < nu; ++ju) {
                    const float pu = axis(origin, u) + (static_cast<float>(ju) + 1.3e-3F) * spacing;
                    const float pv = axis(origin, v) + (static_cast<float>(jv) + 0.7e-3F) * spacing;
                    crossings.clear();
                    for (const std::uint32_t t : rows[jv * nu + ju]) {
                        const Vec3 p0 = points[triangles[t][0]];
                        const Vec3 p1 = points[triangles[t][1]];
                        const Vec3 p2 = points[triangles[t][2]];
                        // Barycentric coordinates of the ray in the triangle's shadow.
                        const float ax = axis(p0, u), ay = axis(p0, v);
                        const float bx = axis(p1, u), by = axis(p1, v);
                        const float cx = axis(p2, u), cy = axis(p2, v);
                        const float area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
                        if (std::fabs(area) < 1.0e-20F) {
                            continue;
                        }
                        const float w0 = ((bx - pu) * (cy - pv) - (cx - pu) * (by - pv)) / area;
                        const float w1 = ((cx - pu) * (ay - pv) - (ax - pu) * (cy - pv)) / area;
                        const float w2 = 1.0F - w0 - w1;
                        if (w0 < 0.0F || w1 < 0.0F || w2 < 0.0F) {
                            continue;
                        }
                        crossings.push_back(w0 * axis(p0, a) + w1 * axis(p1, a) + w2 * axis(p2, a));
                    }
                    std::sort(crossings.begin(), crossings.end());
                    std::size_t passed = 0U;
                    for (std::size_t ia = 0U; ia < na; ++ia) {
                        const float pa = axis(origin, a) + static_cast<float>(ia) * spacing;
                        while (passed < crossings.size() && crossings[passed] < pa) {
                            ++passed;
                        }
                        if ((passed % 2U) == 1U) {
                            std::array<std::size_t, 3> c{};
                            c[static_cast<std::size_t>(a)] = ia;
                            c[static_cast<std::size_t>(u)] = ju;
                            c[static_cast<std::size_t>(v)] = jv;
                            ++votes[at(c[0], c[1], c[2])];
                        }
                    }
                }
            }
        }
    }

    ColliderBake bake;
    bake.solid = solid;
    bake.field.nx = static_cast<std::uint32_t>(nx);
    bake.field.ny = static_cast<std::uint32_t>(ny);
    bake.field.nz = static_cast<std::uint32_t>(nz);
    bake.field.origin = origin;
    bake.field.spacing = spacing;
    bake.field.distances.resize(samples);
    // A thin surface is a shell of a cell's thickness either side.
    const float shell = spacing * 0.75F;
    for (std::size_t s = 0U; s < samples; ++s) {
        const float d = squared[s] < far ? std::sqrt(squared[s]) : longest;
        bake.field.distances[s] = solid ? (votes[s] >= 2U ? -d : d) : d - shell;
    }
    return bake;
}

std::vector<Vec3> mesh_outline(const ImportedMesh& mesh, std::size_t max_segments) {
    std::vector<Vec3> out;
    const std::vector<std::array<std::uint32_t, 3>> triangles = triangles_of(mesh);
    if (triangles.empty() || max_segments == 0U) {
        return out;
    }
    const std::vector<std::uint32_t> corner = welded(mesh);
    struct Use final {
        std::uint32_t count{0U};
        Vec3 normal{};
        float crease{1.0F}; // the smallest cosine between the faces on it
        std::uint32_t a{};
        std::uint32_t b{};
    };
    std::map<Edge, Use> edges;
    for (const auto& t : triangles) {
        const Vec3 normal = normalized(cross(mesh.positions[t[1]] - mesh.positions[t[0]],
                                             mesh.positions[t[2]] - mesh.positions[t[0]]),
                                       Vec3{});
        for (std::size_t k = 0U; k < 3U; ++k) {
            const std::uint32_t a = t[k];
            const std::uint32_t b = t[(k + 1U) % 3U];
            if (corner[a] == corner[b]) {
                continue;
            }
            Use& use = edges[edge(corner[a], corner[b])];
            if (use.count == 0U) {
                use.normal = normal;
                use.a = a;
                use.b = b;
            } else {
                use.crease = std::min(use.crease, dot(use.normal, normal));
            }
            ++use.count;
        }
    }
    // Open edges and creases sharper than about 35 degrees, then the rest.
    std::vector<const Use*> features;
    std::vector<const Use*> others;
    for (const auto& [key, use] : edges) {
        (use.count != 2U || use.crease < 0.82F ? features : others).push_back(&use);
    }
    const auto take = [&](const std::vector<const Use*>& from) {
        if (from.empty() || out.size() / 2U >= max_segments) {
            return;
        }
        const std::size_t room = max_segments - out.size() / 2U;
        const std::size_t stride = std::max<std::size_t>(1U, (from.size() + room - 1U) / room);
        for (std::size_t i = 0U; i < from.size() && out.size() / 2U < max_segments; i += stride) {
            out.push_back(mesh.positions[from[i]->a]);
            out.push_back(mesh.positions[from[i]->b]);
        }
    };
    take(features);
    if (features.size() < 8U) {
        // A smooth model has no creases: its wireframe, thinned, reads instead.
        take(others);
    }
    return out;
}

} // namespace ajfx::studio
