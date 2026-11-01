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
#include <limits>
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

// A rate typed in hertz becomes both halves of its exact fraction in one undo
// step; anything but a number is ignored.
void a_typed_rate_is_stored_as_an_exact_fraction() {
    cmd::DirectorScheduler scheduler{cluster()};
    const auto edit = [&](doc::PropertyValue value, core::u64 gesture) {
        return app::schedule_hardware_property(
            scheduler,
            editor::HardwarePropertyEdit{
                doc::NodeId{30U}, std::string{editor::hardware_refresh_edit},
                std::move(value), gesture},
            cmd::FrameNumber{1U});
    };
    AUTHOR_CHECK(edit(doc::PropertyValue{23.976}, 50U));
    const topology::HardwareMap rated_map =
        hardware_of(scheduler.projected_document());
    const topology::HardwareDevice *const rated =
        rated_map.device(doc::NodeId{30U});
    AUTHOR_CHECK(rated != nullptr && rated->refresh_numerator == 24'000U &&
                 rated->refresh_denominator == 1'001U);
    AUTHOR_CHECK(scheduler.undo_depth(hardware_context) == 1U);
    // Nothing is stored under the edit's own name.
    AUTHOR_CHECK(!scheduler.projected_document()
                      .get(doc::NodeId{30U})
                      ->properties.contains(
                          std::string{editor::hardware_refresh_edit}));

    AUTHOR_CHECK(edit(doc::PropertyValue{std::string{"fast"}}, 51U));
    AUTHOR_CHECK(scheduler.undo_depth(hardware_context) == 1U);

    AUTHOR_CHECK(scheduler.schedule_undo(hardware_context,
                                         cmd::TransactionId{52U},
                                         cmd::FrameNumber{2U}));
    const topology::HardwareMap restored_map =
        hardware_of(scheduler.projected_document());
    const topology::HardwareDevice *const restored =
        restored_map.device(doc::NodeId{30U});
    AUTHOR_CHECK(restored->refresh_numerator == 60U &&
                 restored->refresh_denominator == 1U);
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

void a_processors_reported_walls_are_placed_cabled_in_one_undo() {
    cmd::DirectorScheduler scheduler{cluster()};
    editor::HardwareWallsRequest request{};
    request.processor = doc::NodeId{30U};
    request.gesture = 12U;
    request.walls = {
        topology::HardwareWallProposal{"Main", 2880U, 1536U, 24'000U, 1'001U,
                                       5, -1},
        topology::HardwareWallProposal{"Ceiling", 1152U, 768U, 50U, 1U, 5, 2}};
    const std::vector<doc::NodeId> ids{doc::NodeId{70U}, doc::NodeId{71U},
                                       doc::NodeId{72U}, doc::NodeId{73U}};
    AUTHOR_CHECK(app::schedule_hardware_walls(scheduler, request, ids,
                                              cmd::FrameNumber{1U}));
    const topology::HardwareMap map =
        hardware_of(scheduler.projected_document());
    const topology::HardwareDevice *const main = map.device(doc::NodeId{70U});
    AUTHOR_CHECK(main != nullptr &&
                 main->kind == topology::HardwareDeviceKind::LedWall);
    AUTHOR_CHECK(main->name == "Main" && main->width == 2880U &&
                 main->height == 1536U && main->refresh_numerator == 24'000U &&
                 main->refresh_denominator == 1'001U);
    AUTHOR_CHECK(main->grid_x == 5 && main->grid_y == -1);
    const topology::HardwareLink *const cable = map.link(doc::NodeId{73U});
    AUTHOR_CHECK(cable != nullptr);
    AUTHOR_CHECK(cable->from ==
                 (topology::HardwareEndpoint{
                     doc::NodeId{30U}, topology::processor_led_output_port}));
    AUTHOR_CHECK(cable->to ==
                 (topology::HardwareEndpoint{doc::NodeId{72U}, 0U}));
    AUTHOR_CHECK(map.link(doc::NodeId{71U}) != nullptr);
    AUTHOR_CHECK(scheduler.undo_depth(hardware_context) == 1U);

    // Asked again - a second editor, or a report after a cable - it places
    // nothing, since the LED OUT now drives something.
    request.gesture = 13U;
    const std::vector<doc::NodeId> more{doc::NodeId{80U}, doc::NodeId{81U},
                                        doc::NodeId{82U}, doc::NodeId{83U}};
    AUTHOR_CHECK(app::schedule_hardware_walls(scheduler, request, more,
                                              cmd::FrameNumber{2U}));
    AUTHOR_CHECK(!scheduler.projected_document().contains(doc::NodeId{80U}));
    AUTHOR_CHECK(scheduler.undo_depth(hardware_context) == 1U);

    AUTHOR_CHECK(scheduler.schedule_undo(hardware_context,
                                         cmd::TransactionId{14U},
                                         cmd::FrameNumber{3U}));
    const doc::Root &undone = scheduler.projected_document();
    for (const core::u64 id : {70U, 71U, 72U, 73U}) {
        AUTHOR_CHECK(!undone.contains(doc::NodeId{id}));
    }
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

void a_stage_edit_creates_then_updates_one_node_in_one_gesture() {
    cmd::DirectorScheduler scheduler{doc::Root{}};
    const auto stage_of = [&] {
        output::FeedMapModel model;
        model.reset(scheduler.projected_document());
        return model.evaluate();
    };
    AUTHOR_CHECK(stage_of().stage == output::StagePlacement{});

    // The first edit of a project with no stage creates its node...
    bool allocated = false;
    output::StagePlacement carried{};
    carried.anchor = output::StageAnchor::Camera;
    carried.eye = scene::Vec3{0.0F, 1.6F, 0.25F};
    AUTHOR_CHECK(app::schedule_feed_stage_edit(
        scheduler, editor::FeedStageEditRequest{carried, 60U},
        doc::NodeId{90U}, cmd::FrameNumber{1U}, allocated));
    AUTHOR_CHECK(allocated);
    const output::FeedMap first = stage_of();
    AUTHOR_CHECK(first.stage_node == doc::NodeId{90U});
    AUTHOR_CHECK(first.stage == carried);

    // ...and a scrub in the same gesture updates it in place.
    carried.eye.y = 1.75F;
    allocated = false;
    AUTHOR_CHECK(app::schedule_feed_stage_edit(
        scheduler, editor::FeedStageEditRequest{carried, 60U},
        doc::NodeId{91U}, cmd::FrameNumber{2U}, allocated));
    AUTHOR_CHECK(!allocated);
    const output::FeedMap second = stage_of();
    AUTHOR_CHECK(second.stage_node == doc::NodeId{90U});
    AUTHOR_CHECK(second.stage.eye.y == 1.75F);
    AUTHOR_CHECK(!scheduler.projected_document().contains(doc::NodeId{91U}));

    // A non-finite eye authors nothing.
    carried.eye.x = std::numeric_limits<float>::infinity();
    AUTHOR_CHECK(app::schedule_feed_stage_edit(
        scheduler, editor::FeedStageEditRequest{carried, 61U},
        doc::NodeId{92U}, cmd::FrameNumber{3U}, allocated));
    AUTHOR_CHECK(!allocated);
    AUTHOR_CHECK(stage_of().stage.eye.x == 0.0F);

    // One undo takes the whole gesture back to the world-fixed stage.
    constexpr cmd::UndoContextId feed_context{4U};
    AUTHOR_CHECK(scheduler.undo_depth(feed_context) == 1U);
    AUTHOR_CHECK(scheduler.schedule_undo(feed_context,
                                         cmd::TransactionId{62U},
                                         cmd::FrameNumber{4U}));
    AUTHOR_CHECK(stage_of().stage == output::StagePlacement{});
    AUTHOR_CHECK(!scheduler.projected_document().contains(doc::NodeId{90U}));
}

[[nodiscard]] topology::HardwareOutputBinding
bound_output(core::u64 id, core::u8 port, core::u32 feed, core::u32 target) {
    topology::HardwareOutputBinding binding{};
    binding.node = doc::NodeId{id};
    binding.render_server = doc::NodeId{10U};
    binding.logical_port = port;
    binding.feed_index = feed;
    binding.target = topology::HardwareDisplayTargetKey{0xA1B2C3ULL, target, 7U};
    binding.width = 3840U;
    binding.height = 2160U;
    return binding;
}

void moving_a_cable_to_another_output_keeps_its_region_in_one_undo() {
    // Render 3 drives Processor 30 from OUT 1, bound to feed 4.
    doc::Root start = cluster();
    start = start.set(app::make_hardware_output_binding(
        doc::NodeId{50U}, bound_output(50U, 1U, 4U, 1U)));
    start = start.set(viewport(70U, 4, 0));
    cmd::DirectorScheduler scheduler{start};

    // Another machine's cable is never removed, whatever the request says.
    editor::HardwareLinkRequest foreign{
        topology::HardwareEndpoint{doc::NodeId{10U}, 2U},
        topology::HardwareEndpoint{doc::NodeId{31U}, 0U}, 90U};
    foreign.replaces = doc::NodeId{41U};
    AUTHOR_CHECK(app::schedule_hardware_link_replace(scheduler, foreign,
                                                     cmd::FrameNumber{1U}));
    AUTHOR_CHECK(hardware_of(scheduler.projected_document())
                     .link(doc::NodeId{41U}) != nullptr);
    AUTHOR_CHECK(scheduler.undo_depth(hardware_context) == 0U);

    // OUT 2 takes the cable and feed 4, as the editor sends it.
    editor::HardwareLinkRequest moved{
        topology::HardwareEndpoint{doc::NodeId{10U}, 2U},
        topology::HardwareEndpoint{doc::NodeId{30U}, 0U}, 91U};
    moved.replaces = doc::NodeId{40U};
    moved.output_binding = bound_output(0U, 2U, 4U, 2U);
    moved.output_region = scene::CanvasRect{0U, 0U, 1920U, 1080U};
    const cmd::FrameNumber frame{2U};
    AUTHOR_CHECK(app::schedule_hardware_link_replace(scheduler, moved, frame));
    AUTHOR_CHECK(app::schedule_hardware_output_binding(
        scheduler, editor::HardwareOutputBindingEdit{*moved.output_binding, 91U},
        doc::NodeId{51U}, frame));
    AUTHOR_CHECK(app::schedule_hardware_link(scheduler, moved,
                                             doc::NodeId{52U}, frame));

    const topology::HardwareMap map =
        hardware_of(scheduler.projected_document());
    AUTHOR_CHECK(map.link(doc::NodeId{40U}) == nullptr);
    AUTHOR_CHECK(map.output_binding(doc::NodeId{50U}) == nullptr);
    const topology::HardwareLink *const cable = map.link(doc::NodeId{52U});
    AUTHOR_CHECK(cable != nullptr && cable->from.port == 2U &&
                 cable->to.device == doc::NodeId{30U});
    const topology::HardwareOutputBinding *const binding =
        map.output_binding_for(doc::NodeId{10U}, 2U);
    AUTHOR_CHECK(binding != nullptr && binding->feed_index == 4U);
    AUTHOR_CHECK(scheduler.projected_document().contains(doc::NodeId{70U}));

    // One gesture: one undo puts OUT 1's cable and binding back.
    AUTHOR_CHECK(scheduler.undo_depth(hardware_context) == 1U);
    AUTHOR_CHECK(scheduler.schedule_undo(hardware_context,
                                         cmd::TransactionId{92U},
                                         cmd::FrameNumber{3U}));
    const topology::HardwareMap restored =
        hardware_of(scheduler.projected_document());
    AUTHOR_CHECK(restored.link(doc::NodeId{40U}) != nullptr);
    AUTHOR_CHECK(restored.link(doc::NodeId{52U}) == nullptr);
    AUTHOR_CHECK(restored.output_binding(doc::NodeId{50U}) != nullptr);
    AUTHOR_CHECK(restored.output_binding_for(doc::NodeId{10U}, 2U) == nullptr);
}

// Render 3's OUT 1 is a Mosaic surface feeding two processors. Each cable
// takes a region by hand and gives it back; and pulling one cable leaves the
// surface bound, with its region, for the cable that is left.
void a_shared_output_keeps_its_binding_until_its_last_cable() {
    doc::Root start = cluster();
    start = start.set(processor(32U));
    start = start.set(link(42U, 10U, 32U));
    start = start.set(app::make_hardware_output_binding(
        doc::NodeId{50U}, bound_output(50U, 1U, 4U, 1U)));
    start = start.set(viewport(70U, 4, 0));
    cmd::DirectorScheduler scheduler{start};

    const topology::HardwareOutputRegion half{1920U, 0U, 1920U, 2160U};
    AUTHOR_CHECK(app::schedule_hardware_link_region(
        scheduler, editor::HardwareLinkRegionEdit{doc::NodeId{42U}, half, 80U},
        cmd::FrameNumber{1U}));
    AUTHOR_CHECK(hardware_of(scheduler.projected_document())
                     .link(doc::NodeId{42U})
                     ->region == half);
    // Not a Render Server's cable, or not inside an output: no edit.
    AUTHOR_CHECK(app::schedule_hardware_link_region(
        scheduler,
        editor::HardwareLinkRegionEdit{
            doc::NodeId{42U},
            topology::HardwareOutputRegion{
                topology::max_hardware_output_width, 0U, 1U, 1U},
            81U},
        cmd::FrameNumber{1U}));
    AUTHOR_CHECK(scheduler.undo_depth(hardware_context) == 1U);
    AUTHOR_CHECK(app::schedule_hardware_link_region(
        scheduler, editor::HardwareLinkRegionEdit{doc::NodeId{42U}, {}, 82U},
        cmd::FrameNumber{1U}));
    AUTHOR_CHECK(hardware_of(scheduler.projected_document())
                     .link(doc::NodeId{42U})
                     ->region.empty());

    AUTHOR_CHECK(app::schedule_hardware_delete(
        scheduler,
        editor::HardwareDeleteRequest{
            editor::HardwareSelection{editor::HardwareSelectionKind::Link,
                                      doc::NodeId{40U}},
            83U},
        cmd::FrameNumber{2U}));
    topology::HardwareMap map = hardware_of(scheduler.projected_document());
    AUTHOR_CHECK(map.link(doc::NodeId{40U}) == nullptr);
    AUTHOR_CHECK(map.output_binding(doc::NodeId{50U}) != nullptr);
    AUTHOR_CHECK(scheduler.projected_document().contains(doc::NodeId{70U}));

    // The last cable takes the binding and its region with it.
    AUTHOR_CHECK(app::schedule_hardware_delete(
        scheduler,
        editor::HardwareDeleteRequest{
            editor::HardwareSelection{editor::HardwareSelectionKind::Link,
                                      doc::NodeId{42U}},
            84U},
        cmd::FrameNumber{3U}));
    map = hardware_of(scheduler.projected_document());
    AUTHOR_CHECK(map.output_binding(doc::NodeId{50U}) == nullptr);
    AUTHOR_CHECK(!scheduler.projected_document().contains(doc::NodeId{70U}));
}

struct Case final {
    const char *name;
    void (*function)();
};

constexpr std::array<Case, 11U> cases{{
    {"a typed rate is stored as an exact fraction",
     &a_typed_rate_is_stored_as_an_exact_fraction},
    {"a processor's reported walls are placed cabled in one undo",
     &a_processors_reported_walls_are_placed_cabled_in_one_undo},
    {"a shared output keeps its binding until its last cable",
     &a_shared_output_keeps_its_binding_until_its_last_cable},
    {"moving a cable to another output keeps its region in one undo",
     &moving_a_cable_to_another_output_keeps_its_region_in_one_undo},
    {"a stage edit creates then updates one node in one gesture",
     &a_stage_edit_creates_then_updates_one_node_in_one_gesture},
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
