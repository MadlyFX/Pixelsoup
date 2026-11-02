#pragma once

#include <ajfx/effect.hpp>
#include <ajfx/math.hpp>
#include <ajfx/simulation.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

// The broadphase particles find another emitter's bodies through
// (BodyCollision, Wake): each step the bodies are sorted into a uniform grid,
// by cell and then by body index, and a particle looks only at the cells
// around it. The CPU reference uses it here; the GPU kernels read the same
// grid (pack.hpp writes it, ajfx_particles.hlsli walks it) and walk it the
// same way.
//
// Exactness. A particle meets bodies in body order whether or not the grid
// is used, and a body it is not touching does nothing to it. So looking at a
// superset of the bodies a particle touches, in body order, is meeting every
// body in turn. The grid only ever narrows the search to bodies whose
// bounding ball, grown by 1% (`body_reach_slack`) and by the particle's own
// contact radius, reaches the particle where it is now; after every body met
// the search starts again from where that body left the particle, at the
// next body index. Where the cells around a particle would be too many
// (`body_grid_reach`), or a position is not a number, every remaining body is
// met in turn instead, which is the same thing again.
namespace ajfx::detail {

// Cells one grid holds at most. Cells are about as wide as the largest body;
// when the bodies spread wider than this many cells, the cells grow (by a
// quarter at a time) until they fit, which is slower and never less exact.
inline constexpr std::uint32_t body_grid_cells = 32768U;
// Cells either side of its own that a particle searches, at most. A reach
// wider than this (a particle far bigger than the cells) meets every body in
// turn.
inline constexpr std::uint32_t body_grid_reach = 3U;
// How far a box's corner is from its middle, in half sizes: sqrt(3) as a
// float. The HLSL twin spells the same literal.
inline constexpr float box_corner = 1.7320508F;
// Every reach the grid searches is grown by this, so rounding can never
// leave out a body that is touched.
inline constexpr float body_reach_slack = 1.01F;
// Floats one body takes in the GPU's copy: position, orientation, half size,
// 1 for a box (as a uint), velocity, spawn index and kind (as uints).
inline constexpr std::uint32_t body_record_floats = 14U;
inline constexpr std::uint32_t no_body = 0xffffffffU;

// How far from its middle a body's surface may be.
[[nodiscard]] inline float body_bound(const BodyCollider& body) noexcept {
    return body.box ? body.half * box_corner : body.half;
}

struct BodyGrid final {
    // In body order, as body_colliders gathered them.
    std::vector<BodyCollider> bodies;
    // False when every body is to be met in turn: no bodies, the broadphase
    // turned off, or a body somewhere no number reaches.
    bool usable{false};
    Vec3 origin{};
    float cell{1.0F};
    float inv_cell{1.0F};
    std::uint32_t nx{0U};
    std::uint32_t ny{0U};
    std::uint32_t nz{0U};
    // The farthest any body's surface is from its middle, and the largest
    // half size.
    float bound{0.0F};
    float half{0.0F};
    // Per cell (x fastest), where its bodies start in `order`; one more entry
    // than cells, the last being the body count.
    std::vector<std::uint32_t> start;
    // Body indices by cell, ascending within each cell.
    std::vector<std::uint32_t> order;
};

// Grids by the name of the emitter whose bodies they hold.
using BodyGrids = std::map<std::string, BodyGrid, std::less<>>;

// Sorts `bodies` into a grid; `broadphase` false leaves it unusable, so every
// body is met in turn. Deterministic: a counting sort over cells in body
// order, the cell size grown by fixed steps until the cells fit.
[[nodiscard]] inline BodyGrid make_body_grid(std::vector<BodyCollider> bodies, bool broadphase) {
    BodyGrid grid;
    grid.bodies = std::move(bodies);
    const std::size_t count = grid.bodies.size();
    if (count == 0U) {
        return grid;
    }
    Vec3 lo = grid.bodies[0].position;
    Vec3 hi = lo;
    bool all_finite = true;
    for (const BodyCollider& body : grid.bodies) {
        grid.bound = std::max(grid.bound, body_bound(body));
        grid.half = std::max(grid.half, body.half);
        lo = Vec3{std::min(lo.x, body.position.x), std::min(lo.y, body.position.y), std::min(lo.z, body.position.z)};
        hi = Vec3{std::max(hi.x, body.position.x), std::max(hi.y, body.position.y), std::max(hi.z, body.position.z)};
        all_finite = all_finite && std::isfinite(body.position.x) && std::isfinite(body.position.y) &&
                 std::isfinite(body.position.z) && std::isfinite(body.half);
    }
    if (!broadphase || !all_finite || !std::isfinite(grid.bound)) {
        return grid;
    }
    float width = std::max(grid.bound * 1.02F, 1.0e-4F);
    std::uint32_t dims[3]{};
    std::uint64_t cell_count = 0U;
    for (std::uint32_t attempt = 0U; attempt < 1024U && cell_count == 0U; ++attempt) {
        const float inverse = 1.0F / width;
        // A body's cell is floor((its position - lo) * inverse), at most
        // floor((hi - lo) * inverse): rounding is monotonic, so these hold
        // every body.
        const float spans[3] = {(hi.x - lo.x) * inverse, (hi.y - lo.y) * inverse, (hi.z - lo.z) * inverse};
        std::uint64_t product = 1U;
        bool fits = true;
        for (std::size_t axis = 0U; axis < 3U && fits; ++axis) {
            if (!(spans[axis] < static_cast<float>(body_grid_cells))) {
                fits = false;
                break;
            }
            dims[axis] = static_cast<std::uint32_t>(std::floor(spans[axis])) + 1U;
            product *= dims[axis];
            fits = product <= body_grid_cells;
        }
        if (fits) {
            cell_count = product;
            grid.cell = width;
            grid.inv_cell = inverse;
        } else {
            width *= 1.25F;
        }
    }
    if (cell_count == 0U) {
        return grid;
    }
    grid.origin = lo;
    grid.nx = dims[0];
    grid.ny = dims[1];
    grid.nz = dims[2];
    grid.start.assign(static_cast<std::size_t>(cell_count) + 1U, 0U);
    std::vector<std::uint32_t> cell_of(count);
    for (std::size_t i = 0U; i < count; ++i) {
        const Vec3 p = grid.bodies[i].position;
        const auto coordinate = [&grid](float offset, std::uint32_t dim) {
            const float c = std::floor(offset * grid.inv_cell);
            return c <= 0.0F ? 0U : std::min(static_cast<std::uint32_t>(c), dim - 1U);
        };
        const std::uint32_t x = coordinate(p.x - lo.x, grid.nx);
        const std::uint32_t y = coordinate(p.y - lo.y, grid.ny);
        const std::uint32_t z = coordinate(p.z - lo.z, grid.nz);
        cell_of[i] = (z * grid.ny + y) * grid.nx + x;
        ++grid.start[cell_of[i] + 1U];
    }
    for (std::size_t c = 0U; c + 1U < grid.start.size(); ++c) {
        grid.start[c + 1U] += grid.start[c];
    }
    grid.order.resize(count);
    std::vector<std::uint32_t> fill(grid.start.begin(), grid.start.end() - 1);
    for (std::size_t i = 0U; i < count; ++i) {
        grid.order[fill[cell_of[i]]++] = static_cast<std::uint32_t>(i);
    }
    grid.usable = true;
    return grid;
}

// The cells around a particle that may hold a body reaching it.
struct CellSpan final {
    std::uint32_t lo[3]{};
    std::uint32_t hi[3]{};
    // No cell of the grid is near enough.
    bool none{false};
};

// The cells a body whose surface comes within `reach` of `p` can be in.
// False when every body is to be looked at instead: the grid is unusable,
// the reach spans more than body_grid_reach cells either side, or the
// position is not a number. As ajfx_cell_span does it.
[[nodiscard]] inline bool cell_span(const BodyGrid& grid, Vec3 p, float reach, CellSpan& span) {
    if (!grid.usable) {
        return false;
    }
    const float k = std::ceil(reach * body_reach_slack * grid.inv_cell);
    if (!(k <= static_cast<float>(body_grid_reach))) {
        return false;
    }
    const float at[3] = {std::floor((p.x - grid.origin.x) * grid.inv_cell),
                         std::floor((p.y - grid.origin.y) * grid.inv_cell),
                         std::floor((p.z - grid.origin.z) * grid.inv_cell)};
    const std::uint32_t dims[3] = {grid.nx, grid.ny, grid.nz};
    span = CellSpan{};
    for (std::size_t axis = 0U; axis < 3U; ++axis) {
        if (std::isnan(at[axis])) {
            return false;
        }
    }
    for (std::size_t axis = 0U; axis < 3U; ++axis) {
        const float first = at[axis] - k;
        const float last = at[axis] + k;
        const float top = static_cast<float>(dims[axis] - 1U);
        if (last < 0.0F || first > top) {
            span.none = true;
            return true;
        }
        span.lo[axis] = first <= 0.0F ? 0U : static_cast<std::uint32_t>(first);
        span.hi[axis] = last >= top ? dims[axis] - 1U : static_cast<std::uint32_t>(last);
    }
    return true;
}

// Calls `visit` with the index of every body in `span`'s cells, cell by
// cell: an order a caller must not depend on.
template <class Visit>
void for_each_in_span(const BodyGrid& grid, const CellSpan& span, Visit&& visit) {
    if (span.none) {
        return;
    }
    for (std::uint32_t z = span.lo[2]; z <= span.hi[2]; ++z) {
        for (std::uint32_t y = span.lo[1]; y <= span.hi[1]; ++y) {
            for (std::uint32_t x = span.lo[0]; x <= span.hi[0]; ++x) {
                const std::uint32_t c = (z * grid.ny + y) * grid.nx + x;
                for (std::uint32_t j = grid.start[c]; j < grid.start[c + 1U]; ++j) {
                    visit(grid.order[j]);
                }
            }
        }
    }
}

// The first body at or after `from`, in body order, among `span`'s cells,
// whose bounding ball grown by `extra` (and the slack) reaches `p`; no_body
// if none does. As ajfx_next_body does it.
[[nodiscard]] inline std::uint32_t next_body(const BodyGrid& grid, const CellSpan& span, Vec3 p, float extra,
                                             std::uint32_t from) {
    std::uint32_t best = no_body;
    if (span.none) {
        return best;
    }
    for (std::uint32_t z = span.lo[2]; z <= span.hi[2]; ++z) {
        for (std::uint32_t y = span.lo[1]; y <= span.hi[1]; ++y) {
            for (std::uint32_t x = span.lo[0]; x <= span.hi[0]; ++x) {
                const std::uint32_t c = (z * grid.ny + y) * grid.nx + x;
                for (std::uint32_t j = grid.start[c]; j < grid.start[c + 1U]; ++j) {
                    const std::uint32_t b = grid.order[j];
                    if (b >= best) {
                        break;
                    }
                    if (b < from) {
                        continue;
                    }
                    const BodyCollider& body = grid.bodies[b];
                    const Vec3 d = p - body.position;
                    const float r = (body_bound(body) + extra) * body_reach_slack;
                    if (dot(d, d) <= r * r) {
                        best = b;
                        break;
                    }
                }
            }
        }
    }
    return best;
}

// Meets bodies as meeting every body in turn would, through the grid: calls
// `meet` with each body the particle may be touching, in body order, asking
// `position` where the particle is after every one. `extra` is how far the
// particle itself reaches (a rigid particle's contact radius).
template <class Position, class Meet>
void meet_bodies(const BodyGrid& grid, Position&& position, float extra, Meet&& meet) {
    const auto count = static_cast<std::uint32_t>(grid.bodies.size());
    const float grown = std::max(extra, 0.0F);
    std::uint32_t from = 0U;
    while (from < count) {
        CellSpan span{};
        if (!cell_span(grid, position(), grid.bound + grown, span)) {
            for (std::uint32_t b = from; b < count; ++b) {
                meet(grid.bodies[b]);
            }
            return;
        }
        const std::uint32_t b = next_body(grid, span, position(), grown, from);
        if (b == no_body) {
            return;
        }
        meet(grid.bodies[b]);
        from = b + 1U;
    }
}

// Calls `visit` with the index of every body that may be within `reach` of
// `p`: through the grid where it can, every body where it cannot. In no
// order a caller may depend on.
template <class Visit>
void visit_near(const BodyGrid& grid, Vec3 p, float reach, Visit&& visit) {
    CellSpan span{};
    if (!cell_span(grid, p, reach, span)) {
        for (std::uint32_t b = 0U; b < static_cast<std::uint32_t>(grid.bodies.size()); ++b) {
            visit(b);
        }
        return;
    }
    for_each_in_span(grid, span, visit);
}

// The wake a particle at `p` follows (Wake): the body whose wake is
// strongest there, the earlier one on a tie, and how strong; no_body when
// none reaches it. Independent of the order bodies are looked at in. As
// ajfx_wake does it.
struct WakePick final {
    std::uint32_t body{no_body};
    float weight{0.0F};
};

[[nodiscard]] inline WakePick strongest_wake(const BodyGrid& grid, const Wake& m, Vec3 p) {
    WakePick pick;
    const float slowest = m.min_speed * m.min_speed;
    visit_near(grid, p, m.radius * grid.half, [&](std::uint32_t b) {
        const BodyCollider& body = grid.bodies[b];
        if (dot(body.velocity, body.velocity) < slowest) {
            return;
        }
        if (!m.bodies.empty() && !in_scope(m.bodies, body.spawn_index, body.kind)) {
            return;
        }
        const Vec3 d = p - body.position;
        const float distance = std::sqrt(dot(d, d));
        const float r = m.radius * body.half;
        if (!(distance < r)) {
            return;
        }
        const float w = 1.0F - distance / r;
        if (w > pick.weight || (w == pick.weight && b < pick.body)) {
            pick.body = b;
            pick.weight = w;
        }
    });
    return pick;
}

// The GPU's copy of the bodies, body_record_floats each, in body order.
inline void body_records(const BodyGrid& grid, std::vector<float>& out) {
    out.clear();
    out.reserve(grid.bodies.size() * body_record_floats);
    for (const BodyCollider& body : grid.bodies) {
        out.insert(out.end(), {body.position.x, body.position.y, body.position.z, body.orientation.x,
                               body.orientation.y, body.orientation.z, body.orientation.w, body.half,
                               std::bit_cast<float>(body.box ? 1U : 0U), body.velocity.x, body.velocity.y,
                               body.velocity.z, std::bit_cast<float>(body.spawn_index),
                               std::bit_cast<float>(body.kind)});
    }
}

} // namespace ajfx::detail
