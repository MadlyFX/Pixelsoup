#include "scene_stats.hpp"

#include <algorithm>

namespace ajfx::studio {
namespace {
// "12,480": the core has no widgets, so it groups the digits itself.
std::string count(std::uint64_t value) {
    std::string digits = std::to_string(value);
    for (std::size_t at = digits.size(); at > 3U; at -= 3U) {
        digits.insert(at - 3U, ",");
    }
    return digits;
}
std::string of(std::uint64_t used, std::uint64_t limit) {
    return count(used) + " of " + count(limit);
}
} // namespace

SceneStats scene_stats(const Scene &scene, const Compiled *compiled,
                       const std::map<std::uint32_t, std::uint64_t> *peak_alive) {
    SceneStats out;
    for (const SceneItem &item : scene.items) {
        if (std::holds_alternative<Object>(item.data)) {
            ++out.objects;
        } else if (std::holds_alternative<ParticleSystem>(item.data)) {
            ++out.systems;
            std::size_t acting = 0U;
            for (const SceneItem &other : scene.items) {
                const auto *force = std::get_if<Force>(&other.data);
                if (force && force->kind != ForceKind::Blast && other.simulate && force_targets(*force, item.uid)) {
                    ++acting;
                }
            }
            out.forces_on_busiest = std::max(out.forces_on_busiest, acting);
        } else if (std::holds_alternative<Force>(item.data)) {
            ++out.forces;
        } else if (const auto *grid = std::get_if<SmokeGrid>(&item.data)) {
            ++out.grids;
            const std::uint64_t cells = grid_cells(*grid);
            out.grid_cells += cells;
            out.largest_grid_cells = std::max(out.largest_grid_cells, cells);
            out.grid_bytes += cells * 108U;
        } else {
            ++out.groups;
        }
    }
    out.relationships = scene.relationships.size();
    if (compiled != nullptr && compiled->effect) {
        out.particle_capacity = compiled->total_capacity();
        out.blasts = compiled->effect->blasts.size();
        for (const Emitter &emitter : compiled->effect->emitters) {
            if (const RigidPhysics *physics = rigid_physics(emitter)) {
                out.body_capacity += emitter.capacity;
                out.props += physics->props.size();
                out.body_kinds += physics->bodies.size();
            }
        }
        for (const LayerReport &report : compiled->layers) {
            if (report.included) {
                out.state_bytes += report.bytes;
            }
        }
    }
    if (peak_alive != nullptr) {
        out.traced = true;
        for (const auto &[layer, peak] : *peak_alive) {
            out.peak_particles += peak;
        }
    }
    // A limit within reach is said before it is hit: at four fifths of it.
    if (out.body_kinds * 5U >= max_rigid_body_kinds * 4U) {
        out.near_limits.push_back("Body kinds: " + of(out.body_kinds, max_rigid_body_kinds));
    }
    if (out.forces_on_busiest * 5U >= max_forces_per_system * 4U) {
        out.near_limits.push_back("Forces on one system: " + of(out.forces_on_busiest, max_forces_per_system));
    }
    if (out.blasts * 5U >= max_blasts * 4U) {
        out.near_limits.push_back("Blasts: " + of(out.blasts, max_blasts));
    }
    if (out.relationships * 5U >= max_relationships * 4U) {
        out.near_limits.push_back("Relationships: " + of(out.relationships, max_relationships));
    }
    if (out.body_capacity * 5U >= max_rigid_capacity * 4U) {
        out.near_limits.push_back("Bodies: " + of(out.body_capacity, max_rigid_capacity));
    }
    if (out.grids * 5U >= max_smoke_grids * 4U) {
        out.near_limits.push_back("Smoke grids: " + of(out.grids, max_smoke_grids));
    }
    if (out.largest_grid_cells * 5U >= max_volume_cells * 4U) {
        out.near_limits.push_back("Cells in one smoke grid: " + of(out.largest_grid_cells, max_volume_cells));
    }
    return out;
}

std::vector<std::string> stat_lines(const SceneStats &stats) {
    std::vector<std::string> lines;
    if (stats.particle_capacity > 0U) {
        lines.push_back(stats.traced ? "Particles: up to " + count(stats.peak_particles) + " alive, room for " +
                                           count(stats.particle_capacity)
                                     : "Particles: room for " + count(stats.particle_capacity));
    }
    if (stats.body_capacity > 0U || stats.props > 0U) {
        lines.push_back("Bodies: room for " + count(stats.body_capacity) + ", " + count(stats.props) +
                        (stats.props == 1U ? " standing, " : " standing, ") + count(stats.body_kinds) + " kind" +
                        (stats.body_kinds == 1U ? "" : "s") + " of " + count(max_rigid_body_kinds));
    }
    if (stats.state_bytes > 0U) {
        lines.push_back("State: " + count((stats.state_bytes + 1023U) / 1024U) + " KiB");
    }
    if (stats.grids > 0U) {
        lines.push_back("Smoke: " + count(stats.grids) + (stats.grids == 1U ? " grid, " : " grids, ") +
                        count(stats.grid_cells) + " cells, " + count((stats.grid_bytes + 1048575U) / 1048576U) +
                        " MiB on the GPU");
    }
    const std::size_t items = stats.objects + stats.systems + stats.forces + stats.groups + stats.grids;
    lines.push_back(count(items) + (items == 1U ? " item: " : " items: ") + count(stats.objects) +
                    (stats.objects == 1U ? " object, " : " objects, ") + count(stats.systems) +
                    (stats.systems == 1U ? " system, " : " systems, ") + count(stats.forces) +
                    (stats.forces == 1U ? " force, " : " forces, ") + count(stats.relationships) +
                    (stats.relationships == 1U ? " relationship" : " relationships") +
                    (stats.grids == 0U ? std::string{}
                                       : ", " + count(stats.grids) + (stats.grids == 1U ? " smoke grid" : " smoke grids")));
    for (const std::string &line : stats.near_limits) {
        lines.push_back("Near a limit. " + line);
    }
    return lines;
}
} // namespace ajfx::studio
