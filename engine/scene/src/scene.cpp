#include <mediaserver/scene/scene.hpp>

#include <cstring>

namespace mediaserver::scene {
namespace {

constexpr core::u64 fnv_offset_basis = 14'695'981'039'346'656'037ULL;
constexpr core::u64 fnv_prime = 1'099'511'628'211ULL;

void mix(core::u64& state, core::u64 value) noexcept {
    for (core::u32 shift = 0U; shift < 64U; shift += 8U) {
        state ^= (value >> shift) & 0xffULL;
        state *= fnv_prime;
    }
}

void mix(core::u64& state, float value) noexcept {
    // The bit pattern, not the value: two floats one ulp apart must not hash
    // equal. std::bit_cast would say this more directly, but memcpy says it in
    // C++20 without pulling in <bit> for one call.
    core::u32 bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    // Negative zero is the one case where distinct bit patterns are the same
    // number, and it arises constantly from negating a zero component. Folding
    // it here keeps the digest from reporting a divergence that no renderer
    // could observe.
    if (bits == 0x8000'0000U) {
        bits = 0U;
    }
    mix(state, static_cast<core::u64>(bits));
}

void mix(core::u64& state, Vec3 value) noexcept {
    mix(state, value.x);
    mix(state, value.y);
    mix(state, value.z);
}

void mix(core::u64& state, const Mat4& value) noexcept {
    for (const auto element : value.m) {
        mix(state, element);
    }
}

void mix(core::u64& state, const Material& value) noexcept {
    mix(state, value.base_color);
    mix(state, value.roughness);
    mix(state, value.metallic);
    mix(state, value.emissive);
}

void mix(core::u64& state, const LayerStyle& value) noexcept {
    mix(state, value.opacity);
    mix(state, static_cast<core::u64>(value.blend));
    mix(state, static_cast<core::u64>(value.effects.brightness_enabled));
    mix(state, value.effects.brightness);
    mix(state, static_cast<core::u64>(value.effects.blur_enabled));
    mix(state, value.effects.blur_radius);
}

void mix(core::u64& state, const Camera& value) noexcept {
    mix(state, value.eye);
    mix(state, value.target);
    mix(state, value.up);
    mix(state, value.field_of_view);
    mix(state, value.near_plane);
    mix(state, value.far_plane);
}

void mix(core::u64& state, CanvasRect value) noexcept {
    mix(state, static_cast<core::u64>(value.x));
    mix(state, static_cast<core::u64>(value.y));
    mix(state, static_cast<core::u64>(value.width));
    mix(state, static_cast<core::u64>(value.height));
}

void mix(core::u64& state, MeshKey value) noexcept {
    mix(state, static_cast<core::u64>(value.primitive));
    mix(state, static_cast<core::u64>(value.segments));
}

void mix(core::u64& state, const std::string& value) noexcept {
    // Length before bytes, so one string that is a prefix of another cannot
    // hash equal.
    mix(state, static_cast<core::u64>(value.size()));
    for (const auto byte : value) {
        mix(state, static_cast<core::u64>(static_cast<unsigned char>(byte)));
    }
}

} // namespace

double SceneTime::seconds() const noexcept {
    if (rate_numerator == 0U) {
        return 0.0;
    }
    return static_cast<double>(frame) * static_cast<double>(rate_denominator)
        / static_cast<double>(rate_numerator);
}

const CanvasRect* Scene::slice_for(core::u32 node_index) const noexcept {
    for (const auto& binding : slices) {
        if (binding.node_index == node_index) {
            return &binding.rect;
        }
    }
    return nullptr;
}

core::u64 digest(const Scene& scene) noexcept {
    auto state = fnv_offset_basis;

    mix(state, scene.id.value());
    mix(state, static_cast<core::u64>(scene.canvas.width));
    mix(state, static_cast<core::u64>(scene.canvas.height));
    mix(state, scene.camera);
    mix(state, scene.environment.sky_color);
    mix(state, scene.environment.ground_color);
    mix(state, scene.environment.ambient);

    // Counts are hashed before their contents so that a scene which is a
    // prefix of another cannot collide with it.
    mix(state, static_cast<core::u64>(scene.instances.size()));
    for (const auto& instance : scene.instances) {
        mix(state, static_cast<core::u64>(instance.mesh.primitive));
        mix(state, static_cast<core::u64>(instance.mesh.segments));
        mix(state, instance.world);
        mix(state, instance.normal);
        mix(state, instance.material);
        mix(state, instance.source.value());
        mix(state, instance.style);
    }

    mix(state, static_cast<core::u64>(scene.media.size()));
    for (const auto& clip : scene.media) {
        mix(state, clip.mesh);
        mix(state, clip.world);
        mix(state, clip.normal);
        mix(state, clip.source);
        mix(state, static_cast<core::u64>(clip.frame_index));
        mix(state, clip.source_node.value());
        mix(state, clip.style);
    }

    mix(state, static_cast<core::u64>(scene.models.size()));
    for (const auto& model : scene.models) {
        mix(state, model.world);
        mix(state, model.normal);
        mix(state, model.source);
        mix(state, model.source_node.value());
        mix(state, model.material);
        mix(state, model.style);
    }

    mix(state, static_cast<core::u64>(scene.effects.size()));
    for (const auto& effect : scene.effects) {
        mix(state, effect.source.value());
        mix(state, effect.target.value());
        mix(state, static_cast<core::u64>(effect.kind));
        mix(state, static_cast<core::u64>(effect.order));
        mix(state, static_cast<core::u64>(effect.enabled));
        mix(state, effect.value);
    }

    mix(state, static_cast<core::u64>(scene.directional_lights.size()));
    for (const auto& light : scene.directional_lights) {
        mix(state, light.direction);
        mix(state, light.color);
        mix(state, light.intensity);
        mix(state, light.source.value());
    }

    mix(state, static_cast<core::u64>(scene.point_lights.size()));
    for (const auto& light : scene.point_lights) {
        mix(state, light.position);
        mix(state, light.color);
        mix(state, light.intensity);
        mix(state, light.range);
        mix(state, light.source.value());
    }

    mix(state, static_cast<core::u64>(scene.slices.size()));
    for (const auto& binding : scene.slices) {
        mix(state, static_cast<core::u64>(binding.node_index));
        mix(state, binding.rect);
    }

    return state;
}

} // namespace mediaserver::scene
