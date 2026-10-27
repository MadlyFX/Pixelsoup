#include <mediaserver/render/scene_bridge.hpp>

#include <algorithm>
#include <bit>
#include <new>
#include <stdexcept>
#include <string_view>

namespace mediaserver::render {
namespace {

inline constexpr core::u64 fnv_offset = 14'695'981'039'346'656'037ULL;
inline constexpr core::u64 fnv_prime = 1'099'511'628'211ULL;

void mix(core::u64& state, core::u64 value) noexcept {
    for (core::u32 shift = 0U; shift < 64U; shift += 8U) {
        state ^= (value >> shift) & 0xffU;
        state *= fnv_prime;
    }
}

void mix(core::u64& state, float value) noexcept {
    mix(state, static_cast<core::u64>(std::bit_cast<core::u32>(value)));
}

void mix(core::u64& state, const scene::Vec3& value) noexcept {
    mix(state, value.x);
    mix(state, value.y);
    mix(state, value.z);
}

void mix(core::u64& state, const scene::Mat4& value) noexcept {
    for (const float lane : value.m) {
        mix(state, lane);
    }
}

[[nodiscard]] core::u64 hash(std::string_view text) noexcept {
    core::u64 state = fnv_offset;
    mix(state, static_cast<core::u64>(text.size()));
    for (const char value : text) {
        state ^= static_cast<unsigned char>(value);
        state *= fnv_prime;
    }
    return state;
}

[[nodiscard]] core::u64 hash(const scene::Material& material) noexcept {
    core::u64 state = fnv_offset;
    mix(state, material.base_color);
    mix(state, material.roughness);
    mix(state, material.metallic);
    mix(state, material.emissive);
    mix(state, material.emissive_strength);
    mix(state, material.normal_scale);
    mix(state, material.occlusion_strength);
    mix(state, static_cast<core::u64>(material.alpha_mode));
    mix(state, material.alpha_cutoff);
    mix(state, static_cast<core::u64>(material.double_sided));
    mix(state, hash(material.base_color_texture));
    mix(state, hash(material.metallic_roughness_texture));
    mix(state, hash(material.normal_texture));
    mix(state, hash(material.occlusion_texture));
    mix(state, hash(material.emissive_texture));
    mix(state, material.base_alpha);
    mix(state, material.clearcoat);
    mix(state, material.clearcoat_roughness);
    mix(state, material.clearcoat_normal_scale);
    mix(state, material.sheen_roughness);
    mix(state, material.transmission);
    mix(state, material.ior);
    mix(state, material.specular);
    mix(state, material.anisotropy_strength);
    mix(state, material.anisotropy_rotation);
    mix(state, material.sheen_color);
    mix(state, material.specular_color);
    mix(state, hash(material.clearcoat_texture));
    mix(state, hash(material.clearcoat_roughness_texture));
    mix(state, hash(material.clearcoat_normal_texture));
    mix(state, hash(material.sheen_color_texture));
    mix(state, hash(material.sheen_roughness_texture));
    mix(state, hash(material.transmission_texture));
    mix(state, hash(material.specular_texture));
    mix(state, hash(material.specular_color_texture));
    mix(state, hash(material.anisotropy_texture));
    mix(state, static_cast<core::u64>(material.fill.has_value()));
    if (material.fill.has_value()) {
        const scene::Fill& fill = *material.fill;
        mix(state, static_cast<core::u64>(fill.mode));
        for (const scene::FillColor& color : {fill.color_a, fill.color_b}) {
            mix(state, color.r);
            mix(state, color.g);
            mix(state, color.b);
            mix(state, color.a);
        }
        mix(state, fill.angle);
        mix(state, fill.center_x);
        mix(state, fill.center_y);
        mix(state, fill.size);
    }

    return state;
}

[[nodiscard]] core::u64 visual(
    const scene::Mat4& world,
    core::u64 payload) noexcept {
    core::u64 state = fnv_offset;
    mix(state, world);
    mix(state, payload);
    return state;
}

[[nodiscard]] bool record_less(
    const GpuSceneRecord& left,
    const GpuSceneRecord& right) noexcept {
    if (left.kind != right.kind) {
        return left.kind < right.kind;
    }
    return left.source < right.source;
}

} // namespace

core::Result<SceneBridge, SceneBridgeError> SceneBridge::create(
    core::u32 max_records) {
    using Result = core::Result<SceneBridge, SceneBridgeError>;
    if (max_records == 0U) {
        return Result::failure(SceneBridgeError::InvalidConfiguration);
    }
    try {
        SceneBridge bridge{max_records};
        bridge.records_.reserve(max_records);
        bridge.scratch_.reserve(max_records);
        return Result::success(std::move(bridge));
    } catch (const std::bad_alloc&) {
        return Result::failure(SceneBridgeError::AllocationFailed);
    } catch (const std::length_error&) {
        return Result::failure(SceneBridgeError::AllocationFailed);
    }
}

core::Result<SceneBridgeUpdate, SceneBridgeError> SceneBridge::update(
    const scene::Scene& value) noexcept {
    auto result = prepare(value);
    if (result) {
        commit();
    }
    return result;
}

core::Result<SceneBridgeUpdate, SceneBridgeError> SceneBridge::prepare(
    const scene::Scene& value) noexcept {
    using Result = core::Result<SceneBridgeUpdate, SceneBridgeError>;
    discard();
    const std::size_t required = value.instances.size() + value.media.size() +
        value.models.size() + value.directional_lights.size() +
        value.point_lights.size();
    if (required > capacity_) {
        return Result::failure(SceneBridgeError::CapacityExceeded);
    }

    scratch_.clear();
    const scene::Mat4 identity = scene::identity();
    for (const scene::MeshInstance& instance : value.instances) {
        core::u64 asset = fnv_offset;
        mix(asset, static_cast<core::u64>(instance.mesh.primitive));
        mix(asset, static_cast<core::u64>(instance.mesh.segments));
        const core::u64 appearance = hash(instance.material);
        scratch_.push_back(GpuSceneRecord{
            instance.source, GpuRecordKind::Mesh, asset,
            visual(instance.world, appearance), instance.world,
            instance.world});
    }
    for (const scene::MediaInstance& media : value.media) {
        core::u64 asset = media.asset.value();
        if (asset == 0U) {
            asset = hash(media.source);
        }
        core::u64 payload = fnv_offset;
        mix(payload, static_cast<core::u64>(media.frame_index));
        mix(payload, media.style.opacity);
        scratch_.push_back(GpuSceneRecord{
            media.source_node, GpuRecordKind::Media, asset,
            visual(media.world, payload), media.world, media.world});
    }
    for (const scene::ModelInstance& model : value.models) {
        const core::u64 asset = hash(model.source);
        auto payload=hash(model.material);
        if (model.material_override_mask) mix(payload, static_cast<core::u64>(model.material_override_mask));
        if (model.animation!=scene::ModelAnimation{}) {
            mix(payload,static_cast<core::u64>(model.animation.enabled));
            mix(payload,static_cast<core::u64>(model.animation.clip));
            mix(payload,static_cast<core::u64>(model.animation.loop));
            mix(payload,model.animation.start_frame); mix(payload,model.animation.wind);
            mix(payload,static_cast<core::u64>(model.animation.wind_period_frames));
        }
        scratch_.push_back(GpuSceneRecord{
            model.motion_source.value() ? model.motion_source : model.source_node, GpuRecordKind::Model, asset,
            visual(model.world, payload), model.world,
            model.world});
    }
    for (const scene::DirectionalLight& light : value.directional_lights) {
        core::u64 payload = fnv_offset;
        mix(payload, light.direction);
        mix(payload, light.color);
        mix(payload, light.intensity);
        if (light.unit != scene::LightUnit::Unitless)
            mix(payload, value.renderer_settings.photometric_scale);
        mix(payload, static_cast<core::u64>(light.unit));
        mix(payload, light.angular_radius);
        mix(payload, light.color_temperature_kelvin);
        mix(payload, static_cast<core::u64>(light.use_color_temperature));
        scratch_.push_back(GpuSceneRecord{
            light.source, GpuRecordKind::DirectionalLight, 0U, payload,
            identity, identity});
    }
    for (const scene::PointLight& light : value.point_lights) {
        core::u64 payload = fnv_offset;
        for (const unsigned char c : light.ies_source) mix(payload, static_cast<core::u64>(c));
        mix(payload, light.ies_rotation);
        mix(payload, light.position);
        mix(payload, light.color);
        mix(payload, light.intensity);
        if (light.unit != scene::LightUnit::Unitless)
            mix(payload, value.renderer_settings.photometric_scale);
        mix(payload, light.range);
        mix(payload, static_cast<core::u64>(light.kind));
        mix(payload, static_cast<core::u64>(light.unit));
        mix(payload, light.direction);
        mix(payload, light.inner_cone);
        mix(payload, light.outer_cone);
        mix(payload, light.width);
        mix(payload, light.height);
        mix(payload, static_cast<core::u64>(light.two_sided));
        mix(payload, light.radius);
        mix(payload, light.area_rotation);
        mix(payload, light.color_temperature_kelvin);
        mix(payload, static_cast<core::u64>(light.use_color_temperature));
        scratch_.push_back(GpuSceneRecord{
            light.source, GpuRecordKind::PointLight, 0U, payload,
            identity, identity});
    }

    std::sort(scratch_.begin(), scratch_.end(), record_less);
    SceneBridgeUpdate update{};
    update.records = static_cast<core::u32>(scratch_.size());
    std::size_t old_index = 0U;
    std::size_t new_index = 0U;
    while (old_index < records_.size() && new_index < scratch_.size()) {
        const GpuSceneRecord& old_record = records_[old_index];
        GpuSceneRecord& new_record = scratch_[new_index];
        if (record_less(old_record, new_record)) {
            ++update.removed;
            ++old_index;
        } else if (record_less(new_record, old_record)) {
            ++update.added;
            ++new_index;
        } else {
            new_record.previous_world = old_record.world;
            if (new_record.asset_identity == old_record.asset_identity &&
                new_record.visual_identity == old_record.visual_identity) {
                ++update.unchanged;
            } else {
                ++update.updated;
            }
            ++old_index;
            ++new_index;
        }
    }
    update.removed += static_cast<core::u32>(records_.size() - old_index);
    update.added += static_cast<core::u32>(scratch_.size() - new_index);

    update.topology_digest = fnv_offset;
    update.visual_digest = fnv_offset;
    for (const GpuSceneRecord& record : scratch_) {
        mix(update.topology_digest, static_cast<core::u64>(record.kind));
        mix(update.topology_digest, record.source.value());
        mix(update.topology_digest, record.asset_identity);
        mix(update.visual_digest, record.visual_identity);
    }
    if (value.environment.baked.enabled) {
        mix(update.visual_digest, hash(value.environment.baked.source));
        mix(update.visual_digest, hash(value.environment.baked.content));
        mix(update.visual_digest, value.environment.baked.strength);
    }
    prepared_ = true;
    return Result::success(update);
}

void SceneBridge::commit() noexcept {
    if (!prepared_) {
        return;
    }
    records_.swap(scratch_);
    scratch_.clear();
    prepared_ = false;
}

void SceneBridge::discard() noexcept {
    scratch_.clear();
    prepared_ = false;
}

} // namespace mediaserver::render
