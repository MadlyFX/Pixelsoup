// SceneRenderer particle effects: preparing each placement's draw and
// recording them into the lit pass.

#include "scene_renderer_internal.hpp"

namespace mediaserver::render {

using namespace scene_detail;

namespace {

// FNV-1a over a placement's parameter tracks, where they start and the show
// rate: the inputs its whole history depends on.
core::u64 parameters_digest(const scene::ParticleEffect& effect, scene::MediaFrameRate rate) noexcept {
    core::u64 hash = 0xcbf29ce484222325ULL;
    const auto mix = [&hash](const void* data, std::size_t bytes) {
        const auto* const at = static_cast<const unsigned char*>(data);
        for (std::size_t i = 0U; i < bytes; ++i) {
            hash = (hash ^ at[i]) * 0x100000001b3ULL;
        }
    };
    for (const scene::ParticleParameter& parameter : effect.parameters) {
        mix(parameter.key.data(), parameter.key.size() + 1U);
        mix(&parameter.base, sizeof(parameter.base));
        const scene::ChannelKeyframes& curve = parameter.curve;
        mix(curve.frames.data(), curve.frames.size() * sizeof(core::i64));
        mix(curve.values.data(), curve.values.size() * sizeof(double));
        mix(curve.interpolations.data(), curve.interpolations.size() * sizeof(scene::KeyInterpolation));
        for (const std::optional<scene::KeyTangent>& tangent : curve.tangents) {
            const double handle[5] = {tangent ? static_cast<double>(tangent->mode) : -1.0,
                                      tangent ? tangent->in_slope : 0.0, tangent ? tangent->out_slope : 0.0,
                                      tangent ? tangent->in_weight.value_or(-1.0) : 0.0,
                                      tangent ? tangent->out_weight.value_or(-1.0) : 0.0};
            mix(handle, sizeof(handle));
        }
    }
    mix(&effect.start_frame, sizeof(effect.start_frame));
    mix(&rate.numerator, sizeof(rate.numerator));
    mix(&rate.denominator, sizeof(rate.denominator));
    return hash;
}

// A placement's parameters as the simulation asks for them: each bound
// component at the step's own show time. Step `tick` of an effect starting at
// show frame `start` is at start + tick * num / (rate * den) show frames,
// worked in integers so every node splits it into the same frame and fraction.
// It owns copies of the tracks it reads, so the rigid physics thread can use
// it while the scene changes under the frame.
class PlacedParameters final : public ajfx::ParameterProvider {
public:
    PlacedParameters(std::vector<ParticleParameterBinding> bound, core::u64 start, core::u32 num,
                     core::u32 den, core::u32 rate) noexcept
        : bound_{std::move(bound)}, start_{start}, num_{num}, scale_{static_cast<core::u64>(den) * rate} {}

    void values_at(std::uint64_t tick, std::span<ajfx::ParameterValue> values) const override {
        const core::u64 elapsed = tick * num_;
        const core::u64 whole = elapsed / scale_;
        const double fraction = static_cast<double>(elapsed % scale_) / static_cast<double>(scale_);
        const auto frame = static_cast<core::i64>(start_ + whole);
        for (const ParticleParameterBinding& one : bound_) {
            if (one.index < values.size()) {
                values[one.index][one.component] =
                    static_cast<float>(scene::particle_parameter_at(one.track, frame, fraction));
            }
        }
    }

private:
    std::vector<ParticleParameterBinding> bound_;
    core::u64 start_;
    core::u64 num_;
    core::u64 scale_;
};

// The placement's tracks bound to the package's parameters. A component the
// package does not publish is kept and ignored.
std::shared_ptr<const ajfx::ParameterProvider> placed_parameters(const scene::ParticleEffect& effect,
                                                                 const ajfx::Effect& package,
                                                                 scene::MediaFrameRate rate) {
    std::vector<ParticleParameterBinding> bound;
    for (const scene::ParticleParameter& track : effect.parameters) {
        const std::optional<scene::ParticleParameterKey> key = scene::parse_particle_parameter_key(track.key);
        if (!key) {
            continue;
        }
        for (std::size_t index = 0U; index < package.parameters.size(); ++index) {
            if (package.parameters[index].name == key->name) {
                const std::size_t components = package.parameters[index].type == ajfx::ParameterType::Float ? 1U
                    : package.parameters[index].type == ajfx::ParameterType::Vec3 ? 3U : 4U;
                if (key->component < components) {
                    bound.push_back({index, key->component, track});
                }
                break;
            }
        }
    }
    return std::make_shared<const PlacedParameters>(std::move(bound), effect.start_frame, rate.numerator,
                                                    rate.denominator, package.simulation_rate);
}

} // namespace

void SceneRenderer::prepare_particles(rhi::CommandList& commands, const scene::Scene& scene,
                                      const SceneRenderOptions& options) {
    particle_draws_.clear();
    particle_lights_used_ = 0U;
    const auto refuse = [this](core::u32& count, std::string_view reason) {
        ++count;
        if (stats_.particle_issue.empty()) {
            stats_.particle_issue = reason;
        }
    };
    // Retired instances go once nothing submitted uses them.
    std::erase_if(retired_particles_, [](const ParticleInstance& instance) { return instance.safe_to_release(); });
    // Moves a slot's instance to retirement, or says there is no room yet.
    const auto retire = [this](ParticleSlot& slot) {
        if (!slot.instance) {
            return true;
        }
        if (retired_particles_.size() == max_retired_particles) {
            return false;
        }
        retired_particles_.push_back(std::move(*slot.instance));
        slot.instance.reset();
        return true;
    };
    std::erase_if(particle_slots_, [&scene, &retire](auto& slot) {
        const bool gone = std::none_of(scene.particle_effects.begin(), scene.particle_effects.end(),
            [&slot](const scene::ParticleEffect& effect) { return effect.source_node == slot.first; });
        return gone && retire(slot.second);
    });
    // A reflection shows no particles; the view that reflects it draws them.
    if (scene.particle_effects.empty() || options.reflection_pass) {
        return;
    }
    // At most one new instance a frame: each allocates its buffers and
    // checkpoint ring, which is bounded but not free.
    bool instanced = false;
    core::u64 drawn = 0xcbf29ce484222325ULL;
    const auto mix = [&drawn](core::u64 value) noexcept {
        for (core::u32 byte = 0U; byte < 8U; ++byte) {
            drawn = (drawn ^ ((value >> (byte * 8U)) & 0xFFU)) * 0x100000001b3ULL;
        }
    };
    for (const scene::ParticleEffect& effect : scene.particle_effects) {
        const std::optional<core::u64> frame = scene::particle_effect_frame(effect, scene.show_frame);
        if (!frame) {
            continue; // not started: nothing to draw, and nothing missing
        }
        if (particle_pass_ == nullptr) {
            refuse(stats_.particles_unsupported, particles_unsupported_);
            continue;
        }
        const ParticlePackageObservation observed =
            particle_packages_->observe(effect.source, authored_asset_generation_);
        if (observed.package == nullptr) {
            if (observed.state == ParticlePackageState::Failed) {
                refuse(stats_.particles_failed, observed.error);
            } else {
                ++stats_.particles_pending;
            }
            continue;
        }
        ParticleSlot& slot = particle_slots_[effect.source_node];
        // A package delivered with its cache plays as it was baked: with the
        // seed it was baked with, whatever the placement would derive, so
        // every placement and every node plays the performance itself.
        const core::u32 seed = observed.package->cache != nullptr
                                   ? observed.package->cache->identity().instance_seed
                                   : scene::particle_instance_seed(effect);
        // A new package or seed starts a new instance; the old one keeps
        // drawing until retirement has room for it.
        if ((slot.package != observed.package || slot.seed != seed) && retire(slot)) {
            slot = ParticleSlot{observed.package, seed, std::nullopt, {}, {}, {}, {}, {}};
        }
        if (!slot.instance) {
            if (!slot.error.empty()) {
                refuse(stats_.particles_failed, slot.error);
                continue;
            }
            if (instanced) {
                ++stats_.particles_pending;
                continue;
            }
            instanced = true;
            auto created = particle_pass_->create_instance(slot.package->effect, slot.package->kernels,
                                                           slot.package->textures, slot.package->fields,
                                                           slot.package->meshes, seed, slot.package->cache);
            if (!created) {
                slot.error = std::move(created).error().message;
                refuse(stats_.particles_failed, slot.error);
                continue;
            }
            slot.instance.emplace(std::move(created).value());
            slot.parameters = parameters_digest(effect, scene.show_rate);
            slot.provider = placed_parameters(effect, slot.package->effect, scene.show_rate);
            particle_pass_->restart(*slot.instance, slot.provider);
            slot.actions = effect.actions;
            if (slot.actions) {
                particle_pass_->revise(*slot.instance, slot.actions, 0U);
            }
            slot.generation = ++particle_generation_;
        }
        // New actions change the effect's past from their first new tick on;
        // what came before stands.
        if (effect.actions != slot.actions) {
            slot.actions = effect.actions;
            particle_pass_->revise(*slot.instance, slot.actions, effect.actions_from);
            slot.generation = ++particle_generation_;
        }
        // A changed parameter changes the effect's whole past.
        if (const core::u64 digest = parameters_digest(effect, scene.show_rate); digest != slot.parameters) {
            slot.provider = placed_parameters(effect, slot.package->effect, scene.show_rate);
            particle_pass_->restart(*slot.instance, slot.provider);
            slot.parameters = digest;
            slot.generation = ++particle_generation_;
        }
        if (!particle_pass_->advance(commands, *slot.instance, *frame,
                scene.show_rate.numerator, scene.show_rate.denominator, slot.provider.get())) {
            if (const std::optional<std::string> failed = slot.instance->failure()) {
                refuse(stats_.particles_failed, *failed);
            } else {
                ++stats_.particles_behind;
            }
            continue;
        }
        // Its lights, in the slots after the effects before it. One that
        // would overrun the budget is refused rather than lit partly.
        if (const core::u32 lights = particle_pass_->light_count(*slot.instance); lights != 0U) {
            if (!particle_light_buffer_ || particle_lights_used_ + lights > max_particle_lights) {
                refuse(stats_.particles_failed, "particle effects ask for more than " +
                                                    std::to_string(max_particle_lights) + " lights");
                continue;
            }
            if (!particle_pass_->write_lights(commands, *slot.instance, effect.world, particle_light_buffer_,
                                              particle_lights_used_)) {
                continue;
            }
            particle_lights_used_ += lights;
        }
        particle_draws_.emplace_back(&*slot.instance, effect.world);
        mix(effect.source_node.value());
        mix(slot.generation);
        mix(*frame);
        for (const float lane : effect.world.m) {
            mix(std::bit_cast<core::u32>(lane));
        }
    }
    if (particle_pass_ != nullptr) {
        if (std::optional<ParticleError> failed = particle_pass_->take_error()) {
            // The frame's simulations are suspect: draw none of them.
            refuse(stats_.particles_failed, failed->message);
            particle_draws_.clear();
        }
    }
    if (!particle_draws_.empty()) {
        stats_.particle_digest = drawn;
    }
    if (stats_.particles_pending + stats_.particles_failed + stats_.particles_unsupported
            + stats_.particles_behind != 0U) {
        stats_.assets.ready = false;
    }
}

bool SceneRenderer::record_particles(rhi::CommandList& commands, const scene::ViewMatrices& view,
                                     ParticleTargets targets) {
    // Over everything the scene drew, tested against its depth and writing
    // none: sprites blend back to front where they sort.
    targets.color = &color_;
    targets.depth = &depth_;
    // Every effect's colour first, while the velocity target still holds the
    // scene alone for soft particles to fade against; then their motion.
    for (const auto& [instance, world] : particle_draws_) {
        if (!particle_pass_->draw(commands, *instance, view, world, targets)) {
            return false;
        }
        ++stats_.particles_drawn;
    }
    if (targets.velocity != nullptr) {
        for (const auto& [instance, world] : particle_draws_) {
            if (!particle_pass_->draw_motion(commands, *instance, view, world, targets)) {
                return false;
            }
        }
    }
    return true;
}

} // namespace mediaserver::render
