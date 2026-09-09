#include <mediaserver/output/feed_map_model.hpp>

#include <mediaserver/output/feed_map_schema.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
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

    [[nodiscard]] bool boolean(std::string_view name, bool fallback) noexcept {
        const auto* const value = find(*properties_, name);
        if (value == nullptr) {
            return fallback;
        }
        if (const auto* const held = std::get_if<bool>(value)) {
            return *held;
        }
        if (const auto* const real = std::get_if<double>(value)) {
            if (std::isfinite(*real)) {
                return *real >= 0.5;
            }
        } else if (const auto* const whole =
                       std::get_if<std::int64_t>(value)) {
            return *whole != 0;
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

[[nodiscard]] double sample_channel(const scene::ChannelKeyframes& channel,
                                    core::i64 frame) noexcept {
    const auto& frames = channel.frames;
    const auto& values = channel.values;
    if (frames.empty() || values.size() != frames.size()) {
        return 0.0;
    }
    if (frame <= frames.front()) {
        return values.front();
    }
    if (frame >= frames.back()) {
        return values.back();
    }
    const auto after = std::upper_bound(frames.begin(), frames.end(), frame);
    const std::size_t hi = static_cast<std::size_t>(after - frames.begin());
    const std::size_t lo = hi - 1U;
    if (channel.interpolations.size() == frames.size() &&
        channel.interpolations[lo] == scene::KeyInterpolation::Hold) {
        return values[lo];
    }
    const double span = static_cast<double>(frames[hi] - frames[lo]);
    double t = static_cast<double>(frame - frames[lo]) / span;
    if (channel.interpolations.size() == frames.size() &&
        channel.interpolations[lo] == scene::KeyInterpolation::Cubic) {
        t = t * t * (3.0 - 2.0 * t);
    }
    return values[lo] + (values[hi] - values[lo]) * t;
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
    surface_channels_.erase(id);
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

    if (node.type == scene::schema::channel_type) {
        const auto* const target_value =
            find(node.properties, scene::schema::channel_target);
        const auto* const component_value =
            find(node.properties, scene::schema::channel_component);
        const auto* const target = target_value != nullptr
            ? std::get_if<std::int64_t>(target_value)
            : nullptr;
        const auto* const component = component_value != nullptr
            ? std::get_if<std::string>(component_value)
            : nullptr;
        if (target == nullptr || *target < 0 || component == nullptr) {
            return;
        }

        scene::PropertyChannel property{scene::PropertyChannel::Count};
        if (*component == scene::schema::position_x) {
            property = scene::PropertyChannel::PositionX;
        } else if (*component == scene::schema::position_y) {
            property = scene::PropertyChannel::PositionY;
        } else if (*component == scene::schema::position_z) {
            property = scene::PropertyChannel::PositionZ;
        } else if (*component == scene::schema::rotation_yaw) {
            property = scene::PropertyChannel::RotationYaw;
        } else {
            // Scene channels outside the surface vocabulary remain solely the
            // SceneModel's responsibility.
            return;
        }

        struct ParsedKey final {
            core::i64 frame{};
            double value{};
            scene::KeyInterpolation interpolation{
                scene::KeyInterpolation::Linear};
        };
        std::vector<ParsedKey> parsed;
        parsed.reserve(scene::max_channel_keys);
        bool valid_channel = true;
        const std::string prefix{scene::schema::channel_key_prefix};
        constexpr double maximum_value =
            static_cast<double>(std::numeric_limits<float>::max());
        for (core::u32 index = 0U; index < scene::max_channel_keys; ++index) {
            const std::string ordinal = std::to_string(index);
            const auto* const frame = find(
                node.properties,
                prefix + ordinal +
                    std::string{scene::schema::channel_key_frame_suffix});
            if (frame == nullptr) {
                break;
            }
            const auto* const whole = std::get_if<std::int64_t>(frame);
            if (whole == nullptr) {
                valid_channel = false;
                break;
            }
            ParsedKey key{};
            key.frame = *whole;
            if (const auto* const held = find(
                    node.properties,
                    prefix + ordinal +
                        std::string{scene::schema::channel_key_value_suffix})) {
                if (const auto* const real = std::get_if<double>(held)) {
                    key.value = *real;
                } else if (const auto* const integer =
                               std::get_if<std::int64_t>(held)) {
                    key.value = static_cast<double>(*integer);
                } else {
                    valid_channel = false;
                }
            }
            if (!std::isfinite(key.value) || key.value < -maximum_value ||
                key.value > maximum_value) {
                valid_channel = false;
            }
            if (const auto* const held = find(
                    node.properties,
                    prefix + ordinal +
                        std::string{scene::schema::channel_key_interp_suffix})) {
                const auto* const name = std::get_if<std::string>(held);
                if (name == nullptr) {
                    valid_channel = false;
                } else if (*name == scene::schema::interp_hold) {
                    key.interpolation = scene::KeyInterpolation::Hold;
                } else if (*name == scene::schema::interp_linear) {
                    key.interpolation = scene::KeyInterpolation::Linear;
                } else if (*name == scene::schema::interp_cubic) {
                    key.interpolation = scene::KeyInterpolation::Cubic;
                } else {
                    valid_channel = false;
                }
            }
            parsed.push_back(key);
        }
        if (!valid_channel || parsed.empty()) {
            return;
        }
        std::stable_sort(parsed.begin(), parsed.end(),
                         [](const ParsedKey& left,
                            const ParsedKey& right) noexcept {
                             return left.frame < right.frame;
                         });
        scene::ChannelKeyframes channel{};
        channel.target = doc::NodeId{static_cast<core::u64>(*target)};
        channel.component = property;
        channel.frames.reserve(parsed.size());
        channel.values.reserve(parsed.size());
        channel.interpolations.reserve(parsed.size());
        for (const ParsedKey& key : parsed) {
            channel.frames.push_back(key.frame);
            channel.values.push_back(key.value);
            channel.interpolations.push_back(key.interpolation);
        }
        surface_channels_.insert_or_assign(node.id, std::move(channel));
        return;
    }

    if (node.type != schema::map_type && node.type != schema::surface_type) {
        // Not a feed-map node. Nothing to hold and nothing to report.
        return;
    }

    Reader reader{node.properties};

    if (node.type == schema::surface_type) {
        ProjectionSurface surface{};
        surface.node = node.id;
        surface.name = std::string{reader.text(schema::surface_name, "Surface")};
        surface.locked = reader.boolean(schema::locked, true);
        surface.color.enabled = reader.boolean(schema::wall_color_enabled, false);
        surface.color.camera_inverse_lut_source = reader.text(schema::camera_inverse_lut, "");
        for (std::size_t i = 0; i < surface.color.primaries.size(); ++i)
            surface.color.primaries[i] = reader.number(schema::wall_primaries[i], surface.color.primaries[i]);
        surface.color = scene::clamped(surface.color);
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
        const std::int64_t mesh_asset =
            reader.integer(schema::mesh_asset_id, 0);
        if (mesh_asset < 0) {
            reader.fail();
        } else {
            surface.mesh_asset =
                doc::NodeId{static_cast<core::u64>(mesh_asset)};
        }
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
    core::u32 viewport_x{};
    core::u32 viewport_y{};
    core::u32 viewport_width{};
    core::u32 viewport_height{};
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

    if (!as_u32(reader.integer(schema::viewport_x, -1), viewport_x) ||
        !as_u32(reader.integer(schema::viewport_y, -1), viewport_y) ||
        !as_u32(reader.integer(schema::viewport_width, 0), viewport_width) ||
        !as_u32(reader.integer(schema::viewport_height, 0), viewport_height) ||
        viewport_width == 0U || viewport_height == 0U) {
        reader.fail();
    }

    if (!reader.ok()) {
        ++counters_.rejected;
        return;
    }

    OutputMap map{};
    map.node = node.id;
    map.node_index = index;
    map.source = scene::CanvasRect{x, y, width, height};
    map.viewport = scene::CanvasRect{viewport_x, viewport_y, viewport_width,
                                     viewport_height};
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
    surface_channels_.clear();
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
    return evaluate(scene::SceneTime{});
}

FeedMap FeedMapModel::evaluate(scene::SceneTime time) const {
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

    // Surface transform channels use the same document vocabulary and
    // interpolation as scene objects. Channel source ids are ordered, so the
    // first valid channel owns one (surface, component) slot just as it does in
    // SceneModel. Yaw is deliberately horizontal: this is the same canonical
    // vertical basis authored by the Mapping panel's YAW field.
    std::set<std::pair<doc::NodeId, scene::PropertyChannel>> applied;
    core::u32 channels_seen = 0U;
    const core::i64 frame = time.frame > static_cast<core::u64>(
                                         std::numeric_limits<core::i64>::max())
        ? std::numeric_limits<core::i64>::max()
        : static_cast<core::i64>(time.frame);
    for (const auto& [source, channel] : surface_channels_) {
        static_cast<void>(source);
        const auto found = std::find_if(
            feed_map.surfaces.begin(), feed_map.surfaces.end(),
            [&channel](const ProjectionSurface& surface) noexcept {
                return surface.node == channel.target;
            });
        if (found == feed_map.surfaces.end()) {
            continue;
        }
        if (channels_seen >= scene::max_scene_channels) {
            break;
        }
        ++channels_seen;
        if (!applied.emplace(channel.target, channel.component).second) {
            continue;
        }
        const float value = static_cast<float>(sample_channel(channel, frame));
        switch (channel.component) {
        case scene::PropertyChannel::PositionX:
            found->geometry.center.x = value;
            break;
        case scene::PropertyChannel::PositionY:
            found->geometry.center.y = value;
            break;
        case scene::PropertyChannel::PositionZ:
            found->geometry.center.z = value;
            break;
        case scene::PropertyChannel::RotationYaw:
            found->geometry.right = scene::Vec3{
                scene::deterministic::cos_turns(value), 0.0F,
                -scene::deterministic::sin_turns(value)};
            found->geometry.up = scene::Vec3{0.0F, 1.0F, 0.0F};
            break;
        default:
            break;
        }
    }

    // Entries are in node-id order. Multiple entries may target the same
    // logical window: each is an independent viewport in that window.
    // Truncation at the cap is deterministic by document id.
    for (const auto& [id, map] : entries_) {
        static_cast<void>(id);
        if (feed_map.maps.size() >= max_output_maps) {
            break;
        }
        OutputMap resolved = map;
        if (resolved.projection == FeedProjection::PlanarSurface) {
            const ProjectionSurface* const surface =
                feed_map.surface_for(resolved.surface);
            if (surface != nullptr &&
                surface->shape == ProjectionSurfaceShape::ImportedMesh) {
                // One imported file is one connector patch. UV0 owns the
                // complete destination image, so the source rectangle cannot
                // imply a second crop that the mesh-warp renderer ignores.
                resolved.source = scene::whole(surface->geometry.raster);
                resolved.resolved_surface = ResolvedProjectionSurface{
                    surface->geometry, surface->shape,
                    surface->horizontal_arc_turns, surface->mesh_columns,
                    surface->mesh_source, surface->mesh_scale};
                resolved.resolved_surface->mesh_asset = surface->mesh_asset;
                resolved.resolved_surface->color = surface->color;
            } else if (surface != nullptr &&
                       scene::contains(surface->geometry.raster,
                                       resolved.source)) {
                resolved.resolved_surface = ResolvedProjectionSurface{
                    surface->geometry, surface->shape,
                    surface->horizontal_arc_turns, surface->mesh_columns,
                    surface->mesh_source, surface->mesh_scale};
                resolved.resolved_surface->mesh_asset = surface->mesh_asset;
                resolved.resolved_surface->color = surface->color;
            }
        }
        feed_map.maps.push_back(std::move(resolved));
    }

    // By window index and then document id, which makes each window's maps a
    // contiguous span and gives compositing/digesting a stable order.
    std::sort(
        feed_map.maps.begin(),
        feed_map.maps.end(),
        [](const OutputMap& left, const OutputMap& right) noexcept {
            return left.node_index < right.node_index ||
                (left.node_index == right.node_index &&
                 left.node.value() < right.node.value());
        });

    return feed_map;
}

} // namespace mediaserver::output
