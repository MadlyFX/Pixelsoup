#include <mediaserver/scene/scene.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// scene.particle_effect: parsed, placed like a model, keyframed through
// channels, evaluated into Scene::particle_effects and carried in the digests
// every node compares, with a seed and effect time every node derives alike.
namespace {

namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace scene = mediaserver::scene;
namespace schema = mediaserver::scene::schema;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) + ": check failed: " + expression};
    }
}

#define EFFECT_CHECK(...) check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

using Property = std::pair<std::string, doc::PropertyValue>;

[[nodiscard]] doc::Node node(core::u64 id, std::string_view type, std::initializer_list<Property> properties) {
    doc::Node built{};
    built.id = doc::NodeId{id};
    built.type = std::string{type};
    for (const auto& property : properties) {
        built.properties.emplace(property.first, property.second);
    }
    return built;
}

[[nodiscard]] Property real(std::string_view name, double value) {
    return Property{std::string{name}, doc::PropertyValue{value}};
}
[[nodiscard]] Property whole(std::string_view name, std::int64_t value) {
    return Property{std::string{name}, doc::PropertyValue{value}};
}
[[nodiscard]] Property text(std::string_view name, std::string_view value) {
    return Property{std::string{name}, doc::PropertyValue{std::string{value}}};
}
[[nodiscard]] Property flag(std::string_view name, bool value) {
    return Property{std::string{name}, doc::PropertyValue{value}};
}

// A linear position_x channel from (0, from) to (frames, to).
[[nodiscard]] doc::Node position_channel(core::u64 id, core::u64 target, std::int64_t frames, double from, double to) {
    doc::Node built{};
    built.id = doc::NodeId{id};
    built.type = std::string{schema::channel_type};
    built.properties.emplace(std::string{schema::channel_target},
                             doc::PropertyValue{static_cast<std::int64_t>(target)});
    built.properties.emplace(std::string{schema::channel_component},
                             doc::PropertyValue{std::string{schema::position_x}});
    const std::string prefix{schema::channel_key_prefix};
    std::size_t index = 0U;
    for (const auto& [frame, value] : {std::pair{std::int64_t{0}, from}, std::pair{frames, to}}) {
        const auto suffix = std::to_string(index++);
        built.properties.emplace(prefix + suffix + std::string{schema::channel_key_frame_suffix},
                                 doc::PropertyValue{frame});
        built.properties.emplace(prefix + suffix + std::string{schema::channel_key_value_suffix},
                                 doc::PropertyValue{value});
        built.properties.emplace(prefix + suffix + std::string{schema::channel_key_interp_suffix},
                                 doc::PropertyValue{std::string{schema::interp_linear}});
    }
    return built;
}

constexpr scene::SceneTime at_start{0U, 60U, 1U};

[[nodiscard]] scene::Scene evaluate(const doc::Root& root, scene::SceneTime time = at_start) {
    scene::SceneModel model;
    model.reset(root);
    return model.evaluate(time);
}

[[nodiscard]] doc::Node sparks(core::u64 id, std::initializer_list<Property> extra = {}) {
    doc::Node built = node(id, schema::particle_effect_type,
                           {text(schema::source, "effects/sparks.ajfx"), real(schema::position_x, 2.0),
                            real(schema::position_y, -1.0), whole(schema::particle_seed, 5),
                            whole(schema::particle_start_frame, 48)});
    for (const auto& property : extra) {
        built.properties.insert_or_assign(property.first, property.second);
    }
    return built;
}

void an_effect_evaluates_with_its_placement_seed_and_start() {
    const scene::Scene built = evaluate(doc::Root{}.set(sparks(7U)));
    EFFECT_CHECK(built.particle_effects.size() == 1U);
    const scene::ParticleEffect& effect = built.particle_effects.front();
    EFFECT_CHECK(effect.source == "effects/sparks.ajfx");
    EFFECT_CHECK(effect.source_node == doc::NodeId{7U});
    EFFECT_CHECK(effect.seed == 5U);
    EFFECT_CHECK(effect.start_frame == 48U);
    EFFECT_CHECK(effect.world.at(0U, 3U) == 2.0F && effect.world.at(1U, 3U) == -1.0F);
    EFFECT_CHECK(effect.world.at(0U, 0U) == 1.0F && effect.world.at(3U, 3U) == 1.0F);
    EFFECT_CHECK(!effect.locked);
}

void damaged_effects_are_refused_not_clamped() {
    for (const doc::Node& damaged : {
             node(1U, schema::particle_effect_type, {}),                                  // no package
             sparks(1U, {whole(schema::particle_seed, -1)}),                              // negative seed
             sparks(1U, {whole(schema::particle_seed, std::int64_t{1} << 32)}),           // seed past 32 bits
             sparks(1U, {whole(schema::particle_start_frame, -24)}),                      // before the show
         }) {
        scene::SceneModel model;
        model.reset(doc::Root{}.set(damaged));
        EFFECT_CHECK(model.counters().rejected == 1U);
        EFFECT_CHECK(model.evaluate(at_start).particle_effects.empty());
    }
}

void groups_and_visibility_apply_as_to_a_model() {
    EFFECT_CHECK(evaluate(doc::Root{}.set(sparks(1U, {flag(schema::visible, false)}))).particle_effects.empty());

    doc::Root grouped = doc::Root{}
                            .set(node(2U, schema::group_type, {real(schema::position_x, 1.0), flag(schema::locked, true)}))
                            .set(sparks(3U, {whole(schema::parent_group, 2)}));
    const scene::Scene built = evaluate(grouped);
    EFFECT_CHECK(built.particle_effects.size() == 1U);
    // The group's placement composes: 1 + 2 along x.
    EFFECT_CHECK(built.particle_effects.front().world.at(0U, 3U) == 3.0F);
    EFFECT_CHECK(built.particle_effects.front().locked);
    EFFECT_CHECK(built.particle_effects.front().parent_group == doc::NodeId{2U});

    const doc::Root hidden = grouped.set(node(2U, schema::group_type, {flag(schema::visible, false)}));
    EFFECT_CHECK(evaluate(hidden).particle_effects.empty());
}

void keyframes_move_an_effect() {
    const doc::Root root = doc::Root{}.set(sparks(1U)).set(position_channel(9U, 1U, 60, 0.0, 6.0));
    EFFECT_CHECK(evaluate(root, scene::SceneTime{30U, 60U, 1U}).particle_effects.front().world.at(0U, 3U) == 3.0F);
    EFFECT_CHECK(evaluate(root, scene::SceneTime{60U, 60U, 1U}).particle_effects.front().world.at(0U, 3U) == 6.0F);
}

// A linear channel on a particle parameter component, from (0, from) to
// (frames, to).
[[nodiscard]] doc::Node parameter_channel(core::u64 id, core::u64 target, std::string_view key, std::int64_t frames,
                                          double from, double to) {
    doc::Node built = position_channel(id, target, frames, from, to);
    built.properties.insert_or_assign(std::string{schema::channel_component},
                                      doc::PropertyValue{std::string{schema::particle_parameter_prefix} + std::string{key}});
    return built;
}

void parameters_are_named_valued_and_keyed() {
    // Values by key, a channel on one of them, and one on an unrelated node.
    const doc::Root root = doc::Root{}
                               .set(sparks(1U, {real("particle_parameter.rate.0", 120.0),
                                                real("particle_parameter.wind.2", -0.5)}))
                               .set(parameter_channel(9U, 1U, "tint.3", 10, 0.0, 1.0))
                               .set(parameter_channel(8U, 1U, "rate.0", 10, 20.0, 40.0));
    const scene::Scene built = evaluate(root, scene::SceneTime{5U, 60U, 1U});
    EFFECT_CHECK(built.particle_effects.size() == 1U);
    const std::vector<scene::ParticleParameter>& parameters = built.particle_effects.front().parameters;
    // Sorted by key; a keyed component carries its whole curve.
    EFFECT_CHECK(parameters.size() == 3U);
    EFFECT_CHECK(parameters[0].key == "rate.0" && parameters[0].base == 120.0);
    EFFECT_CHECK(parameters[0].curve.frames == (std::vector<core::i64>{0, 10}));
    EFFECT_CHECK(parameters[0].curve.parameter == "rate.0");
    EFFECT_CHECK(parameters[1].key == "tint.3" && parameters[1].curve.values == (std::vector<double>{0.0, 1.0}));
    EFFECT_CHECK(parameters[2].key == "wind.2" && parameters[2].base == -0.5 && parameters[2].curve.frames.empty());

    // Sampled between show frames, as a simulation stepping faster than the
    // show needs; a fraction of zero is the whole-frame value.
    EFFECT_CHECK(scene::particle_parameter_at(parameters[0], 5, 0.0) == 30.0);
    EFFECT_CHECK(scene::particle_parameter_at(parameters[0], 5, 0.5) == 31.0);
    EFFECT_CHECK(scene::particle_parameter_at(parameters[0], 0, 0.25) == 20.5);
    EFFECT_CHECK(scene::particle_parameter_at(parameters[0], 12, 0.5) == 40.0);
    EFFECT_CHECK(scene::particle_parameter_at(parameters[2], 7, 0.5) == -0.5);

    // The editor sees the channel under its key.
    scene::SceneModel model;
    model.reset(root);
    const std::vector<scene::ChannelKeyframes> channels = model.channel_keyframes();
    EFFECT_CHECK(std::count_if(channels.begin(), channels.end(), [](const scene::ChannelKeyframes& channel) {
                     return channel.component == scene::PropertyChannel::ParticleParameter && channel.parameter == "tint.3";
                 }) == 1);

    // Keys parse as name.component, component 0 to 3.
    EFFECT_CHECK(scene::parse_particle_parameter_key("spawn_rate.0")->name == "spawn_rate");
    EFFECT_CHECK(scene::parse_particle_parameter_key("tint.3")->component == 3U);
    for (const std::string_view bad : {"rate", "rate.4", ".0", "ra-te.0", "rate.0.1", "rate.x"}) {
        EFFECT_CHECK(!scene::parse_particle_parameter_key(bad));
    }

    // Values and keys change the picture, so the digest.
    const auto digest_of = [](const doc::Root& document) { return scene::digest(evaluate(document)); };
    EFFECT_CHECK(digest_of(root) != digest_of(root.set(sparks(1U, {real("particle_parameter.rate.0", 121.0),
                                                                   real("particle_parameter.wind.2", -0.5)}))));
    EFFECT_CHECK(digest_of(root) != digest_of(root.set(parameter_channel(8U, 1U, "rate.0", 10, 20.0, 41.0))));

    // A malformed key or value damages the node; a channel naming a bad key
    // is refused.
    for (const doc::Node& damaged : {sparks(1U, {real("particle_parameter.rate", 1.0)}),
                                     sparks(1U, {text("particle_parameter.rate.0", "fast")}),
                                     parameter_channel(2U, 1U, "rate.9", 10, 0.0, 1.0)}) {
        scene::SceneModel refused;
        refused.reset(doc::Root{}.set(damaged));
        EFFECT_CHECK(refused.counters().rejected == 1U);
    }
}

void the_digests_carry_every_field_that_changes_the_picture() {
    const auto digest_of = [](const doc::Node& effect) { return scene::digest(evaluate(doc::Root{}.set(effect))); };
    const core::u64 base = digest_of(sparks(1U));
    EFFECT_CHECK(digest_of(sparks(1U)) == base);
    EFFECT_CHECK(digest_of(sparks(1U, {whole(schema::particle_seed, 6)})) != base);
    EFFECT_CHECK(digest_of(sparks(1U, {whole(schema::particle_start_frame, 49)})) != base);
    EFFECT_CHECK(digest_of(sparks(1U, {text(schema::source, "effects/smoke.ajfx")})) != base);
    EFFECT_CHECK(digest_of(sparks(1U, {real(schema::position_z, 0.5)})) != base);
    EFFECT_CHECK(digest_of(sparks(2U)) != base); // another node: another seed

    // The package is an asset every node must hold.
    const auto assets_of = [](const doc::Node& effect) {
        return scene::asset_set_digest(evaluate(doc::Root{}.set(effect)));
    };
    EFFECT_CHECK(assets_of(sparks(1U)) != assets_of(sparks(1U, {text(schema::source, "effects/smoke.ajfx")})));

    // The same evaluated scene with and without its effect differs. (That a
    // scene without particles keeps the digest it had before they existed is
    // pinned by the golden digests in scene_model_tests.)
    const doc::Node object = node(4U, schema::object_type, {text(schema::primitive, "box")});
    const scene::Scene with = evaluate(doc::Root{}.set(object).set(sparks(1U)));
    scene::Scene without = with;
    without.particle_effects.clear();
    EFFECT_CHECK(scene::digest(with) != scene::digest(without));
}

void seeds_and_effect_time_are_derived_alike_everywhere() {
    scene::ParticleEffect a;
    a.source_node = doc::NodeId{7U};
    a.seed = 5U;
    scene::ParticleEffect b = a;
    EFFECT_CHECK(scene::particle_instance_seed(a) == scene::particle_instance_seed(b));
    b.source_node = doc::NodeId{8U};
    EFFECT_CHECK(scene::particle_instance_seed(a) != scene::particle_instance_seed(b));
    b = a;
    b.seed = 6U;
    EFFECT_CHECK(scene::particle_instance_seed(a) != scene::particle_instance_seed(b));
    // The value itself is pinned (computed outside C++): it is part of what
    // every node must agree on, so a change to the mixing is a change to show.
    EFFECT_CHECK(scene::particle_instance_seed(a) == 0xe781ff3eU);

    a.start_frame = 48U;
    EFFECT_CHECK(!scene::particle_effect_frame(a, 47U).has_value());
    EFFECT_CHECK(scene::particle_effect_frame(a, 48U) == core::u64{0U});
    EFFECT_CHECK(scene::particle_effect_frame(a, 100U) == core::u64{52U});
}

void the_scene_holds_a_bounded_number_of_effects() {
    doc::Root root;
    for (core::u64 id = 1U; id <= scene::max_scene_particle_effects + 3U; ++id) {
        root = root.set(sparks(id));
    }
    scene::SceneModel model;
    model.reset(root);
    EFFECT_CHECK(model.evaluate(at_start).particle_effects.size() == scene::max_scene_particle_effects);
    EFFECT_CHECK(model.counters().over_capacity() == 3U);
}

} // namespace

int main() {
    struct Case final {
        const char* name;
        void (*run)();
    };
    const Case cases[] = {
        {"an_effect_evaluates_with_its_placement_seed_and_start", an_effect_evaluates_with_its_placement_seed_and_start},
        {"damaged_effects_are_refused_not_clamped", damaged_effects_are_refused_not_clamped},
        {"groups_and_visibility_apply_as_to_a_model", groups_and_visibility_apply_as_to_a_model},
        {"keyframes_move_an_effect", keyframes_move_an_effect},
        {"parameters_are_named_valued_and_keyed", parameters_are_named_valued_and_keyed},
        {"the_digests_carry_every_field_that_changes_the_picture",
         the_digests_carry_every_field_that_changes_the_picture},
        {"seeds_and_effect_time_are_derived_alike_everywhere", seeds_and_effect_time_are_derived_alike_everywhere},
        {"the_scene_holds_a_bounded_number_of_effects", the_scene_holds_a_bounded_number_of_effects},
    };
    int failures = 0;
    for (const Case& test : cases) {
        try {
            test.run();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
