#include <mediaserver/output/feed_map_model.hpp>

#include <mediaserver/output/feed_map.hpp>
#include <mediaserver/output/feed_map_schema.hpp>

#include <array>
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
namespace output = mediaserver::output;
namespace schema = mediaserver::output::schema;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define OUTPUT_CHECK(...) \
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

[[nodiscard]] Property text(std::string_view name, std::string_view value) {
    return Property{std::string{name}, doc::PropertyValue{std::string{value}}};
}

[[nodiscard]] Property whole(std::string_view name, std::int64_t value) {
    return Property{std::string{name}, doc::PropertyValue{value}};
}

// A two-node LED wall across a 2560x720 canvas. Each node outputs a 1280-wide
// half; the two abut at column 1280 with no overlap.
[[nodiscard]] std::vector<doc::Node> demo_maps() {
    return {
        node(
            1U,
            schema::map_type,
            {whole(schema::node_index, 0),
             whole(schema::source_x, 0),
             whole(schema::source_y, 0),
             whole(schema::source_width, 1'280),
             whole(schema::source_height, 720)}),
        node(
            2U,
            schema::map_type,
            {whole(schema::node_index, 1),
             whole(schema::source_x, 1'280),
             whole(schema::source_y, 0),
             whole(schema::source_width, 1'280),
             whole(schema::source_height, 720)}),
    };
}

[[nodiscard]] doc::Root root_of(const std::vector<doc::Node>& nodes) {
    doc::Root root;
    for (const auto& one : nodes) {
        root = root.set(one);
    }
    return root;
}

void a_document_becomes_the_feed_map_it_describes() {
    output::FeedMapModel model;
    model.reset(root_of(demo_maps()));

    OUTPUT_CHECK(model.counters().rejected == 0U);
    OUTPUT_CHECK(model.counters().maps == 2U);
    OUTPUT_CHECK(model.counters().over_capacity() == 0U);

    const auto built = model.evaluate();
    OUTPUT_CHECK(built.maps.size() == 2U);

    const auto* const left = built.map_for(0U);
    const auto* const right = built.map_for(1U);
    OUTPUT_CHECK(left != nullptr && right != nullptr);
    OUTPUT_CHECK(left->source == (mediaserver::scene::CanvasRect{0U, 0U, 1'280U, 720U}));
    OUTPUT_CHECK(
        right->source == (mediaserver::scene::CanvasRect{1'280U, 0U, 1'280U, 720U}));
    // A node the document does not map outputs nothing rather than guessing.
    OUTPUT_CHECK(built.map_for(2U) == nullptr);
}

void a_checkpoint_and_a_stream_of_deltas_agree_exactly() {
    // The replication claim: a node handed the whole document and a node that
    // folded in deltas must hold the same feed map, or their regions of the wall
    // meet at a seam.
    const auto nodes = demo_maps();

    output::FeedMapModel checkpointed;
    checkpointed.reset(root_of(nodes));

    output::FeedMapModel streamed;
    doc::Root running;
    for (const auto& one : nodes) {
        const auto next = running.set(one);
        streamed.apply(diff(running, next));
        running = next;
    }

    OUTPUT_CHECK(
        output::digest(checkpointed.evaluate())
        == output::digest(streamed.evaluate()));
}

void adjacent_maps_tile_the_canvas() {
    // The property the layer exists to guarantee on an LED wall: neighbouring
    // nodes' regions abut edge to edge with no gap and no overlap, so together
    // they tile the canvas exactly. There is no blend; the seam is pixel-exact.
    output::FeedMapModel model;
    model.reset(root_of(demo_maps()));
    const auto built = model.evaluate();
    const auto* const left = built.map_for(0U);
    const auto* const right = built.map_for(1U);
    OUTPUT_CHECK(left != nullptr && right != nullptr);

    // Same rows, and the right region begins exactly where the left one ends.
    OUTPUT_CHECK(left->source.y == right->source.y);
    OUTPUT_CHECK(left->source.height == right->source.height);
    OUTPUT_CHECK(left->source.x + left->source.width == right->source.x);
    // And together they cover the whole 2560-wide canvas.
    OUTPUT_CHECK(left->source.x == 0U);
    OUTPUT_CHECK(right->source.x + right->source.width == 2'560U);
}

void a_property_of_the_wrong_type_rejects_its_node() {
    // source_width as a string rather than a number. Defaulting it would leave
    // a node outputting a zero-width strip with no indication why.
    output::FeedMapModel model;
    model.reset(doc::Root{}.set(node(
        1U,
        schema::map_type,
        {whole(schema::source_height, 720),
         text(schema::source_width, "wide")})));
    OUTPUT_CHECK(model.counters().rejected == 1U);
    OUTPUT_CHECK(model.evaluate().maps.empty());

    // A zero-size source is refused: a node cannot output an empty rectangle.
    output::FeedMapModel empty;
    empty.reset(doc::Root{}.set(node(
        1U,
        schema::map_type,
        {whole(schema::source_width, 0), whole(schema::source_height, 720)})));
    OUTPUT_CHECK(empty.counters().rejected == 1U);

    // An absent property, by contrast, takes its default, so a map authored
    // before a property existed still loads.
    output::FeedMapModel sparse;
    sparse.reset(doc::Root{}.set(node(
        1U,
        schema::map_type,
        {whole(schema::source_width, 1'280), whole(schema::source_height, 720)})));
    OUTPUT_CHECK(sparse.counters().rejected == 0U);
    OUTPUT_CHECK(sparse.evaluate().maps.size() == 1U);
}

void retyping_or_erasing_a_node_removes_its_binding() {
    output::FeedMapModel model;
    doc::Root root = doc::Root{}.set(node(
        1U,
        schema::map_type,
        {whole(schema::source_width, 1'280), whole(schema::source_height, 720)}));
    model.reset(root);
    OUTPUT_CHECK(model.evaluate().maps.size() == 1U);

    const auto retyped = root.set(node(1U, "scene.object", {}));
    model.apply(diff(root, retyped));
    OUTPUT_CHECK(model.evaluate().maps.empty());
    OUTPUT_CHECK(model.counters().maps == 0U);
    OUTPUT_CHECK(model.size() == 0U);

    output::FeedMapModel erased_model;
    erased_model.reset(root);
    const auto erased = root.erase(doc::NodeId{1U});
    OUTPUT_CHECK(erased.has_value());
    erased_model.apply(diff(root, *erased));
    OUTPUT_CHECK(erased_model.evaluate().maps.empty());
    OUTPUT_CHECK(erased_model.counters().maps == 0U);
}

void the_lowest_node_id_wins_a_duplicated_index() {
    output::FeedMapModel model;
    doc::Root root;
    root = root.set(node(
        9U,
        schema::map_type,
        {whole(schema::node_index, 0),
         whole(schema::source_width, 800),
         whole(schema::source_height, 600)}));
    root = root.set(node(
        4U,
        schema::map_type,
        {whole(schema::node_index, 0),
         whole(schema::source_width, 1'024),
         whole(schema::source_height, 768)}));
    model.reset(root);

    const auto built = model.evaluate();
    // Two nodes claim index 0; the document decides which, not the order the
    // nodes were written in.
    OUTPUT_CHECK(built.maps.size() == 1U);
    const auto* const bound = built.map_for(0U);
    OUTPUT_CHECK(bound != nullptr);
    OUTPUT_CHECK(bound->source.width == 1'024U);
}

void the_digest_separates_maps_a_single_bit_apart() {
    output::FeedMapModel base;
    base.reset(root_of(demo_maps()));
    const auto reference = output::digest(base.evaluate());

    // The smallest change to a region a map can hold: one column wider. A digest
    // that missed it would miss the divergence that matters most - the one that
    // opens a one-pixel seam nobody notices until it is on a wall.
    auto nudged = demo_maps();
    nudged[0U].properties.insert_or_assign(
        std::string{schema::source_width}, doc::PropertyValue{std::int64_t{1'281}});

    output::FeedMapModel moved;
    moved.reset(root_of(nudged));
    OUTPUT_CHECK(output::digest(moved.evaluate()) != reference);

    // A feed map that is a prefix of another does not collide with it.
    auto shorter = demo_maps();
    shorter.pop_back();
    output::FeedMapModel truncated;
    truncated.reset(root_of(shorter));
    OUTPUT_CHECK(output::digest(truncated.evaluate()) != reference);
}

void a_document_past_capacity_truncates_the_same_way_everywhere() {
    output::FeedMapModel model;
    doc::Root root;
    const auto count = output::max_output_maps + 16U;
    for (core::u32 index = 0U; index < count; ++index) {
        root = root.set(node(
            static_cast<core::u64>(index) + 1U,
            schema::map_type,
            {whole(schema::node_index, static_cast<std::int64_t>(index)),
             whole(schema::source_width, 1'280),
             whole(schema::source_height, 720)}));
    }
    model.reset(root);
    OUTPUT_CHECK(model.counters().maps == count);
    OUTPUT_CHECK(model.counters().over_capacity() == 16U);

    const auto built = model.evaluate();
    OUTPUT_CHECK(built.maps.size() == output::max_output_maps);

    // Truncation is by node id, so a streamed document and a checkpointed one
    // drop the same maps.
    output::FeedMapModel streamed;
    doc::Root running;
    for (core::u32 index = 0U; index < count; ++index) {
        const auto next = root.get(doc::NodeId{static_cast<core::u64>(index) + 1U});
        OUTPUT_CHECK(next != nullptr);
        const auto grown = running.set(*next);
        streamed.apply(diff(running, grown));
        running = grown;
    }
    OUTPUT_CHECK(
        output::digest(streamed.evaluate()) == output::digest(built));
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 8U> cases{{
    {"a document becomes the feed map it describes",
     &a_document_becomes_the_feed_map_it_describes},
    {"a checkpoint and a stream of deltas agree exactly",
     &a_checkpoint_and_a_stream_of_deltas_agree_exactly},
    {"adjacent maps tile the canvas",
     &adjacent_maps_tile_the_canvas},
    {"a property of the wrong type rejects its node",
     &a_property_of_the_wrong_type_rejects_its_node},
    {"retyping or erasing a node removes its binding",
     &retyping_or_erasing_a_node_removes_its_binding},
    {"the lowest node id wins a duplicated index",
     &the_lowest_node_id_wins_a_duplicated_index},
    {"the digest separates maps a single bit apart",
     &the_digest_separates_maps_a_single_bit_apart},
    {"a document past capacity truncates the same way everywhere",
     &a_document_past_capacity_truncates_the_same_way_everywhere},
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
