#include "director_feed_map_authoring.hpp"

#include "director_compositor_authoring.hpp"

#include <mediaserver/assets/asset_model.hpp>
#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/doc/node.hpp>
#include <mediaserver/output/feed_map_model.hpp>
#include <mediaserver/output/feed_map_schema.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/lens.hpp>
#include <mediaserver/scene/scene.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <variant>

namespace mediaserver::app {

[[nodiscard]] const doc::Node *find_feed_map_node(const doc::Root &root,
                                                  core::u32 node_index) {
    const auto nodes = find_feed_map_nodes(root, node_index);
    return nodes.empty() ? nullptr : nodes.front();
}

[[nodiscard]] std::vector<const doc::Node *>
find_feed_map_nodes(const doc::Root &root, core::u32 node_index) {
    std::vector<const doc::Node *> matches;
    const doc::Delta contents = doc::diff(doc::Root{}, root);
    for (const doc::NodeChange &change : contents.changes) {
        if (!change.after || change.after->type != output::schema::map_type) {
            continue;
        }
        const auto found =
            change.after->properties.find(output::schema::node_index);
        if (found == change.after->properties.end()) {
            continue;
        }
        const auto *const value = std::get_if<std::int64_t>(&found->second);
        if (value != nullptr && *value >= 0 &&
            static_cast<core::u64>(*value) == node_index) {
            matches.push_back(change.after.get());
        }
    }
    return matches;
}

[[nodiscard]] const doc::Node *find_feed_map_node(const doc::Root &root,
                                                  doc::NodeId node) noexcept {
    const doc::Node *const found = root.get(node);
    return found != nullptr && found->type == output::schema::map_type
               ? found
               : nullptr;
}

[[nodiscard]] const doc::Node *find_feed_surface_node(const doc::Root &root,
                                                      doc::NodeId id) noexcept {
    const doc::Node *const node = root.get(id);
    return node != nullptr && node->type == output::schema::surface_type
               ? node
               : nullptr;
}

[[nodiscard]] bool
feed_surface_reference_fits_document(doc::NodeId id) noexcept {
    return id.value() <=
           static_cast<core::u64>(std::numeric_limits<std::int64_t>::max());
}

[[nodiscard]] doc::NodeId projection_asset_for_source(const doc::Root &root,
                                                      std::string_view source) {
    if (source.empty()) {
        return {};
    }
    assets::AssetModel model;
    model.reset(root);
    const assets::AssetRegistry registry = model.evaluate();
    const assets::Asset *const asset =
        registry.asset_for_source(source, assets::AssetKind::ProjectionMesh);
    return asset != nullptr ? assets::asset_id(*asset) : doc::NodeId{};
}

void write_feed_surface_properties(doc::Node &node, std::string_view name,
                                   const output::PhysicalSurface &surface,
                                   output::ProjectionSurfaceShape shape,
                                   float horizontal_arc_turns,
                                   core::u32 mesh_columns,
                                   std::string_view mesh_source,
                                   float mesh_scale, doc::NodeId mesh_asset) {
    std::string_view shape_property = output::schema::shape_planar;
    switch (shape) {
    case output::ProjectionSurfaceShape::Planar:
        break;
    case output::ProjectionSurfaceShape::HorizontalArc:
        shape_property = output::schema::shape_horizontal_arc;
        break;
    case output::ProjectionSurfaceShape::ImportedMesh:
        shape_property = output::schema::shape_imported_mesh;
        break;
    }
    node.properties.insert_or_assign(std::string{output::schema::surface_name},
                                     doc::PropertyValue{std::string{name}});
    node.properties.insert_or_assign(
        std::string{output::schema::surface_shape},
        doc::PropertyValue{std::string{shape_property}});
    node.properties.insert_or_assign(
        std::string{output::schema::center_x},
        doc::PropertyValue{static_cast<double>(surface.center.x)});
    node.properties.insert_or_assign(
        std::string{output::schema::center_y},
        doc::PropertyValue{static_cast<double>(surface.center.y)});
    node.properties.insert_or_assign(
        std::string{output::schema::center_z},
        doc::PropertyValue{static_cast<double>(surface.center.z)});
    node.properties.insert_or_assign(
        std::string{output::schema::right_x},
        doc::PropertyValue{static_cast<double>(surface.right.x)});
    node.properties.insert_or_assign(
        std::string{output::schema::right_y},
        doc::PropertyValue{static_cast<double>(surface.right.y)});
    node.properties.insert_or_assign(
        std::string{output::schema::right_z},
        doc::PropertyValue{static_cast<double>(surface.right.z)});
    node.properties.insert_or_assign(
        std::string{output::schema::up_x},
        doc::PropertyValue{static_cast<double>(surface.up.x)});
    node.properties.insert_or_assign(
        std::string{output::schema::up_y},
        doc::PropertyValue{static_cast<double>(surface.up.y)});
    node.properties.insert_or_assign(
        std::string{output::schema::up_z},
        doc::PropertyValue{static_cast<double>(surface.up.z)});
    node.properties.insert_or_assign(
        std::string{output::schema::physical_width},
        doc::PropertyValue{static_cast<double>(surface.width)});
    node.properties.insert_or_assign(
        std::string{output::schema::physical_height},
        doc::PropertyValue{static_cast<double>(surface.height)});
    node.properties.insert_or_assign(
        std::string{output::schema::raster_width},
        doc::PropertyValue{static_cast<std::int64_t>(surface.raster.width)});
    node.properties.insert_or_assign(
        std::string{output::schema::raster_height},
        doc::PropertyValue{static_cast<std::int64_t>(surface.raster.height)});
    node.properties.insert_or_assign(
        std::string{output::schema::horizontal_arc_turns},
        doc::PropertyValue{static_cast<double>(horizontal_arc_turns)});
    node.properties.insert_or_assign(
        std::string{output::schema::mesh_columns},
        doc::PropertyValue{static_cast<std::int64_t>(mesh_columns)});
    node.properties.insert_or_assign(
        std::string{output::schema::mesh_source},
        doc::PropertyValue{std::string{mesh_source}});
    if (shape == output::ProjectionSurfaceShape::ImportedMesh && mesh_asset &&
        feed_surface_reference_fits_document(mesh_asset)) {
        node.properties.insert_or_assign(
            std::string{output::schema::mesh_asset_id},
            doc::PropertyValue{static_cast<std::int64_t>(mesh_asset.value())});
    } else {
        node.properties.erase(std::string{output::schema::mesh_asset_id});
    }
    node.properties.insert_or_assign(
        std::string{output::schema::mesh_scale},
        doc::PropertyValue{static_cast<double>(mesh_scale)});
}

[[nodiscard]] bool
valid_feed_surface_request(const output::PhysicalSurface &surface,
                           output::ProjectionSurfaceShape shape,
                           float horizontal_arc_turns, core::u32 mesh_columns,
                           std::string_view mesh_source,
                           float mesh_scale) noexcept {
    // The model parses every persisted scalar even when its shape does not
    // currently use it. Refuse dormant non-finite shape parameters here too,
    // so a node accepted for authoring cannot be rejected when it is evaluated
    // or loaded from a saved project.
    if (!output::valid(surface) || !std::isfinite(horizontal_arc_turns) ||
        !std::isfinite(mesh_scale)) {
        return false;
    }
    switch (shape) {
    case output::ProjectionSurfaceShape::Planar:
        return true;
    case output::ProjectionSurfaceShape::HorizontalArc:
        return std::abs(horizontal_arc_turns) > 1.0e-5F &&
               std::abs(horizontal_arc_turns) < 0.5F &&
               mesh_columns >= output::min_horizontal_arc_columns &&
               mesh_columns <= output::max_horizontal_arc_columns;
    case output::ProjectionSurfaceShape::ImportedMesh:
        return !mesh_source.empty() && mesh_scale > 0.0F &&
               mesh_scale <= output::max_imported_mesh_scale;
    }
    return false;
}

[[nodiscard]] doc::Node
make_feed_surface_node(doc::NodeId id,
                       const editor::FeedSurfaceAddRequest &request,
                       doc::NodeId mesh_asset) {
    doc::Node node{};
    node.id = id;
    node.type = std::string{output::schema::surface_type};
    node.properties.insert_or_assign(std::string{output::schema::locked},
                                     doc::PropertyValue{true});
    write_feed_surface_properties(node, request.name, request.surface,
                                  request.shape, request.horizontal_arc_turns,
                                  request.mesh_columns, request.mesh_source,
                                  request.mesh_scale, mesh_asset);
    return node;
}

[[nodiscard]] bool
schedule_feed_surface_add(cmd::DirectorScheduler &scheduler,
                          const editor::FeedSurfaceAddRequest &request,
                          doc::NodeId allocated_id, cmd::FrameNumber apply_at) {
    if (!valid_feed_surface_request(
            request.surface, request.shape, request.horizontal_arc_turns,
            request.mesh_columns, request.mesh_source, request.mesh_scale)) {
        return true;
    }
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{make_feed_surface_node(
            allocated_id, request,
            projection_asset_for_source(scheduler.projected_document(),
                                        request.mesh_source))},
        cmd::EraseNode{allocated_id}, cmd::UndoContextId{4U},
        cmd::TransactionId{request.gesture}, apply_at));
}

[[nodiscard]] bool
schedule_feed_surface_edit(cmd::DirectorScheduler &scheduler,
                           const editor::FeedSurfaceEditRequest &request,
                           cmd::FrameNumber apply_at) {
    const doc::Node *const existing =
        find_feed_surface_node(scheduler.projected_document(), request.node);
    if (existing == nullptr ||
        !valid_feed_surface_request(
            request.surface, request.shape, request.horizontal_arc_turns,
            request.mesh_columns, request.mesh_source, request.mesh_scale)) {
        return true;
    }
    doc::Node updated = *existing;
    write_feed_surface_properties(
        updated, request.name, request.surface, request.shape,
        request.horizontal_arc_turns, request.mesh_columns, request.mesh_source,
        request.mesh_scale,
        projection_asset_for_source(scheduler.projected_document(),
                                    request.mesh_source));

    if (request.color) {
        const auto color = scene::clamped(*request.color);
        updated.properties.insert_or_assign(std::string{output::schema::wall_color_enabled}, color.enabled);
        updated.properties.insert_or_assign(std::string{output::schema::camera_inverse_lut}, color.camera_inverse_lut_source);
        for (std::size_t i = 0; i < color.primaries.size(); ++i)
            updated.properties.insert_or_assign(std::string{output::schema::wall_primaries[i]}, static_cast<double>(color.primaries[i]));
    }

    // Surface views receive evaluated geometry. Preserve every transform
    // property the originating control did not actually change, otherwise an
    // edit to an unanimated axis (or to mesh calibration) would bake another
    // axis's animated value into the authored base beneath its channel.
    const auto preserve = [&](std::string_view property) {
        const auto authored = existing->properties.find(property);
        if (authored != existing->properties.end()) {
            updated.properties.insert_or_assign(std::string{property},
                                                authored->second);
            return;
        }
        const auto generated = updated.properties.find(property);
        if (generated != updated.properties.end()) {
            updated.properties.erase(generated);
        }
    };
    if (!editor::has_transform(request.transform_components,
                               editor::FeedSurfaceTransform::PositionX)) {
        preserve(output::schema::center_x);
    }
    if (!editor::has_transform(request.transform_components,
                               editor::FeedSurfaceTransform::PositionY)) {
        preserve(output::schema::center_y);
    }
    if (!editor::has_transform(request.transform_components,
                               editor::FeedSurfaceTransform::PositionZ)) {
        preserve(output::schema::center_z);
    }
    if (!editor::has_transform(request.transform_components,
                               editor::FeedSurfaceTransform::Yaw)) {
        preserve(output::schema::right_x);
        preserve(output::schema::right_y);
        preserve(output::schema::right_z);
        preserve(output::schema::up_x);
        preserve(output::schema::up_y);
        preserve(output::schema::up_z);
    }
    const cmd::UndoContextId context{request.transform_components ==
                                             editor::FeedSurfaceTransform::None
                                         ? 4U
                                         : 2U};
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)}, cmd::PutNode{*existing}, context,
        cmd::TransactionId{request.gesture}, apply_at));
}

[[nodiscard]] bool
schedule_feed_surface_delete(cmd::DirectorScheduler &scheduler,
                             const editor::FeedSurfaceDeleteRequest &request,
                             cmd::FrameNumber apply_at) {
    const doc::Node *const existing =
        find_feed_surface_node(scheduler.projected_document(), request.node);
    if (existing == nullptr) {
        return true;
    }
    // Output-map references deliberately survive this erase. Their unresolved
    // surface id is a visible, repairable project error and undo restores the
    // exact surface without having to reconstruct assignments.
    return schedule_erase_node(scheduler, existing->id, cmd::UndoContextId{4U},
                               cmd::TransactionId{request.gesture}, apply_at);
}

[[nodiscard]] bool
schedule_feed_projection_edit(cmd::DirectorScheduler &scheduler,
                              const editor::FeedProjectionEdit &edit,
                              cmd::FrameNumber apply_at) {
    const doc::Root &root = scheduler.projected_document();
    const doc::Node *const existing = find_feed_map_node(root, edit.node);
    if (existing == nullptr || edit.source.width == 0U ||
        edit.source.height == 0U) {
        return true;
    }

    std::string_view projection;
    doc::NodeId surface{};
    switch (edit.projection) {
    case output::FeedProjection::CanvasSlice:
        projection = output::schema::projection_canvas;
        break;
    case output::FeedProjection::PlanarSurface: {
        projection = output::schema::projection_planar_surface;
        surface = edit.surface;
        if (!surface || !feed_surface_reference_fits_document(surface) ||
            find_feed_surface_node(root, surface) == nullptr) {
            return true;
        }
        output::FeedMapModel model;
        model.reset(root);
        const output::FeedMap evaluated = model.evaluate();
        const output::ProjectionSurface *const resolved =
            evaluated.surface_for(surface);
        if (resolved == nullptr ||
            !scene::contains(resolved->geometry.raster, edit.source)) {
            return true;
        }
        break;
    }
    default:
        return true;
    }

    doc::Node updated = *existing;
    updated.properties.insert_or_assign(
        std::string{output::schema::projection},
        doc::PropertyValue{std::string{projection}});
    updated.properties.insert_or_assign(
        std::string{output::schema::surface_id},
        doc::PropertyValue{static_cast<std::int64_t>(surface.value())});
    updated.properties.insert_or_assign(
        std::string{output::schema::source_x},
        doc::PropertyValue{static_cast<std::int64_t>(edit.source.x)});
    updated.properties.insert_or_assign(
        std::string{output::schema::source_y},
        doc::PropertyValue{static_cast<std::int64_t>(edit.source.y)});
    updated.properties.insert_or_assign(
        std::string{output::schema::source_width},
        doc::PropertyValue{static_cast<std::int64_t>(edit.source.width)});
    updated.properties.insert_or_assign(
        std::string{output::schema::source_height},
        doc::PropertyValue{static_cast<std::int64_t>(edit.source.height)});
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)}, cmd::PutNode{*existing},
        cmd::UndoContextId{4U}, cmd::TransactionId{edit.gesture}, apply_at));
}

[[nodiscard]] bool schedule_feed_map_edit(cmd::DirectorScheduler &scheduler,
                                          const editor::FeedMapRectEdit &edit,
                                          doc::NodeId allocated_id,
                                          cmd::FrameNumber apply_at) {
    static_cast<void>(allocated_id);
    if (edit.viewport.width == 0U || edit.viewport.height == 0U) {
        return true;
    }
    const doc::Node *const existing =
        find_feed_map_node(scheduler.projected_document(), edit.node);
    if (existing == nullptr) {
        return true;
    }
    doc::Node updated{};
    updated.id = existing->id;
    updated.type = std::string{output::schema::map_type};
    updated = *existing;
    updated.properties.insert_or_assign(
        std::string{output::schema::viewport_x},
        doc::PropertyValue{static_cast<std::int64_t>(edit.viewport.x)});
    updated.properties.insert_or_assign(
        std::string{output::schema::viewport_y},
        doc::PropertyValue{static_cast<std::int64_t>(edit.viewport.y)});
    updated.properties.insert_or_assign(
        std::string{output::schema::viewport_width},
        doc::PropertyValue{static_cast<std::int64_t>(edit.viewport.width)});
    updated.properties.insert_or_assign(
        std::string{output::schema::viewport_height},
        doc::PropertyValue{static_cast<std::int64_t>(edit.viewport.height)});
    cmd::Payload inverse = cmd::PutNode{*existing};
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(updated)}, std::move(inverse),
        cmd::UndoContextId{4U}, cmd::TransactionId{edit.gesture}, apply_at));
}

[[nodiscard]] bool
schedule_feed_map_add(cmd::DirectorScheduler &scheduler,
                      const editor::FeedMapAddRequest &request,
                      doc::NodeId allocated_id, cmd::FrameNumber apply_at,
                      cmd::UndoContextId context) {
    if (request.source.width == 0U || request.source.height == 0U ||
        request.viewport.width == 0U || request.viewport.height == 0U) {
        return true;
    }
    doc::Node node{};
    node.id = allocated_id;
    node.type = std::string{output::schema::map_type};
    node.properties.emplace(
        std::string{output::schema::node_index},
        doc::PropertyValue{static_cast<std::int64_t>(request.node_index)});
    node.properties.emplace(
        std::string{output::schema::source_x},
        doc::PropertyValue{static_cast<std::int64_t>(request.source.x)});
    node.properties.emplace(
        std::string{output::schema::source_y},
        doc::PropertyValue{static_cast<std::int64_t>(request.source.y)});
    node.properties.emplace(
        std::string{output::schema::source_width},
        doc::PropertyValue{static_cast<std::int64_t>(request.source.width)});
    node.properties.emplace(
        std::string{output::schema::source_height},
        doc::PropertyValue{static_cast<std::int64_t>(request.source.height)});
    node.properties.emplace(
        std::string{output::schema::viewport_x},
        doc::PropertyValue{static_cast<std::int64_t>(request.viewport.x)});
    node.properties.emplace(
        std::string{output::schema::viewport_y},
        doc::PropertyValue{static_cast<std::int64_t>(request.viewport.y)});
    node.properties.emplace(
        std::string{output::schema::viewport_width},
        doc::PropertyValue{static_cast<std::int64_t>(request.viewport.width)});
    node.properties.emplace(
        std::string{output::schema::viewport_height},
        doc::PropertyValue{static_cast<std::int64_t>(request.viewport.height)});
    node.properties.emplace(
        std::string{output::schema::projection},
        doc::PropertyValue{std::string{output::schema::projection_canvas}});
    node.properties.emplace(std::string{output::schema::surface_id},
                            doc::PropertyValue{std::int64_t{0}});
    return static_cast<bool>(scheduler.schedule_edit(
        cmd::PutNode{std::move(node)}, cmd::EraseNode{allocated_id}, context,
        cmd::TransactionId{request.gesture}, apply_at));
}

[[nodiscard]] bool
schedule_feed_map_delete(cmd::DirectorScheduler &scheduler,
                         const editor::FeedMapDeleteRequest &request,
                         cmd::FrameNumber apply_at) {
    const doc::Node *const existing =
        find_feed_map_node(scheduler.projected_document(), request.node);
    if (existing == nullptr) {
        return true;
    }
    return schedule_erase_node(scheduler, existing->id, cmd::UndoContextId{4U},
                               cmd::TransactionId{request.gesture}, apply_at);
}

} // namespace mediaserver::app
