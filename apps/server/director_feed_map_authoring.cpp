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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <variant>
#include <vector>

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

namespace {

[[nodiscard]] std::int64_t integer_property(const doc::Node &node,
                                            std::string_view key,
                                            std::int64_t fallback) {
    const auto found = node.properties.find(key);
    if (found == node.properties.end()) {
        return fallback;
    }
    const auto *const value = std::get_if<std::int64_t>(&found->second);
    return value != nullptr ? *value : fallback;
}

// The list `member` is shown in, in the feed model's order: every surface, or
// the viewports of its window, by list order and then document id.
[[nodiscard]] std::vector<const doc::Node *>
feed_list_for(const doc::Root &root, const doc::Node &member) {
    const bool viewport = member.type == output::schema::map_type;
    const std::int64_t window =
        integer_property(member, output::schema::node_index, 0);
    std::vector<const doc::Node *> list;
    const doc::Delta contents = doc::diff(doc::Root{}, root);
    for (const doc::NodeChange &change : contents.changes) {
        if (!change.after || change.after->type != member.type) {
            continue;
        }
        if (viewport &&
            integer_property(*change.after, output::schema::node_index, 0) !=
                window) {
            continue;
        }
        list.push_back(change.after.get());
    }
    // The diff is in id order, so a stable sort leaves ties there.
    std::stable_sort(list.begin(), list.end(),
                     [](const doc::Node *left, const doc::Node *right) {
                         return integer_property(*left,
                                                 output::schema::list_order,
                                                 0) <
                                integer_property(*right,
                                                 output::schema::list_order, 0);
                     });
    return list;
}

[[nodiscard]] std::size_t count_of_type(const doc::Root &root,
                                        std::string_view type) {
    std::size_t count = 0U;
    const doc::Delta contents = doc::diff(doc::Root{}, root);
    for (const doc::NodeChange &change : contents.changes) {
        count += change.after && change.after->type == type ? 1U : 0U;
    }
    return count;
}

// Writes dense list positions in `order`, touching only nodes whose position
// actually changes, all in one transaction.
[[nodiscard]] bool schedule_list_order(cmd::DirectorScheduler &scheduler,
                                       const std::vector<doc::Node> &order,
                                       const editor::FeedOrganizeRequest &request,
                                       doc::NodeId inserted,
                                       cmd::FrameNumber apply_at) {
    for (std::size_t index = 0U; index < order.size(); ++index) {
        doc::Node updated = order[index];
        updated.properties.insert_or_assign(
            std::string{output::schema::list_order},
            doc::PropertyValue{static_cast<std::int64_t>(index)});
        const bool created = inserted != doc::NodeId{} && updated.id == inserted;
        if (!created && updated == order[index]) {
            continue;
        }
        const cmd::Payload inverse =
            created ? cmd::Payload{cmd::EraseNode{inserted}}
                    : cmd::Payload{cmd::PutNode{order[index]}};
        if (!scheduler.schedule_edit(cmd::PutNode{std::move(updated)}, inverse,
                                     cmd::UndoContextId{4U},
                                     cmd::TransactionId{request.gesture},
                                     apply_at)) {
            return false;
        }
    }
    return true;
}

} // namespace

[[nodiscard]] bool
schedule_feed_organize(cmd::DirectorScheduler &scheduler,
                       const editor::FeedOrganizeRequest &request,
                       doc::NodeId allocated_id, cmd::FrameNumber apply_at) {
    const doc::Root &root = scheduler.projected_document();
    const doc::Node *source = find_feed_map_node(root, request.node);
    if (source == nullptr) {
        source = find_feed_surface_node(root, request.node);
    }
    if (source == nullptr) {
        return true;
    }
    const bool viewport = source->type == output::schema::map_type;
    const std::string_view name_key =
        viewport ? output::schema::map_name : output::schema::surface_name;

    switch (request.action) {
    case editor::FeedOrganizeAction::Rename: {
        constexpr std::size_t max_name_bytes = 256U;
        if (request.name.empty() || request.name.size() > max_name_bytes) {
            return true;
        }
        doc::Node updated = *source;
        updated.properties.insert_or_assign(std::string{name_key},
                                            doc::PropertyValue{request.name});
        if (updated == *source) {
            return true;
        }
        return static_cast<bool>(scheduler.schedule_edit(
            cmd::PutNode{std::move(updated)}, cmd::PutNode{*source},
            cmd::UndoContextId{4U}, cmd::TransactionId{request.gesture},
            apply_at));
    }
    case editor::FeedOrganizeAction::Reorder: {
        const std::vector<const doc::Node *> list = feed_list_for(root, *source);
        if (request.order.size() != list.size()) {
            return true;
        }
        std::vector<doc::Node> order;
        order.reserve(list.size());
        for (const doc::NodeId id : request.order) {
            const auto found =
                std::find_if(list.begin(), list.end(),
                             [id](const doc::Node *node) { return node->id == id; });
            if (found == list.end() ||
                std::any_of(order.begin(), order.end(),
                            [id](const doc::Node &node) { return node.id == id; })) {
                return true;
            }
            order.push_back(**found);
        }
        return schedule_list_order(scheduler, order, request, {}, apply_at);
    }
    case editor::FeedOrganizeAction::Duplicate: {
        // A copy past the cap would be dropped by evaluation; refuse it here
        // rather than author an object nobody can see.
        if (count_of_type(root, source->type) >=
            (viewport ? output::max_output_maps
                      : output::max_projection_surfaces)) {
            return true;
        }
        doc::Node copy = *source;
        copy.id = allocated_id;
        const auto named = source->properties.find(name_key);
        const auto *const name =
            named != source->properties.end()
                ? std::get_if<std::string>(&named->second)
                : nullptr;
        // An unnamed viewport stays unnamed and shows its own ordinal.
        if (name != nullptr && !name->empty()) {
            copy.properties.insert_or_assign(
                std::string{name_key}, doc::PropertyValue{*name + " copy"});
        }
        std::vector<doc::Node> order;
        for (const doc::Node *node : feed_list_for(root, *source)) {
            order.push_back(*node);
            if (node->id == source->id) {
                order.push_back(copy);
            }
        }
        return schedule_list_order(scheduler, order, request, allocated_id,
                                   apply_at);
    }
    }
    return true;
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

[[nodiscard]] bool
schedule_feed_layout(cmd::DirectorScheduler &scheduler,
                     const editor::FeedLayoutRequest &request,
                     std::span<const doc::NodeId> ids,
                     cmd::FrameNumber apply_at) {
    const output::FeedLayoutPlan &plan = request.plan;
    if (plan.empty() || ids.size() < plan.viewports.size()) {
        return true;
    }
    const cmd::UndoContextId context{3U};
    const cmd::TransactionId transaction{request.gesture};
    for (const core::u32 window : plan.windows) {
        const std::vector<const doc::Node *> existing =
            find_feed_map_nodes(scheduler.projected_document(), window);
        std::vector<doc::NodeId> erased;
        erased.reserve(existing.size());
        for (const doc::Node *node : existing) {
            erased.push_back(node->id);
        }
        for (const doc::NodeId id : erased) {
            if (!schedule_erase_node(scheduler, id, context, transaction,
                                     apply_at)) {
                return false;
            }
        }
    }
    std::vector<std::int64_t> order(plan.windows.size(), 0);
    for (std::size_t index = 0U; index < plan.viewports.size(); ++index) {
        const output::FeedLayoutViewport &viewport = plan.viewports[index];
        const auto window = std::find(plan.windows.begin(),
                                      plan.windows.end(), viewport.window);
        if (window == plan.windows.end()) {
            continue;
        }
        doc::Node node{};
        node.id = ids[index];
        node.type = std::string{output::schema::map_type};
        const auto put = [&](std::string_view key, doc::PropertyValue value) {
            node.properties.emplace(std::string{key}, std::move(value));
        };
        const auto whole = [](core::u32 value) {
            return doc::PropertyValue{static_cast<std::int64_t>(value)};
        };
        put(output::schema::node_index, whole(viewport.window));
        put(output::schema::source_x, whole(viewport.source.x));
        put(output::schema::source_y, whole(viewport.source.y));
        put(output::schema::source_width, whole(viewport.source.width));
        put(output::schema::source_height, whole(viewport.source.height));
        put(output::schema::viewport_x, whole(viewport.viewport.x));
        put(output::schema::viewport_y, whole(viewport.viewport.y));
        put(output::schema::viewport_width, whole(viewport.viewport.width));
        put(output::schema::viewport_height, whole(viewport.viewport.height));
        put(output::schema::projection,
            doc::PropertyValue{std::string{output::schema::projection_canvas}});
        put(output::schema::surface_id, doc::PropertyValue{std::int64_t{0}});
        put(output::schema::map_name, doc::PropertyValue{viewport.name});
        std::int64_t &position = order[static_cast<std::size_t>(
            window - plan.windows.begin())];
        put(output::schema::list_order, doc::PropertyValue{position++});
        const doc::NodeId id = node.id;
        if (!scheduler.schedule_edit(cmd::PutNode{std::move(node)},
                                     cmd::EraseNode{id}, context, transaction,
                                     apply_at)) {
            return false;
        }
    }
    return true;
}

} // namespace mediaserver::app
