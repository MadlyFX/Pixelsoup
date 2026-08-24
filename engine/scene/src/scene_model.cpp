#include <mediaserver/scene/scene_model.hpp>

#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace mediaserver::scene {
namespace {

[[nodiscard]] const doc::PropertyValue* find(
    const doc::PropertyMap& properties,
    std::string_view name) noexcept {
    const auto found = properties.find(name);
    return found == properties.end() ? nullptr : &found->second;
}

// A property reader that fails loudly rather than quietly.
//
// A property that is absent takes its default: documents should not have to
// spell out every field, and a scene authored before a property existed must
// still load. A property that is *present with the wrong type* rejects the
// whole node instead, because that is a typo or a schema mismatch, and
// silently substituting a default there would leave an operator staring at an
// object in the wrong place with no indication why.
class Reader final {
public:
    explicit Reader(const doc::PropertyMap& properties) noexcept
        : properties_(&properties) {}

    [[nodiscard]] float number(std::string_view name, float fallback) noexcept {
        const auto* const value = find(*properties_, name);
        if (value == nullptr) {
            return fallback;
        }
        double wide{};
        if (const auto* const real = std::get_if<double>(value)) {
            wide = *real;
        } else if (const auto* const whole = std::get_if<std::int64_t>(value)) {
            // An operator who types 3 for a scale should get 3.0, not a
            // rejected node.
            wide = static_cast<double>(*whole);
        } else {
            ok_ = false;
            return fallback;
        }
        const auto narrow = static_cast<float>(wide);
        if (!std::isfinite(narrow)) {
            ok_ = false;
            return fallback;
        }
        return narrow;
    }

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

    [[nodiscard]] Vec3 vector(
        std::string_view x,
        std::string_view y,
        std::string_view z,
        Vec3 fallback) noexcept {
        return Vec3{
            number(x, fallback.x), number(y, fallback.y), number(z, fallback.z)};
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

core::u32 SceneCounters::duplicate_singletons() const noexcept {
    const auto extra_stages = stages > 1U ? stages - 1U : 0U;
    const auto extra_cameras = cameras > 1U ? cameras - 1U : 0U;
    return extra_stages + extra_cameras;
}

core::u32 SceneCounters::over_capacity() const noexcept {
    const auto over = [](core::u32 held, core::u32 cap) noexcept -> core::u32 {
        return held > cap ? held - cap : 0U;
    };
    return over(objects, max_scene_instances)
        + over(media, max_scene_media)
        + over(directional_lights, max_scene_directional_lights)
        + over(point_lights, max_scene_point_lights)
        + over(slices, max_scene_slices)
        + over(channels, max_scene_channels);
}

void SceneModel::note(const Entry& entry, bool added) noexcept {
    const auto step = [added](core::u32& counter) noexcept {
        if (added) {
            ++counter;
        } else if (counter != 0U) {
            --counter;
        }
    };
    if (std::holds_alternative<StageEntry>(entry)) {
        step(counters_.stages);
    } else if (std::holds_alternative<CameraEntry>(entry)) {
        step(counters_.cameras);
    } else if (std::holds_alternative<ObjectEntry>(entry)) {
        step(counters_.objects);
    } else if (std::holds_alternative<MediaEntry>(entry)) {
        step(counters_.media);
    } else if (const auto* const light = std::get_if<LightEntry>(&entry)) {
        step(light->directional ? counters_.directional_lights
                                : counters_.point_lights);
    } else if (std::holds_alternative<ChannelEntry>(entry)) {
        step(counters_.channels);
    } else {
        step(counters_.slices);
    }
}

void SceneModel::forget(doc::NodeId id) noexcept {
    const auto found = entries_.find(id);
    if (found == entries_.end()) {
        return;
    }
    note(found->second, false);
    entries_.erase(found);
}

void SceneModel::put(const doc::Node& node) {
    // A node whose type is not a scene type is not this model's business, but
    // it may have *been* one a moment ago: retyping a node from scene.object
    // to something else has to remove the object. So the old entry goes first,
    // whatever the new type turns out to be.
    forget(node.id);

    Reader reader{node.properties};
    std::optional<Entry> parsed;

    if (node.type == schema::stage_type) {
        StageEntry stage{};
        core::u32 width{};
        core::u32 height{};
        if (!as_u32(reader.integer(schema::canvas_width, 1'920), width)
            || !as_u32(reader.integer(schema::canvas_height, 1'080), height)) {
            reader.fail();
        }
        stage.canvas = Canvas{width, height};
        if (!valid(stage.canvas)) {
            reader.fail();
        }
        const Environment defaults{};
        stage.environment.sky_color = reader.vector(
            schema::sky_r, schema::sky_g, schema::sky_b, defaults.sky_color);
        stage.environment.ground_color = reader.vector(
            schema::ground_r,
            schema::ground_g,
            schema::ground_b,
            defaults.ground_color);
        stage.environment.ambient = reader.number(schema::ambient, defaults.ambient);
        parsed = Entry{stage};
    } else if (node.type == schema::camera_type) {
        CameraEntry camera{};
        const Camera defaults{};
        camera.camera.eye = reader.vector(
            schema::eye_x, schema::eye_y, schema::eye_z, defaults.eye);
        camera.camera.target = reader.vector(
            schema::target_x, schema::target_y, schema::target_z, defaults.target);
        camera.camera.up =
            reader.vector(schema::up_x, schema::up_y, schema::up_z, defaults.up);
        camera.camera.field_of_view =
            reader.number(schema::fov_turns, defaults.field_of_view);
        camera.camera.near_plane =
            reader.number(schema::near_plane, defaults.near_plane);
        camera.camera.far_plane =
            reader.number(schema::far_plane, defaults.far_plane);
        camera.orbit_turns_per_second =
            reader.number(schema::orbit_turns_per_second, 0.0F);
        if (!valid(camera.camera)) {
            reader.fail();
        }
        parsed = Entry{camera};
    } else if (node.type == schema::object_type) {
        ObjectEntry object{};
        auto primitive = Primitive::Box;
        const auto name =
            reader.text(schema::primitive, primitive_name(Primitive::Box));
        if (!parse_primitive(name, primitive)) {
            reader.fail();
        }
        core::u32 segments{};
        if (!as_u32(reader.integer(schema::segments, 16), segments)) {
            reader.fail();
        }
        object.mesh = normalized(MeshKey{primitive, segments});
        object.position = reader.vector(
            schema::position_x, schema::position_y, schema::position_z, Vec3{});
        object.yaw = reader.number(schema::rotation_yaw, 0.0F);
        object.pitch = reader.number(schema::rotation_pitch, 0.0F);
        object.roll = reader.number(schema::rotation_roll, 0.0F);
        object.scale = reader.vector(
            schema::scale_x,
            schema::scale_y,
            schema::scale_z,
            Vec3{1.0F, 1.0F, 1.0F});
        const Material defaults{};
        object.material.base_color = reader.vector(
            schema::color_r, schema::color_g, schema::color_b, defaults.base_color);
        object.material.roughness =
            reader.number(schema::roughness, defaults.roughness);
        object.material.metallic =
            reader.number(schema::metallic, defaults.metallic);
        object.material.emissive = reader.vector(
            schema::emissive_r,
            schema::emissive_g,
            schema::emissive_b,
            defaults.emissive);
        object.spin_turns_per_second =
            reader.number(schema::spin_turns_per_second, 0.0F);
        object.bob_amplitude = reader.number(schema::bob_amplitude, 0.0F);
        object.bob_turns_per_second =
            reader.number(schema::bob_turns_per_second, 0.0F);
        parsed = Entry{object};
    } else if (node.type == schema::media_type) {
        MediaEntry media{};
        media.source = std::string{reader.text(schema::source, "")};
        if (media.source.empty()) {
            // A clip with no sequence to load is a mistake, not an empty
            // default: refuse it so it reads as unauthored rather than blank.
            reader.fail();
        }
        core::u32 start{};
        core::u32 count{};
        if (!as_u32(reader.integer(schema::sequence_start, 0), start)
            || !as_u32(reader.integer(schema::sequence_count, 1), count)) {
            reader.fail();
        }
        if (count == 0U) {
            reader.fail();
        }
        media.sequence_start = start;
        media.sequence_count = count;
        media.position = reader.vector(
            schema::position_x, schema::position_y, schema::position_z, Vec3{});
        media.yaw = reader.number(schema::rotation_yaw, 0.0F);
        media.pitch = reader.number(schema::rotation_pitch, 0.0F);
        media.roll = reader.number(schema::rotation_roll, 0.0F);
        media.scale = reader.vector(
            schema::scale_x,
            schema::scale_y,
            schema::scale_z,
            Vec3{1.0F, 1.0F, 1.0F});
        parsed = Entry{media};
    } else if (node.type == schema::light_type) {
        LightEntry light{};
        const auto kind = reader.text(schema::light_kind, schema::directional_kind);
        if (kind == schema::directional_kind) {
            light.directional = true;
            light.vector = reader.vector(
                schema::direction_x,
                schema::direction_y,
                schema::direction_z,
                Vec3{0.0F, -1.0F, 0.0F});
        } else if (kind == schema::point_kind) {
            light.directional = false;
            light.vector = reader.vector(
                schema::position_x,
                schema::position_y,
                schema::position_z,
                Vec3{});
        } else {
            reader.fail();
        }
        light.color = reader.vector(
            schema::color_r, schema::color_g, schema::color_b, Vec3{1.0F, 1.0F, 1.0F});
        light.intensity = reader.number(schema::intensity, 1.0F);
        light.range = reader.number(schema::range, 10.0F);
        parsed = Entry{light};
    } else if (node.type == schema::slice_type) {
        SliceEntry slice{};
        core::u32 index{};
        core::u32 x{};
        core::u32 y{};
        core::u32 width{};
        core::u32 height{};
        if (!as_u32(reader.integer(schema::node_index, 0), index)
            || !as_u32(reader.integer(schema::slice_x, 0), x)
            || !as_u32(reader.integer(schema::slice_y, 0), y)
            || !as_u32(reader.integer(schema::slice_width, 0), width)
            || !as_u32(reader.integer(schema::slice_height, 0), height)) {
            reader.fail();
        }
        if (width == 0U || height == 0U) {
            reader.fail();
        }
        slice.binding = SliceBinding{index, CanvasRect{x, y, width, height}};
        parsed = Entry{slice};
    } else if (node.type == schema::channel_type) {
        ChannelEntry channel{};
        const auto target = reader.integer(schema::channel_target, -1);
        if (target < 0) {
            reader.fail();
        }
        channel.target = doc::NodeId{static_cast<core::u64>(target)};

        // The component names are the transform property names, so authoring a
        // channel needs no vocabulary the operator doesn't already know.
        const auto component = reader.text(schema::channel_component, "");
        if (component == schema::position_x) {
            channel.component = TransformChannel::PositionX;
        } else if (component == schema::position_y) {
            channel.component = TransformChannel::PositionY;
        } else if (component == schema::position_z) {
            channel.component = TransformChannel::PositionZ;
        } else if (component == schema::rotation_yaw) {
            channel.component = TransformChannel::RotationYaw;
        } else if (component == schema::rotation_pitch) {
            channel.component = TransformChannel::RotationPitch;
        } else if (component == schema::rotation_roll) {
            channel.component = TransformChannel::RotationRoll;
        } else if (component == schema::scale_x) {
            channel.component = TransformChannel::ScaleX;
        } else if (component == schema::scale_y) {
            channel.component = TransformChannel::ScaleY;
        } else if (component == schema::scale_z) {
            channel.component = TransformChannel::ScaleZ;
        } else {
            reader.fail();
        }

        // Keys are indexed properties parsed in order until the first gap. A key
        // absent its frame ends the list; a key present but malformed rejects the
        // whole channel, like any other typed property.
        const std::string prefix{schema::channel_key_prefix};
        for (core::u32 i = 0U; i < max_channel_keys; ++i) {
            const auto index = std::to_string(i);
            const auto* const frame = find(
                node.properties,
                prefix + index + std::string{schema::channel_key_frame_suffix});
            if (frame == nullptr) {
                break;
            }
            ChannelKey key{};
            if (const auto* const whole = std::get_if<std::int64_t>(frame)) {
                key.frame = *whole;
            } else {
                reader.fail();
                break;
            }
            if (const auto* const value = find(
                    node.properties,
                    prefix + index
                        + std::string{schema::channel_key_value_suffix})) {
                if (const auto* const real = std::get_if<double>(value)) {
                    key.value = *real;
                } else if (const auto* const whole =
                               std::get_if<std::int64_t>(value)) {
                    key.value = static_cast<double>(*whole);
                } else {
                    reader.fail();
                }
            }
            if (const auto* const interp = find(
                    node.properties,
                    prefix + index
                        + std::string{schema::channel_key_interp_suffix})) {
                const auto* const name = std::get_if<std::string>(interp);
                if (name == nullptr) {
                    reader.fail();
                } else if (*name == schema::interp_hold) {
                    key.interp = ChannelInterp::Hold;
                } else if (*name == schema::interp_linear) {
                    key.interp = ChannelInterp::Linear;
                } else if (*name == schema::interp_cubic) {
                    key.interp = ChannelInterp::Cubic;
                } else {
                    reader.fail();
                }
            }
            channel.keys.push_back(key);
        }
        if (channel.keys.empty()) {
            reader.fail();
        }
        // Sorted so evaluation brackets a frame by search; stable so equal-frame
        // keys keep authoring order, and two nodes order them identically.
        std::stable_sort(
            channel.keys.begin(),
            channel.keys.end(),
            [](const ChannelKey& left, const ChannelKey& right) noexcept {
                return left.frame < right.frame;
            });
        parsed = Entry{std::move(channel)};
    } else {
        // Not a scene node. Nothing to hold and nothing to report: the
        // document is full of nodes that are not this model's business.
        return;
    }

    if (!reader.ok()) {
        ++counters_.rejected;
        return;
    }

    note(*parsed, true);
    entries_.insert_or_assign(node.id, std::move(*parsed));
}

void SceneModel::reset(const doc::Root& root) {
    entries_.clear();
    counters_ = SceneCounters{};
    // Diffing against an empty root enumerates every node in ascending id
    // order, which is the enumeration the document does not otherwise offer -
    // and the same code path a delta takes, so a checkpointed node and a
    // streaming one cannot parse differently.
    apply(diff(doc::Root{}, root));
}

void SceneModel::apply(const doc::Delta& delta) {
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

double SceneModel::sample_channel(
    const ChannelEntry& channel, core::i64 frame) noexcept {
    const auto& keys = channel.keys;
    // Held flat outside the authored range, so a channel never extrapolates.
    if (frame <= keys.front().frame) {
        return keys.front().value;
    }
    if (frame >= keys.back().frame) {
        return keys.back().value;
    }
    // The first key strictly after the frame; the one before it opens the
    // interval. Both exist: the clamps above rule out the ends.
    const auto after = std::upper_bound(
        keys.begin(),
        keys.end(),
        frame,
        [](core::i64 f, const ChannelKey& key) noexcept {
            return f < key.frame;
        });
    const auto& lo = *(after - 1);
    const auto& hi = *after;
    if (lo.interp == ChannelInterp::Hold) {
        return lo.value;
    }
    const double span = static_cast<double>(hi.frame - lo.frame);
    double t = static_cast<double>(frame - lo.frame) / span;
    if (lo.interp == ChannelInterp::Cubic) {
        // Smoothstep: a basic cubic ease with zero end tangents. Explicit
        // tangent handles are a later refinement.
        t = t * t * (3.0 - 2.0 * t);
    }
    return lo.value + (hi.value - lo.value) * t;
}

std::vector<ChannelKeyframes> SceneModel::channel_keyframes() const {
    // Keyed by (target, component) so the timeline draws diamonds on the right
    // per-component row. Ordered by the map so output is deterministic.
    std::map<std::pair<doc::NodeId, TransformChannel>, std::vector<core::i64>>
        by_channel;
    for (const auto& [id, entry] : entries_) {
        const auto* const channel = std::get_if<ChannelEntry>(&entry);
        if (channel == nullptr) {
            continue;
        }
        auto& frames = by_channel[{channel->target, channel->component}];
        for (const auto& key : channel->keys) {
            frames.push_back(key.frame);
        }
    }
    std::vector<ChannelKeyframes> result;
    result.reserve(by_channel.size());
    for (auto& [key, frames] : by_channel) {
        std::sort(frames.begin(), frames.end());
        frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
        result.push_back(
            ChannelKeyframes{key.first, key.second, std::move(frames)});
    }
    return result;
}

Scene SceneModel::evaluate(SceneTime time) const {
    Scene scene{};
    const auto seconds = time.seconds();

    for (const auto& held : entries_) {
        if (const auto* const stage = std::get_if<StageEntry>(&held.second)) {
            scene.canvas = stage->canvas;
            scene.environment = stage->environment;
            break;
        }
    }

    for (const auto& held : entries_) {
        const auto* const camera = std::get_if<CameraEntry>(&held.second);
        if (camera == nullptr) {
            continue;
        }
        scene.camera = camera->camera;
        if (camera->orbit_turns_per_second != 0.0F) {
            const auto turns = wrap_turns(
                static_cast<double>(camera->orbit_turns_per_second) * seconds);
            const auto offset = rotate_around(
                scene.camera.eye - scene.camera.target, scene.camera.up, turns);
            scene.camera.eye = scene.camera.target + offset;
        }
        break;
    }

    // Keyframe channels override transform components at this frame. Built once,
    // before the drawables, so each object or clip picks up its overrides as it
    // is built. Lowest-id channel wins per (target, component): entries_ is id-
    // ordered, so the first writer of a slot keeps it, matching how a duplicate
    // stage or camera resolves. Past the channel cap the rest are ignored, in id
    // order, so every node truncates identically.
    using Override =
        std::array<std::optional<float>, static_cast<std::size_t>(TransformChannel::Count)>;
    std::map<doc::NodeId, Override> overrides;
    core::u32 channels_seen = 0U;
    for (const auto& [id, entry] : entries_) {
        const auto* const channel = std::get_if<ChannelEntry>(&entry);
        if (channel == nullptr) {
            continue;
        }
        if (channels_seen >= max_scene_channels) {
            break;
        }
        ++channels_seen;
        auto& slot =
            overrides[channel->target][static_cast<std::size_t>(channel->component)];
        if (!slot.has_value()) {
            slot = static_cast<float>(
                sample_channel(*channel, static_cast<core::i64>(time.frame)));
        }
    }
    const auto apply_override = [&overrides](
                                   doc::NodeId id,
                                   Vec3& position,
                                   Turns& yaw,
                                   Turns& pitch,
                                   Turns& roll,
                                   Vec3& scale) {
        const auto found = overrides.find(id);
        if (found == overrides.end()) {
            return;
        }
        const auto& ov = found->second;
        const auto at = [&ov](TransformChannel c) {
            return ov[static_cast<std::size_t>(c)];
        };
        if (const auto v = at(TransformChannel::PositionX)) {
            position.x = *v;
        }
        if (const auto v = at(TransformChannel::PositionY)) {
            position.y = *v;
        }
        if (const auto v = at(TransformChannel::PositionZ)) {
            position.z = *v;
        }
        if (const auto v = at(TransformChannel::RotationYaw)) {
            yaw = *v;
        }
        if (const auto v = at(TransformChannel::RotationPitch)) {
            pitch = *v;
        }
        if (const auto v = at(TransformChannel::RotationRoll)) {
            roll = *v;
        }
        if (const auto v = at(TransformChannel::ScaleX)) {
            scale.x = *v;
        }
        if (const auto v = at(TransformChannel::ScaleY)) {
            scale.y = *v;
        }
        if (const auto v = at(TransformChannel::ScaleZ)) {
            scale.z = *v;
        }
    };

    scene.instances.reserve(
        std::min(counters_.objects, max_scene_instances));
    scene.media.reserve(std::min(counters_.media, max_scene_media));
    scene.slices.reserve(std::min(counters_.slices, max_scene_slices));

    for (const auto& [id, entry] : entries_) {
        if (const auto* const object = std::get_if<ObjectEntry>(&entry)) {
            if (scene.instances.size() >= max_scene_instances) {
                continue;
            }
            // Channels replace the base transform components; the procedural
            // spin and bob still apply on top of whatever the channel set.
            auto position = object->position;
            Turns yaw_base = object->yaw;
            Turns pitch = object->pitch;
            Turns roll = object->roll;
            Vec3 scale = object->scale;
            apply_override(id, position, yaw_base, pitch, roll, scale);

            const auto spin = static_cast<double>(object->spin_turns_per_second)
                * seconds;
            const auto yaw = wrap_turns(static_cast<double>(yaw_base) + spin);

            if (object->bob_amplitude != 0.0F) {
                const auto phase = wrap_turns(
                    static_cast<double>(object->bob_turns_per_second) * seconds);
                position.y += object->bob_amplitude
                    * deterministic::sin_turns(phase);
            }

            const auto world = multiply(
                translation(position),
                multiply(rotation(yaw, pitch, roll), scaling(scale)));
            scene.instances.push_back(MeshInstance{
                object->mesh,
                world,
                normal_basis(world),
                object->material,
                id});
            continue;
        }

        if (const auto* const clip = std::get_if<MediaEntry>(&entry)) {
            if (scene.media.size() >= max_scene_media) {
                continue;
            }
            auto position = clip->position;
            Turns yaw = clip->yaw;
            Turns pitch = clip->pitch;
            Turns roll = clip->roll;
            Vec3 scale = clip->scale;
            apply_override(id, position, yaw, pitch, roll, scale);
            const auto world = multiply(
                translation(position),
                multiply(rotation(yaw, pitch, roll), scaling(scale)));
            // The show frame counts into the sequence and loops. Every node
            // computes the same file number from the same frame by the same
            // arithmetic, so the wall shows one clip frame-exact.
            const auto within =
                static_cast<core::u32>(time.frame % clip->sequence_count);
            scene.media.push_back(MediaInstance{
                clip->mesh,
                world,
                normal_basis(world),
                clip->source,
                clip->sequence_start + within,
                id});
            continue;
        }

        if (const auto* const light = std::get_if<LightEntry>(&entry)) {
            if (light->directional) {
                if (scene.directional_lights.size()
                    < max_scene_directional_lights) {
                    scene.directional_lights.push_back(DirectionalLight{
                        normalize(light->vector, Vec3{0.0F, -1.0F, 0.0F}),
                        light->color,
                        light->intensity});
                }
            } else if (scene.point_lights.size() < max_scene_point_lights) {
                scene.point_lights.push_back(PointLight{
                    light->vector, light->color, light->intensity, light->range});
            }
            continue;
        }

        if (const auto* const slice = std::get_if<SliceEntry>(&entry)) {
            if (scene.slices.size() < max_scene_slices) {
                scene.slices.push_back(slice->binding);
            }
        }
    }

    // Grouped by mesh so the renderer can draw one batch per distinct mesh,
    // and stably so that instances sharing a mesh stay in node id order. Both
    // halves are part of what two nodes have to agree on, which is why this is
    // here rather than in the renderer.
    std::stable_sort(
        scene.instances.begin(),
        scene.instances.end(),
        [](const MeshInstance& left, const MeshInstance& right) noexcept {
            return left.mesh < right.mesh;
        });

    return scene;
}

} // namespace mediaserver::scene
