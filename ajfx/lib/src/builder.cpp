#include <ajfx/builder.hpp>

#include <utility>
#include <variant>

namespace ajfx {

EmitterBuilder::EmitterBuilder(std::string name) {
    emitter_.name = std::move(name);
}

EmitterBuilder& EmitterBuilder::seed(std::uint32_t seed) {
    emitter_.seed = seed;
    return *this;
}

EmitterBuilder& EmitterBuilder::capacity(std::uint32_t capacity) {
    emitter_.capacity = capacity;
    return *this;
}

namespace {

// Gives a module without an id the builder's next one, and keeps the counter
// past any id the caller chose so later modules cannot collide with it.
template <class Variant>
void assign_id(Variant& module, ModuleId& last_id) {
    std::visit(
        [&last_id](auto& m) {
            if (m.id == 0U) {
                m.id = ++last_id;
            } else if (m.id > last_id) {
                last_id = m.id;
            }
        },
        module);
}

} // namespace

EmitterBuilder& EmitterBuilder::spawn(SpawnModule module) {
    assign_id(module, last_id_);
    emitter_.spawn.push_back(std::move(module));
    return *this;
}

EmitterBuilder& EmitterBuilder::init(InitModule module) {
    assign_id(module, last_id_);
    emitter_.init.push_back(std::move(module));
    return *this;
}

EmitterBuilder& EmitterBuilder::update(UpdateModule module) {
    assign_id(module, last_id_);
    emitter_.update.push_back(std::move(module));
    return *this;
}

EmitterBuilder& EmitterBuilder::renderer(SpriteRenderer renderer) {
    emitter_.renderer = std::move(renderer);
    return *this;
}

EffectBuilder::EffectBuilder(std::string name) {
    effect_.name = std::move(name);
}

EffectBuilder& EffectBuilder::simulation_rate(std::uint32_t steps_per_second) {
    effect_.simulation_rate = steps_per_second;
    return *this;
}

EffectBuilder& EffectBuilder::float_parameter(std::string name, float default_value, float min, float max) {
    effect_.parameters.push_back({std::move(name), ParameterType::Float, to_value(default_value), min, max});
    return *this;
}

EffectBuilder& EffectBuilder::playback() {
    if (!effect_.parameters.empty()) {
        effect_.parameters.back().timing = ParameterTiming::Playback;
    }
    return *this;
}

EffectBuilder& EffectBuilder::vec3_parameter(std::string name, Vec3 default_value, float min, float max) {
    effect_.parameters.push_back({std::move(name), ParameterType::Vec3, to_value(default_value), min, max});
    return *this;
}

EffectBuilder& EffectBuilder::color_parameter(std::string name, Color default_value, float min, float max) {
    effect_.parameters.push_back({std::move(name), ParameterType::Color, to_value(default_value), min, max});
    return *this;
}

EffectBuilder& EffectBuilder::blast(Blast blast) {
    effect_.blasts.push_back(std::move(blast));
    return *this;
}

EmitterBuilder& EffectBuilder::emitter(std::string name) {
    for (EmitterBuilder& emitter : emitters_) {
        if (emitter.emitter().name == name) {
            return emitter;
        }
    }
    return emitters_.emplace_back(std::move(name));
}

Result<Effect> EffectBuilder::build() const {
    Effect effect = effect_;
    for (const EmitterBuilder& emitter : emitters_) {
        effect.emitters.push_back(emitter.emitter());
    }
    Result<bool> valid = validate(effect);
    if (!valid) {
        return valid.error();
    }
    return effect;
}

} // namespace ajfx
