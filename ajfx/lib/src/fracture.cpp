#include <ajfx/fracture.hpp>

#include <ajfx/random.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace ajfx {
namespace {

// The stream the site draws are keyed by, so they can never collide with a
// particle's draws, and the draw within a site's attempt.
constexpr std::uint32_t site_stream = 0x66726163U; // 'frac'
constexpr std::uint32_t draws_per_attempt = 4U;
// Attempts to land one site inside the model. A hull fills a decent share of
// its own bounding box, so a site that cannot be placed in this many tries
// means a model too thin to break into the asked-for number of pieces.
constexpr std::uint32_t site_attempts = 512U;

// Tolerances, every one of them relative to the model's own size, so a model
// authored in millimetres and the same model in metres cut the same way.
struct Scale final {
    // The diagonal of the model's bounds.
    float size{};
    // Within this of a cut plane counts as on it.
    float on_plane{};
    // Sites keep this much clear of the hull's surface and of each other,
    // which is what keeps every cell a solid with volume of its own.
    float margin{};
};

[[nodiscard]] float length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

[[nodiscard]] Vec3 normalized(Vec3 v) {
    const float squared = dot(v, v);
    return squared > 0.0F ? v * (1.0F / std::sqrt(squared)) : Vec3{};
}

[[nodiscard]] bool finite(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// Enough of an order on points to pick one of a pair without looking at
// anything but the two of them.
[[nodiscard]] bool before(Vec3 a, Vec3 b) {
    if (a.x != b.x) {
        return a.x < b.x;
    }
    if (a.y != b.y) {
        return a.y < b.y;
    }
    return a.z < b.z;
}

// The side of a plane a cell keeps: dot(normal, x) <= offset, with `normal`
// unit length and pointing out of the cell.
struct Plane final {
    Vec3 normal{};
    float offset{};
};

[[nodiscard]] float distance(const Plane& plane, Vec3 point) {
    return dot(plane.normal, point) - plane.offset;
}

// A convex polyhedron as its faces, each a ring of points wound
// counter-clockwise seen from outside, so its normal points out.
struct Facet final {
    std::vector<Vec3> ring;
    Vec3 normal{};
};

using Polyhedron = std::vector<Facet>;

// Two axes across a plane, taken from the normal alone so that the same plane
// always gets the same pair: a face's winding and its texture coordinates
// must not depend on how the plane was arrived at.
struct Basis final {
    Vec3 u;
    Vec3 v;
};

[[nodiscard]] Basis plane_basis(Vec3 normal) {
    const float ax = std::fabs(normal.x);
    const float ay = std::fabs(normal.y);
    const float az = std::fabs(normal.z);
    // The axis the normal leans on least is the one guaranteed not to be
    // parallel to it.
    const Vec3 helper = (ax <= ay && ax <= az) ? Vec3{1.0F, 0.0F, 0.0F}
                        : (ay <= az)           ? Vec3{0.0F, 1.0F, 0.0F}
                                               : Vec3{0.0F, 0.0F, 1.0F};
    const Vec3 u = normalized(cross(helper, normal));
    // u x v = normal, so an angle rising in (u, v) winds counter-clockwise
    // seen from outside.
    return {u, cross(normal, u)};
}

// A number that rises with the angle of (x, y) right around the turn, from
// divisions and comparisons alone. atan2 differs in its last bits between
// libraries, and the order a cell's cut points come back in must not.
[[nodiscard]] float pseudo_angle(float x, float y) {
    const float span = std::fabs(x) + std::fabs(y);
    if (span == 0.0F) {
        return 0.0F;
    }
    const float along = x / span;
    return y < 0.0F ? 3.0F + along : 1.0F - along;
}

// Where an edge crosses a plane, computed from whichever of its endpoints
// sorts first. The two faces that share an edge walk it in opposite
// directions; working from the same end for both lands them on bit-identical
// points, which closes the cell without a hairline crack and without welding
// anything together afterwards.
[[nodiscard]] Vec3 cut_point(Vec3 a, float da, Vec3 b, float db) {
    if (before(b, a)) {
        std::swap(a, b);
        std::swap(da, db);
    }
    return a + (b - a) * (da / (da - db));
}

// `solid` cut down to the part of it inside `plane`, and left alone when
// nothing of it lies outside. Every face's ring is clipped; the cut leaves an
// open mouth, which the points that landed on the plane close as one new
// face, ordered around the plane's normal.
void clip(Polyhedron& solid, const Plane& plane, const Scale& scale) {
    const auto outside = [&](Vec3 point) { return distance(plane, point) > scale.on_plane; };
    const bool cuts = std::any_of(solid.begin(), solid.end(), [&](const Facet& facet) {
        return std::any_of(facet.ring.begin(), facet.ring.end(), outside);
    });
    if (!cuts) {
        return;
    }

    Polyhedron kept;
    std::vector<Vec3> mouth;
    std::vector<Vec3> ring;
    for (const Facet& facet : solid) {
        ring.clear();
        const std::size_t edges = facet.ring.size();
        for (std::size_t i = 0U; i < edges; ++i) {
            const Vec3 a = facet.ring[i];
            const Vec3 b = facet.ring[(i + 1U) % edges];
            const float da = distance(plane, a);
            const float db = distance(plane, b);
            if (da <= scale.on_plane) {
                ring.push_back(a);
                if (da >= -scale.on_plane) {
                    mouth.push_back(a);
                }
            }
            // Only a crossing that passes clear of the plane needs a point of
            // its own; an edge that ends on the plane keeps that end instead.
            if ((da > scale.on_plane && db < -scale.on_plane) || (da < -scale.on_plane && db > scale.on_plane)) {
                const Vec3 cut = cut_point(a, da, b, db);
                ring.push_back(cut);
                mouth.push_back(cut);
            }
        }
        if (ring.size() >= 3U) {
            kept.push_back({ring, facet.normal});
        }
    }

    // The mouth's points arrive once per face that met the plane, and two
    // faces meeting there agree on the point to the bit, so equality is all
    // the merging this needs.
    std::vector<Vec3> corners;
    for (const Vec3& point : mouth) {
        if (std::find(corners.begin(), corners.end(), point) == corners.end()) {
            corners.push_back(point);
        }
    }
    if (corners.size() >= 3U) {
        Vec3 middle{};
        for (const Vec3& point : corners) {
            middle += point;
        }
        middle = middle * (1.0F / static_cast<float>(corners.size()));
        const Basis basis = plane_basis(plane.normal);
        std::vector<std::pair<float, std::uint32_t>> around;
        for (std::uint32_t i = 0U; i < static_cast<std::uint32_t>(corners.size()); ++i) {
            const Vec3 spoke = corners[i] - middle;
            around.emplace_back(pseudo_angle(dot(spoke, basis.u), dot(spoke, basis.v)), i);
        }
        // The angle first and the index second: a total order, so two points
        // that answer the same angle still sort the same way every run.
        std::sort(around.begin(), around.end());
        Facet face;
        face.normal = plane.normal;
        for (const auto& [angle, index] : around) {
            face.ring.push_back(corners[index]);
        }
        kept.push_back(std::move(face));
    }
    solid = std::move(kept);
}

[[nodiscard]] Polyhedron box(Vec3 low, Vec3 high) {
    // Each face: its outward normal n and two axes with u x v = n, so the
    // corners -u-v, u-v, u+v, -u+v wind counter-clockwise seen from outside.
    struct Side final {
        Vec3 n;
        Vec3 u;
        Vec3 v;
    };
    const Side sides[6] = {
        {{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}},
        {{-1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 1.0F, 0.0F}},
        {{0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F}},
        {{0.0F, -1.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}},
        {{0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}},
        {{0.0F, 0.0F, -1.0F}, {0.0F, 1.0F, 0.0F}, {1.0F, 0.0F, 0.0F}},
    };
    const float corners[4][2] = {{-1.0F, -1.0F}, {1.0F, -1.0F}, {1.0F, 1.0F}, {-1.0F, 1.0F}};
    const Vec3 middle = (low + high) * 0.5F;
    const Vec3 half = (high - low) * 0.5F;
    const auto along = [half](Vec3 axis) { return Vec3{axis.x * half.x, axis.y * half.y, axis.z * half.z}; };
    Polyhedron solid;
    for (const Side& side : sides) {
        Facet face;
        face.normal = side.n;
        for (const auto& corner : corners) {
            face.ring.push_back(middle + along(side.n) + along(side.u) * corner[0] + along(side.v) * corner[1]);
        }
        solid.push_back(std::move(face));
    }
    return solid;
}

// Volume and middle from the signed tetrahedra of a fan over every face,
// measured from the solid's own mean corner so that each term is the size of
// the cell rather than the size of its distance from the origin.
struct Measure final {
    Vec3 center{};
    float volume{};
};

[[nodiscard]] Measure measure(const Polyhedron& solid) {
    Vec3 reference{};
    std::size_t corners = 0U;
    for (const Facet& facet : solid) {
        for (const Vec3& point : facet.ring) {
            reference += point;
            ++corners;
        }
    }
    if (corners == 0U) {
        return {};
    }
    reference = reference * (1.0F / static_cast<float>(corners));

    // Six times the volume and, weighted by it, four times the middle: the
    // constants divide out, so neither is applied until the end.
    float sixfold = 0.0F;
    Vec3 weighted{};
    for (const Facet& facet : solid) {
        for (std::size_t i = 1U; i + 1U < facet.ring.size(); ++i) {
            const Vec3 a = facet.ring[0] - reference;
            const Vec3 b = facet.ring[i] - reference;
            const Vec3 c = facet.ring[i + 1U] - reference;
            const float six = dot(a, cross(b, c));
            sixfold += six;
            weighted += (a + b + c) * (six * 0.25F);
        }
    }
    if (sixfold <= 0.0F) {
        return {reference, 0.0F};
    }
    return {reference + weighted * (1.0F / sixfold), sixfold * (1.0F / 6.0F)};
}

[[nodiscard]] Result<MeshData> cell_mesh(const Polyhedron& solid) {
    std::vector<MeshVertex> vertices;
    std::vector<std::uint32_t> indices;
    for (const Facet& facet : solid) {
        // A corner is a vertex of one face only: a cell's faces meet at hard
        // edges, and each wants its own normal there.
        const Basis basis = plane_basis(facet.normal);
        const Vec3 origin = facet.ring[0];
        const auto first = static_cast<std::uint32_t>(vertices.size());
        for (const Vec3& point : facet.ring) {
            const Vec3 across = point - origin;
            vertices.push_back({point, facet.normal, dot(across, basis.u), dot(across, basis.v)});
        }
        for (std::uint32_t i = 1U; i + 1U < static_cast<std::uint32_t>(facet.ring.size()); ++i) {
            indices.push_back(first);
            indices.push_back(first + i);
            indices.push_back(first + i + 1U);
        }
    }
    return make_mesh(std::move(vertices), std::move(indices));
}

// A hull face while the hull is being built: the three points, and the plane
// they span pointing away from the inside.
struct HullFace final {
    std::uint32_t a{};
    std::uint32_t b{};
    std::uint32_t c{};
    Vec3 normal{};
    float offset{};
};

[[nodiscard]] std::uint64_t packed_edge(std::uint32_t from, std::uint32_t to) {
    return (static_cast<std::uint64_t>(from) << 32U) | static_cast<std::uint64_t>(to);
}

[[nodiscard]] HullFace hull_face(const std::vector<Vec3>& points, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    const Vec3 normal = normalized(cross(points[b] - points[a], points[c] - points[a]));
    return {a, b, c, normal, dot(normal, points[a])};
}

// The directions the faces of the model's hull look in.
//
// Incremental: a tetrahedron spanned by four corners that are not in one
// plane, then every other point in turn, each one removing the faces it can
// see and rebuilding the hull over the horizon those faces leave behind.
// Points are taken in the order the model holds them and ties go to the
// earlier point, so one model always builds one hull.
//
// The cost is every point against every face the hull ever has, which is why
// this belongs to baking. The offsets of the planes are deliberately not
// taken from here: see support_planes.
[[nodiscard]] Result<std::vector<Vec3>> hull_directions(const std::vector<Vec3>& points, const Scale& scale) {
    const auto count = static_cast<std::uint32_t>(points.size());
    const std::string nothing = "fracture has nothing to cut: every corner of the model ";

    // The first corner, then the one farthest from it, then the one farthest
    // from the line of those two, then the one farthest from their plane.
    // Each step failing is one of the ways a model can have no volume.
    std::uint32_t second = 0U;
    float best = 0.0F;
    for (std::uint32_t i = 1U; i < count; ++i) {
        if (const float reach = length(points[i] - points[0]); reach > best) {
            best = reach;
            second = i;
        }
    }
    if (best <= scale.on_plane) {
        return Error{ErrorCode::InvalidEffect, nothing + "is in the same place"};
    }

    const Vec3 line = points[second] - points[0];
    std::uint32_t third = 0U;
    best = 0.0F;
    for (std::uint32_t i = 1U; i < count; ++i) {
        // The cross product is the area of the triangle, twice over, so
        // dividing by the line is the point's distance from it.
        if (const float reach = length(cross(line, points[i] - points[0])) / length(line); reach > best) {
            best = reach;
            third = i;
        }
    }
    if (best <= scale.on_plane) {
        return Error{ErrorCode::InvalidEffect, nothing + "is on one line"};
    }

    const Vec3 face = normalized(cross(line, points[third] - points[0]));
    std::uint32_t fourth = 0U;
    best = 0.0F;
    for (std::uint32_t i = 1U; i < count; ++i) {
        if (const float reach = std::fabs(dot(face, points[i] - points[0])); reach > best) {
            best = reach;
            fourth = i;
        }
    }
    if (best <= scale.on_plane) {
        return Error{ErrorCode::InvalidEffect, nothing + "is in one plane"};
    }

    std::uint32_t a = 0U;
    std::uint32_t b = second;
    std::uint32_t c = third;
    if (dot(face, points[fourth] - points[0]) > 0.0F) {
        // The fourth corner is on the side the first face looks at, so that
        // face is wound the wrong way round; the other three follow from it.
        std::swap(b, c);
    }
    std::vector<HullFace> faces{hull_face(points, a, b, c), hull_face(points, a, c, fourth),
                                hull_face(points, a, fourth, b), hull_face(points, b, fourth, c)};

    // A triangle this thin spans no direction worth clipping by: its height
    // over its own longest edge is under the on-plane tolerance.
    const float thin = scale.size * scale.on_plane;
    std::vector<std::uint32_t> visible;
    std::vector<std::uint64_t> rim;
    std::vector<std::uint64_t> horizon;
    for (std::uint32_t p = 0U; p < count; ++p) {
        const Vec3 point = points[p];
        visible.clear();
        for (std::uint32_t f = 0U; f < static_cast<std::uint32_t>(faces.size()); ++f) {
            if (dot(faces[f].normal, point) - faces[f].offset > scale.on_plane) {
                visible.push_back(f);
            }
        }
        if (visible.empty()) {
            continue; // already inside the hull, this corner included
        }

        rim.clear();
        for (const std::uint32_t f : visible) {
            const HullFace& face_f = faces[f];
            rim.push_back(packed_edge(face_f.a, face_f.b));
            rim.push_back(packed_edge(face_f.b, face_f.c));
            rim.push_back(packed_edge(face_f.c, face_f.a));
        }
        std::sort(rim.begin(), rim.end());
        // The horizon is the edges of the visible faces that the faces on the
        // far side still hold, which is to say those whose other direction no
        // visible face walks.
        horizon.clear();
        for (const std::uint64_t edge : rim) {
            const auto from = static_cast<std::uint32_t>(edge >> 32U);
            const auto to = static_cast<std::uint32_t>(edge & 0xffffffffU);
            if (!std::binary_search(rim.begin(), rim.end(), packed_edge(to, from))) {
                horizon.push_back(edge);
            }
        }

        // The faces the point cannot see, in the order they were built;
        // `visible` rises, so one walk down it is enough to skip them.
        std::vector<HullFace> kept;
        kept.reserve(faces.size() - visible.size() + horizon.size());
        std::size_t skipping = 0U;
        for (std::uint32_t f = 0U; f < static_cast<std::uint32_t>(faces.size()); ++f) {
            if (skipping < visible.size() && visible[skipping] == f) {
                ++skipping;
                continue;
            }
            kept.push_back(faces[f]);
        }
        for (const std::uint64_t edge : horizon) {
            const auto from = static_cast<std::uint32_t>(edge >> 32U);
            const auto to = static_cast<std::uint32_t>(edge & 0xffffffffU);
            if (length(cross(points[to] - points[from], point - points[from])) <= thin) {
                continue; // the three are in a line; the horizon closes over it
            }
            kept.push_back(hull_face(points, from, to, p));
        }
        faces = std::move(kept);
    }

    // One plane per direction: a hull face is a triangle, and a flat side of a
    // model is many triangles looking the same way. Rounding the direction
    // keeps the comparison on integers, and two directions a rounding apart
    // only cost a clip that changes nothing.
    constexpr float quantum = 4096.0F;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> keyed;
    for (std::uint32_t f = 0U; f < static_cast<std::uint32_t>(faces.size()); ++f) {
        const Vec3 n = faces[f].normal;
        const auto part = [](float value) {
            return static_cast<std::uint64_t>(static_cast<std::int64_t>(std::lround(value * quantum)) + 4096);
        };
        keyed.emplace_back((part(n.x) << 28U) | (part(n.y) << 14U) | part(n.z), f);
    }
    std::sort(keyed.begin(), keyed.end());
    std::vector<std::uint32_t> chosen;
    for (std::size_t i = 0U; i < keyed.size(); ++i) {
        if (i == 0U || keyed[i].first != keyed[i - 1U].first) {
            chosen.push_back(keyed[i].second);
        }
    }
    // Back into the order the faces were built in, which depends on the model
    // and nothing else.
    std::sort(chosen.begin(), chosen.end());
    std::vector<Vec3> directions;
    for (const std::uint32_t f : chosen) {
        directions.push_back(faces[f].normal);
    }
    return directions;
}

// The planes to clip by: each hull direction pushed out until it touches the
// model's farthest point that way. Taking the offsets from the points rather
// than from the hull's own triangles means a face the incremental hull built
// a hair out of place still cuts in exactly the right place, and a direction
// it missed only leaves the region wider than the hull, never narrower.
[[nodiscard]] std::vector<Plane> support_planes(const std::vector<Vec3>& points, const std::vector<Vec3>& directions) {
    std::vector<Plane> planes;
    for (const Vec3& normal : directions) {
        float offset = dot(normal, points[0]);
        for (const Vec3& point : points) {
            offset = std::max(offset, dot(normal, point));
        }
        planes.push_back({normal, offset});
    }
    return planes;
}

[[nodiscard]] bool inside(const std::vector<Plane>& planes, Vec3 point, float slack) {
    return std::none_of(planes.begin(), planes.end(),
                        [&](const Plane& plane) { return distance(plane, point) > slack; });
}

// How far along `step` a point may travel from `from` before it leaves the
// region, as a fraction of the step, keeping `margin` clear of the surface.
[[nodiscard]] float travel_allowed(const std::vector<Plane>& planes, Vec3 from, Vec3 step, float margin) {
    float allowed = 1.0F;
    for (const Plane& plane : planes) {
        const float here = distance(plane, from);
        const float rate = dot(plane.normal, step);
        if (rate > 0.0F) {
            allowed = std::min(allowed, (-here - margin) / rate);
        }
    }
    return std::max(allowed, 0.0F);
}

// The sites, one per cell, each inside the region and clear of the others.
//
// Rejection sampling from the model's bounding box: a point outside the hull,
// or on top of a site already placed, is thrown away and the next draw tried.
// Every draw is a `particle_hash` of the seed, the site and which attempt it
// is, so the whole placement is a pure function of the options.
[[nodiscard]] Result<std::vector<Vec3>> place_sites(
    const std::vector<Plane>& planes, Vec3 low, Vec3 high, const FractureOptions& options, const Scale& scale) {
    const Vec3 span = high - low;
    std::vector<Vec3> sites;
    for (std::uint32_t site = 0U; site < options.count; ++site) {
        bool placed = false;
        for (std::uint32_t attempt = 0U; attempt < site_attempts && !placed; ++attempt) {
            const auto draw = [&](std::uint32_t index) {
                return particle_unit(options.seed, site, hash_mix(site_stream, attempt * draws_per_attempt + index));
            };
            Vec3 point{low.x + span.x * draw(0U), low.y + span.y * draw(1U), low.z + span.z * draw(2U)};
            if (!inside(planes, point, -scale.margin)) {
                continue;
            }
            if (options.focus_bias > 0.0F) {
                // The fourth root spreads the pull towards the far end: at a
                // bias of 1 most sites travel most of the way, which is what
                // makes a cluster rather than an even smear along the line.
                const float pull = options.focus_bias * std::sqrt(std::sqrt(draw(3U)));
                const Vec3 step = options.focus - point;
                point += step * std::min(pull, travel_allowed(planes, point, step, scale.margin));
            }
            const bool crowded = std::any_of(sites.begin(), sites.end(),
                                             [&](Vec3 other) { return length(other - point) <= scale.margin; });
            if (crowded) {
                continue;
            }
            sites.push_back(point);
            placed = true;
        }
        if (!placed) {
            return Error{ErrorCode::InvalidEffect, "fracture could not find room for " +
                                                       std::to_string(options.count) + " sites inside the model; " +
                                                       std::to_string(sites.size()) + " of them fitted"};
        }
    }
    return sites;
}

// The plane of the points nearer `mine` than `theirs`: the perpendicular
// bisector of the two, looking away from `mine`.
[[nodiscard]] Plane bisector(Vec3 mine, Vec3 theirs) {
    const Vec3 normal = normalized(theirs - mine);
    return {normal, dot(normal, (mine + theirs) * 0.5F)};
}

} // namespace

Result<std::vector<FractureCell>> fracture(const MeshData& model, const FractureOptions& options) {
    if (options.count == 0U) {
        return Error{ErrorCode::InvalidEffect, "fracture's 'count' is 0; a break needs at least one piece"};
    }
    if (options.count > max_fracture_cells) {
        return Error{ErrorCode::BudgetExceeded, "fracture's 'count' is " + std::to_string(options.count) +
                                                    "; the most it cuts is " + std::to_string(max_fracture_cells)};
    }
    if (!finite(options.focus)) {
        return Error{ErrorCode::InvalidEffect, "fracture's 'focus' is not a number"};
    }
    if (!(options.focus_bias >= 0.0F && options.focus_bias <= 1.0F)) {
        return Error{ErrorCode::InvalidEffect, "fracture's 'focus_bias' is outside 0 to 1"};
    }
    if (model.vertices.size() < 4U) {
        return Error{ErrorCode::InvalidEffect, "fracture was given a model of " +
                                                   std::to_string(model.vertices.size()) +
                                                   " corners; a solid needs at least four"};
    }

    std::vector<Vec3> points;
    points.reserve(model.vertices.size());
    for (const MeshVertex& vertex : model.vertices) {
        if (!finite(vertex.position)) {
            return Error{ErrorCode::InvalidEffect, "fracture was given a model with a corner that is not a number"};
        }
        points.push_back(vertex.position);
    }

    Vec3 low = points[0];
    Vec3 high = points[0];
    for (const Vec3& point : points) {
        low = {std::min(low.x, point.x), std::min(low.y, point.y), std::min(low.z, point.z)};
        high = {std::max(high.x, point.x), std::max(high.y, point.y), std::max(high.z, point.z)};
    }
    Scale scale;
    scale.size = length(high - low);
    scale.on_plane = scale.size * 1.0e-6F;
    scale.margin = scale.size * 1.0e-4F;
    if (!(scale.size > 0.0F)) {
        return Error{ErrorCode::InvalidEffect, "fracture has nothing to cut: every corner of the model is in the "
                                               "same place"};
    }

    Result<std::vector<Vec3>> directions = hull_directions(points, scale);
    if (!directions) {
        return directions.error();
    }
    const std::vector<Plane> planes = support_planes(points, directions.value());

    // Everything is cut out of a box with room to spare around the model, so
    // that the hull's own faces do the cutting and no edge of the box can
    // survive into a piece.
    const Vec3 room = Vec3{1.0F, 1.0F, 1.0F} * (scale.size * 0.01F);
    const Polyhedron around = box(low - room, high + room);
    Polyhedron hull = around;
    for (const Plane& plane : planes) {
        clip(hull, plane, scale);
    }
    if (measure(hull).volume <= 0.0F) {
        return Error{ErrorCode::InvalidEffect, "fracture has nothing to cut: the model's hull came out flat"};
    }

    Result<std::vector<Vec3>> sites = place_sites(planes, low, high, options, scale);
    if (!sites) {
        return sites.error();
    }

    std::vector<FractureCell> cells;
    for (std::uint32_t mine = 0U; mine < options.count; ++mine) {
        // The other sites cut first, on purpose: a handful of bisectors
        // shrinks the box down to this piece's own corner of the model, after
        // which most of the hull's faces are nowhere near it and cost no more
        // than the test that says so. Cutting by the hull first would walk
        // the model's whole surface, face by face, once for every piece.
        Polyhedron cell = around;
        for (std::uint32_t theirs = 0U; theirs < options.count; ++theirs) {
            if (theirs != mine) {
                clip(cell, bisector(sites.value()[mine], sites.value()[theirs]), scale);
            }
        }
        for (const Plane& plane : planes) {
            clip(cell, plane, scale);
        }
        const Measure measured = measure(cell);
        if (measured.volume <= 0.0F) {
            return Error{ErrorCode::InvalidEffect,
                         "fracture's cell " + std::to_string(mine) + " came out with no volume; the model is too "
                         "thin to break into " + std::to_string(options.count) + " pieces"};
        }
        Result<MeshData> mesh = cell_mesh(cell);
        if (!mesh) {
            return Error{mesh.error().code, "fracture's cell " + std::to_string(mine) + ": " + mesh.error().message};
        }
        cells.push_back({std::move(mesh).value(), measured.center, measured.volume});
    }
    return cells;
}

} // namespace ajfx
