#include <mediaserver/scene/scene_model.hpp>

#include <mediaserver/scene/scene_schema.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
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

void the_lowest_node_id_wins_a_duplicated_singleton() {
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
    MODEL_CHECK(model.counters().duplicate_singletons() == 1U);
    // The document decides, not the order the nodes happened to be written in.
    MODEL_CHECK(model.evaluate(at_start).canvas == (scene::Canvas{1'024U, 768U}));
}

void the_digest_separates_scenes_a_single_bit_apart() {
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

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 10U> cases{{
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
    {"the lowest node id wins a duplicated singleton",
     &the_lowest_node_id_wins_a_duplicated_singleton},
    {"the digest separates scenes a single bit apart",
     &the_digest_separates_scenes_a_single_bit_apart},
    {"a document past capacity truncates the same way everywhere",
     &a_document_past_capacity_truncates_the_same_way_everywhere},
    {"a point light is carried through as one",
     &a_point_light_is_carried_through_as_one},
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
