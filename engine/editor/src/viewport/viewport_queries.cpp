#include "viewport_support.hpp"

#include <mediaserver/editor/gizmo.hpp>
#include <mediaserver/editor/light_guides.hpp>
#include <mediaserver/editor/operator_widgets.hpp>
#include <mediaserver/gui/rect_cut.hpp>
#include <mediaserver/gui/shapes.hpp>

#include <mediaserver/scene/manipulate.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <tuple>
#include <utility>

namespace mediaserver::editor {

using namespace viewport;

namespace {

constexpr std::size_t maximum_cached_model_bounds = 128U;
constexpr std::size_t maximum_cached_projection_surfaces = 256U;

[[nodiscard]] std::string deep_select_label(const scene::Scene& view,
                                            doc::NodeId node) {
    for (const scene::MeshInstance& instance : view.instances) {
        if (instance.source == node) {
            return "Object  " + std::to_string(node.value());
        }
    }
    for (const scene::MediaInstance& media : view.media) {
        if (media.source_node == node) {
            return "Media  " + std::to_string(node.value());
        }
    }
    for (const scene::ModelInstance& model : view.models) {
        if (model.source_node == node) {
            return "Model  " + std::to_string(node.value());
        }
    }
    for (const scene::PointLight& light : view.point_lights) {
        if (light.source == node) {
            const char* name = "Point Light  ";
            switch (light.kind) {
            case scene::LocalLightKind::Point: break;
            case scene::LocalLightKind::Spot: name = "Spot Light  "; break;
            case scene::LocalLightKind::Rectangle: name = "Rectangle Light  "; break;
            case scene::LocalLightKind::Sphere: name = "Sphere Light  "; break;
            }
            return name + std::to_string(node.value());
        }
    }
    for (const scene::DirectionalLight& light : view.directional_lights) {
        if (light.source == node) {
            return "Directional Light  " + std::to_string(node.value());
        }
    }
    return "Element  " + std::to_string(node.value());
}

} // namespace

std::string viewport_deep_select_label(
    const scene::Scene& view, doc::NodeId node,
    std::span<const ElementInfo> elements) {
    const auto element = std::find_if(
        elements.begin(), elements.end(),
        [node](const ElementInfo& candidate) { return candidate.node == node; });
    return element != elements.end() ? element_display_name(*element)
                                     : deep_select_label(view, node);
}

std::optional<WorldBounds> ViewportPanel::world_bounds(
    const scene::Scene& scene, std::span<const doc::NodeId> nodes) {
    std::optional<WorldBounds> box;
    const auto include = [&](scene::Vec3 point) {
        if (!box.has_value()) {
            box = WorldBounds{point, point};
            return;
        }
        box->minimum = scene::Vec3{std::min(box->minimum.x, point.x),
                                   std::min(box->minimum.y, point.y),
                                   std::min(box->minimum.z, point.z)};
        box->maximum = scene::Vec3{std::max(box->maximum.x, point.x),
                                   std::max(box->maximum.y, point.y),
                                   std::max(box->maximum.z, point.z)};
    };
    const auto include_box = [&](const scene::Mat4& world,
                                 const scene::ModelLocalBounds& local) {
        for (std::size_t corner = 0U; corner < 8U; ++corner) {
            include(scene::transform_point(
                world,
                scene::Vec3{(corner & 1U) != 0U ? local.maximum.x : local.minimum.x,
                            (corner & 2U) != 0U ? local.maximum.y : local.minimum.y,
                            (corner & 4U) != 0U ? local.maximum.z : local.minimum.z}));
        }
    };
    const auto primitive_bounds = primitive_local_bounds;
    for (const doc::NodeId node : nodes) {
        bool found = false;
        for (const auto& object : scene.instances) {
            if (object.source == node) {
                include_box(object.world,
                            primitive_bounds(object.mesh.primitive));
                found = true;
            }
        }
        for (const auto& clip : scene.media) {
            if (clip.source_node == node) {
                include_box(clip.world, primitive_bounds(clip.mesh.primitive));
                found = true;
            }
        }
        for (const auto& model : scene.models) {
            if (model.source_node == node) {
                include_box(model.world, model_bounds_for(model).value_or(
                                             scene::ModelLocalBounds{}));
                found = true;
            }
        }
        if (!found) {
            if (const auto world = scene::world_for(scene, node)) {
                include(scene::translation_of(*world));
            }
        }
    }
    return box;
}

std::optional<scene::ModelLocalBounds>
ViewportPanel::model_bounds_for(const scene::ModelInstance& model) {
    const std::string& source = model.source;
    if (source.empty()) {
        return std::nullopt;
    }
    auto held = model_bounds_cache_.find(source);
    const render::ModelCacheObservation observation =
        model_cache_.observe(source, model_publication_fence_);
    if (observation.ready()) {
        const scene::ModelLocalBounds bounds{observation.asset->bounds.minimum,
                                             observation.asset->bounds.maximum};
        const core::u64 stamp = next_model_bounds_stamp();
        if (held == model_bounds_cache_.end() &&
            model_bounds_cache_.size() >= maximum_cached_model_bounds) {
            held = std::min_element(
                model_bounds_cache_.begin(), model_bounds_cache_.end(),
                [](const auto& left, const auto& right) noexcept {
                    return left.second.stamp < right.second.stamp;
                });
            if (held != model_bounds_cache_.end()) {
                model_bounds_cache_.erase(held);
            }
            held = model_bounds_cache_.end();
        }
        if (held == model_bounds_cache_.end()) {
            held = model_bounds_cache_
                       .emplace(source,
                                CachedModelBounds{bounds, stamp})
                       .first;
        } else {
            held->second = CachedModelBounds{bounds, stamp};
        }
        return held->second.bounds;
    }

    // Busy/Pending/Failed never borrows another source's bounds. A previous
    // exact-source success is safe to keep through an explicit refresh.
    if (held != model_bounds_cache_.end()) {
        held->second.stamp = next_model_bounds_stamp();
        return held->second.bounds;
    }
    return std::nullopt;
}

std::optional<scene::SurfaceHit>
ViewportPanel::surface_at(const scene::Scene& scene, float canvas_x,
                          float canvas_y, const scene::SurfaceFilter& counts) {
    const auto ray =
        scene::canvas_ray(scene.camera, scene.canvas, canvas_x, canvas_y);
    if (!ray.has_value()) {
        return std::nullopt;
    }
    const scene::ModelRaycaster models =
        [this](const scene::ModelInstance& model, const scene::Ray& local,
               float limit) -> std::optional<scene::RayHit> {
        const std::optional<scene::ModelLocalBounds> bounds =
            model_bounds_for(model);
        const scene::ModelLocalBounds box =
            bounds.value_or(scene::ModelLocalBounds{});
        const auto outer =
            scene::raycast_box(local, box.minimum, box.maximum, limit);
        if (!outer.has_value() || !bounds.has_value()) {
            return outer;
        }
        // Inside its bounds the model's own triangles decide, once their
        // hierarchy is ready; until then the bounds stand in for them.
        const render::ModelCacheObservation observation =
            model_cache_.observe(model.source, model_publication_fence_);
        if (observation.ready()) {
            const std::shared_ptr<const scene::MeshData> mesh{
                observation.asset, &observation.asset->mesh};
            if (const auto bvh = surface_cache_->observe(mesh)) {
                return scene::raycast_mesh(*bvh, *mesh, local, limit);
            }
        }
        return outer;
    };
    return scene::raycast_scene(scene, *ray, models, scene.camera.far_plane,
                                counts);
}

std::shared_ptr<const scene::MeshData>
ViewportPanel::primitive_mesh(scene::MeshKey key) {
    key = scene::normalized(key);
    const auto found = primitive_meshes_.find(key);
    if (found != primitive_meshes_.end()) {
        return found->second;
    }
    // Distinct primitive keys in a show are few; a scene that churns through
    // tessellations starts the table again rather than growing it.
    constexpr std::size_t maximum_primitive_meshes = 64U;
    if (primitive_meshes_.size() >= maximum_primitive_meshes) {
        primitive_meshes_.clear();
    }
    auto mesh = std::make_shared<scene::MeshData>();
    if (!scene::build_mesh(key, *mesh)) {
        return nullptr;
    }
    return primitive_meshes_.emplace(key, std::move(mesh)).first->second;
}

std::optional<MeasurePoint>
ViewportPanel::measure_point_at(const scene::Scene& scene, float canvas_x,
                                float canvas_y, float reach,
                                const scene::SurfaceFilter& counts) {
    const auto ray =
        scene::canvas_ray(scene.camera, scene.canvas, canvas_x, canvas_y);
    if (!ray.has_value()) {
        return std::nullopt;
    }
    const std::optional<scene::SurfaceHit> hit =
        surface_at(scene, canvas_x, canvas_y, counts);
    if (!hit.has_value()) {
        if (ray->direction.y < -1.0e-6F && ray->origin.y > 0.0F) {
            const float distance = -ray->origin.y / ray->direction.y;
            if (distance <= scene.camera.far_plane) {
                scene::Vec3 floor = ray->origin + ray->direction * distance;
                floor.y = 0.0F;
                return MeasurePoint{floor, MeasureSnap::Floor, {}};
            }
        }
        return std::nullopt;
    }
    const MeasurePoint point{hit->position, MeasureSnap::Surface, hit->node};

    std::shared_ptr<const scene::MeshData> mesh;
    scene::Mat4 world{};
    switch (hit->source) {
    case scene::SurfaceSource::Instance:
        mesh = primitive_mesh(scene.instances[hit->index].mesh);
        world = scene.instances[hit->index].world;
        break;
    case scene::SurfaceSource::Media:
        mesh = primitive_mesh(scene.media[hit->index].mesh);
        world = scene.media[hit->index].world;
        break;
    case scene::SurfaceSource::Model: {
        const scene::ModelInstance& model = scene.models[hit->index];
        const render::ModelCacheObservation observation =
            model_cache_.observe(model.source, model_publication_fence_);
        if (observation.ready()) {
            mesh = std::shared_ptr<const scene::MeshData>{
                observation.asset, &observation.asset->mesh};
        }
        world = model.world;
        break;
    }
    }
    const std::shared_ptr<const scene::MeshBvh> bvh =
        mesh != nullptr ? surface_cache_->observe(mesh) : nullptr;
    if (bvh == nullptr) {
        return point;
    }

    // `reach` canvas pixels at the hit's depth, carried into the element's
    // local space. The inverse's longest axis bounds how far that can stretch.
    const float pixel_world = scene::units_per_canvas_pixel(
        scene.camera, hit->distance, static_cast<float>(scene.canvas.height));
    const scene::Mat4 inverse = scene::invert_affine(world);
    float inverse_scale = 0.0F;
    for (core::u32 column = 0U; column < 3U; ++column) {
        inverse_scale = std::max(
            inverse_scale,
            scene::length(scene::Vec3{inverse.at(0U, column),
                                      inverse.at(1U, column),
                                      inverse.at(2U, column)}));
    }
    const float local_reach = reach * pixel_world * inverse_scale * 1.75F;
    std::vector<core::u32> triangles;
    static_cast<void>(scene::mesh_triangles_near(
        *bvh, *mesh, scene::transform_point(inverse, hit->position),
        local_reach, triangles, 512U));

    // Nearest on screen wins; a vertex anywhere in reach beats a midpoint.
    // Points level on screen go to the one nearer the surface the pointer is
    // on: in an orthographic view a corner and the corner straight behind it
    // land on the same pixel, and only the front one can be seen.
    float vertex_reach = reach;
    float midpoint_reach = reach;
    std::optional<scene::Vec3> vertex;
    std::optional<scene::Vec3> midpoint;
    constexpr float level = 0.25F;
    const auto closer = [&](float distance, float best,
                            const std::optional<scene::Vec3>& chosen,
                            scene::Vec3 candidate) {
        if (distance < best - level) {
            return true;
        }
        return chosen.has_value() && distance < best + level &&
               scene::length(candidate - hit->position) <
                   scene::length(*chosen - hit->position);
    };
    const auto screen_distance = [&](scene::Vec3 candidate) {
        const auto projected =
            scene::project_to_canvas(scene.camera, scene.canvas, candidate);
        if (!projected.visible) {
            return std::numeric_limits<float>::infinity();
        }
        const float dx = projected.x - canvas_x;
        const float dy = projected.y - canvas_y;
        return std::sqrt(dx * dx + dy * dy);
    };
    for (const core::u32 triangle : triangles) {
        std::array<scene::Vec3, 3U> corners{};
        for (core::u32 corner = 0U; corner < 3U; ++corner) {
            corners[corner] = scene::transform_point(
                world,
                mesh->vertices[mesh->indices[triangle * 3U + corner]].position);
        }
        for (core::u32 corner = 0U; corner < 3U; ++corner) {
            const float distance = screen_distance(corners[corner]);
            if (distance < reach &&
                (!vertex.has_value() ||
                 closer(distance, vertex_reach, vertex, corners[corner]))) {
                vertex_reach = std::min(vertex_reach, distance);
                vertex = corners[corner];
            }
            const scene::Vec3 middle =
                (corners[corner] + corners[(corner + 1U) % 3U]) * 0.5F;
            const float middle_distance = screen_distance(middle);
            if (middle_distance < reach &&
                (!midpoint.has_value() ||
                 closer(middle_distance, midpoint_reach, midpoint, middle))) {
                midpoint_reach = std::min(midpoint_reach, middle_distance);
                midpoint = middle;
            }
        }
    }
    if (vertex.has_value()) {
        return MeasurePoint{*vertex, MeasureSnap::Vertex, hit->node};
    }
    if (midpoint.has_value()) {
        return MeasurePoint{*midpoint, MeasureSnap::EdgeMidpoint, hit->node};
    }
    return point;
}

scene::ModelLocalBounds
ViewportPanel::element_local_bounds(const scene::Scene& scene,
                                    doc::NodeId node) {
    for (const auto& object : scene.instances) {
        if (object.source == node) {
            return primitive_local_bounds(object.mesh.primitive);
        }
    }
    for (const auto& clip : scene.media) {
        if (clip.source_node == node) {
            return primitive_local_bounds(clip.mesh.primitive);
        }
    }
    for (const auto& model : scene.models) {
        if (model.source_node == node) {
            return model_bounds_for(model).value_or(scene::ModelLocalBounds{});
        }
    }
    return scene::ModelLocalBounds{scene::Vec3{}, scene::Vec3{}};
}

std::optional<scene::Vec3> ViewportPanel::nearest_element_vertex(
    const scene::Scene& scene, doc::NodeId node, const scene::Mat4& world,
    scene::Vec3 near) {
    std::shared_ptr<const scene::MeshData> mesh;
    for (const auto& object : scene.instances) {
        if (object.source == node) {
            mesh = primitive_mesh(object.mesh);
        }
    }
    for (const auto& clip : scene.media) {
        if (mesh == nullptr && clip.source_node == node) {
            mesh = primitive_mesh(clip.mesh);
        }
    }
    for (const auto& model : scene.models) {
        if (mesh == nullptr && model.source_node == node) {
            const render::ModelCacheObservation observation =
                model_cache_.observe(model.source, model_publication_fence_);
            if (observation.ready()) {
                mesh = std::shared_ptr<const scene::MeshData>{
                    observation.asset, &observation.asset->mesh};
            }
        }
    }
    if (mesh == nullptr || mesh->vertices.empty()) {
        return std::nullopt;
    }
    // Once per drag, when V is first held: a plain pass over the vertices.
    const scene::MeshVertex* best = &mesh->vertices.front();
    float best_distance = std::numeric_limits<float>::infinity();
    for (const scene::MeshVertex& vertex : mesh->vertices) {
        const scene::Vec3 offset =
            scene::transform_point(world, vertex.position) - near;
        const float distance = scene::dot(offset, offset);
        if (distance < best_distance) {
            best_distance = distance;
            best = &vertex;
        }
    }
    return scene::transform_point(world, best->position);
}

std::vector<doc::NodeId>
ViewportPanel::elements_in_marquee(const scene::Scene& scene,
                                   const gui::Rect& image,
                                   const gui::Rect& box) {
    std::vector<doc::NodeId> inside;
    if (image.width <= 0.0F || image.height <= 0.0F) {
        return inside;
    }
    // An element counts when the screen box around its visible corners
    // touches the drawn box, as Unreal's marquee does.
    const auto touches = [&](const scene::Mat4& world,
                             const scene::ModelLocalBounds& bounds) {
        float left = std::numeric_limits<float>::infinity();
        float top = left;
        float right = -left;
        float bottom = -left;
        bool any = false;
        for (unsigned corner = 0U; corner < 8U; ++corner) {
            const scene::Vec3 local{
                (corner & 1U) != 0U ? bounds.maximum.x : bounds.minimum.x,
                (corner & 2U) != 0U ? bounds.maximum.y : bounds.minimum.y,
                (corner & 4U) != 0U ? bounds.maximum.z : bounds.minimum.z};
            const auto projected = scene::project_to_canvas(
                scene.camera, scene.canvas, scene::transform_point(world, local));
            if (!projected.visible) {
                continue;
            }
            const gui::UiPoint at = to_ui(image, scene.canvas, projected);
            left = std::min(left, at.x);
            right = std::max(right, at.x);
            top = std::min(top, at.y);
            bottom = std::max(bottom, at.y);
            any = true;
        }
        return any && right >= box.x && left <= box.right() &&
               bottom >= box.y && top <= box.bottom();
    };
    const auto add = [&inside](doc::NodeId node) {
        if (std::find(inside.begin(), inside.end(), node) == inside.end()) {
            inside.push_back(node);
        }
    };
    for (const auto& object : scene.instances) {
        if (!object.locked &&
            touches(object.world, primitive_local_bounds(object.mesh.primitive))) {
            add(object.source);
        }
    }
    for (const auto& clip : scene.media) {
        if (!clip.locked &&
            touches(clip.world, primitive_local_bounds(clip.mesh.primitive))) {
            add(clip.source_node);
        }
    }
    for (const auto& model : scene.models) {
        if (!model.locked &&
            touches(model.world,
                    model_bounds_for(model).value_or(scene::ModelLocalBounds{}))) {
            add(model.source_node);
        }
    }
    for (const auto& light : scene.point_lights) {
        if (!light.locked &&
            touches(scene::translation(light.position),
                    scene::ModelLocalBounds{scene::Vec3{}, scene::Vec3{}})) {
            add(light.source);
        }
    }
    return inside;
}

std::optional<scene::ModelLocalBounds>
ViewportPanel::model_source_bounds(const std::string& source) {
    scene::ModelInstance probe{};
    probe.source = source;
    return model_bounds_for(probe);
}

void ViewportPanel::draw_placement_preview(
    gui::UiContext& ctx, const gui::Rect& image, const scene::Scene& scene,
    std::span<const StagePlacement> placements, std::string_view caption) {
    if (placements.empty()) {
        return;
    }
    const gui::UiColor accent = ctx.theme().accent;
    gui::UiDrawList& list = ctx.draw_list();
    ctx.push_clip(image);
    for (const StagePlacement& placement : placements) {
        const scene::ModelLocalBounds& box = placement.bounds;
        if (!(box.minimum == box.maximum)) {
            wire_box(list, image, scene, placement_world(placement), accent,
                     box);
        }
        // A square on the surface where it rests, and the surface's normal.
        const scene::Vec3 n = placement.normal;
        const scene::Vec3 across = scene::normalize(
            scene::cross(n, std::abs(n.y) < 0.9F ? scene::Vec3{0.0F, 1.0F, 0.0F}
                                                 : scene::Vec3{1.0F, 0.0F, 0.0F}));
        const scene::Vec3 along = scene::cross(n, across);
        constexpr float half = 0.2F;
        const std::array<scene::Vec3, 4U> corners{
            placement.contact + across * half + along * half,
            placement.contact - across * half + along * half,
            placement.contact - across * half - along * half,
            placement.contact + across * half - along * half};
        for (std::size_t index = 0U; index < corners.size(); ++index) {
            haloed_projected_line(list, image, scene, corners[index],
                                  corners[(index + 1U) % corners.size()], 4.0F,
                                  1.8F, accent);
        }
        projected_line(list, image, scene, placement.contact,
                       placement.contact + n * 0.35F, 1.8F, accent);
    }
    const auto anchor = scene::project_to_canvas(scene.camera, scene.canvas,
                                                 placements.front().contact);
    if (!caption.empty() && anchor.visible) {
        // Left of the contact: the Library's drag thumbnail rides below and
        // to the right of the pointer.
        const gui::UiPoint at = to_ui(image, scene.canvas, anchor);
        const float width = ctx.font_width(caption) + 16.0F;
        const gui::Rect badge{at.x - 18.0F - width, at.y - 11.0F, width, 22.0F};
        ctx.panel(badge, accent);
        ctx.label(gui::inset(badge, gui::Padding{8.0F, 0.0F, 0.0F, 0.0F}),
                  caption, ctx.theme().background);
    }
    ctx.pop_clip();
}

core::u64 ViewportPanel::next_model_bounds_stamp() noexcept {
    ++model_bounds_stamp_;
    if (model_bounds_stamp_ != 0U) {
        return model_bounds_stamp_;
    }
    core::u64 largest{};
    for (auto& [unused, cached] : model_bounds_cache_) {
        static_cast<void>(unused);
        cached.stamp = cached.stamp / 2U + 1U;
        largest = std::max(largest, cached.stamp);
    }
    model_bounds_stamp_ = largest + 1U;
    return model_bounds_stamp_;
}

core::u64 ViewportPanel::next_projection_mesh_stamp() noexcept {
    ++projection_mesh_stamp_;
    if (projection_mesh_stamp_ != 0U) {
        return projection_mesh_stamp_;
    }
    core::u64 largest{};
    for (auto& [unused, cached] : projection_mesh_cache_) {
        static_cast<void>(unused);
        cached.stamp = cached.stamp / 2U + 1U;
        largest = std::max(largest, cached.stamp);
    }
    projection_mesh_stamp_ = largest + 1U;
    return projection_mesh_stamp_;
}

std::shared_ptr<const output::ProjectionMesh>
ViewportPanel::projection_mesh_for(
    const output::ProjectionSurface& surface) {
    const core::u64 key = surface.node.value();
    auto held = projection_mesh_cache_.find(key);
    const core::u64 stamp = next_projection_mesh_stamp();
    if (held == projection_mesh_cache_.end()) {
        if (projection_mesh_cache_.size() >=
            maximum_cached_projection_surfaces) {
            const auto oldest = std::min_element(
                projection_mesh_cache_.begin(), projection_mesh_cache_.end(),
                [](const auto& left, const auto& right) noexcept {
                    return left.second.stamp < right.second.stamp;
                });
            if (oldest != projection_mesh_cache_.end()) {
                projection_mesh_cache_.erase(oldest);
            }
        }
        held = projection_mesh_cache_
                   .try_emplace(key, CachedProjectionState{})
                   .first;
    }
    held->second.stamp = stamp;

    output::ResolvedProjectionSurface resolved{
        surface.geometry, surface.shape, surface.horizontal_arc_turns,
        surface.mesh_columns, surface.mesh_source, surface.mesh_scale,
        surface.mesh_asset};
    output::OutputMap map{};
    map.node = surface.node;
    map.source = scene::whole(surface.geometry.raster);
    map.projection = output::FeedProjection::PlanarSurface;
    map.surface = surface.node;
    map.resolved_surface = std::move(resolved);
    return held->second.cache.resolve_mesh(map);
}

void ViewportPanel::reset_project_state() noexcept {
    dragging_ = false;
    surface_dragging_ = false;
    drag_axis_ = GizmoAxis::None;
    surface_preview_.reset();
    deep_select_armed_ = false;
    deep_select_open_ = false;
    deep_candidates_.clear();
    model_publication_fence_ = 0U;
    model_bounds_stamp_ = 0U;
    model_bounds_cache_.clear();
    projection_mesh_stamp_ = 0U;
    projection_mesh_cache_.clear();
    surface_cache_->request_reset();
    primitive_meshes_.clear();
    set_measuring(false);
    pivot_override_.reset();
    pivot_selection_.clear();
    pivot_dragging_ = false;
    marquee_active_ = false;
    mask_gesture_active_ = false;
    mask_gesture_cancelled_ = false;
    mask_gesture_hit_ = MaskViewportHit::None;
    mask_gesture_point_.reset();
}

bool ViewportPanel::refresh_projection_surface(doc::NodeId surface) noexcept {
    const auto found = projection_mesh_cache_.find(surface.value());
    return found != projection_mesh_cache_.end() &&
           found->second.cache.refresh_imported_mesh();
}

bool detail::wait_for_viewport_projection_surface_for_test(
    ViewportPanel& panel, doc::NodeId surface) noexcept {
    const auto found = panel.projection_mesh_cache_.find(surface.value());
    return found != panel.projection_mesh_cache_.end() &&
           render::detail::wait_for_output_projection_cache_for_test(
               found->second.cache);
}

bool detail::wait_for_viewport_surface_hierarchies_for_test(
    ViewportPanel& panel) noexcept {
    return detail::wait_for_model_surface_cache_for_test(*panel.surface_cache_);
}

CanvasPoint to_canvas(const gui::Rect& image, scene::Canvas canvas,
                      float pointer_x, float pointer_y) noexcept {
    CanvasPoint point{};
    if (image.width <= 0.0F || image.height <= 0.0F) {
        return point;
    }
    point.x = ((pointer_x - image.x) / image.width) *
              static_cast<float>(canvas.width);
    point.y = ((pointer_y - image.y) / image.height) *
              static_cast<float>(canvas.height);
    point.inside = image.contains(pointer_x, pointer_y);
    return point;
}

std::optional<gui::UiColor> sample_viewport_color(
    rhi::Device& device, const rhi::Texture& target,
    const gui::Rect& image, float pointer_x, float pointer_y) noexcept {
    if (!target || target.desc().format != rhi::TextureFormat::Rgba8Unorm ||
        target.width() == 0U || target.height() == 0U ||
        image.width <= 0.0F || image.height <= 0.0F ||
        !image.contains(pointer_x, pointer_y)) {
        return std::nullopt;
    }
    const std::uint64_t byte_count =
        static_cast<std::uint64_t>(target.width()) * target.height() * 4U;
    if (byte_count == 0U ||
        byte_count > std::numeric_limits<core::u32>::max()) {
        return std::nullopt;
    }
    auto readback =
        device.create_readback(static_cast<core::u32>(byte_count));
    if (!readback) {
        return std::nullopt;
    }
    auto acquired = device.acquire_commands();
    if (!acquired) {
        return std::nullopt;
    }
    auto commands = std::move(acquired).value();
    if (!commands.download(target, readback.value())) {
        return std::nullopt;
    }
    auto submitted = device.submit(std::move(commands));
    if (!submitted || !std::move(submitted).value().wait()) {
        return std::nullopt;
    }
    const auto pixels = readback.value().map();
    if (pixels.size() < byte_count) {
        return std::nullopt;
    }

    const float normalized_x =
        std::clamp((pointer_x - image.x) / image.width, 0.0F, 1.0F);
    const float normalized_y =
        std::clamp((pointer_y - image.y) / image.height, 0.0F, 1.0F);
    const core::u32 x = std::min(
        static_cast<core::u32>(normalized_x * static_cast<float>(target.width())),
        target.width() - 1U);
    const core::u32 y = std::min(
        static_cast<core::u32>(normalized_y * static_cast<float>(target.height())),
        target.height() - 1U);
    const std::size_t offset =
        (static_cast<std::size_t>(y) * target.width() + x) * 4U;
    constexpr float to_unit = 1.0F / 255.0F;
    const gui::UiColor color{
        static_cast<float>(pixels[offset]) * to_unit,
        static_cast<float>(pixels[offset + 1U]) * to_unit,
        static_cast<float>(pixels[offset + 2U]) * to_unit,
        static_cast<float>(pixels[offset + 3U]) * to_unit};
    readback.value().unmap();
    return color;
}

} // namespace mediaserver::editor
