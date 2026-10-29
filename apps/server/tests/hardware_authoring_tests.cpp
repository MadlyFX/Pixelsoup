#include "director_feed_map_authoring.hpp"
#include "director_hardware_authoring.hpp"

#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/output/feed_map_model.hpp>
#include <mediaserver/output/feed_map_schema.hpp>
#include <mediaserver/topology/hardware_map_model.hpp>
#include <mediaserver/topology/hardware_map_schema.hpp>

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace app = mediaserver::app;
namespace cmd = mediaserver::cmd;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace editor = mediaserver::editor;
namespace output = mediaserver::output;
namespace scene = mediaserver::scene;
namespace topology = mediaserver::topology;
namespace schema = mediaserver::topology::schema;

void check(bool condition, const char *expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define AUTHOR_CHECK(...)                                                      \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

constexpr cmd::UndoContextId hardware_context{3U};

void put(doc::Node &node, std::string_view key, doc::PropertyValue value) {
    node.properties.insert_or_assign(std::string{key}, std::move(value));
}

[[nodiscard]] doc::Node node(core::u64 id, std::string_view type) {
    doc::Node built{};
    built.id = doc::NodeId{id};
    built.type = std::string{type};
    return built;
}

[[nodiscard]] doc::Node render_server(core::u64 id, std::int64_t index,
                                      bool main) {
    doc::Node server = node(id, schema::device_type);
    put(server, schema::kind, std::string{schema::render_server_kind});
    put(server, schema::name, "Render " + std::to_string(index));
    put(server, schema::node_index, index);
    put(server, schema::main_node, main);
    return server;
}

[[nodiscard]] doc::Node processor(core::u64 id) {
    doc::Node device = node(id, schema::device_type);
    put(device, schema::kind, std::string{schema::led_processor_kind});
    put(device, schema::name, std::string{"Processor"});
    return device;
}

[[nodiscard]] doc::Node link(core::u64 id, core::u64 from, core::u64 to) {
    doc::Node cable = node(id, schema::link_type);
    put(cable, schema::from_device, static_cast<std::int64_t>(from));
    put(cable, schema::from_port, std::int64_t{1});
    put(cable, schema::to_device, static_cast<std::int64_t>(to));
    put(cable, schema::to_port, std::int64_t{0});
    return cable;
}

// Render Servers on node indices 3 and 8, each cabled into its own
// processor; node 3 is the main node.
[[nodiscard]] doc::Root cluster() {
    doc::Root root;
    for (const doc::Node &one :
         {render_server(10U, 3, true), render_server(11U, 8, false),
          processor(30U), processor(31U), link(40U, 10U, 30U),
          link(41U, 11U, 31U)}) {
        root = root.set(one);
    }
    return root;
}

[[nodiscard]] std::optional<core::u32> main_of(const doc::Root &root) {
    topology::HardwareMapModel model;
    model.reset(root);
    return topology::main_render_node(model.evaluate());
}

void choosing_a_main_node_clears_the_previous_one_in_one_undo() {
    cmd::DirectorScheduler scheduler{cluster()};
    AUTHOR_CHECK(main_of(scheduler.projected_document()) ==
                 std::optional<core::u32>{3U});

    AUTHOR_CHECK(app::schedule_hardware_property(
        scheduler,
        editor::HardwarePropertyEdit{doc::NodeId{11U},
                                     std::string{schema::main_node},
                                     doc::PropertyValue{true}, 7U},
        cmd::FrameNumber{1U}));
    const doc::Root &chosen = scheduler.projected_document();
    AUTHOR_CHECK(main_of(chosen) == std::optional<core::u32>{8U});
    const doc::Node *const previous = chosen.get(doc::NodeId{10U});
    AUTHOR_CHECK(previous != nullptr);
    AUTHOR_CHECK(previous->properties.at(std::string{schema::main_node}) ==
                 doc::PropertyValue{false});
    AUTHOR_CHECK(scheduler.undo_depth(hardware_context) == 1U);

    AUTHOR_CHECK(scheduler.schedule_undo(hardware_context, cmd::TransactionId{8U},
                                         cmd::FrameNumber{2U}));
    AUTHOR_CHECK(main_of(scheduler.projected_document()) ==
                 std::optional<core::u32>{3U});
}

void clearing_the_main_node_leaves_none_and_bad_values_author_nothing() {
    cmd::DirectorScheduler scheduler{cluster()};
    AUTHOR_CHECK(app::schedule_hardware_property(
        scheduler,
        editor::HardwarePropertyEdit{doc::NodeId{10U},
                                     std::string{schema::main_node},
                                     doc::PropertyValue{false}, 9U},
        cmd::FrameNumber{1U}));
    AUTHOR_CHECK(!main_of(scheduler.projected_document()).has_value());
    AUTHOR_CHECK(scheduler.undo_depth(hardware_context) == 1U);

    // Not a boolean, or not a Render Server: refused without an edit.
    AUTHOR_CHECK(app::schedule_hardware_property(
        scheduler,
        editor::HardwarePropertyEdit{doc::NodeId{11U},
                                     std::string{schema::main_node},
                                     doc::PropertyValue{std::int64_t{1}}, 10U},
        cmd::FrameNumber{1U}));
    AUTHOR_CHECK(app::schedule_hardware_property(
        scheduler,
        editor::HardwarePropertyEdit{doc::NodeId{30U},
                                     std::string{schema::main_node},
                                     doc::PropertyValue{true}, 11U},
        cmd::FrameNumber{1U}));
    AUTHOR_CHECK(scheduler.undo_depth(hardware_context) == 1U);
    AUTHOR_CHECK(!main_of(scheduler.projected_document()).has_value());
}

[[nodiscard]] bool marked_main(const doc::Root &root, core::u64 id) {
    const doc::Node *const device = root.get(doc::NodeId{id});
    if (device == nullptr) {
        throw std::runtime_error{"placed device missing"};
    }
    const auto flag =
        device->properties.find(std::string{schema::main_node});
    return flag != device->properties.end() &&
           flag->second == doc::PropertyValue{true};
}

void the_first_render_server_placed_is_the_main_node() {
    // Other hardware first: it neither takes the flag nor uses it up.
    cmd::DirectorScheduler scheduler{doc::Root{}};
    AUTHOR_CHECK(app::schedule_hardware_place(
        scheduler,
        editor::HardwarePlaceRequest{
            topology::HardwareDeviceKind::LedProcessor, 2, 0, 20U, {}},
        doc::NodeId{50U}, cmd::FrameNumber{1U}));
    AUTHOR_CHECK(!marked_main(scheduler.projected_document(), 50U));

    AUTHOR_CHECK(app::schedule_hardware_place(
        scheduler,
        editor::HardwarePlaceRequest{
            topology::HardwareDeviceKind::RenderServer, 0, 0, 21U, {}},
        doc::NodeId{51U}, cmd::FrameNumber{1U}));
    AUTHOR_CHECK(marked_main(scheduler.projected_document(), 51U));

    // A second card starts unmarked; the first keeps the role.
    AUTHOR_CHECK(app::schedule_hardware_place(
        scheduler,
        editor::HardwarePlaceRequest{
            topology::HardwareDeviceKind::RenderServer, 0, 3, 22U, {}},
        doc::NodeId{52U}, cmd::FrameNumber{1U}));
    AUTHOR_CHECK(!marked_main(scheduler.projected_document(), 52U));
    AUTHOR_CHECK(marked_main(scheduler.projected_document(), 51U));

    // Any existing card counts, even one cleared by hand: adding another to
    // a show that has servers never chooses for the operator.
    cmd::DirectorScheduler unmarked{cluster()};
    AUTHOR_CHECK(app::schedule_hardware_property(
        unmarked,
        editor::HardwarePropertyEdit{doc::NodeId{10U},
                                     std::string{schema::main_node},
                                     doc::PropertyValue{false}, 23U},
        cmd::FrameNumber{1U}));
    AUTHOR_CHECK(app::schedule_hardware_place(
        unmarked,
        editor::HardwarePlaceRequest{
            topology::HardwareDeviceKind::RenderServer, 4, 4, 24U, {}},
        doc::NodeId{53U}, cmd::FrameNumber{1U}));
    AUTHOR_CHECK(!marked_main(unmarked.projected_document(), 53U));
}

[[nodiscard]] topology::HardwareMap hardware_of(const doc::Root &root) {
    topology::HardwareMapModel model;
    model.reset(root);
    return model.evaluate();
}

void a_found_processor_is_placed_speaking_its_protocol() {
    cmd::DirectorScheduler scheduler{doc::Root{}};
    editor::HardwarePlaceRequest request{
        topology::HardwareDeviceKind::LedProcessor, 3, 1, 30U, {}};
    request.machine =
        editor::HardwarePlaceMachine{"MX40 Pro_002311", "192.0.2.41", 0U};
    request.protocol = topology::HardwareProcessorProtocol::NovastarCoex;
    AUTHOR_CHECK(app::schedule_hardware_place(scheduler, request,
                                              doc::NodeId{60U},
                                              cmd::FrameNumber{1U}));
    const topology::HardwareMap map =
        hardware_of(scheduler.projected_document());
    const topology::HardwareDevice *const placed = map.device(doc::NodeId{60U});
    AUTHOR_CHECK(placed != nullptr);
    AUTHOR_CHECK(placed->name == "MX40 Pro_002311");
    AUTHOR_CHECK(placed->address == "192.0.2.41");
    AUTHOR_CHECK(placed->protocol ==
                 topology::HardwareProcessorProtocol::NovastarCoex);
}

void a_protocol_is_only_ever_a_processors() {
    cmd::DirectorScheduler scheduler{cluster()};
    const auto edit = [&](core::u64 device, std::string value,
                          core::u64 gesture) {
        return app::schedule_hardware_property(
            scheduler,
            editor::HardwarePropertyEdit{doc::NodeId{device},
                                         std::string{schema::protocol},
                                         doc::PropertyValue{std::move(value)},
                                         gesture},
            cmd::FrameNumber{1U});
    };
    AUTHOR_CHECK(edit(30U, std::string{schema::novastar_coex_protocol}, 40U));
    AUTHOR_CHECK(hardware_of(scheduler.projected_document())
                     .device(doc::NodeId{30U})
                     ->protocol ==
                 topology::HardwareProcessorProtocol::NovastarCoex);
    AUTHOR_CHECK(scheduler.undo_depth(hardware_context) == 1U);
    // A render server has none, and an unknown protocol is no protocol.
    AUTHOR_CHECK(edit(10U, std::string{schema::novastar_coex_protocol}, 41U));
    AUTHOR_CHECK(edit(31U, "colorlight", 42U));
    AUTHOR_CHECK(scheduler.undo_depth(hardware_context) == 1U);
    AUTHOR_CHECK(hardware_of(scheduler.projected_document())
                     .device(doc::NodeId{31U})
                     ->protocol == topology::HardwareProcessorProtocol::Generic);
}

[[nodiscard]] doc::Node viewport(core::u64 id, std::int64_t window,
                                 std::int64_t x) {
    doc::Node map = node(id, output::schema::map_type);
    put(map, output::schema::node_index, window);
    put(map, output::schema::source_x, x);
    put(map, output::schema::source_y, std::int64_t{0});
    put(map, output::schema::source_width, std::int64_t{1920});
    put(map, output::schema::source_height, std::int64_t{1080});
    put(map, output::schema::viewport_x, std::int64_t{0});
    put(map, output::schema::viewport_y, std::int64_t{0});
    put(map, output::schema::viewport_width, std::int64_t{1920});
    put(map, output::schema::viewport_height, std::int64_t{1080});
    put(map, output::schema::projection,
        std::string{output::schema::projection_canvas});
    put(map, output::schema::surface_id, std::int64_t{0});
    return map;
}

void a_built_feed_layout_replaces_its_windows_in_one_undo() {
    doc::Root start;
    // Window 0 has two hand-made viewports; window 5 is not in the plan.
    for (const doc::Node &one : {viewport(70U, 0, 0), viewport(71U, 0, 1920),
                                 viewport(72U, 5, 0)}) {
        start = start.set(one);
    }
    cmd::DirectorScheduler scheduler{start};

    output::FeedLayoutPlan plan{};
    plan.canvas = scene::Canvas{6912U, 1536U};
    plan.windows = {0U, 1U};
    plan.viewports = {
        output::FeedLayoutViewport{0U, "Main wall - HDMI 1",
                                   scene::CanvasRect{0U, 0U, 3456U, 1536U},
                                   scene::CanvasRect{0U, 0U, 3456U, 1536U}},
        output::FeedLayoutViewport{1U, "Main wall - HDMI 2",
                                   scene::CanvasRect{3456U, 0U, 3456U, 1536U},
                                   scene::CanvasRect{0U, 0U, 3456U, 1536U}},
    };
    const std::array<doc::NodeId, 2U> ids{doc::NodeId{80U}, doc::NodeId{81U}};
    AUTHOR_CHECK(app::schedule_feed_layout(
        scheduler, editor::FeedLayoutRequest{plan, 50U}, ids,
        cmd::FrameNumber{1U}));

    output::FeedMapModel model;
    model.reset(scheduler.projected_document());
    const output::FeedMap built = model.evaluate();
    AUTHOR_CHECK(built.map_node(doc::NodeId{70U}) == nullptr);
    AUTHOR_CHECK(built.map_node(doc::NodeId{71U}) == nullptr);
    AUTHOR_CHECK(built.map_node(doc::NodeId{72U}) != nullptr);
    const output::OutputMap *const first = built.map_node(doc::NodeId{80U});
    const output::OutputMap *const second = built.map_node(doc::NodeId{81U});
    AUTHOR_CHECK(first != nullptr && second != nullptr);
    AUTHOR_CHECK(first->node_index == 0U && second->node_index == 1U);
    AUTHOR_CHECK(first->name == "Main wall - HDMI 1");
    AUTHOR_CHECK(second->source == (scene::CanvasRect{3456U, 0U, 3456U, 1536U}));
    AUTHOR_CHECK(second->viewport == (scene::CanvasRect{0U, 0U, 3456U, 1536U}));
    // One transaction: one undo puts window 0's own viewports back.
    AUTHOR_CHECK(scheduler.undo_depth(hardware_context) == 1U);
    AUTHOR_CHECK(scheduler.schedule_undo(hardware_context,
                                         cmd::TransactionId{51U},
                                         cmd::FrameNumber{2U}));
    model.reset(scheduler.projected_document());
    const output::FeedMap restored = model.evaluate();
    AUTHOR_CHECK(restored.map_node(doc::NodeId{70U}) != nullptr);
    AUTHOR_CHECK(restored.map_node(doc::NodeId{71U}) != nullptr);
    AUTHOR_CHECK(restored.map_node(doc::NodeId{80U}) == nullptr);
}

struct Case final {
    const char *name;
    void (*function)();
};

constexpr std::array<Case, 6U> cases{{
    {"a found processor is placed speaking its protocol",
     &a_found_processor_is_placed_speaking_its_protocol},
    {"a protocol is only ever a processors",
     &a_protocol_is_only_ever_a_processors},
    {"a built feed layout replaces its windows in one undo",
     &a_built_feed_layout_replaces_its_windows_in_one_undo},
    {"the first render server placed is the main node",
     &the_first_render_server_placed_is_the_main_node},
    {"choosing a main node clears the previous one in one undo",
     &choosing_a_main_node_clears_the_previous_one_in_one_undo},
    {"clearing the main node leaves none and bad values author nothing",
     &clearing_the_main_node_leaves_none_and_bad_values_author_nothing},
}};

} // namespace

int main() {
    int failures = 0;
    for (const Case &test : cases) {
        try {
            test.function();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception &error) {
            ++failures;
            std::cerr << "[fail] " << test.name << ": " << error.what()
                      << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
