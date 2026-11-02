#include "check.hpp"

#include <ajfx/fracture.hpp>
#include <ajfx/mesh.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace ajfx;

[[nodiscard]] float length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

// A model to cut, from its corners alone: fracture reads a model's vertex
// positions and never its triangles, so a shape under test needs no faces.
// The one triangle is there because a MeshData holds triangles.
[[nodiscard]] MeshData corners(const std::vector<Vec3>& points) {
    MeshData model;
    for (const Vec3& point : points) {
        model.vertices.push_back({point, {0.0F, 1.0F, 0.0F}, 0.0F, 0.0F});
    }
    model.indices = {0U, 1U, 2U};
    return model;
}

// An L beam: a unit square with a quarter bitten out of one corner, a unit
// deep. Its own volume is 0.75; the hull of its corners loses the bite to a
// bevel from (1, 0.5) to (0.5, 1) and holds 0.875.
[[nodiscard]] MeshData l_beam() {
    const float ring[6][2] = {{0.0F, 0.0F}, {1.0F, 0.0F}, {1.0F, 0.5F}, {0.5F, 0.5F}, {0.5F, 1.0F}, {0.0F, 1.0F}};
    std::vector<Vec3> points;
    for (const auto& corner : ring) {
        points.push_back({corner[0], corner[1], -0.5F});
        points.push_back({corner[0], corner[1], 0.5F});
    }
    return corners(points);
}

// Points spread evenly over a sphere of radius `radius` about `middle`, down
// the spiral a sunflower's seeds sit on. A hull of many faces, none of them
// axis aligned, and a volume to hold the answer against: a little under the
// sphere's own, by the slices the flat faces cut off it.
[[nodiscard]] MeshData sphere_cloud(std::uint32_t count, Vec3 middle, float radius) {
    const float golden = 2.39996323F; // the golden angle, in radians
    std::vector<Vec3> points;
    for (std::uint32_t i = 0U; i < count; ++i) {
        const float up = 1.0F - 2.0F * (static_cast<float>(i) + 0.5F) / static_cast<float>(count);
        const float around = std::sqrt(std::max(0.0F, 1.0F - up * up));
        const float turn = golden * static_cast<float>(i);
        points.push_back(middle + Vec3{around * std::cos(turn), up, around * std::sin(turn)} * radius);
    }
    return corners(points);
}

// One triangle of a cell, with its corners and both of its normals.
struct Triangle final {
    Vec3 a;
    Vec3 b;
    Vec3 c;
    // The plane the cell was cut by, as the mesh carries it on all three
    // corners, and the plane the three corners actually span. A sliver of a
    // triangle spans its plane badly, so the second is only worth comparing
    // with the first where `area` is a real area.
    Vec3 carried;
    Vec3 spanned;
    float area{};
};

[[nodiscard]] std::vector<Triangle> triangles(const MeshData& mesh) {
    std::vector<Triangle> out;
    for (std::size_t i = 0U; i + 2U < mesh.indices.size(); i += 3U) {
        const MeshVertex& a = mesh.vertices[mesh.indices[i]];
        const MeshVertex& b = mesh.vertices[mesh.indices[i + 1U]];
        const MeshVertex& c = mesh.vertices[mesh.indices[i + 2U]];
        Triangle triangle{a.position, b.position, c.position, a.normal, {}, 0.0F};
        const Vec3 twice_area = cross(b.position - a.position, c.position - a.position);
        triangle.area = length(twice_area) * 0.5F;
        if (triangle.area > 0.0F) {
            triangle.spanned = twice_area * (1.0F / length(twice_area));
        }
        out.push_back(triangle);
    }
    return out;
}

// How far outside a convex cell a point lies: positive outside, negative in,
// and the distance to the nearest face either way. Measured against the
// planes the cell was cut by, since those are exact where a sliver triangle's
// own corners only say roughly which way its face looks.
[[nodiscard]] float depth(const std::vector<Triangle>& faces, Vec3 point) {
    float outside = -std::numeric_limits<float>::max();
    for (const Triangle& face : faces) {
        outside = std::max(outside, dot(face.carried, point - face.a));
    }
    return outside;
}

// The volume a closed mesh encloses, from the signed tetrahedra its own
// triangles make with the origin: an expectation worked out from the
// geometry, not the number fracture reports alongside it.
[[nodiscard]] float enclosed_volume(const MeshData& mesh) {
    float sixfold = 0.0F;
    for (const Triangle& face : triangles(mesh)) {
        sixfold += dot(face.a, cross(face.b, face.c));
    }
    return sixfold * (1.0F / 6.0F);
}

[[nodiscard]] float total_volume(const std::vector<FractureCell>& cells) {
    float total = 0.0F;
    for (const FractureCell& cell : cells) {
        total += cell.volume;
    }
    return total;
}

[[nodiscard]] bool fails_with(const Result<std::vector<FractureCell>>& result, ErrorCode code,
                              const std::string& fragment) {
    if (result.ok()) {
        return false;
    }
    std::cout << "  refused: " << result.error().message << '\n';
    return result.error().code == code && result.error().message.find(fragment) != std::string::npos;
}

void a_unit_cube_cuts_into_cells_that_fill_it() {
    for (const std::uint32_t count : {1U, 2U, 7U, 12U, 40U}) {
        const Result<std::vector<FractureCell>> cells = fracture(cube_mesh(), {.count = count, .seed = 7U});
        AJFX_CHECK(cells.ok());
        AJFX_CHECK(cells.value().size() == count);
        float smallest = 1.0F;
        float largest = 0.0F;
        for (const FractureCell& cell : cells.value()) {
            AJFX_CHECK(cell.volume > 0.0F);
            smallest = std::min(smallest, cell.volume);
            largest = std::max(largest, cell.volume);
            // The middle of a piece is inside that piece, and inside the cube
            // the piece came from.
            AJFX_CHECK(depth(triangles(cell.mesh), cell.center) < -1.0e-4F);
            AJFX_CHECK(std::fabs(cell.center.x) < 0.5F && std::fabs(cell.center.y) < 0.5F &&
                       std::fabs(cell.center.z) < 0.5F);
        }
        const float total = total_volume(cells.value());
        std::cout << "  " << count << " cells fill the cube to within " << total - 1.0F << "; smallest " << smallest
                  << ", largest " << largest << '\n';
        AJFX_CHECK(std::fabs(total - 1.0F) < 1.0e-4F);
    }
}

void a_hull_of_many_faces_still_fills_up() {
    // One cell is the hull itself, which is the measure the many-celled cuts
    // are held against; the sphere it came from is the sanity check on that.
    const MeshData cloud = sphere_cloud(150U, Vec3{0.0F, 0.0F, 0.0F}, 1.0F);
    const Result<std::vector<FractureCell>> whole = fracture(cloud, {.count = 1U});
    AJFX_CHECK(whole.ok());
    const float hull = total_volume(whole.value());
    std::cout << "  the hull of 150 points on a unit sphere holds " << hull << ", against the sphere's 4.18879\n";
    AJFX_CHECK(hull > 4.0F && hull < 4.18879F);
    AJFX_CHECK(whole.value()[0].mesh.indices.size() / 3U >= 150U); // a face for each point it touches

    for (const std::uint32_t count : {17U, 60U}) {
        const Result<std::vector<FractureCell>> cells = fracture(cloud, {.count = count, .seed = 99U});
        AJFX_CHECK(cells.ok());
        AJFX_CHECK(cells.value().size() == count);
        const float total = total_volume(cells.value());
        std::cout << "  " << count << " cells fill it to " << total << ", off by " << total - hull << '\n';
        AJFX_CHECK(std::fabs(total - hull) < 1.0e-3F);
        for (const FractureCell& cell : cells.value()) {
            AJFX_CHECK(cell.volume > 0.0F);
            AJFX_CHECK(depth(triangles(cell.mesh), cell.center) < 0.0F);
        }
    }
}

void a_detailed_model_cuts_as_quickly_as_a_simple_one() {
    // 60,000 corners, as a scanned or downloaded prop has: the hull is built
    // from a fixed number of them, so this takes a moment rather than the
    // minutes every corner against every face would. Nothing of the model is
    // left out: every corner is inside the one-cell cut.
    const MeshData dense = sphere_cloud(60000U, Vec3{0.25F, 1.0F, -0.5F}, 1.0F);
    const auto start = std::chrono::steady_clock::now();
    const Result<std::vector<FractureCell>> whole = fracture(dense, {.count = 1U});
    const Result<std::vector<FractureCell>> cells = fracture(dense, {.count = 24U, .seed = 5U});
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    AJFX_CHECK(whole.ok() && cells.ok());
    const float hull = total_volume(whole.value());
    std::cout << "  60,000 corners cut twice in " << seconds << " s; the hull holds " << hull
              << " against the sphere's 4.18879\n";
    // Its faces touch the sphere rather than cut inside it, so it holds a
    // little more than the sphere, never less, and not much more.
    AJFX_CHECK(hull > 4.18879F && hull < 4.18879F * 1.03F);
    const std::vector<Triangle> faces = triangles(whole.value()[0].mesh);
    float worst = -1.0F;
    for (const MeshVertex& corner : dense.vertices) {
        worst = std::max(worst, depth(faces, corner.position));
    }
    std::cout << "  the farthest corner is " << worst << " outside the cut\n";
    AJFX_CHECK(worst < 1.0e-4F);
    AJFX_CHECK(cells.value().size() == 24U);
    AJFX_CHECK(std::fabs(total_volume(cells.value()) - hull) < 1.0e-3F);
    // And it is still one model, one cut.
    const Result<std::vector<FractureCell>> again = fracture(dense, {.count = 24U, .seed = 5U});
    AJFX_CHECK(again.ok() && again.value().size() == cells.value().size());
    for (std::size_t i = 0U; i < cells.value().size() && i < again.value().size(); ++i) {
        AJFX_CHECK(again.value()[i].mesh.vertices.size() == cells.value()[i].mesh.vertices.size());
        AJFX_CHECK(again.value()[i].volume == cells.value()[i].volume);
    }
}

void the_same_model_in_other_units_cuts_the_same_way() {
    // Every tolerance in the cutting is a share of the model's own size, so a
    // model a hundred times the size and a long way from the origin has to
    // break into the same pieces, a hundred times over.
    constexpr float factor = 100.0F;
    const Vec3 away{1000.0F, -500.0F, 2000.0F};
    const FractureOptions options{.count = 20U, .seed = 61U};
    const Result<std::vector<FractureCell>> small = fracture(cube_mesh(), options);
    std::vector<Vec3> moved;
    for (const MeshVertex& vertex : cube_mesh().vertices) {
        moved.push_back(away + vertex.position * factor);
    }
    const Result<std::vector<FractureCell>> large = fracture(corners(moved), options);
    AJFX_CHECK(small.ok() && large.ok());
    AJFX_CHECK(small.value().size() == large.value().size());

    float worst_volume = 0.0F;
    float worst_center = 0.0F;
    for (std::size_t i = 0U; i < small.value().size(); ++i) {
        const float expected = small.value()[i].volume * factor * factor * factor;
        worst_volume = std::max(worst_volume, std::fabs(large.value()[i].volume - expected) / expected);
        const Vec3 where = away + small.value()[i].center * factor;
        worst_center = std::max(worst_center, length(large.value()[i].center - where) / factor);
    }
    std::cout << "  20 cells at a hundred times the size: volumes within " << worst_volume * 100.0F
              << "% and middles within " << worst_center << " of a unit\n";
    AJFX_CHECK(worst_volume < 1.0e-4F);
    AJFX_CHECK(worst_center < 1.0e-4F);
}

void every_cell_is_a_closed_convex_solid() {
    const Result<std::vector<FractureCell>> cells = fracture(cube_mesh(), {.count = 16U, .seed = 21U});
    AJFX_CHECK(cells.ok());
    std::size_t slivers = 0U;
    std::size_t faces = 0U;
    float worst_outside = -1.0F;
    float worst_tilt = 1.0F;
    for (const FractureCell& cell : cells.value()) {
        const std::vector<Triangle> mesh_faces = triangles(cell.mesh);
        faces += mesh_faces.size();

        // Corners welded by position: a cell's faces each carry their own
        // normal, so the same corner is several vertices of the mesh.
        std::vector<Vec3> welded;
        const auto weld = [&welded](Vec3 point) {
            for (std::uint32_t i = 0U; i < static_cast<std::uint32_t>(welded.size()); ++i) {
                if (length(welded[i] - point) <= 1.0e-5F) {
                    return i;
                }
            }
            welded.push_back(point);
            return static_cast<std::uint32_t>(welded.size() - 1U);
        };
        std::vector<std::pair<std::uint32_t, std::uint32_t>> walked;
        for (const Triangle& face : mesh_faces) {
            const std::uint32_t a = weld(face.a);
            const std::uint32_t b = weld(face.b);
            const std::uint32_t c = weld(face.c);
            walked.push_back({a, b});
            walked.push_back({b, c});
            walked.push_back({c, a});
        }
        // Closed and wound one way: every edge is walked twice, once each way.
        for (const auto& [from, to] : walked) {
            AJFX_CHECK(std::count(walked.begin(), walked.end(), std::pair{from, to}) == 1);
            AJFX_CHECK(std::count(walked.begin(), walked.end(), std::pair{to, from}) == 1);
        }
        // Convex: no corner of a cell is on the outside of any of its faces.
        for (const Vec3& corner : welded) {
            worst_outside = std::max(worst_outside, depth(mesh_faces, corner));
        }
        for (const Triangle& face : mesh_faces) {
            // Looking outwards, away from the middle of the piece.
            AJFX_CHECK(dot(face.carried, face.a - cell.center) > 0.0F);
            AJFX_CHECK(std::fabs(length(face.carried) - 1.0F) < 1.0e-5F);
            // Wound to match the normal it carries, where there is enough of
            // the triangle to tell.
            if (face.area > 1.0e-4F) {
                worst_tilt = std::min(worst_tilt, dot(face.spanned, face.carried));
            } else {
                ++slivers;
            }
        }
        AJFX_CHECK(cell.volume > 0.0F);
        AJFX_CHECK(std::fabs(enclosed_volume(cell.mesh) - cell.volume) < 1.0e-5F);
    }
    std::cout << "  16 cells, " << faces << " triangles, " << slivers << " of them slivers; no corner more than "
              << worst_outside << " outside its own cell, no face tilted past " << worst_tilt << '\n';
    AJFX_CHECK(worst_outside < 1.0e-5F);
    AJFX_CHECK(worst_tilt > 0.999F);
}

void cells_do_not_overlap_and_leave_no_gap() {
    const Result<std::vector<FractureCell>> cells = fracture(cube_mesh(), {.count = 10U, .seed = 3U});
    AJFX_CHECK(cells.ok());
    std::vector<std::vector<Triangle>> faces;
    for (const FractureCell& cell : cells.value()) {
        faces.push_back(triangles(cell.mesh));
    }

    // A grid through the cube, each point of it owned by exactly one cell.
    // The boundary tolerance is the width of the no man's land either side of
    // a cut: a point in it may be claimed by both cells, so the two counts
    // bracket the answer instead of fixing it.
    constexpr float boundary = 1.0e-4F;
    constexpr std::uint32_t steps = 13U;
    std::uint32_t sampled = 0U;
    std::uint32_t decided = 0U;
    for (std::uint32_t x = 0U; x < steps; ++x) {
        for (std::uint32_t y = 0U; y < steps; ++y) {
            for (std::uint32_t z = 0U; z < steps; ++z) {
                const auto coordinate = [](std::uint32_t i) {
                    return -0.46F + 0.92F * (static_cast<float>(i) / static_cast<float>(steps - 1U));
                };
                const Vec3 point{coordinate(x), coordinate(y), coordinate(z)};
                std::uint32_t within = 0U;
                std::uint32_t touching = 0U;
                for (const std::vector<Triangle>& cell : faces) {
                    const float how_deep = depth(cell, point);
                    within += how_deep < -boundary ? 1U : 0U;
                    touching += how_deep <= boundary ? 1U : 0U;
                }
                AJFX_CHECK(within <= 1U);  // no two cells share the point
                AJFX_CHECK(touching >= 1U); // and no cell left it out
                ++sampled;
                decided += within == 1U ? 1U : 0U;
            }
        }
    }
    std::cout << "  " << sampled << " points sampled, " << decided << " of them strictly inside one cell\n";
    AJFX_CHECK(decided * 100U >= sampled * 98U);
}

void the_same_options_cut_the_same_bits() {
    const FractureOptions options{.count = 9U, .seed = 1234U, .focus = {0.1F, -0.2F, 0.3F}, .focus_bias = 0.35F};
    const Result<std::vector<FractureCell>> once = fracture(cube_mesh(), options);
    const Result<std::vector<FractureCell>> twice = fracture(cube_mesh(), options);
    AJFX_CHECK(once.ok() && twice.ok());
    AJFX_CHECK(once.value().size() == twice.value().size());
    for (std::size_t i = 0U; i < once.value().size(); ++i) {
        const FractureCell& a = once.value()[i];
        const FractureCell& b = twice.value()[i];
        // Through the library's own writer, so the comparison is over every
        // position, normal and texture coordinate in the piece.
        AJFX_CHECK(write_mesh(a.mesh) == write_mesh(b.mesh));
        AJFX_CHECK(std::memcmp(&a.center, &b.center, sizeof(Vec3)) == 0);
        AJFX_CHECK(std::memcmp(&a.volume, &b.volume, sizeof(float)) == 0);
    }

    FractureOptions other = options;
    other.seed = 1235U;
    const Result<std::vector<FractureCell>> elsewhere = fracture(cube_mesh(), other);
    AJFX_CHECK(elsewhere.ok());
    std::size_t differing = 0U;
    for (std::size_t i = 0U; i < once.value().size(); ++i) {
        differing += write_mesh(once.value()[i].mesh) == write_mesh(elsewhere.value()[i].mesh) ? 0U : 1U;
    }
    std::cout << "  seed 1235 cuts " << differing << " of 9 cells differently; both fill "
              << total_volume(once.value()) << " and " << total_volume(elsewhere.value()) << '\n';
    AJFX_CHECK(differing == once.value().size());
    AJFX_CHECK(std::fabs(total_volume(elsewhere.value()) - 1.0F) < 1.0e-4F);
}

void focus_bias_crowds_the_cells_around_the_focus() {
    const Vec3 focus{0.25F, 0.10F, -0.20F};
    // The cells within a quarter of the cube's side of the impact, how big
    // they are on average, and the smallest piece anywhere.
    struct Crowding final {
        std::uint32_t near{};
        float near_volume{};
        float smallest{};
    };
    const auto measure = [focus](float bias) {
        const Result<std::vector<FractureCell>> cells =
            fracture(cube_mesh(), {.count = 32U, .seed = 404U, .focus = focus, .focus_bias = bias});
        AJFX_CHECK(cells.ok());
        AJFX_CHECK(std::fabs(total_volume(cells.value()) - 1.0F) < 1.0e-4F);
        Crowding crowding;
        crowding.smallest = 1.0F;
        for (const FractureCell& cell : cells.value()) {
            crowding.smallest = std::min(crowding.smallest, cell.volume);
            if (length(cell.center - focus) <= 0.25F) {
                ++crowding.near;
                crowding.near_volume += cell.volume;
            }
        }
        if (crowding.near > 0U) {
            crowding.near_volume /= static_cast<float>(crowding.near);
        }
        return crowding;
    };

    const Crowding even = measure(0.0F);
    const Crowding clustered = measure(1.0F);
    std::cout << "  bias 0: " << even.near << " cells within 0.25 of the impact, " << even.near_volume
              << " each on average, smallest piece " << even.smallest << '\n';
    std::cout << "  bias 1: " << clustered.near << " cells within 0.25 of the impact, " << clustered.near_volume
              << " each on average, smallest piece " << clustered.smallest << '\n';
    AJFX_CHECK(clustered.near >= even.near * 2U);
    AJFX_CHECK(clustered.near_volume < even.near_volume * 0.5F);
    AJFX_CHECK(clustered.smallest < even.smallest * 0.25F);
}

void a_concave_model_fractures_as_its_hull_not_its_hollow() {
    const Result<std::vector<FractureCell>> cells = fracture(l_beam(), {.count = 14U, .seed = 5U});
    AJFX_CHECK(cells.ok());
    const float total = total_volume(cells.value());
    std::cout << "  the L beam's 14 cells fill " << total << "; its hull holds 0.875 and the beam itself 0.75\n";
    AJFX_CHECK(std::fabs(total - 0.875F) < 2.0e-3F);
    // Which is the limit, stated as a number: the bite out of the corner is
    // not in the beam, and the pieces fill it anyway.
    AJFX_CHECK(total > 0.8F);
    const Vec3 in_the_bite{0.8F, 0.6F, 0.0F};
    std::uint32_t owning = 0U;
    for (const FractureCell& cell : cells.value()) {
        owning += depth(triangles(cell.mesh), in_the_bite) <= 1.0e-4F ? 1U : 0U;
    }
    AJFX_CHECK(owning >= 1U);
}

void nothing_to_cut_is_refused_with_the_reason_named() {
    const MeshData cube = cube_mesh();
    AJFX_CHECK(fails_with(fracture(cube, {.count = 0U}), ErrorCode::InvalidEffect, "'count'"));
    AJFX_CHECK(fails_with(fracture(cube, {.count = max_fracture_cells + 1U}), ErrorCode::BudgetExceeded, "1024"));
    AJFX_CHECK(fails_with(fracture(cube, {.focus = {0.0F, std::numeric_limits<float>::infinity(), 0.0F}}),
                          ErrorCode::InvalidEffect, "'focus'"));
    AJFX_CHECK(fails_with(fracture(cube, {.focus_bias = 1.5F}), ErrorCode::InvalidEffect, "'focus_bias'"));

    AJFX_CHECK(fails_with(fracture(corners({{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}}), {}),
                          ErrorCode::InvalidEffect, "four"));
    AJFX_CHECK(fails_with(fracture(corners({{0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 0.0F},
                                            {0.0F, 0.0F, 0.0F}}),
                                   {}),
                          ErrorCode::InvalidEffect, "same place"));
    AJFX_CHECK(fails_with(fracture(corners({{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {2.0F, 0.0F, 0.0F},
                                            {3.0F, 0.0F, 0.0F}}),
                                   {}),
                          ErrorCode::InvalidEffect, "one line"));
    AJFX_CHECK(fails_with(fracture(corners({{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 0.0F},
                                            {0.0F, 1.0F, 0.0F}}),
                                   {}),
                          ErrorCode::InvalidEffect, "one plane"));
    AJFX_CHECK(fails_with(fracture(corners({{0.0F, 0.0F, 0.0F},
                                            {std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F},
                                            {1.0F, 1.0F, 0.0F},
                                            {0.0F, 1.0F, 1.0F}}),
                                   {}),
                          ErrorCode::InvalidEffect, "not a number"));
    // A sheet of a model has room for a piece or two, not a hundred.
    AJFX_CHECK(fails_with(fracture(corners({{0.0F, 0.0F, 0.0F},
                                            {1.0F, 0.0F, 0.0F},
                                            {0.0F, 1.0F, 0.0F},
                                            {0.0F, 0.0F, 1.0e-4F}}),
                                   {.count = 200U}),
                          ErrorCode::InvalidEffect, "room"));
}

} // namespace

int main() {
    const ajfx::test::Case cases[] = {
        {"a_unit_cube_cuts_into_cells_that_fill_it", a_unit_cube_cuts_into_cells_that_fill_it},
        {"a_hull_of_many_faces_still_fills_up", a_hull_of_many_faces_still_fills_up},
        {"a_detailed_model_cuts_as_quickly_as_a_simple_one", a_detailed_model_cuts_as_quickly_as_a_simple_one},
        {"the_same_model_in_other_units_cuts_the_same_way", the_same_model_in_other_units_cuts_the_same_way},
        {"every_cell_is_a_closed_convex_solid", every_cell_is_a_closed_convex_solid},
        {"cells_do_not_overlap_and_leave_no_gap", cells_do_not_overlap_and_leave_no_gap},
        {"the_same_options_cut_the_same_bits", the_same_options_cut_the_same_bits},
        {"focus_bias_crowds_the_cells_around_the_focus", focus_bias_crowds_the_cells_around_the_focus},
        {"a_concave_model_fractures_as_its_hull_not_its_hollow", a_concave_model_fractures_as_its_hull_not_its_hollow},
        {"nothing_to_cut_is_refused_with_the_reason_named", nothing_to_cut_is_refused_with_the_reason_named},
    };
    return ajfx::test::run(cases);
}
