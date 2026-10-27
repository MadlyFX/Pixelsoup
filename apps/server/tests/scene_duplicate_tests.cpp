#include "scene_duplicate.hpp"

#include <mediaserver/scene/scene_schema.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <variant>

// Alt+drag duplication on Stage: what the Director copies in place, what it
// leaves alone, and that every reference in the copies points at the copies.
namespace {
using namespace mediaserver;
namespace schema = scene::schema;
#define CHECK(...)                                                             \
    do {                                                                       \
        if (!(__VA_ARGS__)) throw std::runtime_error{#__VA_ARGS__};            \
    } while (false)

[[nodiscard]] doc::Node node(std::uint64_t id, std::string_view type) {
    doc::Node out{doc::NodeId{id}, std::string{type}, {}};
    out.properties.emplace(std::string{schema::scene_id}, std::int64_t{31});
    return out;
}

[[nodiscard]] std::int64_t integer(const doc::Node& from, std::string_view name) {
    return std::get<std::int64_t>(from.properties.at(std::string{name}));
}

void elements_are_copied_in_place_with_what_hangs_off_them() {
    doc::Node box = node(10U, schema::object_type);
    box.properties.emplace(std::string{schema::position_x}, 2.5);
    box.properties.emplace(std::string{schema::parent_group}, std::int64_t{40});
    doc::Node effect = node(11U, schema::effect_type);
    effect.properties.emplace(std::string{schema::effect_target}, std::int64_t{10});
    doc::Node keys = node(12U, schema::channel_type);
    keys.properties.emplace(std::string{schema::channel_target}, std::int64_t{11});
    doc::Node mask = node(13U, schema::mask_type);
    mask.properties.emplace(std::string{schema::mask_target}, std::int64_t{10});
    doc::Node point = node(14U, schema::mask_point_type);
    point.properties.emplace(std::string{schema::mask_id}, std::int64_t{13});
    doc::Node locked = node(20U, schema::media_type);
    locked.properties.emplace(std::string{schema::locked}, true);
    const doc::Node group = node(40U, schema::group_type);
    doc::Node unrelated = node(50U, schema::effect_type);
    unrelated.properties.emplace(std::string{schema::effect_target}, std::int64_t{99});
    const doc::Root document = doc::Root{}
                                   .set(box)
                                   .set(effect)
                                   .set(keys)
                                   .set(mask)
                                   .set(point)
                                   .set(locked)
                                   .set(group)
                                   .set(unrelated);

    const std::array<doc::NodeId, 3U> selection{doc::NodeId{10U}, doc::NodeId{20U},
                                                doc::NodeId{40U}};
    const auto plan = app::duplicate_in_place(document, selection, 60U);
    CHECK(plan.has_value());
    // Only the box: the clip is locked and a group is not copied.
    CHECK(plan->created.size() == 1U);
    const doc::NodeId copy = plan->created.front();
    CHECK(copy == doc::NodeId{60U});
    const doc::Node* const copied = plan->document.get(copy);
    CHECK(copied != nullptr && copied->type == schema::object_type);
    // In place: same position, group and scene.
    CHECK(std::get<double>(copied->properties.at(std::string{schema::position_x})) == 2.5);
    CHECK(integer(*copied, schema::parent_group) == 40);
    CHECK(integer(*copied, schema::scene_id) == 31);

    // The effect, its keys, the mask and its point came too, each pointing
    // at its copied owner; the originals are untouched.
    CHECK(plan->document.get(doc::NodeId{61U})->type == schema::effect_type);
    CHECK(integer(*plan->document.get(doc::NodeId{61U}), schema::effect_target) == 60);
    CHECK(plan->document.get(doc::NodeId{62U})->type == schema::channel_type);
    CHECK(integer(*plan->document.get(doc::NodeId{62U}), schema::channel_target) == 61);
    CHECK(plan->document.get(doc::NodeId{63U})->type == schema::mask_type);
    CHECK(integer(*plan->document.get(doc::NodeId{63U}), schema::mask_target) == 60);
    CHECK(plan->document.get(doc::NodeId{64U})->type == schema::mask_point_type);
    CHECK(integer(*plan->document.get(doc::NodeId{64U}), schema::mask_id) == 63);
    CHECK(!plan->document.contains(doc::NodeId{65U}));
    CHECK(plan->next_node == 65U);
    CHECK(*plan->document.get(doc::NodeId{11U}) == effect);
    CHECK(*plan->document.get(doc::NodeId{50U}) == unrelated);

    // Fresh ids skip what the document already holds.
    const auto around = app::duplicate_in_place(
        document.set(node(61U, schema::light_type)), selection, 60U);
    CHECK(around.has_value() && around->next_node == 66U);
    CHECK(around->document.get(doc::NodeId{61U})->type == schema::light_type);

    // Nothing copyable selected: no plan.
    const std::array<doc::NodeId, 2U> nothing{doc::NodeId{20U}, doc::NodeId{40U}};
    CHECK(!app::duplicate_in_place(document, nothing, 60U).has_value());
}

} // namespace

int main() {
    try {
        elements_are_copied_in_place_with_what_hangs_off_them();
        std::cout << "[pass] elements are copied in place with what hangs off them\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[fail] " << error.what() << '\n';
        return 1;
    }
}
