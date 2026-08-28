#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_project.hpp>

#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace scene = mediaserver::scene;
namespace schema = mediaserver::scene::schema;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define MODEL_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

using Property = std::pair<std::string, doc::PropertyValue>;

[[nodiscard]] doc::Node node(
    core::u64 id,
    std::string_view type,
    std::initializer_list<Property> properties) {
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

// One keyframe for the channel builder below.
struct Key final {
    std::int64_t frame;
    double value;
    std::string_view interp;
};

// A scene.channel node: target, component, and its indexed keys. Mirrors how the
// editor's timeline will author a channel - the keys are ordinary properties.
[[nodiscard]] doc::Node channel_node(
    core::u64 id,
    core::u64 target,
    std::string_view component,
    std::initializer_list<Key> keys) {
    doc::Node built{};
    built.id = doc::NodeId{id};
    built.type = std::string{schema::channel_type};
    built.properties.emplace(
        std::string{schema::channel_target},
        doc::PropertyValue{static_cast<std::int64_t>(target)});
    built.properties.emplace(
        std::string{schema::channel_component}, doc::PropertyValue{std::string{component}});
    std::size_t index = 0U;
    const std::string prefix{schema::channel_key_prefix};
    for (const auto& key : keys) {
        const auto suffix = std::to_string(index++);
        built.properties.emplace(
            prefix + suffix + std::string{schema::channel_key_frame_suffix},
            doc::PropertyValue{key.frame});
        built.properties.emplace(
            prefix + suffix + std::string{schema::channel_key_value_suffix},
            doc::PropertyValue{key.value});
        built.properties.emplace(
            prefix + suffix + std::string{schema::channel_key_interp_suffix},
            doc::PropertyValue{std::string{key.interp}});
    }
    return built;
}

[[nodiscard]] doc::Node effect_node(
    core::u64 id,
    core::u64 target,
    std::string_view kind,
    core::i64 order,
    double value,
    bool enabled = true) {
    doc::Node built = node(
        id,
        schema::effect_type,
        {whole(schema::effect_target, static_cast<core::i64>(target)),
         text(schema::effect_kind, kind),
         whole(schema::effect_order, order),
         flag(schema::effect_enabled, enabled)});
    built.properties.emplace(
        std::string{kind == schema::brightness_kind ? schema::brightness
                                                     : schema::blur_radius},
        doc::PropertyValue{value});
    return built;
}

// A one-frame clip at the origin, for a channel to drive.
[[nodiscard]] doc::Node clip_node(core::u64 id) {
    return node(
        id,
        schema::media_type,
        {text(schema::source, "plate_%04u.png"), whole(schema::sequence_count, 1)});
}

constexpr scene::SceneTime at_start{0U, 60U, 1U};

// A small but complete show: a canvas, a camera, a ground plane, two moving
// objects, a key light, and a two-node feed map.
[[nodiscard]] std::vector<doc::Node> demo_nodes() {
    return {
        node(
            1U,
            schema::stage_type,
            {whole(schema::canvas_width, 2'560),
             whole(schema::canvas_height, 720),
             real(schema::sky_r, 0.04),
             real(schema::sky_g, 0.05),
             real(schema::sky_b, 0.08),
             real(schema::ambient, 0.8)}),
        node(
            2U,
            schema::camera_type,
            {real(schema::eye_x, 0.0),
             real(schema::eye_y, 2.5),
             real(schema::eye_z, 7.0),
             real(schema::target_y, 1.0),
             real(schema::fov_turns, 0.1),
             real(schema::near_plane, 0.05),
             real(schema::far_plane, 400.0),
             real(schema::orbit_turns_per_second, 0.02)}),
        node(
            3U,
            schema::object_type,
            {text(schema::primitive, "plane"),
             whole(schema::segments, 4),
             real(schema::scale_x, 40.0),
             real(schema::scale_z, 40.0),
             real(schema::color_r, 0.18),
             real(schema::color_g, 0.19),
             real(schema::color_b, 0.2)}),
        node(
            4U,
            schema::object_type,
            {text(schema::primitive, "box"),
             real(schema::position_x, -1.5),
             real(schema::position_y, 0.5),
             real(schema::spin_turns_per_second, 0.15),
             real(schema::color_r, 0.9),
             real(schema::color_g, 0.35),
             real(schema::color_b, 0.15)}),
        node(
            5U,
            schema::object_type,
            {text(schema::primitive, "sphere"),
             whole(schema::segments, 24),
             real(schema::position_x, 1.5),
             real(schema::position_y, 0.75),
             real(schema::bob_amplitude, 0.4),
             real(schema::bob_turns_per_second, 0.25),
             real(schema::roughness, 0.25)}),
        node(
            6U,
            schema::light_type,
            {text(schema::light_kind, "directional"),
             real(schema::direction_x, -0.4),
             real(schema::direction_y, -1.0),
             real(schema::direction_z, -0.3),
             real(schema::intensity, 3.0)}),
        node(
            7U,
            schema::slice_type,
            {whole(schema::node_index, 0),
             whole(schema::slice_x, 0),
             whole(schema::slice_y, 0),
             whole(schema::slice_width, 1'280),
             whole(schema::slice_height, 720)}),
        node(
            8U,
            schema::slice_type,
            {whole(schema::node_index, 1),
             whole(schema::slice_x, 1'280),
             whole(schema::slice_y, 0),
             whole(schema::slice_width, 1'280),
             whole(schema::slice_height, 720)}),
    };
}

[[nodiscard]] doc::Root root_of(const std::vector<doc::Node>& nodes) {
    doc::Root root;
    for (const auto& one : nodes) {
        root = root.set(one);
    }
    return root;
}

void a_document_becomes_the_scene_it_describes() {
    scene::SceneModel model;
    model.reset(root_of(demo_nodes()));

    MODEL_CHECK(model.counters().rejected == 0U);
    MODEL_CHECK(model.counters().duplicate_singletons() == 0U);
    MODEL_CHECK(model.counters().over_capacity() == 0U);
    MODEL_CHECK(model.counters().objects == 3U);

    const auto built = model.evaluate(at_start);
    MODEL_CHECK(built.canvas == (scene::Canvas{2'560U, 720U}));
    MODEL_CHECK(built.instances.size() == 3U);
    MODEL_CHECK(built.directional_lights.size() == 1U);
    MODEL_CHECK(built.point_lights.empty());
    MODEL_CHECK(built.slices.size() == 2U);
    MODEL_CHECK(scene::valid(built.camera));

    // At frame zero the orbit contributes nothing, so the camera is where the
    // document put it.
    MODEL_CHECK(built.camera.eye == (scene::Vec3{0.0F, 2.5F, 7.0F}));

    // Instances arrive grouped by mesh, which is what the renderer batches on.
    for (std::size_t index = 1U; index < built.instances.size(); ++index) {
        MODEL_CHECK(
            !(built.instances[index].mesh < built.instances[index - 1U].mesh));
    }

    // A directional light's vector is normalised on the way out, so the shader
    // never has to.
    const auto direction = built.directional_lights.front().direction;
    MODEL_CHECK(std::abs(scene::length(direction) - 1.0F) <= 1.0e-5F);

    const auto* const first = built.slice_for(0U);
    const auto* const second = built.slice_for(1U);
    MODEL_CHECK(first != nullptr && second != nullptr);
    MODEL_CHECK(*first == (scene::CanvasRect{0U, 0U, 1'280U, 720U}));
    MODEL_CHECK(*second == (scene::CanvasRect{1'280U, 0U, 1'280U, 720U}));
    // A node the document does not map renders nothing rather than guessing.
    MODEL_CHECK(built.slice_for(2U) == nullptr);
}

void a_checkpoint_and_a_stream_of_deltas_agree_exactly() {
    // This is the replication claim in miniature. One node joins late and is
    // handed the whole document; another has been folding in deltas since the
    // show started. They must hold the same scene, bit for bit, or the wall
    // has a seam that no amount of rendering care can close.
    const auto nodes = demo_nodes();

    scene::SceneModel checkpointed;
    checkpointed.reset(root_of(nodes));

    scene::SceneModel streamed;
    doc::Root running;
    for (const auto& one : nodes) {
        const auto next = running.set(one);
        streamed.apply(diff(running, next));
        running = next;
    }

    for (const auto frame : {core::u64{0U}, core::u64{1U}, core::u64{97U},
                             core::u64{5'000U}, core::u64{3'600'000U}}) {
        const scene::SceneTime moment{frame, 60'000U, 1'001U};
        const auto left = checkpointed.evaluate(moment);
        const auto right = streamed.evaluate(moment);
        MODEL_CHECK(scene::digest(left) == scene::digest(right));
    }
}

void evaluation_is_a_pure_function_of_the_frame() {
    scene::SceneModel model;
    model.reset(root_of(demo_nodes()));

    const scene::SceneTime moment{1'234U, 60U, 1U};
    const auto first = scene::digest(model.evaluate(moment));
    const auto again = scene::digest(model.evaluate(moment));
    MODEL_CHECK(first == again);

    // And it actually depends on the frame: a digest that never moved would
    // pass the check above while proving nothing.
    const auto later =
        scene::digest(model.evaluate(scene::SceneTime{1'235U, 60U, 1U}));
    MODEL_CHECK(first != later);
}

void a_long_show_keeps_moving_rather_than_seizing_up() {
    // Ten hours in at sixty frames a second. An implementation that let the
    // accumulated angle live in a float would have quantised to a standstill
    // long before here, and the wall would freeze mid-show.
    scene::SceneModel model;
    model.reset(root_of(demo_nodes()));

    constexpr core::u64 ten_hours = 60ULL * 60ULL * 60ULL * 10ULL;
    const auto a = scene::digest(model.evaluate(scene::SceneTime{ten_hours, 60U, 1U}));
    const auto b =
        scene::digest(model.evaluate(scene::SceneTime{ten_hours + 1U, 60U, 1U}));
    MODEL_CHECK(a != b);

    // The rotation is still a rotation rather than a number that has run away:
    // every element of every transform is finite and bounded.
    const auto built = model.evaluate(scene::SceneTime{ten_hours, 60U, 1U});
    for (const auto& instance : built.instances) {
        for (const auto element : instance.world.m) {
            MODEL_CHECK(std::isfinite(element));
            MODEL_CHECK(std::abs(element) < 1.0e4F);
        }
    }
}

void a_property_of_the_wrong_type_rejects_its_node() {
    scene::SceneModel model;
    doc::Root root;
    // position_x as a string rather than a number. Defaulting it would leave
    // an object silently at the origin.
    root = root.set(node(
        1U,
        schema::object_type,
        {text(schema::primitive, "box"), text(schema::position_x, "over there")}));
    model.reset(root);
    MODEL_CHECK(model.counters().rejected == 1U);
    MODEL_CHECK(model.evaluate(at_start).instances.empty());

    // An unknown primitive is the same kind of mistake.
    scene::SceneModel other;
    other.reset(doc::Root{}.set(
        node(1U, schema::object_type, {text(schema::primitive, "teapot")})));
    MODEL_CHECK(other.counters().rejected == 1U);

    // An absent property, by contrast, is not a mistake: it takes its default,
    // so a scene authored before a property existed still loads.
    scene::SceneModel sparse;
    sparse.reset(doc::Root{}.set(node(1U, schema::object_type, {})));
    MODEL_CHECK(sparse.counters().rejected == 0U);
    MODEL_CHECK(sparse.evaluate(at_start).instances.size() == 1U);
}

void retyping_or_erasing_a_node_removes_what_it_drew() {
    scene::SceneModel model;
    doc::Root root = doc::Root{}.set(
        node(1U, schema::object_type, {text(schema::primitive, "box")}));
    model.reset(root);
    MODEL_CHECK(model.evaluate(at_start).instances.size() == 1U);

    // Retyped to something the scene does not know. The object has to go: a
    // node that stops being a scene object stops being drawn.
    const auto retyped = root.set(node(1U, "layer.text", {}));
    model.apply(diff(root, retyped));
    MODEL_CHECK(model.evaluate(at_start).instances.empty());
    MODEL_CHECK(model.counters().objects == 0U);
    MODEL_CHECK(model.size() == 0U);

    // And erased outright.
    scene::SceneModel erased_model;
    erased_model.reset(root);
    const auto erased = root.erase(doc::NodeId{1U});
    MODEL_CHECK(erased.has_value());
    erased_model.apply(diff(root, *erased));
    MODEL_CHECK(erased_model.evaluate(at_start).instances.empty());
    MODEL_CHECK(erased_model.counters().objects == 0U);
}

void stages_are_scenes_and_the_lowest_is_the_default() {
    scene::SceneModel model;
    doc::Root root;
    root = root.set(node(
        9U, schema::stage_type, {whole(schema::canvas_width, 800),
                                 whole(schema::canvas_height, 600)}));
    root = root.set(node(
        4U, schema::stage_type, {whole(schema::canvas_width, 1'024),
                                 whole(schema::canvas_height, 768)}));
    model.reset(root);

    MODEL_CHECK(model.counters().stages == 2U);
    MODEL_CHECK(model.counters().duplicate_singletons() == 0U);
    MODEL_CHECK(model.scenes().size() == 2U);
    MODEL_CHECK(model.scenes()[0].id == doc::NodeId{4U});
    MODEL_CHECK(model.scenes()[1].id == doc::NodeId{9U});
    // The document decides the compatibility default, not write order.
    MODEL_CHECK(model.evaluate(at_start).canvas == (scene::Canvas{1'024U, 768U}));
    MODEL_CHECK(
        model.evaluate(doc::NodeId{9U}, at_start).canvas
        == (scene::Canvas{800U, 600U}));
}

void the_digest_separates_scenes_a_single_bit_apart() {
    scene::Scene first_identity{};
    scene::Scene second_identity{};
    first_identity.id = doc::NodeId{10U};
    second_identity.id = doc::NodeId{20U};
    MODEL_CHECK(scene::digest(first_identity) != scene::digest(second_identity));

    scene::SceneModel base;
    base.reset(root_of(demo_nodes()));
    const auto reference = scene::digest(base.evaluate(at_start));

    auto nudged = demo_nodes();
    // The smallest change to a position that binary32 can hold at this
    // magnitude. A digest that missed it would miss the divergence that
    // matters most: the one nobody notices until it is on a wall.
    const auto original = -1.5F;
    const auto shifted = std::nextafter(original, 0.0F);
    MODEL_CHECK(shifted != original);
    nudged[3U].properties.insert_or_assign(
        std::string{schema::position_x},
        doc::PropertyValue{static_cast<double>(shifted)});

    scene::SceneModel moved;
    moved.reset(root_of(nudged));
    MODEL_CHECK(scene::digest(moved.evaluate(at_start)) != reference);

    // A scene that is a prefix of another does not collide with it.
    auto shorter = demo_nodes();
    shorter.pop_back();
    scene::SceneModel truncated;
    truncated.reset(root_of(shorter));
    MODEL_CHECK(scene::digest(truncated.evaluate(at_start)) != reference);
}

void a_document_past_capacity_truncates_the_same_way_everywhere() {
    scene::SceneModel model;
    doc::Root root;
    const auto count = scene::max_scene_instances + 32U;
    for (core::u32 index = 0U; index < count; ++index) {
        root = root.set(node(
            static_cast<core::u64>(index) + 1U,
            schema::object_type,
            {text(schema::primitive, "box"),
             real(schema::position_x, static_cast<double>(index))}));
    }
    model.reset(root);
    MODEL_CHECK(model.counters().objects == count);
    MODEL_CHECK(model.counters().over_capacity() == 32U);

    const auto built = model.evaluate(at_start);
    MODEL_CHECK(built.instances.size() == scene::max_scene_instances);

    // Truncation is by node id, so a node that streamed the document and a
    // node that was handed it whole drop the same objects.
    scene::SceneModel streamed;
    doc::Root running;
    for (core::u32 index = 0U; index < count; ++index) {
        const auto next = root.get(doc::NodeId{static_cast<core::u64>(index) + 1U});
        MODEL_CHECK(next != nullptr);
        const auto grown = running.set(*next);
        streamed.apply(diff(running, grown));
        running = grown;
    }
    MODEL_CHECK(
        scene::digest(streamed.evaluate(at_start)) == scene::digest(built));
}

void a_point_light_is_carried_through_as_one() {
    scene::SceneModel model;
    doc::Root root;
    root = root.set(node(
        1U,
        schema::light_type,
        {text(schema::light_kind, "point"),
         real(schema::position_x, 2.0),
         real(schema::position_y, 3.0),
         real(schema::position_z, -1.0),
         real(schema::range, 25.0),
         real(schema::intensity, 12.0)}));
    root = root.set(node(
        2U, schema::light_type, {text(schema::light_kind, "spot")}));
    model.reset(root);

    MODEL_CHECK(model.counters().rejected == 1U);
    const auto built = model.evaluate(at_start);
    MODEL_CHECK(built.point_lights.size() == 1U);
    MODEL_CHECK(built.directional_lights.empty());
    MODEL_CHECK(built.point_lights.front().position == (scene::Vec3{2.0F, 3.0F, -1.0F}));
    MODEL_CHECK(built.point_lights.front().range == 25.0F);
}

void selection_locks_are_carried_by_every_element_kind() {
    doc::Root root;
    root = root.set(node(1U, schema::object_type,
                         {text(schema::primitive, "box"),
                          flag(schema::locked, true)}));
    root = root.set(node(2U, schema::media_type,
                         {text(schema::source, "locked.png"),
                          whole(schema::sequence_count, 1),
                          flag(schema::locked, true)}));
    root = root.set(node(3U, schema::model_type,
                         {text(schema::source, "locked.obj"),
                          flag(schema::locked, true)}));
    root = root.set(node(4U, schema::light_type,
                         {text(schema::light_kind, "point"),
                          flag(schema::locked, true)}));

    scene::SceneModel model;
    model.reset(root);
    const scene::Scene built = model.evaluate(at_start);
    MODEL_CHECK(built.instances.size() == 1U);
    MODEL_CHECK(built.media.size() == 1U);
    MODEL_CHECK(built.models.size() == 1U);
    MODEL_CHECK(built.point_lights.size() == 1U);
    MODEL_CHECK(built.instances.front().locked);
    MODEL_CHECK(built.media.front().locked);
    MODEL_CHECK(built.models.front().locked);
    MODEL_CHECK(built.point_lights.front().locked);
}

void a_media_clip_advances_with_the_show_frame() {
    scene::SceneModel model;
    model.reset(doc::Root{}.set(node(
        1U,
        schema::media_type,
        {text(schema::source, "plate_%04u.png"),
         whole(schema::sequence_start, 100),
         whole(schema::sequence_count, 24),
         real(schema::position_x, 1.0),
         real(schema::scale_x, 16.0),
         real(schema::scale_y, 9.0)})));

    MODEL_CHECK(model.counters().rejected == 0U);
    MODEL_CHECK(model.counters().media == 1U);

    const auto opening = model.evaluate(scene::SceneTime{0U, 60U, 1U});
    MODEL_CHECK(opening.media.size() == 1U);
    const auto& clip = opening.media.front();
    MODEL_CHECK(clip.source == "plate_%04u.png");
    MODEL_CHECK(clip.mesh.primitive == scene::Primitive::Quad);
    MODEL_CHECK(clip.source_node == doc::NodeId{1U});
    // The show frame counts into the sequence from the start file number.
    MODEL_CHECK(clip.frame_index == 100U);

    // It advances with the show frame...
    MODEL_CHECK(
        model.evaluate(scene::SceneTime{5U, 60U, 1U}).media.front().frame_index
        == 105U);
    // ...and loops back to the start once the sequence runs out (count is 24).
    MODEL_CHECK(
        model.evaluate(scene::SceneTime{24U, 60U, 1U}).media.front().frame_index
        == 100U);
    MODEL_CHECK(
        model.evaluate(scene::SceneTime{9'999U, 60U, 1U})
            .media.front()
            .frame_index
        == 100U + (9'999U % 24U));

    // A clip with no sequence to load is refused rather than shown blank.
    scene::SceneModel missing;
    missing.reset(doc::Root{}.set(
        node(1U, schema::media_type, {whole(schema::sequence_count, 10)})));
    MODEL_CHECK(missing.counters().rejected == 1U);
    MODEL_CHECK(missing.evaluate(at_start).media.empty());
}

void native_media_rate_resamples_show_time_exactly() {
    scene::SceneModel model;
    model.reset(doc::Root{}.set(node(
        1U,
        schema::media_type,
        {text(schema::source, "native-rate.mp4"),
         whole(schema::sequence_start, 100),
         whole(schema::sequence_count, 97),
         whole(schema::source_rate_numerator, 24'000),
         whole(schema::source_rate_denominator, 1'001)})));

    MODEL_CHECK(model.counters().rejected == 0U);
    const auto frame_at = [&](core::u64 frame) {
        return model
            .evaluate(scene::SceneTime{frame, 60'000U, 1'001U})
            .media.front()
            .frame_index;
    };
    MODEL_CHECK(frame_at(0U) == 100U);
    MODEL_CHECK(frame_at(4U) == 101U);
    MODEL_CHECK(frame_at(5U) == 102U);
    MODEL_CHECK(frame_at(6U) == 102U);

    // The 24000/1001 : 60000/1001 ratio reduces exactly to 2/5. This also
    // exercises a show ordinal whose direct numerator product would overflow.
    constexpr core::u64 longest = std::numeric_limits<core::u64>::max();
    const core::u32 longest_within = static_cast<core::u32>(
        ((longest / 5U) * 2U) % 97U);
    MODEL_CHECK(frame_at(longest) == 100U + longest_within);

    // Source rates above the show rate can advance by multiple ordinals at a
    // boundary; the integer quotient portion participates in the same mapping.
    scene::SceneModel faster;
    faster.reset(doc::Root{}.set(node(
        2U,
        schema::media_type,
        {text(schema::source, "high-rate.mp4"),
         whole(schema::sequence_count, 100),
         whole(schema::source_rate_numerator, 60'000),
         whole(schema::source_rate_denominator, 1'001)})));
    MODEL_CHECK(
        faster.evaluate(scene::SceneTime{3U, 24'000U, 1'001U})
            .media.front()
            .frame_index == 7U);

    // A partially authored or non-positive pair is malformed; it must not be
    // mistaken for the legacy absent-rate contract.
    scene::SceneModel partial;
    partial.reset(doc::Root{}.set(node(
        3U, schema::media_type,
        {text(schema::source, "partial.mp4"),
         whole(schema::source_rate_numerator, 24'000)})));
    MODEL_CHECK(partial.counters().rejected == 1U);
    MODEL_CHECK(partial.evaluate(at_start).media.empty());

    scene::SceneModel zero;
    zero.reset(doc::Root{}.set(node(
        4U, schema::media_type,
        {text(schema::source, "zero.mp4"),
         whole(schema::source_rate_numerator, 24'000),
         whole(schema::source_rate_denominator, 0)})));
    MODEL_CHECK(zero.counters().rejected == 1U);
    MODEL_CHECK(zero.evaluate(at_start).media.empty());
}

void first_class_effects_are_ordered_folded_and_keep_orphans() {
    doc::Root root;
    root = root.set(node(
        100U,
        schema::object_type,
        {text(schema::primitive, "box"),
         flag(schema::brightness_enabled, true),
         real(schema::brightness, 0.5),
         flag(schema::blur_enabled, true),
         real(schema::blur_radius, 2.0)}));
    // Authored order wins first, and the effect node id breaks an order tie.
    root = root.set(
        effect_node(20U, 100U, schema::blur_kind, -4, 11.0));
    root = root.set(
        effect_node(30U, 100U, schema::brightness_kind, 2, 2.0));
    root = root.set(
        effect_node(10U, 100U, schema::brightness_kind, 2, 3.0));
    // Disabled effects remain first-class but do not alter the folded style.
    root = root.set(
        effect_node(40U, 100U, schema::blur_kind, 10, 50.0, false));
    // An effect may arrive before its target in a streamed document. It must
    // remain held so adding that target later activates it without reparsing.
    root = root.set(
        effect_node(50U, 999U, schema::brightness_kind, 0, 4.0));

    scene::SceneModel model;
    model.reset(root);
    MODEL_CHECK(model.counters().rejected == 0U);
    MODEL_CHECK(model.counters().effects == 5U);
    const auto authored = model.effects();
    MODEL_CHECK(authored.size() == 5U);
    const std::array<doc::NodeId, 5U> expected{
        doc::NodeId{20U}, doc::NodeId{10U}, doc::NodeId{30U},
        doc::NodeId{40U}, doc::NodeId{50U}};
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        MODEL_CHECK(authored[index].source == expected[index]);
    }

    const auto built = model.evaluate(at_start);
    // The orphan is retained in authored state but is not part of an evaluated
    // scene until its drawable arrives.
    MODEL_CHECK(built.effects.size() == 4U);
    MODEL_CHECK(std::none_of(
        built.effects.begin(), built.effects.end(), [](const auto& effect) {
            return effect.source == doc::NodeId{50U};
        }));
    MODEL_CHECK(built.instances.size() == 1U);
    const auto& style = built.instances.front().style;
    MODEL_CHECK(style.effects.brightness_enabled);
    // Legacy 0.5, then the two ordered first-class factors 3 and 2.
    MODEL_CHECK(std::abs(style.effects.brightness - 3.0F) <= 1.0e-6F);
    MODEL_CHECK(style.effects.blur_enabled);
    MODEL_CHECK(std::abs(style.effects.blur_radius - 11.0F) <= 1.0e-6F);

    const auto resolved = root.set(
        node(999U, schema::object_type, {text(schema::primitive, "box")}));
    model.apply(diff(root, resolved));
    const auto with_target = model.evaluate(at_start);
    MODEL_CHECK(with_target.effects == authored);
    const auto resolved_instance = std::find_if(
        with_target.instances.begin(), with_target.instances.end(),
        [](const scene::MeshInstance& instance) noexcept {
            return instance.source == doc::NodeId{999U};
        });
    MODEL_CHECK(resolved_instance != with_target.instances.end());
    MODEL_CHECK(resolved_instance->style.effects.brightness_enabled);
    MODEL_CHECK(
        std::abs(resolved_instance->style.effects.brightness - 4.0F)
        <= 1.0e-6F);

    const auto removed = resolved.erase(doc::NodeId{999U});
    MODEL_CHECK(removed.has_value());
    model.apply(diff(resolved, *removed));
    MODEL_CHECK(model.counters().effects == 5U);
    MODEL_CHECK(model.effects().size() == 5U);
    MODEL_CHECK(model.evaluate(at_start).instances.size() == 1U);
}

void malformed_effects_are_refused_and_values_are_bounded() {
    doc::Node missing_target = node(
        1U,
        schema::effect_type,
        {text(schema::effect_kind, schema::brightness_kind),
         real(schema::brightness, 2.0)});
    doc::Node wrong_enable =
        effect_node(3U, 100U, schema::brightness_kind, 0, 2.0);
    wrong_enable.properties.insert_or_assign(
        std::string{schema::effect_enabled},
        doc::PropertyValue{std::string{"sometimes"}});
    doc::Node non_finite_enable =
        effect_node(7U, 100U, schema::brightness_kind, 0, 2.0);
    non_finite_enable.properties.insert_or_assign(
        std::string{schema::effect_enabled},
        doc::PropertyValue{std::numeric_limits<double>::quiet_NaN()});

    doc::Root malformed;
    malformed = malformed.set(missing_target);
    malformed = malformed.set(
        effect_node(2U, 100U, "glow", 0, 2.0));
    malformed = malformed.set(wrong_enable);
    malformed = malformed.set(non_finite_enable);
    scene::SceneModel rejected;
    rejected.reset(malformed);
    MODEL_CHECK(rejected.counters().rejected == 4U);
    MODEL_CHECK(rejected.counters().effects == 0U);
    MODEL_CHECK(rejected.effects().empty());

    doc::Node numeric_disable =
        effect_node(6U, 100U, schema::brightness_kind, 2, 1.0);
    numeric_disable.properties.insert_or_assign(
        std::string{schema::effect_enabled}, doc::PropertyValue{0.0});
    doc::Root bounded;
    bounded = bounded.set(
        effect_node(4U, 100U, schema::brightness_kind, 0, 100.0));
    bounded = bounded.set(
        effect_node(5U, 100U, schema::blur_kind, 1, -10.0));
    bounded = bounded.set(numeric_disable);
    scene::SceneModel accepted;
    accepted.reset(bounded);
    MODEL_CHECK(accepted.counters().rejected == 0U);
    const auto effects = accepted.effects();
    MODEL_CHECK(effects.size() == 3U);
    MODEL_CHECK(effects[0].value == 16.0F);
    MODEL_CHECK(effects[1].value == 0.0F);
    MODEL_CHECK(!effects[2].enabled);
}

void property_channels_drive_opacity_and_individual_effects() {
    doc::Root root;
    root = root.set(node(
        1U,
        schema::object_type,
        {text(schema::primitive, "box"), real(schema::opacity, 0.9)}));
    root = root.set(
        effect_node(2U, 1U, schema::brightness_kind, 0, 1.0));
    root = root.set(effect_node(3U, 1U, schema::blur_kind, 1, 8.0));
    root = root.set(channel_node(
        10U,
        1U,
        schema::opacity,
        {{0, 0.2, schema::interp_linear},
         {10, 0.8, schema::interp_linear}}));
    root = root.set(channel_node(
        11U,
        2U,
        schema::brightness,
        {{0, 2.0, schema::interp_linear},
         {10, 4.0, schema::interp_linear}}));
    root = root.set(channel_node(
        12U,
        3U,
        schema::blur_radius,
        {{0, 4.0, schema::interp_linear},
         {10, 20.0, schema::interp_linear}}));

    scene::SceneModel model;
    model.reset(root);
    MODEL_CHECK(model.counters().rejected == 0U);
    MODEL_CHECK(model.counters().channels == 3U);
    MODEL_CHECK(model.counters().effects == 2U);
    const auto built = model.evaluate(scene::SceneTime{5U, 60U, 1U});
    MODEL_CHECK(built.instances.size() == 1U);
    MODEL_CHECK(built.effects.size() == 2U);
    MODEL_CHECK(std::abs(built.effects[0].value - 3.0F) <= 1.0e-6F);
    MODEL_CHECK(std::abs(built.effects[1].value - 12.0F) <= 1.0e-6F);
    const auto& style = built.instances.front().style;
    MODEL_CHECK(std::abs(style.opacity - 0.5F) <= 1.0e-6F);
    MODEL_CHECK(style.effects.brightness_enabled);
    MODEL_CHECK(std::abs(style.effects.brightness - 3.0F) <= 1.0e-6F);
    MODEL_CHECK(style.effects.blur_enabled);
    MODEL_CHECK(std::abs(style.effects.blur_radius - 12.0F) <= 1.0e-6F);
}

void the_lowest_id_property_channel_wins_everywhere() {
    doc::Root root;
    root = root.set(node(
        1U, schema::object_type, {text(schema::primitive, "box")}));
    root = root.set(
        effect_node(2U, 1U, schema::brightness_kind, 0, 1.0));
    root = root.set(channel_node(
        10U, 2U, schema::brightness,
        {{0, 2.0, schema::interp_hold}}));
    root = root.set(channel_node(
        11U, 1U, schema::opacity,
        {{0, 0.25, schema::interp_hold}}));
    root = root.set(channel_node(
        20U, 2U, schema::brightness,
        {{0, 8.0, schema::interp_linear},
         {10, 12.0, schema::interp_linear}}));
    root = root.set(channel_node(
        21U, 1U, schema::opacity,
        {{0, 0.75, schema::interp_linear},
         {10, 0.8, schema::interp_linear}}));

    scene::SceneModel model;
    model.reset(root);
    MODEL_CHECK(model.counters().channels == 4U);
    const auto built = model.evaluate(scene::SceneTime{10U, 60U, 1U});
    MODEL_CHECK(built.effects.size() == 1U);
    MODEL_CHECK(std::abs(built.effects.front().value - 2.0F) <= 1.0e-6F);
    MODEL_CHECK(built.instances.size() == 1U);
    MODEL_CHECK(
        std::abs(built.instances.front().style.opacity - 0.25F) <= 1.0e-6F);
    MODEL_CHECK(
        std::abs(built.instances.front().style.effects.brightness - 2.0F)
        <= 1.0e-6F);

    // The editor sees the same winning curve, not a union containing keys from
    // the otherwise-inert duplicate channel.
    const auto channels = model.channel_keyframes();
    MODEL_CHECK(channels.size() == 2U);
    const auto opacity = std::find_if(
        channels.begin(), channels.end(), [](const auto& channel) noexcept {
            return channel.target == doc::NodeId{1U}
                && channel.component == scene::PropertyChannel::Opacity;
        });
    const auto brightness = std::find_if(
        channels.begin(), channels.end(), [](const auto& channel) noexcept {
            return channel.target == doc::NodeId{2U}
                && channel.component == scene::PropertyChannel::Brightness;
        });
    MODEL_CHECK(opacity != channels.end());
    MODEL_CHECK(brightness != channels.end());
    MODEL_CHECK(opacity->frames == std::vector<core::i64>({0}));
    MODEL_CHECK(opacity->values == std::vector<double>({0.25}));
    MODEL_CHECK(brightness->frames == std::vector<core::i64>({0}));
    MODEL_CHECK(brightness->values == std::vector<double>({2.0}));
}

void effect_identity_order_and_animation_participate_in_the_digest() {
    doc::Root base;
    base = base.set(node(
        1U, schema::object_type, {text(schema::primitive, "box")}));
    base = base.set(
        effect_node(2U, 1U, schema::brightness_kind, 0, 2.0));
    const auto digest_of = [](const doc::Root& root, core::u64 frame = 0U) {
        scene::SceneModel model;
        model.reset(root);
        return scene::digest(
            model.evaluate(scene::SceneTime{frame, 60U, 1U}));
    };

    const auto reference = digest_of(base);
    MODEL_CHECK(
        digest_of(base.set(
            effect_node(2U, 1U, schema::brightness_kind, 1, 2.0)))
        != reference);
    MODEL_CHECK(
        digest_of(base.set(
            effect_node(2U, 999U, schema::brightness_kind, 0, 2.0)))
        != reference);
    MODEL_CHECK(
        digest_of(base.set(
            effect_node(2U, 1U, schema::brightness_kind, 0, 2.0, false)))
        != reference);

    const auto animated = base.set(channel_node(
        3U,
        2U,
        schema::brightness,
        {{0, 1.0, schema::interp_linear},
         {10, 4.0, schema::interp_linear}}));
    MODEL_CHECK(digest_of(animated, 0U) != digest_of(animated, 10U));
}

void a_channel_drives_a_component_over_show_frames() {
    scene::SceneModel model;
    doc::Root root;
    root = root.set(clip_node(1U));
    // position_x goes 0 -> 10 linearly over frames 0..10.
    root = root.set(channel_node(
        2U,
        1U,
        schema::position_x,
        {{0, 0.0, schema::interp_linear}, {10, 10.0, schema::interp_linear}}));
    model.reset(root);
    MODEL_CHECK(model.counters().rejected == 0U);
    MODEL_CHECK(model.counters().channels == 1U);
    MODEL_CHECK(model.counters().media == 1U);

    const auto x_at = [&model](core::u64 frame) {
        const auto built = model.evaluate(scene::SceneTime{frame, 60U, 1U});
        MODEL_CHECK(built.media.size() == 1U);
        return built.media.front().world.at(0U, 3U);
    };
    MODEL_CHECK(std::abs(x_at(0U) - 0.0F) <= 1.0e-6F);
    MODEL_CHECK(std::abs(x_at(5U) - 5.0F) <= 1.0e-5F);
    MODEL_CHECK(std::abs(x_at(10U) - 10.0F) <= 1.0e-6F);
    // Held flat past the last key rather than extrapolating.
    MODEL_CHECK(std::abs(x_at(50U) - 10.0F) <= 1.0e-6F);
}

void channel_interpolation_modes_differ() {
    // Three components of one clip, one per interpolation, sampled where they
    // disagree: at frame 2 of a 0..10 ramp to 10.
    scene::SceneModel model;
    doc::Root root;
    root = root.set(clip_node(1U));
    root = root.set(channel_node(
        2U,
        1U,
        schema::position_x,
        {{0, 0.0, schema::interp_cubic}, {10, 10.0, schema::interp_linear}}));
    root = root.set(channel_node(
        3U,
        1U,
        schema::position_y,
        {{0, 0.0, schema::interp_hold}, {10, 10.0, schema::interp_linear}}));
    root = root.set(channel_node(
        4U,
        1U,
        schema::position_z,
        {{0, 0.0, schema::interp_linear}, {10, 10.0, schema::interp_linear}}));
    model.reset(root);
    MODEL_CHECK(model.counters().channels == 3U);

    // Keep the scene alive: binding a reference straight into evaluate()'s
    // temporary would dangle, since front() is a call, not a subobject access.
    const auto built = model.evaluate(scene::SceneTime{2U, 60U, 1U});
    const auto& world = built.media.front().world;
    // Cubic smoothstep at t=0.2: 3t^2 - 2t^3 = 0.104, so 1.04.
    MODEL_CHECK(std::abs(world.at(0U, 3U) - 1.04F) <= 1.0e-4F);
    // Hold keeps the earlier key's value.
    MODEL_CHECK(std::abs(world.at(1U, 3U) - 0.0F) <= 1.0e-6F);
    // Linear is the straight fraction.
    MODEL_CHECK(std::abs(world.at(2U, 3U) - 2.0F) <= 1.0e-5F);
}

void the_timeline_query_keeps_values_and_interpolation() {
    scene::SceneModel model;
    doc::Root root;
    root = root.set(channel_node(
        2U,
        9U,
        schema::position_x,
        {{10, 8.0, schema::interp_cubic},
         {0, 2.0, schema::interp_hold}}));
    model.reset(root);

    const auto channels = model.channel_keyframes();
    MODEL_CHECK(channels.size() == 1U);
    MODEL_CHECK(channels[0].target == doc::NodeId{9U});
    MODEL_CHECK(channels[0].component == scene::TransformChannel::PositionX);
    MODEL_CHECK(channels[0].frames == std::vector<core::i64>({0, 10}));
    MODEL_CHECK(channels[0].values == std::vector<double>({2.0, 8.0}));
    MODEL_CHECK(channels[0].interpolations.size() == 2U);
    MODEL_CHECK(channels[0].interpolations[0] ==
                scene::KeyInterpolation::Hold);
    MODEL_CHECK(channels[0].interpolations[1] ==
                scene::KeyInterpolation::Cubic);
}

void the_lowest_id_channel_wins_a_component() {
    scene::SceneModel model;
    doc::Root root;
    root = root.set(clip_node(1U));
    // Two channels drive position_x; the lower id must win, like a singleton.
    root = root.set(
        channel_node(5U, 1U, schema::position_x, {{0, 5.0, schema::interp_hold}}));
    root = root.set(
        channel_node(3U, 1U, schema::position_x, {{0, 3.0, schema::interp_hold}}));
    model.reset(root);
    MODEL_CHECK(model.counters().channels == 2U);
    MODEL_CHECK(
        std::abs(model.evaluate(at_start).media.front().world.at(0U, 3U) - 3.0F)
        <= 1.0e-6F);
}

void a_channel_replicates_bit_exact() {
    // The replication claim for channels: a node handed the whole document and a
    // node that streamed it must sample the curve to the same bits every frame.
    std::vector<doc::Node> nodes;
    nodes.push_back(clip_node(1U));
    nodes.push_back(channel_node(
        2U,
        1U,
        schema::position_x,
        {{0, -3.0, schema::interp_cubic},
         {30, 3.0, schema::interp_cubic},
         {60, -3.0, schema::interp_linear}}));

    scene::SceneModel checkpointed;
    checkpointed.reset(root_of(nodes));
    scene::SceneModel streamed;
    doc::Root running;
    for (const auto& one : nodes) {
        const auto grown = running.set(one);
        streamed.apply(diff(running, grown));
        running = grown;
    }
    for (core::u64 frame = 0U; frame <= 90U; ++frame) {
        const scene::SceneTime moment{frame, 60U, 1U};
        MODEL_CHECK(
            scene::digest(checkpointed.evaluate(moment))
            == scene::digest(streamed.evaluate(moment)));
    }
}

void a_malformed_channel_is_refused() {
    // No keys: a channel that animates nothing is a mistake, not an empty curve.
    scene::SceneModel no_keys;
    no_keys.reset(doc::Root{}.set(channel_node(1U, 2U, schema::position_x, {})));
    MODEL_CHECK(no_keys.counters().rejected == 1U);
    MODEL_CHECK(no_keys.counters().channels == 0U);

    // A component name that is not a transform property.
    scene::SceneModel bad_component;
    bad_component.reset(doc::Root{}.set(
        channel_node(1U, 2U, "wobble", {{0, 0.0, schema::interp_linear}})));
    MODEL_CHECK(bad_component.counters().rejected == 1U);

    // An interpolation name that does not name a mode.
    scene::SceneModel bad_interp;
    bad_interp.reset(doc::Root{}.set(
        channel_node(1U, 2U, schema::position_x, {{0, 0.0, "wiggle"}})));
    MODEL_CHECK(bad_interp.counters().rejected == 1U);

    // Non-finite values would poison transforms and effect folds downstream.
    scene::SceneModel non_finite;
    non_finite.reset(doc::Root{}.set(channel_node(
        1U,
        2U,
        schema::brightness,
        {{0, std::numeric_limits<double>::quiet_NaN(),
          schema::interp_hold}})));
    MODEL_CHECK(non_finite.counters().rejected == 1U);

    scene::SceneModel too_wide;
    too_wide.reset(doc::Root{}.set(channel_node(
        1U,
        2U,
        schema::opacity,
        {{0, std::numeric_limits<double>::max(), schema::interp_hold}})));
    MODEL_CHECK(too_wide.counters().rejected == 1U);

    // A negative target cannot name a node.
    doc::Node negative{};
    negative.id = doc::NodeId{1U};
    negative.type = std::string{schema::channel_type};
    negative.properties.emplace(
        std::string{schema::channel_target}, doc::PropertyValue{std::int64_t{-1}});
    negative.properties.emplace(
        std::string{schema::channel_component},
        doc::PropertyValue{std::string{schema::position_x}});
    negative.properties.emplace(
        std::string{schema::channel_key_prefix} + "0"
            + std::string{schema::channel_key_frame_suffix},
        doc::PropertyValue{std::int64_t{0}});
    scene::SceneModel negative_target;
    negative_target.reset(doc::Root{}.set(negative));
    MODEL_CHECK(negative_target.counters().rejected == 1U);
}

void scenes_isolate_drawables_lights_channels_and_effects() {
    doc::Root root;
    root = root.set(node(
        10U, schema::stage_type,
        {text(schema::scene_name, "Opening"),
         whole(schema::canvas_width, 1'000),
         whole(schema::canvas_height, 500)}));
    root = root.set(node(
        20U, schema::stage_type,
        {text(schema::scene_name, "Finale"),
         whole(schema::canvas_width, 2'000),
         whole(schema::canvas_height, 1'000)}));
    // Missing scene_id is legacy ownership: it belongs to the lowest stage.
    root = root.set(node(
        50U, schema::object_type,
        {text(schema::primitive, "box"), real(schema::position_x, 0.5)}));
    root = root.set(node(
        100U, schema::object_type,
        {whole(schema::scene_id, 10), text(schema::primitive, "sphere"),
         real(schema::opacity, 0.9)}));
    root = root.set(node(
        110U, schema::media_type,
        {whole(schema::scene_id, 10), text(schema::source, "a_%04u.png")}));
    root = root.set(node(
        200U, schema::object_type,
        {whole(schema::scene_id, 20), text(schema::primitive, "plane")}));
    root = root.set(node(
        210U, schema::model_type,
        {whole(schema::scene_id, 20), text(schema::source, "finale.glb")}));
    root = root.set(node(
        120U, schema::light_type,
        {whole(schema::scene_id, 10),
         text(schema::light_kind, schema::directional_kind)}));
    root = root.set(node(
        220U, schema::light_type,
        {whole(schema::scene_id, 20),
         text(schema::light_kind, schema::point_kind)}));
    root = root.set(
        effect_node(300U, 100U, schema::brightness_kind, 0, 2.0));
    root = root.set(effect_node(301U, 200U, schema::blur_kind, 0, 4.0));
    root = root.set(channel_node(
        400U, 100U, schema::opacity,
        {{0, 0.25, schema::interp_hold}}));
    root = root.set(channel_node(
        401U, 301U, schema::blur_radius,
        {{0, 12.0, schema::interp_hold}}));
    root = root.set(node(
        230U, schema::object_type,
        {whole(schema::scene_id, 999), text(schema::primitive, "box")}));

    scene::SceneModel model;
    model.reset(root);
    MODEL_CHECK(model.counters().rejected == 0U);
    const auto metadata = model.scenes();
    MODEL_CHECK(metadata.size() == 2U);
    MODEL_CHECK(metadata[0] == (scene::SceneInfo{
        doc::NodeId{10U}, "Opening", scene::Canvas{1'000U, 500U}}));
    MODEL_CHECK(metadata[1] == (scene::SceneInfo{
        doc::NodeId{20U}, "Finale", scene::Canvas{2'000U, 1'000U}}));

    const auto opening = model.evaluate(doc::NodeId{10U}, at_start);
    const auto finale = model.evaluate(doc::NodeId{20U}, at_start);
    MODEL_CHECK(opening.id == doc::NodeId{10U});
    MODEL_CHECK(finale.id == doc::NodeId{20U});
    MODEL_CHECK(opening.instances.size() == 2U);
    MODEL_CHECK(finale.instances.size() == 1U);
    MODEL_CHECK(opening.media.size() == 1U && opening.models.empty());
    MODEL_CHECK(finale.media.empty() && finale.models.size() == 1U);
    MODEL_CHECK(opening.directional_lights.size() == 1U);
    MODEL_CHECK(opening.point_lights.empty());
    MODEL_CHECK(finale.directional_lights.empty());
    MODEL_CHECK(finale.point_lights.size() == 1U);
    MODEL_CHECK(opening.effects.size() == 1U);
    MODEL_CHECK(opening.effects.front().source == doc::NodeId{300U});
    MODEL_CHECK(finale.effects.size() == 1U);
    MODEL_CHECK(finale.effects.front().source == doc::NodeId{301U});
    MODEL_CHECK(std::abs(finale.effects.front().value - 12.0F) <= 1.0e-6F);
    const auto opening_animated = std::find_if(
        opening.instances.begin(), opening.instances.end(), [](const auto& value) {
            return value.source == doc::NodeId{100U};
        });
    MODEL_CHECK(opening_animated != opening.instances.end());
    MODEL_CHECK(std::abs(opening_animated->style.opacity - 0.25F) <= 1.0e-6F);
    MODEL_CHECK(
        scene::digest(model.evaluate(at_start)) == scene::digest(opening));
    MODEL_CHECK(scene::digest(opening) != scene::digest(finale));

    const auto missing = model.evaluate(doc::NodeId{999U}, at_start);
    MODEL_CHECK(missing.id == doc::NodeId{999U});
    MODEL_CHECK(missing.instances.empty() && missing.effects.empty());
}

void legacy_scope_tracks_the_lowest_stage() {
    doc::Root running;
    running = running.set(node(
        1U, schema::object_type, {text(schema::primitive, "box")}));
    running = running.set(node(
        2U, schema::camera_type, {real(schema::eye_x, 2.0)}));

    scene::SceneModel model;
    model.reset(running);
    MODEL_CHECK(model.scenes().empty());
    MODEL_CHECK(model.evaluate(at_start).id == scene::legacy_scene_id);
    MODEL_CHECK(model.evaluate(at_start).instances.size() == 1U);

    auto next = running.set(node(
        20U, schema::stage_type, {text(schema::scene_name, "Only")}));
    model.apply(diff(running, next));
    running = next;
    MODEL_CHECK(model.evaluate(at_start).id == doc::NodeId{20U});
    MODEL_CHECK(model.evaluate(doc::NodeId{20U}, at_start).instances.size() == 1U);

    next = running.set(node(
        10U, schema::stage_type, {text(schema::scene_name, "New default")}));
    model.apply(diff(running, next));
    running = next;
    MODEL_CHECK(model.evaluate(at_start).id == doc::NodeId{10U});
    MODEL_CHECK(model.evaluate(doc::NodeId{10U}, at_start).instances.size() == 1U);
    MODEL_CHECK(model.evaluate(doc::NodeId{20U}, at_start).instances.empty());

    const auto erased = running.erase(doc::NodeId{10U});
    MODEL_CHECK(erased.has_value());
    model.apply(diff(running, *erased));
    MODEL_CHECK(model.evaluate(at_start).id == doc::NodeId{20U});
    MODEL_CHECK(model.evaluate(at_start).instances.size() == 1U);

    // A present but malformed scope is rejected rather than silently treated
    // as legacy ownership.
    scene::SceneModel malformed;
    malformed.reset(doc::Root{}.set(node(
        1U, schema::object_type,
        {text(schema::primitive, "box"), text(schema::scene_id, "Opening")})));
    MODEL_CHECK(malformed.counters().rejected == 1U);
}

void cameras_are_singletons_within_each_scene() {
    doc::Root root;
    root = root.set(node(10U, schema::stage_type, {}));
    root = root.set(node(20U, schema::stage_type, {}));
    // This legacy camera belongs to scene 10 and wins there by node id.
    root = root.set(node(
        5U, schema::camera_type, {real(schema::eye_x, 5.0)}));
    root = root.set(node(
        25U, schema::camera_type,
        {whole(schema::scene_id, 10), real(schema::eye_x, 25.0)}));
    root = root.set(node(
        30U, schema::camera_type,
        {whole(schema::scene_id, 10), real(schema::eye_x, 30.0)}));
    root = root.set(node(
        40U, schema::camera_type,
        {whole(schema::scene_id, 20), real(schema::eye_x, 40.0)}));
    // Dangling explicit scope is retained but is not a duplicate in a valid
    // scene and is never selected for one.
    root = root.set(node(
        50U, schema::camera_type,
        {whole(schema::scene_id, 999), real(schema::eye_x, 50.0)}));

    scene::SceneModel model;
    model.reset(root);
    MODEL_CHECK(model.counters().cameras == 5U);
    MODEL_CHECK(model.counters().duplicate_singletons() == 2U);
    MODEL_CHECK(model.evaluate(doc::NodeId{10U}, at_start).camera.eye.x == 5.0F);
    MODEL_CHECK(model.evaluate(doc::NodeId{20U}, at_start).camera.eye.x == 40.0F);

    const auto removed = root.erase(doc::NodeId{10U});
    MODEL_CHECK(removed.has_value());
    model.apply(diff(root, *removed));
    // The legacy camera dynamically follows the new lowest stage and now wins
    // over scene 20's explicit camera.
    MODEL_CHECK(model.counters().duplicate_singletons() == 1U);
    MODEL_CHECK(model.evaluate(doc::NodeId{20U}, at_start).camera.eye.x == 5.0F);
}

void multi_scene_reset_and_streamed_deltas_agree() {
    // Dependencies deliberately arrive before their targets and stages.
    std::vector<doc::Node> nodes;
    nodes.push_back(channel_node(
        600U, 500U, schema::brightness,
        {{0, 2.0, schema::interp_linear},
         {10, 6.0, schema::interp_linear}}));
    nodes.push_back(
        effect_node(500U, 100U, schema::brightness_kind, 0, 1.0));
    nodes.push_back(node(
        100U, schema::object_type,
        {whole(schema::scene_id, 10), text(schema::primitive, "box")}));
    nodes.push_back(node(
        200U, schema::object_type,
        {whole(schema::scene_id, 20), text(schema::primitive, "sphere")}));
    nodes.push_back(node(
        50U, schema::object_type, {text(schema::primitive, "plane")}));
    nodes.push_back(node(
        20U, schema::stage_type, {text(schema::scene_name, "B")}));
    nodes.push_back(node(
        10U, schema::stage_type, {text(schema::scene_name, "A")}));
    nodes.push_back(node(
        30U, schema::camera_type,
        {whole(schema::scene_id, 20), real(schema::eye_x, 3.0)}));

    const doc::Root final_root = root_of(nodes);
    scene::SceneModel checkpointed;
    checkpointed.reset(final_root);
    scene::SceneModel streamed;
    doc::Root running;
    for (const auto& value : nodes) {
        const auto next = running.set(value);
        streamed.apply(diff(running, next));
        running = next;
    }
    MODEL_CHECK(checkpointed.counters() == streamed.counters());
    MODEL_CHECK(checkpointed.scenes() == streamed.scenes());
    MODEL_CHECK(checkpointed.effects() == streamed.effects());
    MODEL_CHECK(
        checkpointed.channel_keyframes() == streamed.channel_keyframes());
    for (const auto id : {doc::NodeId{10U}, doc::NodeId{20U}}) {
        for (core::u64 frame = 0U; frame <= 10U; ++frame) {
            const scene::SceneTime time{frame, 60U, 1U};
            MODEL_CHECK(
                scene::digest(checkpointed.evaluate(id, time))
                == scene::digest(streamed.evaluate(id, time)));
        }
    }
}

void clone_planning_is_deterministic_and_rewrites_scene_references() {
    doc::Root root;
    root = root.set(node(
        10U, schema::stage_type,
        {text(schema::scene_name, "Source"),
         whole(schema::canvas_width, 1'280)}));
    root = root.set(node(20U, schema::stage_type, {text(schema::scene_name, "Other")}));
    root = root.set(node(
        30U, schema::camera_type,
        {whole(schema::scene_id, 10), real(schema::eye_x, 3.0)}));
    root = root.set(node(
        40U, schema::light_type,
        {whole(schema::scene_id, 10),
         text(schema::light_kind, schema::directional_kind)}));
    root = root.set(node(
        50U, schema::object_type,
        {text(schema::primitive, "box"), real(schema::position_x, 5.0)}));
    root = root.set(node(
        60U, schema::object_type,
        {whole(schema::scene_id, 20), text(schema::primitive, "sphere")}));
    root = root.set(node(
        100U, schema::object_type,
        {whole(schema::scene_id, 10), text(schema::primitive, "box"),
         real(schema::position_x, 10.0)}));
    root = root.set(node(
        101U, schema::media_type,
        {whole(schema::scene_id, 10), text(schema::source, "source_%04u.png")}));
    root = root.set(
        effect_node(200U, 100U, schema::brightness_kind, 0, 2.0));
    root = root.set(channel_node(
        201U, 200U, schema::brightness,
        {{0, 3.0, schema::interp_hold}}));
    root = root.set(channel_node(
        202U, 50U, schema::opacity,
        {{0, 0.5, schema::interp_hold}}));
    root = root.set(
        effect_node(400U, 999U, schema::brightness_kind, 0, 2.0));
    root = root.set(channel_node(
        401U, 400U, schema::brightness,
        {{0, 4.0, schema::interp_hold}}));
    root = root.set(node(
        500U, schema::slice_type,
        {whole(schema::slice_width, 100), whole(schema::slice_height, 100)}));
    // Occupied allocator slot and unrelated node; both must remain untouched.
    root = root.set(node(1'001U, "editor.note", {text("value", "keep")}));
    const auto original_size = root.size();

    const auto planned = scene::plan_clone_scene(
        root, doc::NodeId{10U}, doc::NodeId{1'000U});
    MODEL_CHECK(planned.has_value());
    const auto& plan = planned.value();
    MODEL_CHECK(root.size() == original_size);
    MODEL_CHECK(plan.source == doc::NodeId{10U});
    MODEL_CHECK(plan.destination == doc::NodeId{1'000U});
    const std::vector<std::pair<doc::NodeId, doc::NodeId>> expected{
        {doc::NodeId{10U}, doc::NodeId{1'000U}},
        {doc::NodeId{30U}, doc::NodeId{1'002U}},
        {doc::NodeId{40U}, doc::NodeId{1'003U}},
        {doc::NodeId{50U}, doc::NodeId{1'004U}},
        {doc::NodeId{100U}, doc::NodeId{1'005U}},
        {doc::NodeId{101U}, doc::NodeId{1'006U}},
        {doc::NodeId{200U}, doc::NodeId{1'007U}},
        {doc::NodeId{201U}, doc::NodeId{1'008U}},
        {doc::NodeId{202U}, doc::NodeId{1'009U}},
    };
    MODEL_CHECK(plan.remap == expected);
    MODEL_CHECK(plan.puts.size() == expected.size());
    MODEL_CHECK(!plan.cloned_id(doc::NodeId{60U}).has_value());
    MODEL_CHECK(!plan.cloned_id(doc::NodeId{400U}).has_value());
    MODEL_CHECK(!plan.cloned_id(doc::NodeId{500U}).has_value());

    const auto reference_of = [](const doc::Node& value, std::string_view name) {
        const auto found = value.properties.find(name);
        MODEL_CHECK(found != value.properties.end());
        const auto* const integer = std::get_if<std::int64_t>(&found->second);
        MODEL_CHECK(integer != nullptr && *integer >= 0);
        return doc::NodeId{static_cast<core::u64>(*integer)};
    };
    for (const auto& put : plan.puts) {
        if (put.type == schema::camera_type || put.type == schema::object_type
            || put.type == schema::media_type || put.type == schema::model_type
            || put.type == schema::light_type) {
            MODEL_CHECK(
                reference_of(put, schema::scene_id) == plan.destination);
        }
    }
    const auto cloned_effect = std::find_if(
        plan.puts.begin(), plan.puts.end(), [](const doc::Node& value) {
            return value.id == doc::NodeId{1'007U};
        });
    MODEL_CHECK(cloned_effect != plan.puts.end());
    MODEL_CHECK(
        reference_of(*cloned_effect, schema::effect_target)
        == doc::NodeId{1'005U});
    const auto effect_channel = std::find_if(
        plan.puts.begin(), plan.puts.end(), [](const doc::Node& value) {
            return value.id == doc::NodeId{1'008U};
        });
    const auto legacy_channel = std::find_if(
        plan.puts.begin(), plan.puts.end(), [](const doc::Node& value) {
            return value.id == doc::NodeId{1'009U};
        });
    MODEL_CHECK(effect_channel != plan.puts.end());
    MODEL_CHECK(legacy_channel != plan.puts.end());
    MODEL_CHECK(
        reference_of(*effect_channel, schema::channel_target)
        == doc::NodeId{1'007U});
    MODEL_CHECK(
        reference_of(*legacy_channel, schema::channel_target)
        == doc::NodeId{1'004U});

    const auto repeated = scene::plan_clone_scene(
        root, doc::NodeId{10U}, doc::NodeId{1'000U});
    MODEL_CHECK(repeated.has_value());
    MODEL_CHECK(repeated.value().remap == plan.remap);
    MODEL_CHECK(repeated.value().puts == plan.puts);

    doc::Root with_clone = root;
    for (const auto& put : plan.puts) {
        with_clone = with_clone.set(put);
    }
    scene::SceneModel model;
    model.reset(with_clone);
    const auto source = model.evaluate(doc::NodeId{10U}, at_start);
    const auto clone = model.evaluate(plan.destination, at_start);
    MODEL_CHECK(source.instances.size() == clone.instances.size());
    MODEL_CHECK(source.media.size() == clone.media.size());
    MODEL_CHECK(source.effects.size() == clone.effects.size());
    MODEL_CHECK(clone.effects.front().target == doc::NodeId{1'005U});
    MODEL_CHECK(std::abs(clone.effects.front().value - 3.0F) <= 1.0e-6F);

    doc::Node moved_clone = *with_clone.get(doc::NodeId{1'005U});
    moved_clone.properties.insert_or_assign(
        std::string{schema::position_x}, doc::PropertyValue{99.0});
    const auto moved_root = with_clone.set(std::move(moved_clone));
    scene::SceneModel moved;
    moved.reset(moved_root);
    const auto source_after = moved.evaluate(doc::NodeId{10U}, at_start);
    const auto clone_after = moved.evaluate(plan.destination, at_start);
    const auto source_100 = std::find_if(
        source_after.instances.begin(), source_after.instances.end(),
        [](const auto& value) { return value.source == doc::NodeId{100U}; });
    const auto clone_100 = std::find_if(
        clone_after.instances.begin(), clone_after.instances.end(),
        [](const auto& value) { return value.source == doc::NodeId{1'005U}; });
    MODEL_CHECK(source_100 != source_after.instances.end());
    MODEL_CHECK(clone_100 != clone_after.instances.end());
    MODEL_CHECK(source_100->world.at(0U, 3U) == 10.0F);
    MODEL_CHECK(clone_100->world.at(0U, 3U) == 99.0F);
}

void clone_planning_reports_source_and_id_errors() {
    doc::Root root = doc::Root{}.set(node(10U, schema::stage_type, {}));
    const auto absent = scene::plan_clone_scene(
        root, doc::NodeId{999U}, doc::NodeId{100U});
    MODEL_CHECK(!absent.has_value());
    MODEL_CHECK(absent.error() == scene::SceneCloneError::SourceNotFound);

    constexpr core::u64 signed_max =
        static_cast<core::u64>(std::numeric_limits<std::int64_t>::max());
    const auto invalid = scene::plan_clone_scene(
        root, doc::NodeId{10U}, doc::NodeId{signed_max + 1U});
    MODEL_CHECK(!invalid.has_value());
    MODEL_CHECK(invalid.error() == scene::SceneCloneError::InvalidFirstId);

    root = root.set(node(signed_max, "occupied", {}));
    const auto exhausted = scene::plan_clone_scene(
        root, doc::NodeId{10U}, doc::NodeId{signed_max});
    MODEL_CHECK(!exhausted.has_value());
    MODEL_CHECK(exhausted.error() == scene::SceneCloneError::IdExhausted);
}

void effect_delete_planning_cascades_only_its_animation_channels() {
    doc::Root root;
    root = root.set(node(
        100U, schema::object_type, {text(schema::primitive, "box")}));
    root = root.set(
        effect_node(200U, 100U, schema::brightness_kind, 0, 2.0));
    root = root.set(channel_node(
        203U, 200U, schema::brightness,
        {{0, 1.0, schema::interp_linear}}));
    root = root.set(channel_node(
        201U, 200U, schema::brightness,
        {{10, 3.0, schema::interp_linear}}));
    // Animation on the drawable and on a different effect must survive.
    root = root.set(channel_node(
        202U, 100U, schema::opacity,
        {{0, 0.5, schema::interp_hold}}));
    root = root.set(effect_node(300U, 100U, schema::blur_kind, 1, 8.0));
    root = root.set(channel_node(
        301U, 300U, schema::blur_radius,
        {{0, 12.0, schema::interp_linear}}));

    const auto planned =
        scene::plan_delete_effect(root, doc::NodeId{200U});
    MODEL_CHECK(planned.has_value());
    const std::vector<doc::NodeId> expected{
        doc::NodeId{201U}, doc::NodeId{203U}, doc::NodeId{200U}};
    MODEL_CHECK(planned.value().erases == expected);
    MODEL_CHECK(root.contains(doc::NodeId{200U}));
    MODEL_CHECK(root.contains(doc::NodeId{201U}));

    const auto missing =
        scene::plan_delete_effect(root, doc::NodeId{999U});
    MODEL_CHECK(!missing.has_value());
    MODEL_CHECK(
        missing.error() == scene::EffectDeletePlanError::EffectNotFound);
    const auto drawable =
        scene::plan_delete_effect(root, doc::NodeId{100U});
    MODEL_CHECK(!drawable.has_value());
    MODEL_CHECK(drawable.error() == scene::EffectDeletePlanError::NotAnEffect);
}


void a_placed_clip_is_on_air_only_inside_its_own_span() {
    scene::SceneModel model;
    model.reset(doc::Root{}.set(node(
        1U, schema::media_type,
        {text(schema::source, "plate_%04u.png"),
         whole(schema::sequence_start, 100),
         whole(schema::sequence_count, 24),
         whole(schema::clip_start, 50),
         whole(schema::clip_duration, 10)})));
    MODEL_CHECK(model.counters().rejected == 0U);

    const auto at = [&](core::u64 frame) {
        return model.evaluate(scene::SceneTime{frame, 60U, 1U});
    };
    MODEL_CHECK(at(49U).media.empty());
    MODEL_CHECK(at(50U).media.size() == 1U);
    MODEL_CHECK(at(59U).media.size() == 1U);
    MODEL_CHECK(at(60U).media.empty());

    // The clip counts from its own first frame, so moving it on the timeline
    // moves the picture with it rather than reshuffling which frame plays.
    MODEL_CHECK(at(50U).media.front().frame_index == 100U);
    MODEL_CHECK(at(55U).media.front().frame_index == 105U);

    // An unplaced clip keeps the legacy always-on, loop-from-zero contract.
    scene::SceneModel legacy;
    legacy.reset(doc::Root{}.set(node(
        2U, schema::media_type,
        {text(schema::source, "plate_%04u.png"),
         whole(schema::sequence_count, 24)})));
    MODEL_CHECK(
        legacy.evaluate(scene::SceneTime{55U, 60U, 1U}).media.size() == 1U);
    MODEL_CHECK(legacy.evaluate(scene::SceneTime{55U, 60U, 1U})
                    .media.front()
                    .frame_index == 55U % 24U);
}

void a_clip_trim_selects_which_source_frames_play() {
    scene::SceneModel model;
    model.reset(doc::Root{}.set(node(
        1U, schema::media_type,
        {text(schema::source, "plate_%04u.png"),
         whole(schema::sequence_start, 100),
         whole(schema::sequence_count, 24),
         whole(schema::clip_start, 8),
         whole(schema::clip_duration, 6),
         whole(schema::source_in, 5)})));
    MODEL_CHECK(model.counters().rejected == 0U);

    const auto index_at = [&](core::u64 frame) {
        return model.evaluate(scene::SceneTime{frame, 60U, 1U})
            .media.front()
            .frame_index;
    };
    MODEL_CHECK(index_at(8U) == 105U);
    MODEL_CHECK(index_at(13U) == 110U);

    // Trimming past the end of the source wraps, exactly as an untrimmed clip
    // does, rather than holding the last frame or refusing the node.
    scene::SceneModel wrapped;
    wrapped.reset(doc::Root{}.set(node(
        2U, schema::media_type,
        {text(schema::source, "plate_%04u.png"),
         whole(schema::sequence_start, 0),
         whole(schema::sequence_count, 4),
         whole(schema::clip_start, 0),
         whole(schema::clip_duration, 4),
         whole(schema::source_in, 7)})));
    MODEL_CHECK(wrapped.evaluate(scene::SceneTime{0U, 60U, 1U})
                    .media.front()
                    .frame_index == 3U);
}

void clip_ramps_scale_the_authored_opacity() {
    scene::SceneModel model;
    model.reset(doc::Root{}.set(node(
        1U, schema::media_type,
        {text(schema::source, "plate_%04u.png"),
         whole(schema::sequence_count, 100),
         whole(schema::clip_start, 0),
         whole(schema::clip_duration, 10),
         whole(schema::fade_in, 4),
         whole(schema::fade_out, 2),
         real(schema::opacity, 0.5)})));
    MODEL_CHECK(model.counters().rejected == 0U);

    const auto opacity_at = [&](core::u64 frame) {
        return model.evaluate(scene::SceneTime{frame, 60U, 1U})
            .media.front()
            .style.opacity;
    };
    // Fully out on the first frame, fully in after the head ramp, and the
    // authored 0.5 is what "fully in" means - a ramp scales the layer rather
    // than replacing it.
    MODEL_CHECK(opacity_at(0U) == 0.0F);
    MODEL_CHECK(std::abs(opacity_at(2U) - 0.25F) < 1.0e-6F);
    MODEL_CHECK(std::abs(opacity_at(4U) - 0.5F) < 1.0e-6F);
    MODEL_CHECK(std::abs(opacity_at(6U) - 0.5F) < 1.0e-6F);
    // Tail: the last frame is fully out again.
    MODEL_CHECK(std::abs(opacity_at(8U) - 0.25F) < 1.0e-6F);
    MODEL_CHECK(opacity_at(9U) == 0.0F);

    // The same arithmetic, reachable without a document, is what the editor
    // draws its ramps from.
    const scene::ClipTiming timing{0, 10, 0, 4, 2};
    MODEL_CHECK(scene::clip_visible(timing, 9));
    MODEL_CHECK(!scene::clip_visible(timing, 10));
    MODEL_CHECK(scene::clip_fade(timing, 4) == 1.0F);
    MODEL_CHECK(scene::clip_fade(timing, 9) == 0.0F);
    MODEL_CHECK(scene::clip_fade(scene::ClipTiming{}, 12'345) == 1.0F);
}

void a_negative_clip_placement_is_refused() {
    scene::SceneModel model;
    model.reset(doc::Root{}.set(node(
        1U, schema::media_type,
        {text(schema::source, "plate_%04u.png"),
         whole(schema::sequence_count, 4),
         whole(schema::clip_start, -3),
         whole(schema::clip_duration, 4)})));
    MODEL_CHECK(model.counters().rejected == 1U);
    MODEL_CHECK(model.evaluate(at_start).media.empty());
}

void clip_placements_are_listed_for_the_editor_off_air() {
    doc::Root root;
    root = root.set(node(10U, schema::stage_type, {}));
    root = root.set(node(
        1U, schema::media_type,
        {text(schema::source, "a_%04u.png"), whole(schema::sequence_count, 8),
         whole(schema::clip_start, 0), whole(schema::clip_duration, 4)}));
    root = root.set(node(
        2U, schema::media_type,
        {text(schema::source, "b_%04u.png"), whole(schema::sequence_count, 8),
         whole(schema::clip_start, 4), whole(schema::clip_duration, 4)}));
    scene::SceneModel model;
    model.reset(root);

    // At frame zero only the first clip draws, but the editor must still see
    // both placements or the second one becomes undraggable.
    const auto opening = model.evaluate(scene::SceneTime{0U, 60U, 1U});
    MODEL_CHECK(opening.media.size() == 1U);
    const auto placements = model.clips(scene::SceneId{10U});
    MODEL_CHECK(placements.size() == 2U);
    MODEL_CHECK(placements[0U].node == doc::NodeId{1U});
    MODEL_CHECK(placements[0U].timing == (scene::ClipTiming{0, 4, 0, 0, 0}));
    MODEL_CHECK(placements[1U].node == doc::NodeId{2U});
    MODEL_CHECK(placements[1U].timing == (scene::ClipTiming{4, 4, 0, 0, 0}));
    MODEL_CHECK(placements[1U].sequence_count == 8U);
    MODEL_CHECK(model.clips(scene::SceneId{999U}).empty());
}


void project_effect_settings_are_clamped_and_replicated() {
    scene::SceneModel model;
    model.reset(doc::Root{}.set(node(
        3U, schema::settings_type,
        {whole(schema::blur_taps, 200),
         real(schema::blur_sigma_ratio, 9.0),
         whole(schema::blur_passes, 99),
         text(schema::brightness_curve, schema::gain_curve),
         real(schema::brightness_gamma, 0.0)})));
    MODEL_CHECK(model.counters().rejected == 0U);

    const scene::EffectSettings settings = model.effect_settings();
    MODEL_CHECK(settings.blur_taps == scene::max_blur_taps);
    MODEL_CHECK(settings.blur_sigma_ratio == scene::max_blur_sigma_ratio);
    MODEL_CHECK(settings.blur_passes == scene::max_blur_passes);
    MODEL_CHECK(settings.brightness_curve == scene::BrightnessCurve::Gain);
    MODEL_CHECK(settings.brightness_gamma == scene::min_brightness_gamma);
    MODEL_CHECK(model.effect_settings_node() == doc::NodeId{3U});

    // An even tap count has no centre sample, so it is rounded up rather than
    // silently filtering off by half a pixel.
    scene::SceneModel even;
    even.reset(doc::Root{}.set(
        node(1U, schema::settings_type, {whole(schema::blur_taps, 8)})));
    MODEL_CHECK(even.effect_settings().blur_taps == 9U);

    // The evaluated scene carries them, and the digest covers them: two nodes
    // filtering differently would render a visibly different frame.
    const auto built = model.evaluate(at_start);
    MODEL_CHECK(built.effect_settings == settings);
    scene::Scene other = built;
    other.effect_settings.blur_taps = 5U;
    MODEL_CHECK(scene::digest(other) != scene::digest(built));

    // A project with no settings node is the built-in default, not a refusal.
    scene::SceneModel bare;
    bare.reset(doc::Root{});
    MODEL_CHECK(bare.effect_settings() == (scene::EffectSettings{}));
    MODEL_CHECK(bare.effect_settings_node() == doc::NodeId{});

    // A curve this build does not know is refused rather than guessed at.
    scene::SceneModel unknown;
    unknown.reset(doc::Root{}.set(node(
        1U, schema::settings_type,
        {text(schema::brightness_curve, "filmic")})));
    MODEL_CHECK(unknown.counters().rejected == 1U);
    MODEL_CHECK(unknown.effect_settings() == (scene::EffectSettings{}));
}

void the_brightness_gamma_shapes_the_folded_multiplier() {
    doc::Root root;
    root = root.set(node(1U, schema::object_type, {}));
    root = root.set(node(
        2U, schema::effect_type,
        {whole(schema::effect_target, 1),
         text(schema::effect_kind, schema::brightness_kind),
         real(schema::brightness, 4.0)}));
    root = root.set(node(
        3U, schema::effect_type,
        {whole(schema::effect_target, 1),
         text(schema::effect_kind, schema::brightness_kind),
         real(schema::brightness, 0.5)}));

    scene::SceneModel plain;
    plain.reset(root);
    const scene::Scene neutral_scene = plain.evaluate(at_start);
    const auto& neutral = neutral_scene.instances.front().style.effects;
    MODEL_CHECK(neutral.brightness_enabled);
    MODEL_CHECK(std::abs(neutral.brightness - 2.0F) < 1.0e-6F);

    // The gamma applies once to the composed multiplier, not once per effect:
    // two brightness nodes of 4 and 0.5 mean one of 2, and squaring that has
    // to give 4 rather than 4 * 0.25.
    scene::SceneModel shaped;
    shaped.reset(root.set(node(4U, schema::settings_type,
                               {real(schema::brightness_gamma, 2.0)})));
    const scene::Scene curved_scene = shaped.evaluate(at_start);
    const auto& curved = curved_scene.instances.front().style.effects;
    MODEL_CHECK(std::abs(curved.brightness - 4.0F) < 1.0e-5F);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 38U> cases{{
    {"a document becomes the scene it describes",
     &a_document_becomes_the_scene_it_describes},
    {"a checkpoint and a stream of deltas agree exactly",
     &a_checkpoint_and_a_stream_of_deltas_agree_exactly},
    {"evaluation is a pure function of the frame",
     &evaluation_is_a_pure_function_of_the_frame},
    {"a long show keeps moving rather than seizing up",
     &a_long_show_keeps_moving_rather_than_seizing_up},
    {"a property of the wrong type rejects its node",
     &a_property_of_the_wrong_type_rejects_its_node},
    {"retyping or erasing a node removes what it drew",
     &retyping_or_erasing_a_node_removes_what_it_drew},
    {"stages are scenes and the lowest is the default",
     &stages_are_scenes_and_the_lowest_is_the_default},
    {"the digest separates scenes a single bit apart",
     &the_digest_separates_scenes_a_single_bit_apart},
    {"a document past capacity truncates the same way everywhere",
     &a_document_past_capacity_truncates_the_same_way_everywhere},
    {"a point light is carried through as one",
     &a_point_light_is_carried_through_as_one},
    {"selection locks are carried by every element kind",
     &selection_locks_are_carried_by_every_element_kind},
    {"a media clip advances with the show frame",
     &a_media_clip_advances_with_the_show_frame},
    {"a placed clip is on air only inside its own span",
     &a_placed_clip_is_on_air_only_inside_its_own_span},
    {"a clip trim selects which source frames play",
     &a_clip_trim_selects_which_source_frames_play},
    {"clip ramps scale the authored opacity",
     &clip_ramps_scale_the_authored_opacity},
    {"a negative clip placement is refused",
     &a_negative_clip_placement_is_refused},
    {"clip placements are listed for the editor off air",
     &clip_placements_are_listed_for_the_editor_off_air},
    {"project effect settings are clamped and replicated",
     &project_effect_settings_are_clamped_and_replicated},
    {"the brightness gamma shapes the folded multiplier",
     &the_brightness_gamma_shapes_the_folded_multiplier},
    {"native media rate resamples show time exactly",
     &native_media_rate_resamples_show_time_exactly},
    {"first class effects are ordered folded and keep orphans",
     &first_class_effects_are_ordered_folded_and_keep_orphans},
    {"malformed effects are refused and values are bounded",
     &malformed_effects_are_refused_and_values_are_bounded},
    {"property channels drive opacity and individual effects",
     &property_channels_drive_opacity_and_individual_effects},
    {"the lowest id property channel wins everywhere",
     &the_lowest_id_property_channel_wins_everywhere},
    {"effect identity order and animation participate in the digest",
     &effect_identity_order_and_animation_participate_in_the_digest},
    {"a channel drives a component over show frames",
     &a_channel_drives_a_component_over_show_frames},
    {"channel interpolation modes differ",
     &channel_interpolation_modes_differ},
    {"the timeline query keeps values and interpolation",
     &the_timeline_query_keeps_values_and_interpolation},
    {"the lowest id channel wins a component",
     &the_lowest_id_channel_wins_a_component},
    {"a channel replicates bit exact", &a_channel_replicates_bit_exact},
    {"a malformed channel is refused", &a_malformed_channel_is_refused},
    {"scenes isolate drawables lights channels and effects",
     &scenes_isolate_drawables_lights_channels_and_effects},
    {"legacy scope tracks the lowest stage",
     &legacy_scope_tracks_the_lowest_stage},
    {"cameras are singletons within each scene",
     &cameras_are_singletons_within_each_scene},
    {"multi scene reset and streamed deltas agree",
     &multi_scene_reset_and_streamed_deltas_agree},
    {"clone planning is deterministic and rewrites scene references",
     &clone_planning_is_deterministic_and_rewrites_scene_references},
    {"clone planning reports source and id errors",
     &clone_planning_reports_source_and_id_errors},
    {"effect delete planning cascades only its animation channels",
     &effect_delete_planning_cascades_only_its_animation_channels},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const auto& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
