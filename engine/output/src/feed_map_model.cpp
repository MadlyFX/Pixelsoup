#include <mediaserver/output/feed_map_model.hpp>

#include <mediaserver/output/feed_map_schema.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

namespace mediaserver::output {
namespace {

[[nodiscard]] const doc::PropertyValue* find(
    const doc::PropertyMap& properties,
    std::string_view name) noexcept {
    const auto found = properties.find(name);
    return found == properties.end() ? nullptr : &found->second;
}

// A property reader with the same policy as the scene model's: an absent
// property takes its default, a property present with the wrong type rejects
// the whole node. A feed map is all whole numbers, so this carries one getter.
class Reader final {
public:
    explicit Reader(const doc::PropertyMap& properties) noexcept
        : properties_(&properties) {}

    [[nodiscard]] std::int64_t integer(
        std::string_view name,
        std::int64_t fallback) noexcept {
        const auto* const value = find(*properties_, name);
        if (value == nullptr) {
            return fallback;
        }
        if (const auto* const whole = std::get_if<std::int64_t>(value)) {
            return *whole;
        }
        ok_ = false;
        return fallback;
    }

    [[nodiscard]] float number(std::string_view name,
                               float fallback) noexcept {
        const auto* const value = find(*properties_, name);
        if (value == nullptr) {
            return fallback;
        }
        double wide{};
        if (const auto* const real = std::get_if<double>(value)) {
            wide = *real;
        } else if (const auto* const whole =
                       std::get_if<std::int64_t>(value)) {
            wide = static_cast<double>(*whole);
        } else {
            ok_ = false;
            return fallback;
        }
        const float narrow = static_cast<float>(wide);
        if (!std::isfinite(narrow)) {
            ok_ = false;
            return fallback;
        }
        return narrow;
    }

    [[nodiscard]] std::string_view text(
        std::string_view name,
        std::string_view fallback) noexcept {
        const auto* const value = find(*properties_, name);
        if (value == nullptr) {
            return fallback;
        }
        if (const auto* const string = std::get_if<std::string>(value)) {
            return *string;
        }
        ok_ = false;
        return fallback;
    }

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    void fail() noexcept { ok_ = false; }

private:
    const doc::PropertyMap* properties_;
    bool ok_{true};
};

// An integer property that has to fit an unsigned 32-bit field. Rejects rather
// than clamps: a negative width is not a small width.
[[nodiscard]] bool as_u32(std::int64_t value, core::u32& into) noexcept {
    if (value < 0 || value > static_cast<std::int64_t>(UINT32_MAX)) {
        return false;
    }
    into = static_cast<core::u32>(value);
    return true;
}

} // namespace

core::u32 FeedMapCounters::over_capacity() const noexcept {
    return maps > max_output_maps ? maps - max_output_maps : 0U;
}

core::u32 FeedMapCounters::surfaces_over_capacity() const noexcept {
    return surfaces > max_projection_surfaces
        ? surfaces - max_projection_surfaces
        : 0U;
}

void FeedMapModel::forget(doc::NodeId id) noexcept {
    const auto found = entries_.find(id);
    if (found != entries_.end()) {
        if (counters_.maps != 0U) {
            --counters_.maps;
        }
        entries_.erase(found);
    }
    const auto surface = surfaces_.find(id);
    if (surface != surfaces_.end()) {
        if (counters_.surfaces != 0U) {
            --counters_.surfaces;
        }
        surfaces_.erase(surface);
    }
}

void FeedMapModel::put(const doc::Node& node) {
    // A node that stops being a feed-map node has to stop being a binding, so
    // the old entry goes first whatever the new type turns out to be.
    forget(node.id);

    if (node.type != schema::map_type && node.type != schema::surface_type) {
        // Not a feed-map node. Nothing to hold and nothing to report.
        return;
    }

    Reader reader{node.properties};

    if (node.type == schema::surface_type) {
        ProjectionSurface surface{};
        surface.node = node.id;
        surface.name = std::string{reader.text(schema::surface_name, "Surface")};
        const std::string_view shape =
            reader.text(schema::surface_shape, schema::shape_planar);
        if (shape == schema::shape_planar) {
            surface.shape = ProjectionSurfaceShape::Planar;
        } else if (shape == schema::shape_horizontal_arc) {
            surface.shape = ProjectionSurfaceShape::HorizontalArc;
        } else if (shape == schema::shape_imported_mesh) {
            surface.shape = ProjectionSurfaceShape::ImportedMesh;
        } else {
            reader.fail();
        }
        surface.geometry.center = scene::Vec3{
            reader.number(schema::center_x, 0.0F),
            reader.number(schema::center_y, 0.0F),
            reader.number(schema::center_z, -4.0F)};
        surface.geometry.right = scene::Vec3{
            reader.number(schema::right_x, 1.0F),
            reader.number(schema::right_y, 0.0F),
            reader.number(schema::right_z, 0.0F)};
        surface.geometry.up = scene::Vec3{
            reader.number(schema::up_x, 0.0F),
            reader.number(schema::up_y, 1.0F),
            reader.number(schema::up_z, 0.0F)};
        surface.geometry.width =
            reader.number(schema::physical_width, 8.0F);
        surface.geometry.height =
            reader.number(schema::physical_height, 4.5F);
        core::u32 raster_width{};
        core::u32 raster_height{};
        if (!as_u32(reader.integer(schema::raster_width, 1'920),
                    raster_width) ||
            !as_u32(reader.integer(schema::raster_height, 1'080),
                    raster_height)) {
            reader.fail();
        }
        surface.geometry.raster =
            scene::Canvas{raster_width, raster_height};
        surface.horizontal_arc_turns =
            reader.number(schema::horizontal_arc_turns, 0.25F);
        core::u32 mesh_columns{};
        if (!as_u32(reader.integer(schema::mesh_columns, 32), mesh_columns)) {
            reader.fail();
        }
        surface.mesh_columns = mesh_columns;
        surface.mesh_source =
            std::string{reader.text(schema::mesh_source, "")};
        surface.mesh_scale = reader.number(schema::mesh_scale, 1.0F);
        if (!valid(surface.geometry)) {
            reader.fail();
        }
        if (surface.shape == ProjectionSurfaceShape::HorizontalArc &&
            (!std::isfinite(surface.horizontal_arc_turns) ||
             !(std::abs(surface.horizontal_arc_turns) > 1.0e-5F) ||
             !(std::abs(surface.horizontal_arc_turns) < 0.5F) ||
             surface.mesh_columns < min_horizontal_arc_columns ||
             surface.mesh_columns > max_horizontal_arc_columns)) {
            reader.fail();
        }
        if (surface.shape == ProjectionSurfaceShape::ImportedMesh &&
            (surface.mesh_source.empty() ||
             !std::isfinite(surface.mesh_scale) ||
             !(surface.mesh_scale > 0.0F) ||
             !(surface.mesh_scale <= max_imported_mesh_scale))) {
            reader.fail();
        }
        if (!reader.ok()) {
            ++counters_.rejected;
            return;
        }
        ++counters_.surfaces;
        surfaces_.insert_or_assign(node.id, std::move(surface));
        return;
    }

    core::u32 index{};
    core::u32 x{};
    core::u32 y{};
    core::u32 width{};
    core::u32 height{};
    if (!as_u32(reader.integer(schema::node_index, 0), index)
        || !as_u32(reader.integer(schema::source_x, 0), x)
        || !as_u32(reader.integer(schema::source_y, 0), y)
        || !as_u32(reader.integer(schema::source_width, 0), width)
        || !as_u32(reader.integer(schema::source_height, 0), height)) {
        reader.fail();
    }
    if (width == 0U || height == 0U) {
        reader.fail();
    }

    if (!reader.ok()) {
        ++counters_.rejected;
        return;
    }

    OutputMap map{};
    map.node_index = index;
    map.source = scene::CanvasRect{x, y, width, height};
    const std::string_view policy =
        reader.text(schema::projection, schema::projection_canvas);
    if (policy == schema::projection_canvas) {
        map.projection = FeedProjection::CanvasSlice;
    } else if (policy == schema::projection_planar_surface) {
        map.projection = FeedProjection::PlanarSurface;
    } else {
        reader.fail();
    }
    const std::int64_t surface_id = reader.integer(schema::surface_id, 0);
    if (surface_id < 0) {
        reader.fail();
    } else {
        map.surface = doc::NodeId{static_cast<core::u64>(surface_id)};
    }
    if (!reader.ok()) {
        ++counters_.rejected;
        return;
    }
    ++counters_.maps;
    entries_.insert_or_assign(node.id, map);
}

void FeedMapModel::reset(const doc::Root& root) {
    entries_.clear();
    surfaces_.clear();
    counters_ = FeedMapCounters{};
    // Diffing against an empty root enumerates every node in ascending id
    // order - the same code path a delta takes, so a checkpointed node and a
    // streamed one cannot parse differently.
    apply(diff(doc::Root{}, root));
}

void FeedMapModel::apply(const doc::Delta& delta) {
    for (const auto& change : delta.changes) {
        switch (change.kind) {
        case doc::ChangeKind::Removed:
            forget(change.id);
            break;
        case doc::ChangeKind::Added:
        case doc::ChangeKind::Modified:
            if (change.after) {
                put(*change.after);
            }
            break;
        }
    }
}

FeedMap FeedMapModel::evaluate() const {
    FeedMap feed_map{};

    feed_map.surfaces.reserve(
        std::min<std::size_t>(surfaces_.size(), max_projection_surfaces));
    for (const auto& [id, surface] : surfaces_) {
        static_cast<void>(id);
        if (feed_map.surfaces.size() >= max_projection_surfaces) {
            break;
        }
        feed_map.surfaces.push_back(surface);
    }

    // Entries are in node id order, so the first binding seen for a node index
    // is the lowest-id one and wins. Truncation at the cap is by node id too,
    // so every node keeps the same maps.
    for (const auto& [id, map] : entries_) {
        static_cast<void>(id);
        if (feed_map.maps.size() >= max_output_maps) {
            break;
        }
        const auto already = std::any_of(
            feed_map.maps.begin(),
            feed_map.maps.end(),
            [&map](const OutputMap& held) noexcept {
                return held.node_index == map.node_index;
            });
        if (already) {
            continue;
        }
        OutputMap resolved = map;
        if (resolved.projection == FeedProjection::PlanarSurface) {
            const ProjectionSurface* const surface =
                feed_map.surface_for(resolved.surface);
            if (surface != nullptr &&
                surface->shape == ProjectionSurfaceShape::ImportedMesh) {
                // One imported file is one connector patch. UV0 owns the
                // complete destination image, so a legacy/stale crop cannot
                // imply a second crop that the mesh-warp renderer ignores.
                resolved.source = scene::whole(surface->geometry.raster);
                resolved.resolved_surface = ResolvedProjectionSurface{
                    surface->geometry, surface->shape,
                    surface->horizontal_arc_turns, surface->mesh_columns,
                    surface->mesh_source, surface->mesh_scale};
            } else if (surface != nullptr &&
                       scene::contains(surface->geometry.raster,
                                       resolved.source)) {
                resolved.resolved_surface = ResolvedProjectionSurface{
                    surface->geometry, surface->shape,
                    surface->horizontal_arc_turns, surface->mesh_columns,
                    surface->mesh_source, surface->mesh_scale};
            }
        }
        feed_map.maps.push_back(std::move(resolved));
    }

    // By node index, so the order the renderer and the digest see is a property
    // of the document rather than of node ids.
    std::sort(
        feed_map.maps.begin(),
        feed_map.maps.end(),
        [](const OutputMap& left, const OutputMap& right) noexcept {
            return left.node_index < right.node_index;
        });

    return feed_map;
}

} // namespace mediaserver::output
