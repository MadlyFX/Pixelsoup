#include <mediaserver/editor/mapping_panels.hpp>

#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/topology/hardware_map_schema.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;
namespace output = mediaserver::output;
namespace scene = mediaserver::scene;
namespace sync = mediaserver::sync;
namespace topology = mediaserver::topology;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define MAPPING_CHECK(...)                                                     \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class ProjectionMeshFixture final {
public:
  ProjectionMeshFixture() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    directory_ =
        std::filesystem::temp_directory_path() /
        (std::string{"mediaserver-editor-uv-preview-"} + std::to_string(stamp));
    std::filesystem::create_directories(directory_);
  }

  ProjectionMeshFixture(const ProjectionMeshFixture &) = delete;
  ProjectionMeshFixture &operator=(const ProjectionMeshFixture &) = delete;

  ~ProjectionMeshFixture() {
    std::error_code ignored;
    static_cast<void>(std::filesystem::remove_all(directory_, ignored));
  }

  [[nodiscard]] std::string write(std::string_view name,
                                  std::string_view contents) const {
    const std::filesystem::path path = directory_ / name;
    std::ofstream stream{path, std::ios::binary | std::ios::trunc};
    stream.write(contents.data(),
                 static_cast<std::streamsize>(contents.size()));
    stream.close();
    MAPPING_CHECK(stream.good());
    return path.string();
  }

private:
  std::filesystem::path directory_;
};

[[nodiscard]] topology::HardwareDevice device(core::u64 id,
                                              topology::HardwareDeviceKind kind,
                                              std::string name, core::i32 x,
                                              core::i32 y) {
  topology::HardwareDevice built{};
  built.node = doc::NodeId{id};
  built.kind = kind;
  built.name = std::move(name);
  built.grid_x = x;
  built.grid_y = y;
  return built;
}

[[nodiscard]] topology::HardwareMap hardware_demo(bool with_link = false) {
  topology::HardwareMap map{};
  map.devices = {
      device(1U, topology::HardwareDeviceKind::RenderServer, "Render A", -5,
             -2),
      device(2U, topology::HardwareDeviceKind::LedProcessor, "Processor A", 5,
             2),
      device(3U, topology::HardwareDeviceKind::LedWall, "Main Wall", 7, 3),
  };
  if (with_link) {
    map.links.push_back(topology::HardwareLink{
        doc::NodeId{10U}, topology::HardwareEndpoint{doc::NodeId{1U}, 1U},
        topology::HardwareEndpoint{doc::NodeId{2U}, 0U}});
  }
  return map;
}

[[nodiscard]] topology::HardwareRuntimeSnapshot
render_runtime(core::u64 device_id, std::size_t output_count,
               topology::HardwareRuntimeInventoryState inventory =
                   topology::HardwareRuntimeInventoryState::Available,
               topology::HardwareRuntimeReachability reachability =
                   topology::HardwareRuntimeReachability::Online,
               topology::HardwareRuntimeProcessState process =
                   topology::HardwareRuntimeProcessState::Running,
               bool stale = false) {
  topology::HardwareRuntimeNode node{};
  node.device = doc::NodeId{device_id};
  node.reachability = reachability;
  node.process = process;
  node.inventory = inventory;
  node.hostname = "render-a";
  node.operating_system = "Windows";
  node.logical_processor_count = 32U;
  node.total_memory_bytes = 64ULL * 1024ULL * 1024ULL * 1024ULL;
  node.gpu_adapters.push_back(topology::HardwareGpuAdapter{
      "Test GPU", 24ULL * 1024ULL * 1024ULL * 1024ULL, false, false});
  node.video_outputs.reserve(output_count);
  for (std::size_t index = 0U; index < output_count; ++index) {
    node.video_outputs.push_back(
        topology::HardwareVideoOutput{
            topology::HardwareDisplayTargetKey{
                0x1122334455667788ULL,
                static_cast<core::u32>(index + 1U),
                static_cast<core::u32>(index + 10U)},
            "Display " + std::to_string(index + 1U),
            "DP-" + std::to_string(index + 1U), 3840U, 2160U, 60'000U,
            1'001U, true, true});
  }
  node.stale = stale;
  topology::HardwareRuntimeSnapshot snapshot{};
  snapshot.nodes.push_back(std::move(node));
  return snapshot;
}

struct HardwareHarness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::HardwareMappingPanel panel{gui::ui_id("hardware.mapping.test")};
  static constexpr gui::Rect bounds{0.0F, 0.0F, 900.0F, 500.0F};

  editor::HardwareMappingFrame frame(float x, float y, bool down,
                                     const topology::HardwareMap &map,
                                     std::optional<gui::Key> key = std::nullopt,
                                     bool middle_down = false,
                                     float wheel = 0.0F) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    input.set_mouse_button(gui::MouseButton::Middle, middle_down);
    if (wheel != 0.0F) {
      input.add_wheel(wheel);
    }
    if (key.has_value()) {
      input.add_key(*key);
    }
    ctx.new_frame();
    return panel.draw(ctx, bounds, map);
  }

  editor::HardwareMappingFrame
  runtime_frame(float x, float y, bool down, const topology::HardwareMap &map,
                const topology::HardwareRuntimeSnapshot &runtime,
                std::optional<gui::Key> key = std::nullopt,
                bool middle_down = false, float wheel = 0.0F) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    input.set_mouse_button(gui::MouseButton::Middle, middle_down);
    if (wheel != 0.0F) {
      input.add_wheel(wheel);
    }
    if (key.has_value()) {
      input.add_key(*key);
    }
    ctx.new_frame();
    return panel.draw(ctx, bounds, map, runtime);
  }

  editor::HardwareMappingFrame sync_frame(float x, float y, bool down,
                                          const topology::HardwareMap &map,
                                          const sync::SyncPolicy &policy) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    ctx.new_frame();
    return panel.draw(ctx, bounds, map,
                      topology::HardwareRuntimeSnapshot{}, policy);
  }

  editor::HardwareMappingFrame canvas_frame(float x, float y, bool down,
                                            scene::Canvas canvas) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    ctx.new_frame();
    return panel.draw(ctx, bounds, topology::HardwareMap{},
                      topology::HardwareRuntimeSnapshot{}, sync::SyncPolicy{},
                      canvas);
  }
};

// The synchronization policy is a project setting authored beside the control
// port, and cycling MODE must produce one complete, valid policy value rather
// than a partially edited one.
void the_sync_policy_mode_cycles_through_every_state() {
  HardwareHarness harness;
  const topology::HardwareMap map;
  const gui::Rect button =
      editor::hardware_sync_mode_button_bounds(HardwareHarness::bounds);
  const float x = button.x + button.width * 0.5F;
  const float y = button.y + button.height * 0.5F;

  sync::SyncPolicy policy{};
  MAPPING_CHECK(!policy.requires_physical_sync());

  static_cast<void>(harness.sync_frame(x, y, true, map, policy));
  auto released = harness.sync_frame(x, y, false, map, policy);
  MAPPING_CHECK(released.sync_policy_edit.has_value());
  MAPPING_CHECK(released.sync_policy_edit->policy == doc::NodeId{});
  MAPPING_CHECK(released.sync_policy_edit->value.mode ==
                sync::SyncMode::Framelock);
  MAPPING_CHECK(released.sync_policy_edit->value.authored);
  MAPPING_CHECK(sync::valid_policy(released.sync_policy_edit->value));

  policy = released.sync_policy_edit->value;
  policy.node = doc::NodeId{77U};
  static_cast<void>(harness.sync_frame(x, y, true, map, policy));
  released = harness.sync_frame(x, y, false, map, policy);
  MAPPING_CHECK(released.sync_policy_edit.has_value());
  MAPPING_CHECK(released.sync_policy_edit->policy == doc::NodeId{77U});
  MAPPING_CHECK(released.sync_policy_edit->value.mode ==
                sync::SyncMode::ExternalGenlock);
  // Cycling only ever changes the mode; timing, group, and both failure
  // policies survive untouched.
  MAPPING_CHECK(released.sync_policy_edit->value.required_timing ==
                policy.required_timing);
  MAPPING_CHECK(released.sync_policy_edit->value.group == policy.group);
  MAPPING_CHECK(released.sync_policy_edit->value.barrier == policy.barrier);
  MAPPING_CHECK(released.sync_policy_edit->value.failure == policy.failure);
  MAPPING_CHECK(released.sync_policy_edit->value.late_frame ==
                policy.late_frame);

  policy = released.sync_policy_edit->value;
  policy.node = doc::NodeId{77U};
  static_cast<void>(harness.sync_frame(x, y, true, map, policy));
  released = harness.sync_frame(x, y, false, map, policy);
  MAPPING_CHECK(released.sync_policy_edit.has_value());
  MAPPING_CHECK(released.sync_policy_edit->value.mode ==
                sync::SyncMode::Unsynced);
}

// Drawing the panel must never author anything on its own: an operator who
// only looks at Hardware Map has made no claim about physical timing.
void drawing_the_sync_section_authors_nothing_without_a_click() {
  HardwareHarness harness;
  const topology::HardwareMap map;
  sync::SyncPolicy policy{};
  policy.authored = true;
  policy.mode = sync::SyncMode::ExternalGenlock;
  const gui::Rect button =
      editor::hardware_sync_mode_button_bounds(HardwareHarness::bounds);
  for (int index = 0; index < 3; ++index) {
    const auto frame = harness.sync_frame(button.x + button.width + 40.0F,
                                          button.y, false, map, policy);
    MAPPING_CHECK(!frame.sync_policy_edit.has_value());
  }
}

void isometric_projection_round_trips_grid_cells() {
  const editor::IsometricProjection projection{431.0F, 117.0F, 68.0F, 34.0F};
  constexpr std::array<std::pair<core::i32, core::i32>, 6U> cells{{
      {0, 0},
      {1, 0},
      {-3, 7},
      {12, -4},
      {-9, -8},
      {25, 13},
  }};
  for (const auto [x, y] : cells) {
    const editor::IsoPoint point = editor::project_isometric(projection, x, y);
    MAPPING_CHECK(editor::unproject_isometric(projection, point.x, point.y) ==
                  std::pair<core::i32, core::i32>{x, y});
  }
}

void dragging_a_palette_device_requests_a_grid_placement() {
  HardwareHarness harness;
  const topology::HardwareMap empty;
  const gui::Rect item = editor::hardware_palette_item_bounds(
      HardwareHarness::bounds, topology::HardwareDeviceKind::LedProcessor);
  const editor::IsometricProjection projection =
      editor::hardware_mapping_projection(HardwareHarness::bounds);
  const editor::IsoPoint drop = editor::project_isometric(projection, 3, -2);

  static_cast<void>(
      harness.frame(item.x + 20.0F, item.y + 20.0F, false, empty));
  const auto pressed =
      harness.frame(item.x + 20.0F, item.y + 20.0F, true, empty);
  MAPPING_CHECK(pressed.dragging);
  static_cast<void>(harness.frame(drop.x, drop.y, true, empty));
  const auto released = harness.frame(drop.x, drop.y, false, empty);

  MAPPING_CHECK(released.place.has_value());
  MAPPING_CHECK(released.place->kind ==
                topology::HardwareDeviceKind::LedProcessor);
  MAPPING_CHECK(released.place->grid_x == 3);
  MAPPING_CHECK(released.place->grid_y == -2);
  MAPPING_CHECK(!released.dragging);
}

void dragging_a_device_requests_one_document_move() {
  HardwareHarness harness;
  topology::HardwareMap map{};
  map.devices.push_back(device(
      4U, topology::HardwareDeviceKind::OperatorMachine, "Operator", 0, 0));
  const editor::IsometricProjection projection =
      editor::hardware_mapping_projection(HardwareHarness::bounds);
  const editor::IsoPoint start = editor::project_isometric(projection, 0, 0);
  const editor::IsoPoint destination =
      editor::project_isometric(projection, 4, 1);

  static_cast<void>(harness.frame(start.x, start.y, false, map));
  const auto pressed = harness.frame(start.x, start.y, true, map);
  MAPPING_CHECK(pressed.selection ==
                editor::HardwareSelection{editor::HardwareSelectionKind::Device,
                                          doc::NodeId{4U}});
  static_cast<void>(harness.frame(destination.x, destination.y, true, map));
  const auto released = harness.frame(destination.x, destination.y, false, map);

  MAPPING_CHECK(released.move ==
                editor::HardwareMoveRequest{doc::NodeId{4U}, 4, 1});
  MAPPING_CHECK(!released.dragging);
}

void compatible_ports_create_a_link_request() {
  HardwareHarness harness;
  const topology::HardwareMap map = hardware_demo();
  const editor::IsometricProjection projection =
      editor::hardware_mapping_projection(HardwareHarness::bounds);
  const editor::IsoPoint output_port =
      editor::hardware_port_position(projection, map.devices[0U], 1U);
  const editor::IsoPoint input_port =
      editor::hardware_port_position(projection, map.devices[1U], 0U);

  static_cast<void>(harness.frame(output_port.x, output_port.y, false, map));
  static_cast<void>(harness.frame(output_port.x, output_port.y, true, map));
  static_cast<void>(harness.frame(output_port.x, output_port.y, false, map));
  MAPPING_CHECK(harness.panel.pending_link() ==
                topology::HardwareEndpoint{doc::NodeId{1U}, 1U});

  static_cast<void>(harness.frame(input_port.x, input_port.y, false, map));
  static_cast<void>(harness.frame(input_port.x, input_port.y, true, map));
  const auto released = harness.frame(input_port.x, input_port.y, false, map);

  MAPPING_CHECK(released.link ==
                editor::HardwareLinkRequest{
                    topology::HardwareEndpoint{doc::NodeId{1U}, 1U},
                    topology::HardwareEndpoint{doc::NodeId{2U}, 0U}});
  MAPPING_CHECK(!released.incompatible_link_attempted);
  MAPPING_CHECK(!harness.panel.pending_link().has_value());
}

void output_ports_support_press_drag_release_cabling() {
  HardwareHarness harness;
  const topology::HardwareMap map = hardware_demo();
  const editor::IsometricProjection projection =
      harness.panel.view_projection(HardwareHarness::bounds);
  const editor::IsoPoint output_port =
      editor::hardware_port_position(projection, map.devices[0U], 1U);
  const editor::IsoPoint input_port =
      editor::hardware_port_position(projection, map.devices[1U], 0U);

  static_cast<void>(harness.frame(output_port.x, output_port.y, false, map));
  const auto pressed = harness.frame(output_port.x, output_port.y, true, map);
  MAPPING_CHECK(pressed.dragging);
  MAPPING_CHECK(pressed.link_feedback ==
                editor::HardwareLinkFeedback::Dragging);
  const auto over_target = harness.frame(input_port.x, input_port.y, true, map);
  MAPPING_CHECK(over_target.dragging);
  MAPPING_CHECK(over_target.link_feedback ==
                editor::HardwareLinkFeedback::Compatible);
  const auto released = harness.frame(input_port.x, input_port.y, false, map);

  MAPPING_CHECK(released.link ==
                editor::HardwareLinkRequest{
                    topology::HardwareEndpoint{doc::NodeId{1U}, 1U},
                    topology::HardwareEndpoint{doc::NodeId{2U}, 0U}});
  MAPPING_CHECK(released.link_feedback ==
                editor::HardwareLinkFeedback::Compatible);
  MAPPING_CHECK(!released.dragging);
  MAPPING_CHECK(!harness.panel.pending_link().has_value());
}

void incompatible_ports_do_not_create_a_link() {
  HardwareHarness harness;
  topology::HardwareMap map{};
  map.devices = {
      device(1U, topology::HardwareDeviceKind::RenderServer, "Render A", -4, 0),
      device(2U, topology::HardwareDeviceKind::RenderServer, "Render B", 4, 0),
  };
  const editor::IsometricProjection projection =
      editor::hardware_mapping_projection(HardwareHarness::bounds);
  const editor::IsoPoint video_out =
      editor::hardware_port_position(projection, map.devices[0U], 1U);
  const editor::IsoPoint network_in =
      editor::hardware_port_position(projection, map.devices[1U], 0U);

  static_cast<void>(harness.frame(video_out.x, video_out.y, false, map));
  static_cast<void>(harness.frame(video_out.x, video_out.y, true, map));
  static_cast<void>(harness.frame(video_out.x, video_out.y, false, map));
  static_cast<void>(harness.frame(network_in.x, network_in.y, false, map));
  static_cast<void>(harness.frame(network_in.x, network_in.y, true, map));
  const auto released = harness.frame(network_in.x, network_in.y, false, map);

  MAPPING_CHECK(!released.link.has_value());
  MAPPING_CHECK(released.incompatible_link_attempted);
}

void cable_drag_distinguishes_incompatible_and_occupied_inputs() {
  HardwareHarness incompatible_harness;
  topology::HardwareMap incompatible{};
  incompatible.devices = {
      device(1U, topology::HardwareDeviceKind::RenderServer, "Render A", -4, 0),
      device(2U, topology::HardwareDeviceKind::RenderServer, "Render B", 4, 0),
  };
  const editor::IsometricProjection incompatible_projection =
      incompatible_harness.panel.view_projection(HardwareHarness::bounds);
  const editor::IsoPoint video_out = editor::hardware_port_position(
      incompatible_projection, incompatible.devices[0U], 1U);
  const editor::IsoPoint network_in = editor::hardware_port_position(
      incompatible_projection, incompatible.devices[1U], 0U);
  static_cast<void>(incompatible_harness.frame(video_out.x, video_out.y, false,
                                               incompatible));
  static_cast<void>(
      incompatible_harness.frame(video_out.x, video_out.y, true, incompatible));
  const auto mismatch = incompatible_harness.frame(network_in.x, network_in.y,
                                                   true, incompatible);
  MAPPING_CHECK(mismatch.link_feedback ==
                editor::HardwareLinkFeedback::Incompatible);
  const auto mismatch_drop = incompatible_harness.frame(
      network_in.x, network_in.y, false, incompatible);
  MAPPING_CHECK(!mismatch_drop.link.has_value());
  MAPPING_CHECK(mismatch_drop.incompatible_link_attempted);
  MAPPING_CHECK(mismatch_drop.link_feedback ==
                editor::HardwareLinkFeedback::Incompatible);

  HardwareHarness occupied_harness;
  topology::HardwareMap occupied = hardware_demo(true);
  occupied.devices.push_back(device(
      4U, topology::HardwareDeviceKind::RenderServer, "Render B", -2, 5));
  const editor::IsometricProjection occupied_projection =
      occupied_harness.panel.view_projection(HardwareHarness::bounds);
  const editor::IsoPoint second_video_out = editor::hardware_port_position(
      occupied_projection, occupied.devices[3U], 1U);
  const editor::IsoPoint used_input = editor::hardware_port_position(
      occupied_projection, occupied.devices[1U], 0U);
  static_cast<void>(occupied_harness.frame(
      second_video_out.x, second_video_out.y, false, occupied));
  static_cast<void>(occupied_harness.frame(second_video_out.x,
                                           second_video_out.y, true, occupied));
  const auto used =
      occupied_harness.frame(used_input.x, used_input.y, true, occupied);
  MAPPING_CHECK(used.link_feedback == editor::HardwareLinkFeedback::Occupied);
  const auto used_drop =
      occupied_harness.frame(used_input.x, used_input.y, false, occupied);
  MAPPING_CHECK(!used_drop.link.has_value());
  MAPPING_CHECK(used_drop.incompatible_link_attempted);
  MAPPING_CHECK(used_drop.link_feedback ==
                editor::HardwareLinkFeedback::Occupied);
}

void hardware_view_pans_and_zooms_without_changing_authored_grid_space() {
  HardwareHarness harness;
  const topology::HardwareMap empty;
  const gui::Rect canvas =
      editor::hardware_mapping_canvas(HardwareHarness::bounds);
  const float start_x = canvas.x + canvas.width * 0.52F;
  const float start_y = canvas.y + canvas.height * 0.55F;
  const editor::IsometricProjection initial =
      harness.panel.view_projection(HardwareHarness::bounds);

  static_cast<void>(
      harness.frame(start_x, start_y, false, empty, std::nullopt, true));
  const auto panned_frame = harness.frame(start_x + 37.0F, start_y - 19.0F,
                                          false, empty, std::nullopt, true);
  MAPPING_CHECK(panned_frame.navigating);
  const editor::IsometricProjection panned =
      harness.panel.view_projection(HardwareHarness::bounds);
  MAPPING_CHECK(std::abs(panned.origin_x - initial.origin_x - 37.0F) < 0.01F);
  MAPPING_CHECK(std::abs(panned.origin_y - initial.origin_y + 19.0F) < 0.01F);
  static_cast<void>(harness.frame(start_x + 37.0F, start_y - 19.0F, false,
                                  empty, std::nullopt, false));

  constexpr core::i32 authored_x = 3;
  constexpr core::i32 authored_y = -2;
  const editor::IsoPoint cursor =
      editor::project_isometric(panned, authored_x, authored_y);
  const auto zoomed_frame = harness.frame(cursor.x, cursor.y, false, empty,
                                          std::nullopt, false, 1.0F);
  MAPPING_CHECK(zoomed_frame.navigating);
  const editor::IsometricProjection zoomed =
      harness.panel.view_projection(HardwareHarness::bounds);
  MAPPING_CHECK(zoomed.tile_width > panned.tile_width);
  const editor::IsoPoint anchored =
      editor::project_isometric(zoomed, authored_x, authored_y);
  MAPPING_CHECK(std::abs(anchored.x - cursor.x) < 0.01F);
  MAPPING_CHECK(std::abs(anchored.y - cursor.y) < 0.01F);
  MAPPING_CHECK(editor::unproject_isometric(zoomed, anchored.x, anchored.y) ==
                std::pair<core::i32, core::i32>{authored_x, authored_y});

  const gui::Rect palette_item = editor::hardware_palette_item_bounds(
      HardwareHarness::bounds, topology::HardwareDeviceKind::LedWall);
  const float palette_x = palette_item.x + palette_item.width * 0.5F;
  const float palette_y = palette_item.y + palette_item.height * 0.5F;
  static_cast<void>(harness.frame(palette_x, palette_y, false, empty));
  static_cast<void>(harness.frame(palette_x, palette_y, true, empty));
  static_cast<void>(harness.frame(anchored.x, anchored.y, true, empty));
  const auto placed = harness.frame(anchored.x, anchored.y, false, empty);
  MAPPING_CHECK(placed.place == editor::HardwarePlaceRequest{
                                    topology::HardwareDeviceKind::LedWall,
                                    authored_x, authored_y});
}

void an_authored_cable_can_be_selected() {
  HardwareHarness harness;
  const topology::HardwareMap map = hardware_demo(true);
  const editor::IsometricProjection projection =
      editor::hardware_mapping_projection(HardwareHarness::bounds);
  const editor::IsoPoint from =
      editor::hardware_port_position(projection, map.devices[0U], 1U);
  const editor::IsoPoint to =
      editor::hardware_port_position(projection, map.devices[1U], 0U);
  const float x = (from.x + to.x) * 0.5F;
  const float y = (from.y + to.y) * 0.5F;

  static_cast<void>(harness.frame(x, y, false, map));
  static_cast<void>(harness.frame(x, y, true, map));
  const auto released = harness.frame(x, y, false, map);
  MAPPING_CHECK(released.selection ==
                editor::HardwareSelection{editor::HardwareSelectionKind::Link,
                                          doc::NodeId{10U}});
}

void selected_hardware_objects_return_safe_delete_requests() {
  topology::HardwareMap device_map{};
  device_map.devices.push_back(device(
      4U, topology::HardwareDeviceKind::OperatorMachine, "Operator", 0, 0));
  HardwareHarness device_harness;
  const editor::IsometricProjection device_projection =
      editor::hardware_mapping_projection(HardwareHarness::bounds);
  const editor::IsoPoint card =
      editor::project_isometric(device_projection, 0, 0);

  static_cast<void>(device_harness.frame(card.x, card.y, false, device_map));
  const auto selected = device_harness.frame(card.x, card.y, true, device_map);
  MAPPING_CHECK(selected.selection ==
                editor::HardwareSelection{editor::HardwareSelectionKind::Device,
                                          doc::NodeId{4U}});
  static_cast<void>(device_harness.frame(card.x, card.y, false, device_map));

  // Re-clicking the already-selected card is still surfaced so the shell can
  // restore Hardware as the active Delete domain.
  const auto reselected =
      device_harness.frame(card.x, card.y, true, device_map);
  MAPPING_CHECK(reselected.selection ==
                editor::HardwareSelection{editor::HardwareSelectionKind::Device,
                                          doc::NodeId{4U}});
  static_cast<void>(device_harness.frame(card.x, card.y, false, device_map));

  const auto key_delete =
      device_harness.frame(card.x, card.y, false, device_map, gui::Key::Delete);
  MAPPING_CHECK(key_delete.remove ==
                editor::HardwareDeleteRequest{editor::HardwareSelection{
                    editor::HardwareSelectionKind::Device, doc::NodeId{4U}}});
  MAPPING_CHECK(key_delete.consume_delete);
  const auto idle = device_harness.frame(card.x, card.y, false, device_map);
  MAPPING_CHECK(!idle.remove.has_value());
  MAPPING_CHECK(!idle.consume_delete);

  // The same request is available from the honest visible header action.
  constexpr float remove_x = 830.0F;
  constexpr float remove_y = 13.0F;
  static_cast<void>(
      device_harness.frame(remove_x, remove_y, false, device_map));
  static_cast<void>(device_harness.frame(remove_x, remove_y, true, device_map));
  const auto button_delete =
      device_harness.frame(remove_x, remove_y, false, device_map);
  MAPPING_CHECK(button_delete.remove ==
                editor::HardwareDeleteRequest{editor::HardwareSelection{
                    editor::HardwareSelectionKind::Device, doc::NodeId{4U}}});

  HardwareHarness cable_harness;
  const topology::HardwareMap cable_map = hardware_demo(true);
  const editor::IsometricProjection cable_projection =
      editor::hardware_mapping_projection(HardwareHarness::bounds);
  const editor::IsoPoint from = editor::hardware_port_position(
      cable_projection, cable_map.devices[0U], 1U);
  const editor::IsoPoint to = editor::hardware_port_position(
      cable_projection, cable_map.devices[1U], 0U);
  const float cable_x = (from.x + to.x) * 0.5F;
  const float cable_y = (from.y + to.y) * 0.5F;
  static_cast<void>(cable_harness.frame(cable_x, cable_y, false, cable_map));
  static_cast<void>(cable_harness.frame(cable_x, cable_y, true, cable_map));
  const auto cable_selected =
      cable_harness.frame(cable_x, cable_y, false, cable_map);
  MAPPING_CHECK(cable_selected.selection ==
                editor::HardwareSelection{editor::HardwareSelectionKind::Link,
                                          doc::NodeId{10U}});
  const auto cable_delete =
      cable_harness.frame(cable_x, cable_y, false, cable_map, gui::Key::Delete);
  MAPPING_CHECK(cable_delete.remove ==
                editor::HardwareDeleteRequest{editor::HardwareSelection{
                    editor::HardwareSelectionKind::Link, doc::NodeId{10U}}});
  MAPPING_CHECK(cable_delete.consume_delete);
}

void selected_device_details_author_exact_configuration_properties() {
  HardwareHarness harness;
  topology::HardwareMap map{};
  map.devices.push_back(device(
      4U, topology::HardwareDeviceKind::OperatorMachine, "Operator", 0, 0));
  const editor::IsometricProjection projection =
      editor::hardware_mapping_projection(HardwareHarness::bounds);
  const editor::IsoPoint card = editor::project_isometric(projection, 0, 0);
  static_cast<void>(harness.frame(card.x, card.y, false, map));
  static_cast<void>(harness.frame(card.x, card.y, true, map));
  static_cast<void>(harness.frame(card.x, card.y, false, map));

  // The details rail is right-aligned. Width is the first numeric row.
  constexpr float width_field_x = 800.0F;
  constexpr float width_field_y = 127.0F;
  static_cast<void>(harness.frame(width_field_x, width_field_y, false, map));
  static_cast<void>(harness.frame(width_field_x, width_field_y, true, map));
  const auto changed =
      harness.frame(width_field_x + 20.0F, width_field_y, true, map);
  MAPPING_CHECK(changed.property_edit.has_value());
  MAPPING_CHECK(changed.property_edit->device == doc::NodeId{4U});
  MAPPING_CHECK(changed.property_edit->property == topology::schema::width);
  const auto *const value =
      std::get_if<std::int64_t>(&changed.property_edit->value);
  MAPPING_CHECK(value != nullptr);
  MAPPING_CHECK(*value == 1940);
  MAPPING_CHECK(changed.editing);

  // Non-render hardware keeps the broader raster authoring range.
  const auto broad =
      harness.frame(width_field_x + 40'000.0F, width_field_y, true, map);
  MAPPING_CHECK(broad.property_edit.has_value());
  const auto *const broad_value =
      std::get_if<std::int64_t>(&broad.property_edit->value);
  MAPPING_CHECK(broad_value != nullptr);
  MAPPING_CHECK(*broad_value == 32'768);
}

void project_setup_authors_one_control_port_range() {
  constexpr float control_port_x = 132.0F;
  constexpr float control_port_y = 284.0F;

  HardwareHarness new_project;
  topology::HardwareMap defaults{};
  static_cast<void>(
      new_project.frame(control_port_x, control_port_y, false, defaults));
  static_cast<void>(
      new_project.frame(control_port_x, control_port_y, true, defaults));
  const auto create =
      new_project.frame(control_port_x + 10.0F, control_port_y, true, defaults);
  MAPPING_CHECK(
      create.control_port_edit ==
      editor::HardwareControlPortEdit{
          doc::NodeId{}, topology::default_hardware_control_base_port + 10U});
  MAPPING_CHECK(create.editing);
  MAPPING_CHECK(!create.property_edit.has_value());
  static_cast<void>(new_project.frame(control_port_x + 10.0F, control_port_y,
                                      false, defaults));

  HardwareHarness existing_project;
  topology::HardwareMap authored{};
  authored.control =
      topology::HardwareControlSettings{doc::NodeId{88U}, 50'000U, true};
  static_cast<void>(
      existing_project.frame(control_port_x, control_port_y, false, authored));
  static_cast<void>(
      existing_project.frame(control_port_x, control_port_y, true, authored));
  const auto edit = existing_project.frame(control_port_x + 5.0F,
                                           control_port_y, true, authored);
  MAPPING_CHECK(edit.control_port_edit ==
                editor::HardwareControlPortEdit{doc::NodeId{88U}, 50'005U});
  MAPPING_CHECK(!edit.property_edit.has_value());
}

// The scene canvas is authored beside the control port: each axis scrubs on
// its own, the other axis is carried unchanged, and the output limit holds.
void project_setup_authors_the_scene_canvas() {
  const scene::Canvas canvas{1'920U, 1'080U};
  for (const bool height : {false, true}) {
    HardwareHarness harness;
    const gui::Rect field =
        editor::hardware_canvas_field_bounds(HardwareHarness::bounds, height);
    const float x = field.x + 20.0F;
    const float y = field.y + field.height * 0.5F;
    static_cast<void>(harness.canvas_frame(x, y, false, canvas));
    static_cast<void>(harness.canvas_frame(x, y, true, canvas));
    const auto edit = harness.canvas_frame(x + 12.0F, y, true, canvas);
    MAPPING_CHECK(edit.canvas_edit.has_value());
    MAPPING_CHECK(edit.canvas_edit->canvas ==
                  (height ? scene::Canvas{1'920U, 1'092U}
                          : scene::Canvas{1'932U, 1'080U}));
    MAPPING_CHECK(edit.editing);
    MAPPING_CHECK(!edit.control_port_edit.has_value());
    MAPPING_CHECK(!edit.sync_policy_edit.has_value());
    static_cast<void>(harness.canvas_frame(x + 12.0F, y, false, canvas));
  }

  // Scrubbing past the limit authors the limit, not the scrubbed value.
  HardwareHarness limit;
  const gui::Rect width_field =
      editor::hardware_canvas_field_bounds(HardwareHarness::bounds, false);
  const float x = width_field.x + 20.0F;
  const float y = width_field.y + width_field.height * 0.5F;
  const scene::Canvas wide{16'380U, 1'536U};
  static_cast<void>(limit.canvas_frame(x, y, false, wide));
  static_cast<void>(limit.canvas_frame(x, y, true, wide));
  const auto clamped = limit.canvas_frame(x + 40.0F, y, true, wide);
  MAPPING_CHECK(clamped.canvas_edit.has_value());
  MAPPING_CHECK(clamped.canvas_edit->canvas ==
                (scene::Canvas{16'384U, 1'536U}));

  // With no scene there is nothing to author; the same gesture edits nothing.
  HardwareHarness empty;
  static_cast<void>(empty.canvas_frame(x, y, false, scene::Canvas{}));
  static_cast<void>(empty.canvas_frame(x, y, true, scene::Canvas{}));
  const auto none = empty.canvas_frame(x + 12.0F, y, true, scene::Canvas{});
  MAPPING_CHECK(!none.canvas_edit.has_value());
}

void render_server_details_author_launch_geometry_and_not_control_targets() {
  HardwareHarness harness;
  topology::HardwareMap map{};
  topology::HardwareDevice render =
      device(4U, topology::HardwareDeviceKind::RenderServer, "Render", 0, 0);
  render.node_index = 3U;
  render.lifecycle_endpoint = "legacy.example:41000";
  render.width = topology::max_hardware_output_width + 1U;
  render.height = topology::max_hardware_output_height + 1U;
  map.devices.push_back(render);
  map.control =
      topology::HardwareControlSettings{doc::NodeId{90U}, 50'000U, true};
  MAPPING_CHECK(topology::lifecycle_endpoint_for(map, map.devices[0U]) ==
                std::optional<std::string>{"127.0.0.1:50003"});

  const editor::IsometricProjection projection =
      editor::hardware_mapping_projection(HardwareHarness::bounds);
  const editor::IsoPoint card = editor::project_isometric(projection, 0, 0);
  static_cast<void>(harness.frame(card.x, card.y, false, map));
  static_cast<void>(harness.frame(card.x, card.y, true, map));
  static_cast<void>(harness.frame(card.x, card.y, false, map));
  MAPPING_CHECK(editor::hardware_window_settings_source_text(
                    topology::HardwareWindowMode::Inherit) ==
                "USES LAUNCH SETTINGS");

  constexpr float field_x = 820.0F;
  // The derived target is presentation-only. The legacy endpoint is hidden
  // once project settings exist and clicking this row cannot focus/edit it.
  constexpr float control_y = 124.0F;
  static_cast<void>(harness.frame(field_x, control_y, false, map));
  static_cast<void>(harness.frame(field_x, control_y, true, map));
  const auto target_release = harness.frame(field_x, control_y, false, map);
  MAPPING_CHECK(!target_release.property_edit.has_value());
  MAPPING_CHECK(!target_release.control_port_edit.has_value());
  MAPPING_CHECK(!target_release.editing);

  // The compact mode control cycles from legacy Inherit to an explicitly
  // persisted launch mode.
  constexpr float window_mode_y = 220.0F;
  static_cast<void>(harness.frame(field_x, window_mode_y, false, map));
  static_cast<void>(harness.frame(field_x, window_mode_y, true, map));
  const auto windowed = harness.frame(field_x, window_mode_y, false, map);
  MAPPING_CHECK(windowed.property_edit.has_value());
  MAPPING_CHECK(windowed.property_edit->device == doc::NodeId{4U});
  MAPPING_CHECK(windowed.property_edit->property ==
                topology::schema::window_mode);
  const auto *const window_mode =
      std::get_if<std::string>(&windowed.property_edit->value);
  MAPPING_CHECK(window_mode != nullptr);
  MAPPING_CHECK(*window_mode == topology::schema::windowed_window_mode);

  // Simulate the shell applying that property. The legacy oversized device
  // remains selected and its fields remain available for repair.
  map.devices[0U].window_mode = topology::HardwareWindowMode::Windowed;
  static_cast<void>(harness.frame(field_x, window_mode_y, false, map));
  MAPPING_CHECK(harness.panel.selection() ==
                editor::HardwareSelection{
                    editor::HardwareSelectionKind::Device, doc::NodeId{4U}});
  MAPPING_CHECK(editor::hardware_window_settings_source_text(
                    topology::HardwareWindowMode::Windowed) ==
                "USES PROJECT SETTINGS");

  // Desktop coordinates are signed so a display left of the primary can be
  // authored without wrapping through an unsigned value.
  constexpr float desktop_x_y = 265.0F;
  static_cast<void>(harness.frame(field_x, desktop_x_y, false, map));
  static_cast<void>(harness.frame(field_x, desktop_x_y, true, map));
  const auto moved_left =
      harness.frame(field_x - 20.0F, desktop_x_y, true, map);
  MAPPING_CHECK(moved_left.property_edit.has_value());
  MAPPING_CHECK(moved_left.property_edit->property ==
                topology::schema::desktop_x);
  const auto *const desktop_x =
      std::get_if<std::int64_t>(&moved_left.property_edit->value);
  MAPPING_CHECK(desktop_x != nullptr);
  MAPPING_CHECK(*desktop_x == -20);

  static_cast<void>(harness.frame(field_x - 20.0F, desktop_x_y, false, map));

  // New values remain bounded to presenter limits, so scrubbing repairs the
  // retained legacy value instead of authoring another oversized value.
  constexpr float width_y = 318.0F;
  static_cast<void>(harness.frame(field_x, width_y, false, map));
  static_cast<void>(harness.frame(field_x, width_y, true, map));
  const auto width_clamped =
      harness.frame(field_x + 10'000.0F, width_y, true, map);
  MAPPING_CHECK(width_clamped.property_edit.has_value());
  MAPPING_CHECK(width_clamped.property_edit->property ==
                topology::schema::width);
  const auto *const width =
      std::get_if<std::int64_t>(&width_clamped.property_edit->value);
  MAPPING_CHECK(width != nullptr);
  MAPPING_CHECK(*width == topology::max_hardware_output_width);
  static_cast<void>(harness.frame(field_x + 10'000.0F, width_y, false, map));

  constexpr float height_y = 345.0F;
  static_cast<void>(harness.frame(field_x, height_y, false, map));
  static_cast<void>(harness.frame(field_x, height_y, true, map));
  const auto height_clamped =
      harness.frame(field_x + 10'000.0F, height_y, true, map);
  MAPPING_CHECK(height_clamped.property_edit.has_value());
  MAPPING_CHECK(height_clamped.property_edit->property ==
                topology::schema::height);
  const auto *const height =
      std::get_if<std::int64_t>(&height_clamped.property_edit->value);
  MAPPING_CHECK(height != nullptr);
  MAPPING_CHECK(*height == topology::max_hardware_output_height);
}

void output_deployment_is_created_by_wiring_and_remains_repairable() {
  HardwareHarness harness;
  topology::HardwareMap map = hardware_demo();
  map.devices[0U].node_index = 0U;
  const topology::HardwareRuntimeSnapshot two_outputs = render_runtime(1U, 2U);
  const editor::IsometricProjection projection =
      harness.panel.view_projection(HardwareHarness::bounds);
  const editor::IsoPoint video_out = editor::hardware_presented_port_position(
      projection, map, map.devices[0U], two_outputs, 1U);
  const editor::IsoPoint video_in = editor::hardware_presented_port_position(
      projection, map, map.devices[1U], two_outputs, 0U);
  static_cast<void>(harness.runtime_frame(video_out.x, video_out.y, false, map,
                                          two_outputs));
  static_cast<void>(harness.runtime_frame(video_out.x, video_out.y, true, map,
                                          two_outputs));
  static_cast<void>(harness.runtime_frame(video_in.x, video_in.y, true, map,
                                          two_outputs));
  const auto wired = harness.runtime_frame(video_in.x, video_in.y, false, map,
                                           two_outputs);
  MAPPING_CHECK(wired.link.has_value());
  MAPPING_CHECK(wired.link->output_binding.has_value());
  const topology::HardwareOutputBinding created =
      *wired.link->output_binding;
  MAPPING_CHECK(!wired.output_binding_edit.has_value());
  MAPPING_CHECK(created.node == doc::NodeId{});
  MAPPING_CHECK(created.render_server == doc::NodeId{1U});
  MAPPING_CHECK(created.logical_port == 1U);
  MAPPING_CHECK(created.feed_index == 0U);
  MAPPING_CHECK(created.target ==
                two_outputs.nodes[0U].video_outputs[0U].target);
  MAPPING_CHECK(created.width == 3840U && created.height == 2160U);
  MAPPING_CHECK(created.refresh_numerator == 60'000U);
  MAPPING_CHECK(created.refresh_denominator == 1'001U);
  MAPPING_CHECK(created.window_mode ==
                topology::HardwareWindowMode::Fullscreen);

  const gui::Rect card = editor::hardware_presented_device_bounds(
      projection, map, map.devices[0U], two_outputs);
  static_cast<void>(harness.runtime_frame(card.x + 20.0F, card.y + 18.0F,
                                          false, map, two_outputs));
  static_cast<void>(harness.runtime_frame(card.x + 20.0F, card.y + 18.0F,
                                          true, map, two_outputs));
  static_cast<void>(harness.runtime_frame(card.x + 20.0F, card.y + 18.0F,
                                          false, map, two_outputs));

  constexpr float details_x = 820.0F;
  topology::HardwareOutputBinding authored = created;
  authored.node = doc::NodeId{77U};
  map.output_bindings.push_back(authored);
  constexpr float binding_row_y = 197.0F;
  static_cast<void>(harness.runtime_frame(details_x, binding_row_y, false,
                                          map, two_outputs));
  static_cast<void>(harness.runtime_frame(details_x, binding_row_y, true,
                                          map, two_outputs));
  static_cast<void>(harness.runtime_frame(details_x, binding_row_y, false,
                                          map, two_outputs));

  // The selected binding owns an exact feed field instead of inferring the
  // feed from display order.
  constexpr float feed_y = 245.0F;
  static_cast<void>(
      harness.runtime_frame(details_x, feed_y, false, map, two_outputs));
  static_cast<void>(
      harness.runtime_frame(details_x, feed_y, true, map, two_outputs));
  const auto feed_changed = harness.runtime_frame(
      details_x + 7.0F, feed_y, true, map, two_outputs);
  MAPPING_CHECK(feed_changed.output_binding_edit.has_value());
  MAPPING_CHECK(feed_changed.output_binding_edit->binding.node ==
                doc::NodeId{77U});
  MAPPING_CHECK(feed_changed.output_binding_edit->binding.feed_index == 7U);
  static_cast<void>(harness.runtime_frame(details_x + 7.0F, feed_y, false,
                                          map, two_outputs));

  // Precision belongs to this physical output; editing it preserves its target.
  for (const core::u32 depth : {10U, 16U, 8U}) {
    static_cast<void>(harness.runtime_frame(details_x, 299.0F, false, map, two_outputs));
    static_cast<void>(harness.runtime_frame(details_x, 299.0F, true, map, two_outputs));
    const auto changed = harness.runtime_frame(details_x, 299.0F, false, map, two_outputs);
    MAPPING_CHECK(changed.output_binding_edit.has_value());
    MAPPING_CHECK(changed.output_binding_edit->binding.output_bit_depth == depth);
    MAPPING_CHECK(changed.output_binding_edit->binding.target == authored.target);
    map.output_bindings[0] = changed.output_binding_edit->binding;
  }

  // Hotplug does not rewrite anything. Only pressing the visible repair
  // control copies a different stable observed identity into the request.
  topology::HardwareRuntimeSnapshot replacement = render_runtime(1U, 1U);
  replacement.nodes[0U].video_outputs[0U] =
      two_outputs.nodes[0U].video_outputs[1U];
  constexpr float repair_y = 435.0F;
  static_cast<void>(
      harness.runtime_frame(details_x, repair_y, false, map, replacement));
  static_cast<void>(
      harness.runtime_frame(details_x, repair_y, true, map, replacement));
  const auto repaired =
      harness.runtime_frame(details_x, repair_y, false, map, replacement);
  MAPPING_CHECK(repaired.output_binding_edit.has_value());
  MAPPING_CHECK(repaired.output_binding_edit->binding.target ==
                replacement.nodes[0U].video_outputs[0U].target);

  MAPPING_CHECK(!repaired.output_binding_delete.has_value());
}

void live_render_outputs_are_distinct_cable_targets_without_mutating_map() {
  HardwareHarness harness;
  const topology::HardwareMap map = hardware_demo();
  const topology::HardwareRuntimeSnapshot runtime = render_runtime(1U, 3U);
  const core::u64 authored_digest = topology::digest(map);
  const editor::IsometricProjection projection =
      harness.panel.view_projection(HardwareHarness::bounds);
  const gui::Rect baseline =
      editor::hardware_device_bounds(projection, map.devices[0U]);
  const gui::Rect presented = editor::hardware_presented_device_bounds(
      projection, map, map.devices[0U], runtime);
  MAPPING_CHECK(std::abs(presented.height - baseline.height - 30.0F) < 0.01F);

  const editor::IsoPoint output_one = editor::hardware_presented_port_position(
      projection, map, map.devices[0U], runtime, 1U);
  const editor::IsoPoint output_two = editor::hardware_presented_port_position(
      projection, map, map.devices[0U], runtime, 2U);
  const editor::IsoPoint output_three =
      editor::hardware_presented_port_position(projection, map, map.devices[0U],
                                               runtime, 3U);
  MAPPING_CHECK(std::abs(output_one.x - output_two.x) < 0.01F);
  MAPPING_CHECK(std::abs(output_two.x - output_three.x) < 0.01F);
  MAPPING_CHECK(std::abs(output_two.y - output_one.y - 15.0F) < 0.01F);
  MAPPING_CHECK(std::abs(output_three.y - output_two.y - 15.0F) < 0.01F);
  // Uncabled, the card is a parked alternative whatever its machine reports;
  // a cable puts it in the show.
  MAPPING_CHECK(
      editor::hardware_runtime_status_text(map, map.devices[0U], runtime) ==
      "PARKED | NOT CABLED | NOT IN SHOW");
  const topology::HardwareMap cabled = hardware_demo(true);
  MAPPING_CHECK(editor::hardware_runtime_status_text(
                    cabled, cabled.devices[0U], runtime) ==
                "ONLINE | RUNNING | 3 ACTIVE OUT");

  const editor::IsoPoint processor_input =
      editor::hardware_presented_port_position(projection, map, map.devices[1U],
                                               runtime, 0U);
  static_cast<void>(harness.runtime_frame(output_three.x, output_three.y, false,
                                          map, runtime));
  const auto pressed =
      harness.runtime_frame(output_three.x, output_three.y, true, map, runtime);
  MAPPING_CHECK(pressed.dragging);
  const auto over_target = harness.runtime_frame(
      processor_input.x, processor_input.y, true, map, runtime);
  MAPPING_CHECK(over_target.link_feedback ==
                editor::HardwareLinkFeedback::Compatible);
  const auto released = harness.runtime_frame(
      processor_input.x, processor_input.y, false, map, runtime);
  MAPPING_CHECK(released.link.has_value());
  MAPPING_CHECK(released.link->from ==
                topology::HardwareEndpoint{doc::NodeId{1U}, 3U});
  MAPPING_CHECK(released.link->to ==
                topology::HardwareEndpoint{doc::NodeId{2U}, 0U});
  MAPPING_CHECK(released.link->output_binding.has_value());
  MAPPING_CHECK(released.link->output_binding->logical_port == 3U);
  MAPPING_CHECK(released.link->output_binding->target ==
                runtime.nodes[0U].video_outputs[2U].target);
  MAPPING_CHECK(!released.property_edit.has_value());
  MAPPING_CHECK(topology::digest(map) == authored_digest);
}

void output_inventory_preserves_unknown_zero_and_last_good_semantics() {
  const topology::HardwareMap map = hardware_demo(true);
  const editor::IsometricProjection projection =
      editor::hardware_mapping_projection(HardwareHarness::bounds);
  const gui::Rect baseline =
      editor::hardware_device_bounds(projection, map.devices[0U]);

  const topology::HardwareRuntimeSnapshot unknown =
      render_runtime(1U, 3U, topology::HardwareRuntimeInventoryState::Unknown);
  MAPPING_CHECK(
      editor::hardware_runtime_status_text(map, map.devices[0U], unknown) ==
      "ONLINE | RUNNING | OUTPUTS UNKNOWN");
  const gui::Rect unknown_bounds = editor::hardware_presented_device_bounds(
      projection, map, map.devices[0U], unknown);
  MAPPING_CHECK(std::abs(unknown_bounds.height - baseline.height) < 0.01F);

  const topology::HardwareRuntimeSnapshot unmonitored =
      render_runtime(1U, 3U, topology::HardwareRuntimeInventoryState::Available,
                     topology::HardwareRuntimeReachability::Unmonitored);
  MAPPING_CHECK(
      editor::hardware_runtime_status_text(map, map.devices[0U], unmonitored) ==
      "CONFIGURED | UNMONITORED | OUTPUTS UNKNOWN");

  const topology::HardwareRuntimeSnapshot detected_zero =
      render_runtime(1U, 0U);
  MAPPING_CHECK(editor::hardware_runtime_status_text(map, map.devices[0U],
                                                     detected_zero) ==
                "ONLINE | RUNNING | 0 ACTIVE OUT");

  const topology::HardwareRuntimeSnapshot partial =
      render_runtime(1U, 2U, topology::HardwareRuntimeInventoryState::Partial);
  MAPPING_CHECK(
      editor::hardware_runtime_status_text(map, map.devices[0U], partial) ==
      "ONLINE | RUNNING | 2+ ACTIVE OUT");
  const gui::Rect partial_bounds = editor::hardware_presented_device_bounds(
      projection, map, map.devices[0U], partial);
  MAPPING_CHECK(std::abs(partial_bounds.height - baseline.height - 15.0F) <
                0.01F);

  const topology::HardwareRuntimeSnapshot unsupported = render_runtime(
      1U, 0U, topology::HardwareRuntimeInventoryState::Unsupported);
  MAPPING_CHECK(
      editor::hardware_runtime_status_text(map, map.devices[0U], unsupported) ==
      "ONLINE | RUNNING | OUTPUTS UNSUPPORTED");
  const topology::HardwareRuntimeSnapshot failed =
      render_runtime(1U, 0U, topology::HardwareRuntimeInventoryState::Failed);
  MAPPING_CHECK(
      editor::hardware_runtime_status_text(map, map.devices[0U], failed) ==
      "ONLINE | RUNNING | OUTPUTS ERROR");

  const topology::HardwareRuntimeSnapshot stopped =
      render_runtime(1U, 0U, topology::HardwareRuntimeInventoryState::Available,
                     topology::HardwareRuntimeReachability::Online,
                     topology::HardwareRuntimeProcessState::Stopped);
  MAPPING_CHECK(
      editor::hardware_runtime_status_text(map, map.devices[0U], stopped) ==
      "ONLINE | STOPPED | 0 ACTIVE OUT");
  const topology::HardwareRuntimeSnapshot stale =
      render_runtime(1U, 2U, topology::HardwareRuntimeInventoryState::Available,
                     topology::HardwareRuntimeReachability::Offline,
                     topology::HardwareRuntimeProcessState::Unknown, true);
  MAPPING_CHECK(editor::hardware_runtime_status_text(
                    map, map.devices[0U], stale) == "OFFLINE | 2 LAST OUT");
}

void linked_missing_outputs_stay_compact_and_live_details_are_read_only() {
  HardwareHarness harness;
  topology::HardwareMap map = hardware_demo();
  map.links.push_back(topology::HardwareLink{
      doc::NodeId{10U}, topology::HardwareEndpoint{doc::NodeId{1U}, 5U},
      topology::HardwareEndpoint{doc::NodeId{2U}, 0U}});
  const topology::HardwareRuntimeSnapshot runtime = render_runtime(1U, 1U);
  const core::u64 authored_digest = topology::digest(map);
  const editor::IsometricProjection projection =
      harness.panel.view_projection(HardwareHarness::bounds);
  const gui::Rect baseline =
      editor::hardware_device_bounds(projection, map.devices[0U]);
  const gui::Rect presented = editor::hardware_presented_device_bounds(
      projection, map, map.devices[0U], runtime);
  MAPPING_CHECK(std::abs(presented.height - baseline.height - 15.0F) < 0.01F);
  const editor::IsoPoint output_one = editor::hardware_presented_port_position(
      projection, map, map.devices[0U], runtime, 1U);
  const editor::IsoPoint output_five = editor::hardware_presented_port_position(
      projection, map, map.devices[0U], runtime, 5U);
  MAPPING_CHECK(std::abs(output_five.y - output_one.y - 15.0F) < 0.01F);

  const editor::IsoPoint card = editor::project_isometric(
      projection, map.devices[0U].grid_x, map.devices[0U].grid_y);
  static_cast<void>(harness.runtime_frame(card.x, card.y, false, map, runtime));
  const auto selected =
      harness.runtime_frame(card.x, card.y, true, map, runtime);
  MAPPING_CHECK(selected.selection ==
                editor::HardwareSelection{editor::HardwareSelectionKind::Device,
                                          doc::NodeId{1U}});
  static_cast<void>(harness.runtime_frame(card.x, card.y, false, map, runtime));

  // The lower details rail is presentation-only telemetry. Pressing it must
  // not leak through to the canvas or emit a document property edit.
  constexpr float live_details_x = 810.0F;
  constexpr float live_details_y = 450.0F;
  static_cast<void>(harness.runtime_frame(live_details_x, live_details_y, false,
                                          map, runtime));
  const auto details_press =
      harness.runtime_frame(live_details_x, live_details_y, true, map, runtime);
  const auto details_release = harness.runtime_frame(
      live_details_x, live_details_y, false, map, runtime);
  MAPPING_CHECK(!details_press.property_edit.has_value());
  MAPPING_CHECK(!details_release.property_edit.has_value());
  MAPPING_CHECK(!details_release.move.has_value());
  MAPPING_CHECK(!details_release.link.has_value());
  MAPPING_CHECK(!details_release.remove.has_value());
  MAPPING_CHECK(topology::digest(map) == authored_digest);
}

[[nodiscard]] output::FeedMap feed_demo() {
  output::FeedMap map{};
  output::OutputMap first{};
  first.node = doc::NodeId{10U};
  first.node_index = 0U;
  first.source = scene::CanvasRect{160U, 90U, 640U, 360U};
  output::OutputMap second{};
  second.node = doc::NodeId{11U};
  second.node_index = 1U;
  second.source = scene::CanvasRect{960U, 540U, 800U, 450U};
  map.maps = {first, second};
  for (output::OutputMap &feed : map.maps) {
    feed.viewport = feed.source;
  }
  return map;
}

[[nodiscard]] output::PhysicalSurface
feed_surface(float x, scene::Canvas raster = scene::Canvas{1920U, 1080U}) {
  output::PhysicalSurface surface{};
  surface.center = scene::Vec3{x, 0.0F, -4.0F};
  surface.right = scene::Vec3{1.0F, 0.0F, 0.0F};
  surface.up = scene::Vec3{0.0F, 1.0F, 0.0F};
  surface.width = 8.0F;
  surface.height = 4.5F;
  surface.raster = raster;
  return surface;
}

[[nodiscard]] output::FeedMap feed_surface_demo() {
  output::FeedMap map = feed_demo();
  map.surfaces = {
      output::ProjectionSurface{doc::NodeId{70U}, "Main Wall",
                                feed_surface(0.0F)},
      output::ProjectionSurface{
          doc::NodeId{71U}, "Stage Left",
          feed_surface(-6.0F, scene::Canvas{1280U, 1080U})},
  };
  return map;
}

struct FeedHarness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::FeedMappingPanel panel{gui::ui_id("feed.mapping.test")};
  static constexpr gui::Rect bounds{0.0F, 0.0F, 800.0F, 460.0F};
  static constexpr scene::Canvas canvas{1920U, 1080U};

  editor::FeedMappingFrame frame(float x, float y, bool down,
                                  const output::FeedMap &map,
                                  std::optional<gui::Key> key = std::nullopt,
                                  core::u64 show_frame = 0U,
                                  const topology::HardwareMap *hardware =
                                      nullptr,
                                  const gui::Rect &draw_bounds = bounds) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    if (key.has_value()) {
      input.add_key(*key);
    }
    ctx.new_frame(time += frame_seconds);
    return panel.draw(ctx, draw_bounds, canvas, map, show_frame, hardware);
  }

  editor::FeedMappingFrame type(std::string_view text,
                                const output::FeedMap &map) {
    input.new_frame();
    input.add_text(text);
    ctx.new_frame(time += frame_seconds);
    return panel.draw(ctx, bounds, canvas, map);
  }

  editor::FeedMappingFrame click(float x, float y,
                                 const output::FeedMap &map) {
    static_cast<void>(frame(x, y, false, map));
    static_cast<void>(frame(x, y, true, map));
    return frame(x, y, false, map);
  }

  // Separate clicks are a second apart unless a test asks for a
  // double-click, which is what an operator's two deliberate clicks are.
  double time{};
  double frame_seconds{1.0};
};

// Feed list rows at the harness size: viewport row n starts at 84 + 37n;
// with two viewports the first surface row starts at 243, 31 apart. A row
// ends in 26-pixel actions: duplicate then remove for viewports, duplicate,
// padlock, remove for surfaces.
constexpr float feed_row_x = 70.0F;
constexpr float feed_viewport_duplicate_x = 147.0F;
constexpr float feed_surface_duplicate_x = 121.0F;

[[nodiscard]] output::FeedMap feed_one_window_demo() {
  output::FeedMap map{};
  for (core::u64 id : {20U, 21U, 22U}) {
    output::OutputMap viewport{};
    viewport.node = doc::NodeId{id};
    viewport.source = scene::CanvasRect{0U, 0U, 640U, 360U};
    viewport.viewport = viewport.source;
    map.maps.push_back(viewport);
  }
  return map;
}

void feed_rectangles_move_in_canvas_pixels_and_clamp() {
  FeedHarness harness;
  const output::FeedMap map = feed_demo();
  const gui::Rect fitted =
      editor::feed_mapping_canvas(FeedHarness::bounds, FeedHarness::canvas);
  const gui::Rect rectangle = editor::feed_map_screen_rect(
      fitted, FeedHarness::canvas, map.maps[0U].source);
  const float start_x = rectangle.x + rectangle.width * 0.5F;
  const float start_y = rectangle.y + rectangle.height * 0.5F;
  const float delta_x = 44.0F;
  const float delta_y = 23.0F;
  const float pixel_scale =
      fitted.width / static_cast<float>(FeedHarness::canvas.width);
  const core::u32 expected_x =
      map.maps[0U].source.x +
      static_cast<core::u32>(std::llround(delta_x / pixel_scale));
  const core::u32 expected_y =
      map.maps[0U].source.y +
      static_cast<core::u32>(std::llround(delta_y / pixel_scale));

  static_cast<void>(harness.frame(start_x, start_y, false, map));
  const auto pressed = harness.frame(start_x, start_y, true, map);
  MAPPING_CHECK(pressed.selection == doc::NodeId{10U});
  static_cast<void>(
      harness.frame(start_x + delta_x, start_y + delta_y, true, map));
  const auto released =
      harness.frame(start_x + delta_x, start_y + delta_y, false, map);

  MAPPING_CHECK(released.edit.has_value());
  MAPPING_CHECK(released.edit->node == doc::NodeId{10U});
  MAPPING_CHECK(released.edit->viewport.x == expected_x);
  MAPPING_CHECK(released.edit->viewport.y == expected_y);
  MAPPING_CHECK(released.edit->viewport.width == 640U);
  MAPPING_CHECK(released.edit->viewport.height == 360U);

  // A large negative move is authored at the canvas edge, never wrapped.
  const scene::CanvasRect moved_source = released.edit->viewport;
  output::FeedMap moved_map{};
  output::OutputMap moved{};
  moved.node = doc::NodeId{10U};
  moved.node_index = 0U;
  moved.source = moved_source;
  moved_map.maps.push_back(moved);
  moved_map.maps.back().viewport = moved_source;
  const gui::Rect moved_screen =
      editor::feed_map_screen_rect(fitted, FeedHarness::canvas, moved_source);
  const float moved_x = moved_screen.x + moved_screen.width * 0.5F;
  const float moved_y = moved_screen.y + moved_screen.height * 0.5F;
  static_cast<void>(harness.frame(moved_x, moved_y, false, moved_map));
  static_cast<void>(harness.frame(moved_x, moved_y, true, moved_map));
  static_cast<void>(
      harness.frame(fitted.x - 500.0F, fitted.y - 500.0F, true, moved_map));
  const auto clamped =
      harness.frame(fitted.x - 500.0F, fitted.y - 500.0F, false, moved_map);
  MAPPING_CHECK(clamped.edit.has_value());
  MAPPING_CHECK(clamped.edit->viewport.x == 0U);
  MAPPING_CHECK(clamped.edit->viewport.y == 0U);
}

void a_selected_feed_rectangle_resizes_from_its_handle() {
  FeedHarness harness;
  const output::FeedMap map = feed_demo();
  const gui::Rect fitted =
      editor::feed_mapping_canvas(FeedHarness::bounds, FeedHarness::canvas);
  const gui::Rect rectangle = editor::feed_map_screen_rect(
      fitted, FeedHarness::canvas, map.maps[0U].source);
  const float centre_x = rectangle.x + rectangle.width * 0.5F;
  const float centre_y = rectangle.y + rectangle.height * 0.5F;

  static_cast<void>(harness.frame(centre_x, centre_y, false, map));
  static_cast<void>(harness.frame(centre_x, centre_y, true, map));
  static_cast<void>(harness.frame(centre_x, centre_y, false, map));
  MAPPING_CHECK(harness.panel.selected_map_node() == doc::NodeId{10U});

  const float handle_x = rectangle.x + rectangle.width;
  const float handle_y = rectangle.y + rectangle.height;
  const float delta_x = 36.0F;
  const float delta_y = 18.0F;
  const float pixel_scale =
      fitted.width / static_cast<float>(FeedHarness::canvas.width);
  const core::u32 expected_width =
      map.maps[0U].source.width +
      static_cast<core::u32>(std::llround(delta_x / pixel_scale));
  const core::u32 expected_height =
      map.maps[0U].source.height +
      static_cast<core::u32>(std::llround(delta_y / pixel_scale));

  static_cast<void>(harness.frame(handle_x, handle_y, false, map));
  static_cast<void>(harness.frame(handle_x, handle_y, true, map));
  static_cast<void>(
      harness.frame(handle_x + delta_x, handle_y + delta_y, true, map));
  const auto released =
      harness.frame(handle_x + delta_x, handle_y + delta_y, false, map);

  MAPPING_CHECK(released.edit.has_value());
  MAPPING_CHECK(released.edit->viewport.width == expected_width);
  MAPPING_CHECK(released.edit->viewport.height == expected_height);
  MAPPING_CHECK(released.edit->viewport.x == map.maps[0U].viewport.x);
  MAPPING_CHECK(released.edit->viewport.y == map.maps[0U].viewport.y);
}

void hardware_window_rasters_form_one_viewport_mapping_area() {
  output::FeedMap map{};
  topology::HardwareMap hardware{};
  for (core::u32 index = 0U; index < 4U; ++index) {
    output::OutputMap feed{};
    feed.node = doc::NodeId{20U + index};
    feed.node_index = index;
    feed.source = scene::CanvasRect{0U, 0U, 3'456U, 1'356U};
    feed.viewport = scene::CanvasRect{0U, 402U, 3'456U, 1'356U};
    map.maps.push_back(feed);

    topology::HardwareOutputBinding binding{};
    binding.node = doc::NodeId{100U + index};
    binding.feed_index = index;
    binding.width = 3'840U;
    binding.height = 2'160U;
    hardware.output_bindings.push_back(binding);
  }

  FeedHarness harness;
  const scene::Canvas aggregate{15'360U, 2'160U};
  const gui::Rect fitted =
      editor::feed_mapping_canvas(FeedHarness::bounds, aggregate);
  const gui::Rect fourth = editor::feed_map_screen_rect(
      fitted, aggregate,
      scene::CanvasRect{11'520U, 402U, 3'456U, 1'356U});
  const float centre_x = fourth.x + fourth.width * 0.5F;
  const float centre_y = fourth.y + fourth.height * 0.5F;
  static_cast<void>(
      harness.frame(centre_x, centre_y, false, map, std::nullopt, 0U,
                    &hardware));
  const auto pressed = harness.frame(centre_x, centre_y, true, map,
                                     std::nullopt, 0U, &hardware);
  MAPPING_CHECK(pressed.selection == doc::NodeId{23U});
  static_cast<void>(harness.frame(centre_x + 2.0F, centre_y, true, map,
                                  std::nullopt, 0U, &hardware));
  const auto released = harness.frame(centre_x + 2.0F, centre_y, false, map,
                                      std::nullopt, 0U, &hardware);
  MAPPING_CHECK(released.edit.has_value());
  MAPPING_CHECK(released.edit->node == doc::NodeId{23U});
  MAPPING_CHECK(released.edit->viewport.x > 0U);
  MAPPING_CHECK(map.maps[3U].source ==
                (scene::CanvasRect{0U, 0U, 3'456U, 1'356U}));
}

void one_mosaic_window_contains_four_independent_viewports() {
  output::FeedMap map{};
  for (core::u32 index = 0U; index < 4U; ++index) {
    output::OutputMap feed{};
    feed.node = doc::NodeId{40U + index};
    feed.node_index = 0U;
    feed.source = scene::CanvasRect{index * 3'456U, 0U, 3'456U, 1'356U};
    feed.viewport =
        scene::CanvasRect{index * 3'840U, 402U, 3'456U, 1'356U};
    map.maps.push_back(feed);
  }
  topology::HardwareMap hardware{};
  topology::HardwareOutputBinding binding{};
  binding.node = doc::NodeId{100U};
  binding.feed_index = 0U;
  binding.width = 15'360U;
  binding.height = 2'160U;
  hardware.output_bindings.push_back(binding);

  FeedHarness harness;
  const scene::Canvas mosaic{15'360U, 2'160U};
  const gui::Rect fitted =
      editor::feed_mapping_canvas(FeedHarness::bounds, mosaic);
  const gui::Rect fourth = editor::feed_map_screen_rect(
      fitted, mosaic,
      scene::CanvasRect{11'520U, 402U, 3'456U, 1'356U});
  const float centre_x = fourth.x + fourth.width * 0.5F;
  const float centre_y = fourth.y + fourth.height * 0.5F;
  static_cast<void>(harness.frame(centre_x, centre_y, false, map,
                                  std::nullopt, 0U, &hardware));
  const auto pressed = harness.frame(centre_x, centre_y, true, map,
                                     std::nullopt, 0U, &hardware);
  MAPPING_CHECK(pressed.selection == doc::NodeId{43U});
  static_cast<void>(harness.frame(centre_x + 2.0F, centre_y, true, map,
                                  std::nullopt, 0U, &hardware));
  const auto released = harness.frame(centre_x + 2.0F, centre_y, false, map,
                                      std::nullopt, 0U, &hardware);
  MAPPING_CHECK(released.edit.has_value());
  MAPPING_CHECK(released.edit->node == doc::NodeId{43U});
  MAPPING_CHECK(released.edit->viewport.x > map.maps[3U].viewport.x);
  MAPPING_CHECK(map.maps_for(0U).size() == 4U);

  constexpr float add_x = 96.0F;
  constexpr float add_y = 67.0F;
  static_cast<void>(harness.frame(add_x, add_y, false, map, std::nullopt, 0U,
                                  &hardware));
  static_cast<void>(harness.frame(add_x, add_y, true, map, std::nullopt, 0U,
                                  &hardware));
  const auto added = harness.frame(add_x, add_y, false, map, std::nullopt, 0U,
                                   &hardware);
  MAPPING_CHECK(added.add.has_value());
  MAPPING_CHECK(added.add->node_index == 0U);
  MAPPING_CHECK(added.add->source == map.maps[3U].source);
}

void numeric_rectangles_keep_source_and_destination_independent() {
  const auto map = feed_demo();
  FeedHarness destination;
  static_cast<void>(destination.frame(70.0F, 110.0F, false, map));
  static_cast<void>(destination.frame(70.0F, 110.0F, true, map));
  static_cast<void>(destination.frame(70.0F, 110.0F, false, map));
  static_cast<void>(destination.frame(240.0F, 409.0F, true, map));
  const auto moved = destination.frame(263.0F, 409.0F, true, map);
  MAPPING_CHECK(moved.edit.has_value());
  MAPPING_CHECK(moved.edit->node == map.maps.front().node);
  MAPPING_CHECK(moved.edit->viewport.x == map.maps.front().viewport.x + 23U);
  MAPPING_CHECK(moved.edit->viewport.width == map.maps.front().viewport.width);
  MAPPING_CHECK(!moved.projection_edit.has_value());

  FeedHarness source;
  static_cast<void>(source.frame(70.0F, 110.0F, false, map));
  static_cast<void>(source.frame(70.0F, 110.0F, true, map));
  static_cast<void>(source.frame(70.0F, 110.0F, false, map));
  static_cast<void>(source.frame(240.0F, 448.0F, true, map));
  const auto cropped = source.frame(271.0F, 448.0F, true, map);
  MAPPING_CHECK(cropped.projection_edit.has_value());
  MAPPING_CHECK(cropped.projection_edit->source.x == map.maps.front().source.x + 31U);
  MAPPING_CHECK(cropped.projection_edit->source.width == map.maps.front().source.width);
  MAPPING_CHECK(!cropped.edit.has_value());
}

void numeric_rectangles_release_focus_when_removed_or_hidden() {
  const auto map = feed_demo();
  const auto open_destination = [&](FeedHarness &harness) {
    static_cast<void>(harness.frame(70.0F, 110.0F, false, map));
    static_cast<void>(harness.frame(70.0F, 110.0F, true, map));
    static_cast<void>(harness.frame(70.0F, 110.0F, false, map));
    static_cast<void>(harness.frame(240.0F, 409.0F, true, map));
    const auto opened = harness.frame(240.0F, 409.0F, false, map);
    MAPPING_CHECK(harness.ctx.typing());
    MAPPING_CHECK(opened.editing);
  };

  FeedHarness removed;
  open_destination(removed);
  auto remaining = map;
  remaining.maps.erase(remaining.maps.begin());
  const auto erased = removed.frame(240.0F, 409.0F, false, remaining);
  MAPPING_CHECK(!removed.ctx.typing());
  MAPPING_CHECK(!erased.editing);
  MAPPING_CHECK(!erased.edit && !erased.projection_edit);

  for (const gui::Rect resized : {gui::Rect{0.0F, 0.0F, 400.0F, 460.0F},
                                  gui::Rect{0.0F, 0.0F, 800.0F, 100.0F},
                                  gui::Rect{0.0F, 0.0F, 0.0F, 0.0F}}) {
    FeedHarness hidden;
    open_destination(hidden);
    const auto resized_frame = hidden.frame(240.0F, 409.0F, false, map,
                                            std::nullopt, 0U, nullptr, resized);
    MAPPING_CHECK(!hidden.ctx.typing());
    MAPPING_CHECK(!resized_frame.editing);
    MAPPING_CHECK(!resized_frame.edit && !resized_frame.projection_edit);
    // Restoring the panel must restore a usable field, not an abandoned edit.
    open_destination(hidden);
  }
}

void imported_mesh_hides_source_crop_but_keeps_destination_fields() {
  auto map = feed_surface_demo();
  FeedHarness harness;
  static_cast<void>(harness.frame(70.0F, 110.0F, false, map));
  static_cast<void>(harness.frame(70.0F, 110.0F, true, map));
  static_cast<void>(harness.frame(70.0F, 110.0F, false, map));
  // Begin typing the ordinary canvas source width before the replicated
  // mapping changes to a UV mesh; hiding that field must release its focus.
  static_cast<void>(harness.frame(420.0F, 448.0F, true, map));
  static_cast<void>(harness.frame(420.0F, 448.0F, false, map));
  MAPPING_CHECK(harness.ctx.typing());
  auto& surface = map.surfaces.front();
  surface.shape = output::ProjectionSurfaceShape::ImportedMesh;
  surface.mesh_source = "calibration/feed-0.glb";
  auto& feed = map.maps.front();
  feed.projection = output::FeedProjection::PlanarSurface;
  feed.surface = surface.node;
  feed.source = scene::whole(surface.geometry.raster);
  const auto changed = harness.frame(420.0F, 448.0F, false, map);
  MAPPING_CHECK(!harness.ctx.typing());
  MAPPING_CHECK(!changed.edit && !changed.projection_edit);
  static_cast<void>(harness.frame(420.0F, 448.0F, true, map));
  const auto hidden = harness.frame(390.0F, 448.0F, true, map);
  const auto released = harness.frame(390.0F, 448.0F, false, map);
  MAPPING_CHECK(!hidden.edit && !hidden.projection_edit);
  MAPPING_CHECK(!released.edit && !released.projection_edit);
  MAPPING_CHECK(!harness.ctx.typing());

  static_cast<void>(harness.frame(240.0F, 409.0F, true, map));
  const auto moved = harness.frame(263.0F, 409.0F, true, map);
  MAPPING_CHECK(moved.edit.has_value());
  MAPPING_CHECK(moved.edit->viewport.x == feed.viewport.x + 23U);
  MAPPING_CHECK(!moved.projection_edit);
}

void feed_mapping_adds_another_viewport_to_the_current_window() {
  constexpr float add_x = 96.0F;
  constexpr float add_y = 67.0F;

  FeedHarness empty_harness;
  const output::FeedMap empty;
  static_cast<void>(empty_harness.frame(add_x, add_y, false, empty));
  static_cast<void>(empty_harness.frame(add_x, add_y, true, empty));
  const auto first = empty_harness.frame(add_x, add_y, false, empty);
  MAPPING_CHECK(first.add == editor::FeedMapAddRequest{
                                 0U, scene::CanvasRect{0U, 0U, 1920U, 1080U},
                                 scene::CanvasRect{0U, 0U, 1920U, 1080U}});
  MAPPING_CHECK(!first.edit.has_value());

  output::FeedMap sparse{};
  output::OutputMap sparse_first{};
  sparse_first.node = doc::NodeId{30U};
  sparse_first.node_index = 0U;
  sparse_first.source = scene::CanvasRect{0U, 0U, 640U, 1080U};
  sparse_first.viewport = sparse_first.source;
  output::OutputMap sparse_second{};
  sparse_second.node = doc::NodeId{31U};
  sparse_second.node_index = 2U;
  sparse_second.source = scene::CanvasRect{1280U, 0U, 640U, 1080U};
  sparse_second.viewport = sparse_second.source;
  sparse.maps = {sparse_first, sparse_second};
  FeedHarness sparse_harness;
  static_cast<void>(sparse_harness.frame(add_x, add_y, false, sparse));
  static_cast<void>(sparse_harness.frame(add_x, add_y, true, sparse));
  const auto gap = sparse_harness.frame(add_x, add_y, false, sparse);
  MAPPING_CHECK(gap.add == editor::FeedMapAddRequest{
                               0U, scene::CanvasRect{0U, 0U, 1920U, 1080U},
                               scene::CanvasRect{0U, 0U, 1920U, 1080U}});
}

void output_identity_is_explicit_and_the_empty_surface_prompt_is_inert() {
  MAPPING_CHECK(editor::feed_output_identity(0U) ==
                "FEED 01  FEED INDEX 0");
  MAPPING_CHECK(editor::feed_output_identity(12U) ==
                "FEED 13  FEED INDEX 12");
  MAPPING_CHECK(editor::feed_runtime_binding(0U) ==
                "ASSIGN IN HARDWARE MAP  |  FEED INDEX 0");
  MAPPING_CHECK(editor::feed_runtime_binding(12U) ==
                "ASSIGN IN HARDWARE MAP  |  FEED INDEX 12");

  output::FeedMap dangling{};
  output::OutputMap dangling_map{};
  dangling_map.node = doc::NodeId{40U};
  dangling_map.node_index = 0U;
  dangling_map.source = scene::CanvasRect{0U, 0U, 1920U, 1080U};
  dangling_map.viewport = dangling_map.source;
  dangling_map.projection = output::FeedProjection::PlanarSurface;
  dangling_map.surface = doc::NodeId{999U};
  dangling.maps.push_back(dangling_map);
  FeedHarness harness;

  // Select FEED 01 from the left rail.
  constexpr float row_x = 96.0F;
  constexpr float row_y = 101.0F;
  static_cast<void>(harness.frame(row_x, row_y, false, dangling));
  static_cast<void>(harness.frame(row_x, row_y, true, dangling));
  static_cast<void>(harness.frame(row_x, row_y, false, dangling));
  MAPPING_CHECK(harness.panel.selected_map_node() == doc::NodeId{40U});

  // With no projection surfaces, the second full-width row is guidance only:
  // it never becomes hot and cannot emit a projection edit.
  constexpr float prompt_x = 680.0F;
  constexpr float prompt_y = 107.0F;
  const auto hovered = harness.frame(prompt_x, prompt_y, false, dangling);
  MAPPING_CHECK(harness.ctx.hot() == 0U);
  MAPPING_CHECK(!hovered.projection_edit.has_value());
  static_cast<void>(harness.frame(prompt_x, prompt_y, true, dangling));
  const auto released = harness.frame(prompt_x, prompt_y, false, dangling);
  MAPPING_CHECK(!released.projection_edit.has_value());

  // CANVAS remains a real full-width repair action above the prompt.
  constexpr float canvas_x = 680.0F;
  constexpr float canvas_y = 82.0F;
  static_cast<void>(harness.frame(canvas_x, canvas_y, false, dangling));
  static_cast<void>(harness.frame(canvas_x, canvas_y, true, dangling));
  const auto repaired = harness.frame(canvas_x, canvas_y, false, dangling);
  MAPPING_CHECK(repaired.projection_edit.has_value());
  MAPPING_CHECK(repaired.projection_edit->node == doc::NodeId{40U});
  MAPPING_CHECK(repaired.projection_edit->projection ==
                output::FeedProjection::CanvasSlice);
}

// A double-click on a viewport row, or F2 after clicking a surface row, opens
// the name in place; Enter commits exactly the typed name.
void feed_rows_rename_in_place() {
  const output::FeedMap map = feed_surface_demo();
  FeedHarness harness;
  harness.frame_seconds = 0.05;
  static_cast<void>(harness.click(feed_row_x, 101.0F, map));
  static_cast<void>(harness.click(feed_row_x, 101.0F, map));
  static_cast<void>(harness.type("Processor A", map));
  const auto viewport =
      harness.frame(feed_row_x, 101.0F, false, map, gui::Key::Enter);
  MAPPING_CHECK(viewport.organize ==
                (editor::FeedOrganizeRequest{
                    editor::FeedOrganizeAction::Rename, doc::NodeId{10U},
                    "Processor A"}));

  harness.frame_seconds = 1.0;
  static_cast<void>(harness.click(feed_row_x, 286.0F, map));
  static_cast<void>(
      harness.frame(feed_row_x, 286.0F, false, map, gui::Key::Rename));
  static_cast<void>(harness.type("Left Wing", map));
  const auto surface =
      harness.frame(feed_row_x, 286.0F, false, map, gui::Key::Enter);
  MAPPING_CHECK(surface.organize ==
                (editor::FeedOrganizeRequest{
                    editor::FeedOrganizeAction::Rename, doc::NodeId{71U},
                    "Left Wing"}));

  // A single click, a second later, is a selection and nothing more.
  const auto single = harness.click(feed_row_x, 101.0F, map);
  MAPPING_CHECK(!single.organize.has_value());
  MAPPING_CHECK(!harness.ctx.typing());
}

void feed_rows_duplicate_from_their_action() {
  const output::FeedMap map = feed_surface_demo();
  FeedHarness harness;
  const auto viewport =
      harness.click(feed_viewport_duplicate_x, 101.0F, map);
  MAPPING_CHECK(viewport.organize ==
                (editor::FeedOrganizeRequest{
                    editor::FeedOrganizeAction::Duplicate, doc::NodeId{10U}}));
  MAPPING_CHECK(!viewport.remove.has_value());
  const auto surface = harness.click(feed_surface_duplicate_x, 257.0F, map);
  MAPPING_CHECK(surface.organize ==
                (editor::FeedOrganizeRequest{
                    editor::FeedOrganizeAction::Duplicate, doc::NodeId{70U}}));
  MAPPING_CHECK(!surface.surface_remove.has_value());
  MAPPING_CHECK(!surface.surface_lock.has_value());
}

// Dragging a row within its list reorders the whole list; releasing it where
// it started authors nothing.
void feed_rows_reorder_by_dragging() {
  const output::FeedMap viewports = feed_one_window_demo();
  FeedHarness harness;
  static_cast<void>(harness.frame(feed_row_x, 101.0F, false, viewports));
  static_cast<void>(harness.frame(feed_row_x, 101.0F, true, viewports));
  static_cast<void>(harness.frame(feed_row_x, 150.0F, true, viewports));
  const auto moving = harness.frame(feed_row_x, 185.0F, true, viewports);
  MAPPING_CHECK(moving.dragging && !moving.organize.has_value());
  const auto dropped = harness.frame(feed_row_x, 185.0F, false, viewports);
  MAPPING_CHECK(dropped.organize.has_value());
  MAPPING_CHECK(dropped.organize->action ==
                editor::FeedOrganizeAction::Reorder);
  MAPPING_CHECK(dropped.organize->order ==
                (std::vector<doc::NodeId>{doc::NodeId{21U}, doc::NodeId{22U},
                                          doc::NodeId{20U}}));

  static_cast<void>(harness.frame(feed_row_x, 101.0F, true, viewports));
  static_cast<void>(harness.frame(feed_row_x, 108.0F, true, viewports));
  const auto unmoved = harness.frame(feed_row_x, 108.0F, false, viewports);
  MAPPING_CHECK(!unmoved.organize.has_value());

  const output::FeedMap surfaces = feed_surface_demo();
  FeedHarness list;
  static_cast<void>(list.frame(feed_row_x, 250.0F, false, surfaces));
  static_cast<void>(list.frame(feed_row_x, 250.0F, true, surfaces));
  static_cast<void>(list.frame(feed_row_x, 270.0F, true, surfaces));
  static_cast<void>(list.frame(feed_row_x, 295.0F, true, surfaces));
  const auto reordered = list.frame(feed_row_x, 295.0F, false, surfaces);
  MAPPING_CHECK(reordered.organize.has_value());
  MAPPING_CHECK(reordered.organize->order ==
                (std::vector<doc::NodeId>{doc::NodeId{71U}, doc::NodeId{70U}}));
  // Dropped on the surface list, it is not also assigned to an output.
  MAPPING_CHECK(!reordered.projection_edit.has_value());
}

void selected_feed_regions_return_undoable_delete_requests() {
  FeedHarness harness;
  const output::FeedMap map = feed_demo();
  constexpr float row_x = 96.0F;
  constexpr float row_y = 101.0F;
  static_cast<void>(harness.frame(row_x, row_y, false, map));
  static_cast<void>(harness.frame(row_x, row_y, true, map));
  const auto selected = harness.frame(row_x, row_y, false, map);
  MAPPING_CHECK(selected.selection == doc::NodeId{10U});

  // A re-click is surfaced so the shell can restore Feed Mapping as the
  // global Delete domain after another editor surface had focus.
  static_cast<void>(harness.frame(row_x, row_y, true, map));
  const auto reselected = harness.frame(row_x, row_y, false, map);
  MAPPING_CHECK(reselected.selection == doc::NodeId{10U});

  const auto key_delete =
      harness.frame(row_x, row_y, false, map, gui::Key::Delete);
  MAPPING_CHECK(key_delete.remove ==
                editor::FeedMapDeleteRequest{doc::NodeId{10U}});
  MAPPING_CHECK(key_delete.consume_delete);
  const auto idle = harness.frame(row_x, row_y, false, map);
  MAPPING_CHECK(!idle.remove.has_value());
  MAPPING_CHECK(!idle.consume_delete);

  // The same dedicated action column used by Library assets lives on the
  // right edge of each output row; it does not also select the row.
  constexpr float remove_x = 172.0F;
  constexpr float remove_y = 101.0F;
  static_cast<void>(harness.frame(remove_x, remove_y, false, map));
  static_cast<void>(harness.frame(remove_x, remove_y, true, map));
  const auto visible_delete = harness.frame(remove_x, remove_y, false, map);
  MAPPING_CHECK(visible_delete.remove ==
                editor::FeedMapDeleteRequest{doc::NodeId{10U}});
}

void feed_mapping_creates_a_canvas_sized_front_surface() {
  FeedHarness harness;
  const output::FeedMap map = feed_demo();
  // The physical-surface section follows the two output rows in the left
  // rail. Its add action is separate from + ADD REGION.
  constexpr float add_x = 96.0F;
  constexpr float add_y = 195.0F;
  static_cast<void>(harness.frame(add_x, add_y, false, map));
  static_cast<void>(harness.frame(add_x, add_y, true, map));
  const auto added = harness.frame(add_x, add_y, false, map);

  MAPPING_CHECK(added.surface_add.has_value());
  MAPPING_CHECK(added.surface_add->name == "LED Surface 1");
  MAPPING_CHECK(added.surface_add->surface.center ==
                scene::Vec3{0.0F, 0.0F, -4.0F});
  MAPPING_CHECK(added.surface_add->surface.right ==
                scene::Vec3{1.0F, 0.0F, 0.0F});
  MAPPING_CHECK(added.surface_add->surface.up == scene::Vec3{0.0F, 1.0F, 0.0F});
  MAPPING_CHECK(added.surface_add->surface.width == 8.0F);
  MAPPING_CHECK(added.surface_add->surface.height == 4.5F);
  MAPPING_CHECK(added.surface_add->surface.raster == FeedHarness::canvas);
  MAPPING_CHECK(added.surface_add->shape ==
                output::ProjectionSurfaceShape::Planar);
  MAPPING_CHECK(added.surface_add->horizontal_arc_turns == 0.25F);
  MAPPING_CHECK(added.surface_add->mesh_columns == 32U);
  MAPPING_CHECK(added.surface_add->mesh_source.empty());
  MAPPING_CHECK(added.surface_add->mesh_scale == 1.0F);
}

void imported_mesh_add_and_browse_are_explicit_panel_contracts() {
  FeedHarness harness;
  const output::FeedMap map = feed_surface_demo();

  // The common workflow is one externally split mesh per feed. Selecting a
  // region before import carries that output identity into the compound
  // surface-add/assignment request.
  const gui::Rect fitted =
      editor::feed_mapping_canvas(FeedHarness::bounds, FeedHarness::canvas);
  const gui::Rect selected_region = editor::feed_map_screen_rect(
      fitted, FeedHarness::canvas, map.maps[0U].source);
  const float selected_x = selected_region.x + selected_region.width * 0.5F;
  const float selected_y = selected_region.y + selected_region.height * 0.5F;
  static_cast<void>(harness.frame(selected_x, selected_y, false, map));
  static_cast<void>(harness.frame(selected_x, selected_y, true, map));
  static_cast<void>(harness.frame(selected_x, selected_y, false, map));

  // Import is distinct from adding a parametric surface. The empty source is
  // intentional: EditorShell owns the native chooser and only forwards this
  // request after a successful UV-mesh validation.
  constexpr float import_x = 96.0F;
  constexpr float import_y = 222.0F;
  static_cast<void>(harness.frame(import_x, import_y, false, map));
  static_cast<void>(harness.frame(import_x, import_y, true, map));
  const auto imported = harness.frame(import_x, import_y, false, map);
  MAPPING_CHECK(imported.surface_add.has_value());
  MAPPING_CHECK(imported.surface_add->name == "UV Mesh 3");
  MAPPING_CHECK(imported.surface_add->shape ==
                output::ProjectionSurfaceShape::ImportedMesh);
  MAPPING_CHECK(imported.surface_add->mesh_source.empty());
  MAPPING_CHECK(imported.surface_add->mesh_scale == 1.0F);
  MAPPING_CHECK(imported.surface_add->surface.center == scene::Vec3{});
  MAPPING_CHECK(imported.surface_add->assign_map_node == doc::NodeId{10U});

  constexpr float row_x = 96.0F;
  constexpr float row_y = 101.0F;
  static_cast<void>(harness.frame(row_x, row_y, false, map));
  static_cast<void>(harness.frame(row_x, row_y, true, map));
  static_cast<void>(harness.frame(row_x, row_y, false, map));

  // Choosing UV MESH locally does not publish an invalid imported surface
  // with an empty source. The explicit chooser returns a complete value for
  // the shell to fill and validate.
  constexpr float mesh_shape_x = 750.0F;
  constexpr float shape_y = 232.0F;
  static_cast<void>(harness.frame(mesh_shape_x, shape_y, false, map));
  static_cast<void>(harness.frame(mesh_shape_x, shape_y, true, map));
  const auto selected = harness.frame(mesh_shape_x, shape_y, false, map);
  MAPPING_CHECK(!selected.surface_edit.has_value());

  constexpr float center_x = 650.0F;
  constexpr float center_y = 286.0F;
  static_cast<void>(harness.frame(center_x, center_y, false, map));
  static_cast<void>(harness.frame(center_x, center_y, true, map));
  const auto draft = harness.frame(center_x + 10.0F, center_y, true, map);
  MAPPING_CHECK(!draft.surface_edit.has_value());
  static_cast<void>(harness.frame(center_x + 10.0F, center_y, false, map));

  constexpr float browse_x = 700.0F;
  constexpr float browse_y = 356.0F;
  static_cast<void>(harness.frame(browse_x, browse_y, false, map));
  static_cast<void>(harness.frame(browse_x, browse_y, true, map));
  const auto browse = harness.frame(browse_x, browse_y, false, map);
  MAPPING_CHECK(browse.mesh_browse.has_value());
  MAPPING_CHECK(browse.mesh_browse->node == doc::NodeId{70U});
  MAPPING_CHECK(browse.mesh_browse->name == "Main Wall");
  MAPPING_CHECK(std::abs(browse.mesh_browse->surface.center.x - 0.1F) <
                0.0001F);
  MAPPING_CHECK(browse.mesh_browse->transform_components ==
                editor::FeedSurfaceTransform::None);
  MAPPING_CHECK(browse.mesh_browse->shape ==
                output::ProjectionSurfaceShape::ImportedMesh);
  MAPPING_CHECK(browse.mesh_browse->mesh_source.empty());
  MAPPING_CHECK(browse.mesh_browse->mesh_scale == 1.0F);

  output::FeedMap resolved = map;
  resolved.surfaces[0U].name = "main-wall";
  resolved.surfaces[0U].geometry = browse.mesh_browse->surface;
  resolved.surfaces[0U].shape = output::ProjectionSurfaceShape::ImportedMesh;
  resolved.surfaces[0U].mesh_source = "calibration/main-wall.glb";
  resolved.surfaces[0U].mesh_scale = 0.5F;
  static_cast<void>(harness.frame(browse_x, browse_y, false, resolved));

  // Once the shell-authored document comes back, the panel reconciles that
  // externally filled path without disturbing ordinary live numeric scrubs.
  constexpr float scale_x = 700.0F;
  constexpr float scale_y = 383.0F;
  static_cast<void>(harness.frame(scale_x, scale_y, false, resolved));
  static_cast<void>(harness.frame(scale_x, scale_y, true, resolved));
  const auto scale = harness.frame(scale_x + 10.0F, scale_y, true, resolved);
  MAPPING_CHECK(scale.surface_edit.has_value());
  MAPPING_CHECK(scale.surface_edit->mesh_source == "calibration/main-wall.glb");
  MAPPING_CHECK(std::abs(scale.surface_edit->mesh_scale - 0.6F) < 0.0001F);
}

void imported_mesh_shape_scale_and_reset_preserve_complete_values() {
  FeedHarness harness;
  output::FeedMap map = feed_surface_demo();
  map.surfaces[0U].mesh_source = "calibration/main-wall.glb";
  map.surfaces[0U].mesh_scale = 0.5F;
  constexpr float row_x = 96.0F;
  constexpr float row_y = 101.0F;
  static_cast<void>(harness.frame(row_x, row_y, false, map));
  static_cast<void>(harness.frame(row_x, row_y, true, map));
  static_cast<void>(harness.frame(row_x, row_y, false, map));

  constexpr float mesh_shape_x = 750.0F;
  constexpr float shape_y = 232.0F;
  static_cast<void>(harness.frame(mesh_shape_x, shape_y, false, map));
  static_cast<void>(harness.frame(mesh_shape_x, shape_y, true, map));
  const auto shape = harness.frame(mesh_shape_x, shape_y, false, map);
  MAPPING_CHECK(shape.surface_edit.has_value());
  MAPPING_CHECK(shape.surface_edit->shape ==
                output::ProjectionSurfaceShape::ImportedMesh);
  MAPPING_CHECK(shape.surface_edit->mesh_source == "calibration/main-wall.glb");
  MAPPING_CHECK(shape.surface_edit->mesh_scale == 0.5F);

  constexpr float scale_x = 700.0F;
  constexpr float scale_y = 383.0F;
  static_cast<void>(harness.frame(scale_x, scale_y, false, map));
  static_cast<void>(harness.frame(scale_x, scale_y, true, map));
  const auto scaled = harness.frame(scale_x + 20.0F, scale_y, true, map);
  MAPPING_CHECK(scaled.surface_edit.has_value());
  MAPPING_CHECK(scaled.surface_edit->shape ==
                output::ProjectionSurfaceShape::ImportedMesh);
  MAPPING_CHECK(scaled.surface_edit->mesh_source ==
                "calibration/main-wall.glb");
  MAPPING_CHECK(std::abs(scaled.surface_edit->mesh_scale - 0.7F) < 0.0001F);
  static_cast<void>(harness.frame(scale_x + 20.0F, scale_y, false, map));

  harness.panel.reset_project_state();
  const output::FeedMap replacement = feed_surface_demo();
  static_cast<void>(harness.frame(row_x, row_y, false, replacement));
  static_cast<void>(harness.frame(row_x, row_y, true, replacement));
  static_cast<void>(harness.frame(row_x, row_y, false, replacement));
  constexpr float center_x = 650.0F;
  constexpr float center_y = 286.0F;
  static_cast<void>(harness.frame(center_x, center_y, false, replacement));
  static_cast<void>(harness.frame(center_x, center_y, true, replacement));
  const auto reset_edit =
      harness.frame(center_x + 10.0F, center_y, true, replacement);
  MAPPING_CHECK(reset_edit.surface_edit.has_value());
  MAPPING_CHECK(reset_edit.surface_edit->shape ==
                output::ProjectionSurfaceShape::Planar);
  MAPPING_CHECK(reset_edit.surface_edit->mesh_source.empty());
  MAPPING_CHECK(reset_edit.surface_edit->mesh_scale == 1.0F);
}

void imported_mesh_uv0_keeps_the_destination_viewport_editable() {
  FeedHarness harness;
  output::FeedMap map = feed_surface_demo();
  output::ProjectionSurface &surface = map.surfaces[0U];
  surface.shape = output::ProjectionSurfaceShape::ImportedMesh;
  surface.mesh_source = "calibration/feed-0.glb";
  surface.mesh_scale = 1.0F;
  output::OutputMap &feed = map.maps[0U];
  feed.projection = output::FeedProjection::PlanarSurface;
  feed.surface = surface.node;
  feed.source = scene::whole(surface.geometry.raster);
  feed.resolved_surface = output::ResolvedProjectionSurface{
      surface.geometry,     surface.shape,       surface.horizontal_arc_turns,
      surface.mesh_columns, surface.mesh_source, surface.mesh_scale};

  constexpr float row_x = 96.0F;
  constexpr float row_y = 101.0F;
  static_cast<void>(harness.frame(row_x, row_y, false, map));
  static_cast<void>(harness.frame(row_x, row_y, true, map));
  static_cast<void>(harness.frame(row_x, row_y, false, map));

  const gui::Rect fitted =
      editor::feed_mapping_canvas(FeedHarness::bounds, surface.geometry.raster);
  const gui::Rect viewport = editor::feed_map_screen_rect(
      fitted, surface.geometry.raster, feed.viewport);
  const float centre_x = viewport.x + viewport.width * 0.5F;
  const float centre_y = viewport.y + viewport.height * 0.5F;
  static_cast<void>(harness.frame(centre_x, centre_y, false, map));
  const auto pressed = harness.frame(centre_x, centre_y, true, map);
  MAPPING_CHECK(pressed.selection == doc::NodeId{10U});
  const auto held =
      harness.frame(centre_x + 40.0F, centre_y + 20.0F, true, map);
  MAPPING_CHECK(held.dragging);
  const auto released =
      harness.frame(centre_x + 40.0F, centre_y + 20.0F, false, map);
  MAPPING_CHECK(released.edit.has_value());
  MAPPING_CHECK(released.edit->viewport != feed.source);
  MAPPING_CHECK(feed.source == scene::whole(surface.geometry.raster));
}

void uv_wireframe_preview_reloads_explicit_same_path_replacements() {
  ProjectionMeshFixture files;
  const std::string source = files.write("connector.obj", "v -1 1 0\n"
                                                          "v -1 -1 0\n"
                                                          "v 1 1 0\n"
                                                          "vt 0 1\n"
                                                          "vt 0 0\n"
                                                          "vt 1 1\n"
                                                          "f 1/1 2/2 3/3\n");

  output::FeedMap map = feed_surface_demo();
  output::ProjectionSurface &surface = map.surfaces[0U];
  surface.shape = output::ProjectionSurfaceShape::ImportedMesh;
  surface.mesh_source = source;
  surface.mesh_scale = 1.0F;
  output::OutputMap &feed = map.maps[0U];
  feed.projection = output::FeedProjection::PlanarSurface;
  feed.surface = surface.node;
  feed.source = scene::whole(surface.geometry.raster);
  feed.resolved_surface = output::ResolvedProjectionSurface{
      surface.geometry,     surface.shape,       surface.horizontal_arc_turns,
      surface.mesh_columns, surface.mesh_source, surface.mesh_scale};

  FeedHarness harness;
  constexpr float row_x = 96.0F;
  constexpr float row_y = 101.0F;
  static_cast<void>(harness.frame(row_x, row_y, false, map));
  static_cast<void>(harness.frame(row_x, row_y, true, map));
  static_cast<void>(harness.frame(row_x, row_y, false, map));
  static_cast<void>(harness.frame(400.0F, 40.0F, false, map));
  MAPPING_CHECK(
      editor::detail::wait_for_feed_mapping_projection_cache_for_test(
          harness.panel));
  static_cast<void>(harness.frame(400.0F, 40.0F, false, map));
  const std::size_t one_triangle = harness.list.vertices().size();

  static_cast<void>(files.write("connector.obj", "v -1 1 0\n"
                                                 "v -1 -1 0\n"
                                                 "v 1 1 0\n"
                                                 "v 1 -1 0\n"
                                                 "vt 0 1\n"
                                                 "vt 0 0\n"
                                                 "vt 1 1\n"
                                                 "vt 1 0\n"
                                                 "f 1/1 2/2 3/3\n"
                                                 "f 3/3 2/2 4/4\n"));
  static_cast<void>(harness.frame(400.0F, 40.0F, false, map));
  MAPPING_CHECK(harness.list.vertices().size() == one_triangle);

  // REPLACE can return the same pathname after an external tool overwrote its
  // contents. Clicking it must invalidate both the decoded mesh and its key.
  constexpr float browse_x = 700.0F;
  constexpr float browse_y = 356.0F;
  static_cast<void>(harness.frame(browse_x, browse_y, false, map));
  static_cast<void>(harness.frame(browse_x, browse_y, true, map));
  const auto browse = harness.frame(browse_x, browse_y, false, map);
  MAPPING_CHECK(browse.mesh_browse.has_value());
  MAPPING_CHECK(
      editor::detail::wait_for_feed_mapping_projection_cache_for_test(
          harness.panel));
  static_cast<void>(harness.frame(400.0F, 40.0F, false, map));
  MAPPING_CHECK(harness.list.vertices().size() > one_triangle);
}

void output_regions_switch_projection_and_take_dragged_surfaces() {
  FeedHarness harness;
  const output::FeedMap map = feed_surface_demo();
  constexpr float row_x = 96.0F;
  constexpr float row_y = 101.0F;
  static_cast<void>(harness.frame(row_x, row_y, false, map));
  static_cast<void>(harness.frame(row_x, row_y, true, map));
  static_cast<void>(harness.frame(row_x, row_y, false, map));

  // The projection mode buttons occupy the top of the right details rail.
  constexpr float physical_x = 735.0F;
  constexpr float mode_y = 80.0F;
  static_cast<void>(harness.frame(physical_x, mode_y, false, map));
  static_cast<void>(harness.frame(physical_x, mode_y, true, map));
  const auto physical = harness.frame(physical_x, mode_y, false, map);
  MAPPING_CHECK(physical.projection_edit ==
                editor::FeedProjectionEdit{
                    doc::NodeId{10U}, output::FeedProjection::PlanarSurface,
                    doc::NodeId{70U},
                    scene::CanvasRect{0U, 0U, 1920U, 1080U}});

  output::FeedMap assigned = map;
  assigned.maps[0U].projection = output::FeedProjection::PlanarSurface;
  assigned.maps[0U].surface = doc::NodeId{70U};
  assigned.maps[0U].source =
      scene::whole(assigned.surfaces[0U].geometry.raster);
  assigned.maps[0U].resolved_surface =
      output::ResolvedProjectionSurface{assigned.surfaces[0U].geometry};

  // Assignment is a drag out of the surface list onto the output that surface
  // drives - here the second surface onto the first output's row. It changes
  // both the reusable surface reference and the region's pixel domain to that
  // surface's complete raster, and reports the same selection a click would.
  constexpr float surface_x = 60.0F;
  constexpr float second_surface_y = 288.0F;
  static_cast<void>(harness.frame(surface_x, second_surface_y, false, assigned));
  static_cast<void>(harness.frame(surface_x, second_surface_y, true, assigned));
  const auto lifted = harness.frame(row_x, row_y, true, assigned);
  MAPPING_CHECK(!lifted.projection_edit.has_value());
  const auto dropped = harness.frame(row_x, row_y, false, assigned);
  MAPPING_CHECK(dropped.projection_edit ==
                editor::FeedProjectionEdit{
                    doc::NodeId{10U}, output::FeedProjection::PlanarSurface,
                    doc::NodeId{71U},
                    scene::CanvasRect{0U, 0U, 1280U, 1080U}});
  MAPPING_CHECK(dropped.selection == doc::NodeId{10U});
  MAPPING_CHECK(dropped.surface_selection == doc::NodeId{71U});

  // A press that never travels is still an ordinary selection, and authors
  // nothing.
  static_cast<void>(harness.frame(surface_x, second_surface_y, false, assigned));
  static_cast<void>(harness.frame(surface_x, second_surface_y, true, assigned));
  const auto picked =
      harness.frame(surface_x, second_surface_y, false, assigned);
  MAPPING_CHECK(!picked.projection_edit.has_value());
  MAPPING_CHECK(picked.surface_selection == doc::NodeId{71U});

  // Released clear of every output, a drag assigns nothing.
  static_cast<void>(harness.frame(surface_x, second_surface_y, false, assigned));
  static_cast<void>(harness.frame(surface_x, second_surface_y, true, assigned));
  static_cast<void>(harness.frame(surface_x, 400.0F, true, assigned));
  const auto missed = harness.frame(surface_x, 400.0F, false, assigned);
  MAPPING_CHECK(!missed.projection_edit.has_value());
}

void calibrated_surfaces_emit_whole_value_edits_and_safe_deletes() {
  FeedHarness harness;
  const output::FeedMap map = feed_surface_demo();
  constexpr float row_x = 96.0F;
  constexpr float row_y = 101.0F;
  static_cast<void>(harness.frame(row_x, row_y, false, map));
  static_cast<void>(harness.frame(row_x, row_y, true, map));
  static_cast<void>(harness.frame(row_x, row_y, false, map));

  output::FeedMap animated = map;
  animated.surfaces[0U].geometry.center.x = 10.0F;
  static_cast<void>(
      harness.frame(row_x, row_y, false, animated, std::nullopt, 42U));

  // Scrub centre X in the calibration rail. The idle field follows the
  // evaluated playhead value before the gesture begins rather than retaining
  // the surface's frame-zero base.
  constexpr float center_x = 650.0F;
  constexpr float center_y = 286.0F;
  static_cast<void>(
      harness.frame(center_x, center_y, false, animated, std::nullopt, 42U));
  static_cast<void>(
      harness.frame(center_x, center_y, true, animated, std::nullopt, 42U));
  const auto changed = harness.frame(center_x + 20.0F, center_y, true, animated,
                                     std::nullopt, 42U);
  MAPPING_CHECK(changed.surface_edit.has_value());
  MAPPING_CHECK(changed.surface_edit->node == doc::NodeId{70U});
  MAPPING_CHECK(changed.surface_edit->name == "Main Wall");
  MAPPING_CHECK(std::abs(changed.surface_edit->surface.center.x - 10.2F) <
                0.0001F);
  MAPPING_CHECK(changed.surface_edit->transform_components ==
                editor::FeedSurfaceTransform::PositionX);
  MAPPING_CHECK(changed.surface_edit->surface.raster ==
                scene::Canvas{1920U, 1080U});
  MAPPING_CHECK(changed.editing);
  static_cast<void>(harness.frame(center_x + 20.0F, center_y, false, animated,
                                  std::nullopt, 42U));

  constexpr float yaw_x = 700.0F;
  constexpr float yaw_y = 313.0F;
  static_cast<void>(
      harness.frame(yaw_x, yaw_y, false, animated, std::nullopt, 42U));
  static_cast<void>(
      harness.frame(yaw_x, yaw_y, true, animated, std::nullopt, 42U));
  const auto yaw = harness.frame(yaw_x + 40.0F, yaw_y, true, animated,
                                 std::nullopt, 42U);
  MAPPING_CHECK(yaw.surface_edit.has_value());
  MAPPING_CHECK(yaw.surface_edit->transform_components ==
                editor::FeedSurfaceTransform::Yaw);
  static_cast<void>(harness.frame(yaw_x + 40.0F, yaw_y, false, animated,
                                  std::nullopt, 42U));

  // Surface deletion is the dedicated action at the right of its list row,
  // matching imported assets. It names only the calibrated surface; output
  // regions remain visible as missing and can be repaired.
  constexpr float delete_x = 172.0F;
  constexpr float delete_y = 255.0F;
  static_cast<void>(
      harness.frame(delete_x, delete_y, false, animated, std::nullopt, 42U));
  static_cast<void>(
      harness.frame(delete_x, delete_y, true, animated, std::nullopt, 42U));
  const auto removed = harness.frame(delete_x, delete_y, false, animated,
                                     std::nullopt, 42U);
  MAPPING_CHECK(removed.surface_remove ==
                editor::FeedSurfaceDeleteRequest{doc::NodeId{70U}});
  MAPPING_CHECK(!removed.remove.has_value());
  MAPPING_CHECK(!removed.projection_edit.has_value());
}

void wall_colour_controls_author_only_the_selected_surface() {
  FeedHarness harness;
  auto map = feed_surface_demo();
  const auto click = [&](float x, float y) {
    static_cast<void>(harness.frame(x, y, false, map));
    static_cast<void>(harness.frame(x, y, true, map));
    return harness.frame(x, y, false, map);
  };
  static_cast<void>(click(96.0F, 101.0F));
  static_cast<void>(click(730.0F, 203.0F));
  const auto enabled = click(615.0F, 232.0F);
  MAPPING_CHECK(enabled.surface_edit.has_value());
  MAPPING_CHECK(enabled.surface_edit->node == doc::NodeId{70U});
  MAPPING_CHECK(enabled.surface_edit->color.has_value());
  MAPPING_CHECK(enabled.surface_edit->color->enabled);
  MAPPING_CHECK(enabled.surface_edit->surface == map.surfaces[0].geometry);
  map.surfaces[0].color = *enabled.surface_edit->color;
  static_cast<void>(harness.frame(650.0F, 288.0F, false, map));
  static_cast<void>(harness.frame(650.0F, 288.0F, true, map));
  const auto matrix = harness.frame(670.0F, 288.0F, true, map);
  MAPPING_CHECK(matrix.surface_edit.has_value() && matrix.surface_edit->color.has_value());
  MAPPING_CHECK(std::abs(matrix.surface_edit->color->primaries[0] - 1.02F) < 0.0001F);
  MAPPING_CHECK(matrix.surface_edit->color->primaries[4] == 1.0F);
  static_cast<void>(harness.frame(670.0F, 288.0F, false, map));
  map.surfaces[0].color.primaries[0] = 0.75F;
  const auto replicated = click(615.0F, 232.0F);
  MAPPING_CHECK(replicated.surface_edit.has_value() && replicated.surface_edit->color.has_value());
  MAPPING_CHECK(replicated.surface_edit->color->primaries[0] == 0.75F);
  static_cast<void>(click(96.0F, 286.0F));
  const auto other = click(615.0F, 232.0F);
  MAPPING_CHECK(other.surface_edit.has_value() && other.surface_edit->color.has_value());
  MAPPING_CHECK(other.surface_edit->node == doc::NodeId{71U});
  MAPPING_CHECK(other.surface_edit->color->primaries[0] == 1.0F);
}

void horizontal_arc_shape_and_mesh_controls_emit_complete_surface_edits() {
  FeedHarness harness;
  const output::FeedMap map = feed_surface_demo();
  constexpr float row_x = 96.0F;
  constexpr float row_y = 101.0F;
  static_cast<void>(harness.frame(row_x, row_y, false, map));
  static_cast<void>(harness.frame(row_x, row_y, true, map));
  static_cast<void>(harness.frame(row_x, row_y, false, map));

  // Shape choices are the first row of Surface Calibration. Changing shape
  // preserves the physical calibration and authors the mesh defaults in the
  // same value request.
  constexpr float arc_shape_x = 685.0F;
  constexpr float shape_y = 232.0F;
  static_cast<void>(harness.frame(arc_shape_x, shape_y, false, map));
  static_cast<void>(harness.frame(arc_shape_x, shape_y, true, map));
  const auto shape = harness.frame(arc_shape_x, shape_y, false, map);
  MAPPING_CHECK(shape.surface_edit.has_value());
  MAPPING_CHECK(shape.surface_edit->node == doc::NodeId{70U});
  MAPPING_CHECK(shape.surface_edit->name == "Main Wall");
  MAPPING_CHECK(shape.surface_edit->surface == map.surfaces[0U].geometry);
  MAPPING_CHECK(shape.surface_edit->shape ==
                output::ProjectionSurfaceShape::HorizontalArc);
  MAPPING_CHECK(shape.surface_edit->horizontal_arc_turns == 0.25F);
  MAPPING_CHECK(shape.surface_edit->mesh_columns == 32U);
  MAPPING_CHECK(shape.surface_edit->mesh_source.empty());
  MAPPING_CHECK(shape.surface_edit->mesh_scale == 1.0F);
  MAPPING_CHECK(shape.surface_edit->transform_components ==
                editor::FeedSurfaceTransform::None);

  // The arc is shown in degrees while the authored value remains turns.
  // Forty pixels at 0.25 degrees/pixel changes the default 90-degree arc to
  // 100 degrees.
  constexpr float bend_x = 670.0F;
  constexpr float arc_fields_y = 340.0F;
  static_cast<void>(harness.frame(bend_x, arc_fields_y, false, map));
  static_cast<void>(harness.frame(bend_x, arc_fields_y, true, map));
  const auto bend = harness.frame(bend_x + 40.0F, arc_fields_y, true, map);
  MAPPING_CHECK(bend.surface_edit.has_value());
  MAPPING_CHECK(bend.surface_edit->shape ==
                output::ProjectionSurfaceShape::HorizontalArc);
  MAPPING_CHECK(std::abs(bend.surface_edit->horizontal_arc_turns -
                         (100.0F / 360.0F)) < 0.0001F);
  MAPPING_CHECK(bend.surface_edit->mesh_columns == 32U);
  static_cast<void>(harness.frame(bend_x + 40.0F, arc_fields_y, false, map));

  constexpr float columns_x = 750.0F;
  static_cast<void>(harness.frame(columns_x, arc_fields_y, false, map));
  static_cast<void>(harness.frame(columns_x, arc_fields_y, true, map));
  const auto columns = harness.frame(columns_x + 5.0F, arc_fields_y, true, map);
  MAPPING_CHECK(columns.surface_edit.has_value());
  MAPPING_CHECK(columns.surface_edit->shape ==
                output::ProjectionSurfaceShape::HorizontalArc);
  MAPPING_CHECK(std::abs(columns.surface_edit->horizontal_arc_turns -
                         (100.0F / 360.0F)) < 0.0001F);
  MAPPING_CHECK(columns.surface_edit->mesh_columns == 37U);
  MAPPING_CHECK(columns.editing);
}

void project_replacement_clears_mapping_document_state() {
  HardwareHarness hardware;
  topology::HardwareMap hardware_map{};
  hardware_map.devices.push_back(device(
      4U, topology::HardwareDeviceKind::OperatorMachine, "Operator", 0, 0));
  const editor::IsometricProjection projection =
      hardware.panel.view_projection(HardwareHarness::bounds);
  const editor::IsoPoint device_point =
      editor::project_isometric(projection, 0, 0);

  static_cast<void>(
      hardware.frame(device_point.x, device_point.y, false, hardware_map));
  static_cast<void>(
      hardware.frame(device_point.x, device_point.y, true, hardware_map));
  static_cast<void>(
      hardware.frame(device_point.x, device_point.y, false, hardware_map));
  MAPPING_CHECK(hardware.panel.selection().kind ==
                editor::HardwareSelectionKind::Device);

  hardware.panel.reset_project_state();
  MAPPING_CHECK(hardware.panel.selection() == editor::HardwareSelection{});
  MAPPING_CHECK(!hardware.panel.pending_link().has_value());

  FeedHarness feed;
  const output::FeedMap feed_map = feed_demo();
  const gui::Rect fitted =
      editor::feed_mapping_canvas(FeedHarness::bounds, FeedHarness::canvas);
  const gui::Rect rectangle = editor::feed_map_screen_rect(
      fitted, FeedHarness::canvas, feed_map.maps[0U].source);
  const float centre_x = rectangle.x + rectangle.width * 0.5F;
  const float centre_y = rectangle.y + rectangle.height * 0.5F;

  static_cast<void>(feed.frame(centre_x, centre_y, false, feed_map));
  static_cast<void>(feed.frame(centre_x, centre_y, true, feed_map));
  static_cast<void>(feed.frame(centre_x, centre_y, false, feed_map));
  MAPPING_CHECK(feed.panel.selected_map_node() == doc::NodeId{10U});

  feed.panel.reset_project_state();
  MAPPING_CHECK(!feed.panel.selected_map_node().has_value());
}

// Render machines found on the network: a strip over the bottom of the
// canvas whose cards place a Render Server already describing the machine.
topology::HardwareDiscoveredMachine detected_machine(std::string hostname,
                                                     std::string address,
                                                     core::u32 node_index,
                                                     core::u32 control_port) {
  topology::HardwareDiscoveredMachine machine{};
  machine.hostname = std::move(hostname);
  machine.address = address;
  machine.addresses = {std::move(address)};
  machine.node_index = node_index;
  machine.control_port = control_port;
  return machine;
}

topology::HardwareRuntimeSnapshot
discovering(std::vector<topology::HardwareDiscoveredMachine> machines,
            bool password_set = false) {
  topology::HardwareRuntimeSnapshot runtime{};
  runtime.discovery.state = topology::HardwareDiscoveryState::Searching;
  runtime.discovery.port = 47'839U;
  runtime.discovery.password_set = password_set;
  runtime.discovery.machines = std::move(machines);
  return runtime;
}

topology::HardwareDevice render_server_card(core::u64 node,
                                            std::string address,
                                            core::u32 node_index,
                                            core::i32 grid_x = 0,
                                            core::i32 grid_y = 0) {
  topology::HardwareDevice device{};
  device.node = doc::NodeId{node};
  device.kind = topology::HardwareDeviceKind::RenderServer;
  device.name = "Render Server";
  device.address = std::move(address);
  device.node_index = node_index;
  device.grid_x = grid_x;
  device.grid_y = grid_y;
  return device;
}

void a_detected_machine_drags_onto_the_map_already_configured() {
  HardwareHarness harness;
  const topology::HardwareMap empty;
  const auto runtime =
      discovering({detected_machine("RENDER-02", "192.0.2.20", 2U, 47'842U)});
  const gui::Rect card = editor::hardware_discovered_card_bounds(
      HardwareHarness::bounds, runtime.discovery, 0U);
  MAPPING_CHECK(card.width > 0.0F);
  MAPPING_CHECK(editor::hardware_discovered_status_text(
                    empty, runtime.discovery,
                    runtime.discovery.machines.front()) ==
                "DRAG OR CLICK TO ADD");
  const editor::IsoPoint drop = editor::project_isometric(
      editor::hardware_mapping_projection(HardwareHarness::bounds), 1, 1);

  static_cast<void>(harness.runtime_frame(card.x + 30.0F, card.y + 20.0F,
                                          false, empty, runtime));
  static_cast<void>(harness.runtime_frame(card.x + 30.0F, card.y + 20.0F,
                                          true, empty, runtime));
  // The machine may stop answering mid-drag; the drop still lands.
  static_cast<void>(
      harness.runtime_frame(drop.x, drop.y, true, empty, discovering({})));
  const auto released =
      harness.runtime_frame(drop.x, drop.y, false, empty, discovering({}));
  MAPPING_CHECK(released.place.has_value());
  MAPPING_CHECK(released.place->kind ==
                topology::HardwareDeviceKind::RenderServer);
  MAPPING_CHECK(released.place->grid_x == 1);
  MAPPING_CHECK(released.place->grid_y == 1);
  MAPPING_CHECK(released.place->machine ==
                editor::HardwarePlaceMachine{"RENDER-02", "192.0.2.20", 2U});
  // It listens where the map's control range already expects node 2.
  MAPPING_CHECK(!released.control_port_edit.has_value());

  // Released anywhere but the map or its own card, nothing is placed.
  static_cast<void>(harness.runtime_frame(card.x + 30.0F, card.y + 20.0F,
                                          true, empty, runtime));
  static_cast<void>(harness.runtime_frame(40.0F, 470.0F, true, empty, runtime));
  const auto dropped_on_palette =
      harness.runtime_frame(40.0F, 470.0F, false, empty, runtime);
  MAPPING_CHECK(!dropped_on_palette.place.has_value());
}

void a_click_adds_a_detected_machine_and_the_first_brings_its_port() {
  HardwareHarness harness;
  topology::HardwareMap map{};
  map.control = topology::HardwareControlSettings{doc::NodeId{9U}, 47'840U, true};
  // An operator machine occupies the origin, so the click finds the next cell.
  topology::HardwareDevice desk{};
  desk.node = doc::NodeId{3U};
  desk.kind = topology::HardwareDeviceKind::OperatorMachine;
  map.devices.push_back(desk);
  const auto runtime =
      discovering({detected_machine("STAGE-LEFT", "192.0.2.31", 1U, 47'900U)});
  MAPPING_CHECK(editor::hardware_discovered_status_text(
                    map, runtime.discovery,
                    runtime.discovery.machines.front()) ==
                "DRAG OR CLICK TO ADD");
  const gui::Rect card = editor::hardware_discovered_card_bounds(
      HardwareHarness::bounds, runtime.discovery, 0U);
  static_cast<void>(harness.runtime_frame(card.x + 30.0F, card.y + 20.0F,
                                          false, map, runtime));
  static_cast<void>(harness.runtime_frame(card.x + 30.0F, card.y + 20.0F,
                                          true, map, runtime));
  const auto clicked = harness.runtime_frame(card.x + 31.0F, card.y + 20.0F,
                                             false, map, runtime);
  MAPPING_CHECK(clicked.place.has_value());
  MAPPING_CHECK(clicked.place->machine ==
                editor::HardwarePlaceMachine{"STAGE-LEFT", "192.0.2.31", 1U});
  MAPPING_CHECK(clicked.place->grid_x != 0 || clicked.place->grid_y != 0);
  // No Render Server depends on the range yet, so it moves to where this
  // machine listens: 47900 for node 1.
  MAPPING_CHECK(clicked.control_port_edit ==
                editor::HardwareControlPortEdit{doc::NodeId{9U}, 47'899U});

  // Once a Render Server is on the map the range is no longer the first
  // machine's to move; the card says why it would not connect instead.
  map.devices.push_back(render_server_card(4U, "192.0.2.40", 0U, 9, 9));
  MAPPING_CHECK(editor::hardware_discovered_status_text(
                    map, runtime.discovery,
                    runtime.discovery.machines.front()) ==
                "LISTENS ON 47900, MAP USES 47841");
}

void a_detected_machine_already_on_the_map_selects_its_card() {
  HardwareHarness harness;
  topology::HardwareMap map{};
  map.devices.push_back(render_server_card(5U, "192.0.2.20", 2U));
  map.devices.push_back(render_server_card(6U, "", 0U, 6, 0));
  auto local = detected_machine("THIS-PC", "192.0.2.50", 0U, 47'840U);
  local.this_machine = true;
  const auto runtime = discovering(
      {detected_machine("RENDER-02", "192.0.2.20", 2U, 47'842U), local});
  MAPPING_CHECK(editor::hardware_discovered_device(
                    map, runtime.discovery.machines[0]) == &map.devices[0]);
  // A blank address is this machine, whichever address it answered from.
  MAPPING_CHECK(editor::hardware_discovered_device(
                    map, runtime.discovery.machines[1]) == &map.devices[1]);
  MAPPING_CHECK(editor::hardware_discovered_status_text(
                    map, runtime.discovery, runtime.discovery.machines[0]) ==
                "ON MAP - CLICK TO SELECT");

  const gui::Rect card = editor::hardware_discovered_card_bounds(
      HardwareHarness::bounds, runtime.discovery, 0U);
  static_cast<void>(harness.runtime_frame(card.x + 30.0F, card.y + 20.0F,
                                          false, map, runtime));
  const auto pressed = harness.runtime_frame(card.x + 30.0F, card.y + 20.0F,
                                             true, map, runtime);
  MAPPING_CHECK(pressed.selection ==
                editor::HardwareSelection{editor::HardwareSelectionKind::Device,
                                          doc::NodeId{5U}});
  const auto released = harness.runtime_frame(card.x + 30.0F, card.y + 20.0F,
                                              false, map, runtime);
  MAPPING_CHECK(!released.place.has_value());
}

void detected_machines_say_why_they_would_not_connect() {
  topology::HardwareMap map{};
  map.devices.push_back(render_server_card(5U, "192.0.2.99", 2U));
  auto discovery = discovering({}).discovery;
  const auto moved = detected_machine("RENDER-02", "192.0.2.20", 2U, 47'842U);
  // The card for node 2 points elsewhere - perhaps an old address of this
  // very machine. A click selects that card so it can be repaired.
  MAPPING_CHECK(editor::hardware_discovered_status_text(map, discovery, moved) ==
                "NODE 2 USED BY 192.0.2.99");

  auto unsecured = detected_machine("RENDER-03", "192.0.2.21", 3U, 47'843U);
  discovery.password_set = true;
  MAPPING_CHECK(editor::hardware_discovered_status_text(map, discovery,
                                                        unsecured) ==
                "NO PASSWORD SET ON IT");
  discovery.password_set = false;
  unsecured.password_set = true;
  MAPPING_CHECK(editor::hardware_discovered_status_text(map, discovery,
                                                        unsecured) ==
                "NEEDS THE RENDER NODE PASSWORD");
  discovery.password_set = true;
  MAPPING_CHECK(editor::hardware_discovered_status_text(map, discovery,
                                                        unsecured) ==
                "DRAG OR CLICK TO ADD");
}

void the_detection_strip_leaves_the_map_its_own_input() {
  const topology::HardwareDiscovery off{};
  MAPPING_CHECK(editor::hardware_discovery_strip_bounds(HardwareHarness::bounds,
                                                        off)
                    .height == 0.0F);
  const auto searching = discovering({});
  const gui::Rect header = editor::hardware_discovery_strip_bounds(
      HardwareHarness::bounds, searching.discovery);
  const gui::Rect canvas =
      editor::hardware_mapping_canvas(HardwareHarness::bounds);
  MAPPING_CHECK(header.height > 0.0F && header.height < 40.0F);
  MAPPING_CHECK(header.y + header.height == canvas.y + canvas.height);
  const auto found =
      discovering({detected_machine("RENDER-02", "192.0.2.20", 2U, 47'842U)});
  const gui::Rect strip = editor::hardware_discovery_strip_bounds(
      HardwareHarness::bounds, found.discovery);
  MAPPING_CHECK(strip.height > header.height);
  // A short panel keeps its map rather than the strip.
  MAPPING_CHECK(editor::hardware_discovery_strip_bounds(
                    gui::Rect{0.0F, 0.0F, 900.0F, 200.0F}, found.discovery)
                    .height == 0.0F);

  // The wheel over the strip does not zoom the map, and a palette device
  // dropped on it is not placed.
  HardwareHarness harness;
  const topology::HardwareMap empty;
  const float strip_x = strip.x + strip.width * 0.5F;
  const float strip_y = strip.y + 10.0F;
  static_cast<void>(harness.runtime_frame(strip_x, strip_y, false, empty,
                                          found, std::nullopt, false, 2.0F));
  MAPPING_CHECK(harness.panel.view_zoom() == 1.0F);
  const gui::Rect item = editor::hardware_palette_item_bounds(
      HardwareHarness::bounds, topology::HardwareDeviceKind::LedWall);
  static_cast<void>(harness.runtime_frame(item.x + 20.0F, item.y + 20.0F,
                                          false, empty, found));
  static_cast<void>(harness.runtime_frame(item.x + 20.0F, item.y + 20.0F,
                                          true, empty, found));
  static_cast<void>(
      harness.runtime_frame(strip_x, strip_y, true, empty, found));
  const auto released =
      harness.runtime_frame(strip_x, strip_y, false, empty, found);
  MAPPING_CHECK(!released.place.has_value());
}

void detected_machines_past_the_strip_scroll_into_view() {
  HardwareHarness harness;
  const topology::HardwareMap empty;
  const auto runtime = discovering(
      {detected_machine("A", "192.0.2.1", 1U, 47'841U),
       detected_machine("B", "192.0.2.2", 2U, 47'842U),
       detected_machine("C", "192.0.2.3", 3U, 47'843U),
       detected_machine("D", "192.0.2.4", 4U, 47'844U)});
  const gui::Rect strip = editor::hardware_discovery_strip_bounds(
      HardwareHarness::bounds, runtime.discovery);
  const gui::Rect first = editor::hardware_discovered_card_bounds(
      HardwareHarness::bounds, runtime.discovery, 0U);
  const gui::Rect second = editor::hardware_discovered_card_bounds(
      HardwareHarness::bounds, runtime.discovery, 1U);
  // Four cards do not fit this canvas; the arrow at the right end scrolls by
  // one card, so the first card's place now holds the second machine.
  MAPPING_CHECK(editor::hardware_discovered_card_bounds(
                    HardwareHarness::bounds, runtime.discovery, 3U)
                    .x > strip.x + strip.width);
  const float arrow_x = strip.x + strip.width - 8.0F;
  const float arrow_y = first.y + first.height * 0.5F;
  static_cast<void>(
      harness.runtime_frame(arrow_x, arrow_y, false, empty, runtime));
  static_cast<void>(
      harness.runtime_frame(arrow_x, arrow_y, true, empty, runtime));
  const auto scrolled =
      harness.runtime_frame(arrow_x, arrow_y, false, empty, runtime);
  MAPPING_CHECK(!scrolled.place.has_value());
  const float x = first.x + 40.0F;
  const float y = first.y + 20.0F;
  static_cast<void>(harness.runtime_frame(x, y, false, empty, runtime));
  static_cast<void>(harness.runtime_frame(x, y, true, empty, runtime));
  const auto clicked = harness.runtime_frame(x, y, false, empty, runtime);
  MAPPING_CHECK(clicked.place.has_value());
  MAPPING_CHECK(clicked.place->machine.has_value());
  MAPPING_CHECK(clicked.place->machine->name == "B");
  MAPPING_CHECK(second.x - first.x > 200.0F);
}

void mapping_panels_draw_without_devices_or_a_gpu() {
  HardwareHarness hardware;
  const topology::HardwareMap empty_hardware;
  static_cast<void>(hardware.frame(500.0F, 300.0F, false, empty_hardware));
  MAPPING_CHECK(hardware.list.vertices().size() > 500U);

  FeedHarness feed;
  const output::FeedMap empty_feed;
  const auto frame = feed.frame(500.0F, 300.0F, false, empty_feed);
  MAPPING_CHECK(!frame.edit.has_value());
  MAPPING_CHECK(feed.list.vertices().size() > 100U);
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 50U> cases{{
    {"UV mesh source crop is hidden while destination stays editable",
     &imported_mesh_hides_source_crop_but_keeps_destination_fields},
    {"numeric rectangles release focus when removed or hidden",
     &numeric_rectangles_release_focus_when_removed_or_hidden},
    {"source and destination pixel fields are independent",
     &numeric_rectangles_keep_source_and_destination_independent},
    {"wall colour controls author only the selected surface",
     &wall_colour_controls_author_only_the_selected_surface},
    {"isometric projection round trips grid cells",
     &isometric_projection_round_trips_grid_cells},
    {"dragging a palette device requests a grid placement",
     &dragging_a_palette_device_requests_a_grid_placement},
    {"dragging a device requests one document move",
     &dragging_a_device_requests_one_document_move},
    {"compatible ports create a link request",
     &compatible_ports_create_a_link_request},
    {"project setup authors the scene canvas",
     &project_setup_authors_the_scene_canvas},
    {"the sync policy mode cycles through every state",
     &the_sync_policy_mode_cycles_through_every_state},
    {"drawing the sync section authors nothing without a click",
     &drawing_the_sync_section_authors_nothing_without_a_click},
    {"output ports support press drag release cabling",
     &output_ports_support_press_drag_release_cabling},
    {"incompatible ports do not create a link",
     &incompatible_ports_do_not_create_a_link},
    {"cable drag distinguishes incompatible and occupied inputs",
     &cable_drag_distinguishes_incompatible_and_occupied_inputs},
    {"hardware view pans and zooms without changing authored grid space",
     &hardware_view_pans_and_zooms_without_changing_authored_grid_space},
    {"an authored cable can be selected", &an_authored_cable_can_be_selected},
    {"selected hardware objects return safe delete requests",
     &selected_hardware_objects_return_safe_delete_requests},
    {"selected device details author exact configuration properties",
     &selected_device_details_author_exact_configuration_properties},
    {"project setup authors one control port range",
     &project_setup_authors_one_control_port_range},
    {"render server details author launch geometry not control targets",
     &render_server_details_author_launch_geometry_and_not_control_targets},
    {"output deployment is created by wiring and remains repairable",
     &output_deployment_is_created_by_wiring_and_remains_repairable},
    {"live render outputs are distinct cable targets without mutating map",
     &live_render_outputs_are_distinct_cable_targets_without_mutating_map},
    {"output inventory preserves unknown zero and last good semantics",
     &output_inventory_preserves_unknown_zero_and_last_good_semantics},
    {"linked missing outputs stay compact and live details are read only",
     &linked_missing_outputs_stay_compact_and_live_details_are_read_only},
    {"feed rectangles move in canvas pixels and clamp",
     &feed_rectangles_move_in_canvas_pixels_and_clamp},
    {"a selected feed rectangle resizes from its handle",
     &a_selected_feed_rectangle_resizes_from_its_handle},
    {"hardware window rasters form one viewport mapping area",
     &hardware_window_rasters_form_one_viewport_mapping_area},
    {"one Mosaic window contains four independent viewports",
     &one_mosaic_window_contains_four_independent_viewports},
    {"feed mapping adds another viewport to the current window",
     &feed_mapping_adds_another_viewport_to_the_current_window},
    {"output identity is explicit and the empty surface prompt is inert",
     &output_identity_is_explicit_and_the_empty_surface_prompt_is_inert},
    {"feed rows rename in place", &feed_rows_rename_in_place},
    {"feed rows duplicate from their action",
     &feed_rows_duplicate_from_their_action},
    {"feed rows reorder by dragging", &feed_rows_reorder_by_dragging},
    {"selected feed regions return undoable delete requests",
     &selected_feed_regions_return_undoable_delete_requests},
    {"feed mapping creates a canvas sized front surface",
     &feed_mapping_creates_a_canvas_sized_front_surface},
    {"imported mesh add and browse are explicit panel contracts",
     &imported_mesh_add_and_browse_are_explicit_panel_contracts},
    {"imported mesh shape scale and reset preserve complete values",
     &imported_mesh_shape_scale_and_reset_preserve_complete_values},
    {"imported mesh UV0 keeps the destination viewport editable",
     &imported_mesh_uv0_keeps_the_destination_viewport_editable},
    {"UV wireframe preview reloads explicit same path replacements",
     &uv_wireframe_preview_reloads_explicit_same_path_replacements},
    {"output regions switch projection and take dragged surfaces",
     &output_regions_switch_projection_and_take_dragged_surfaces},
    {"calibrated surfaces emit whole value edits and safe deletes",
     &calibrated_surfaces_emit_whole_value_edits_and_safe_deletes},
    {"horizontal arc shape and mesh controls emit complete surface edits",
     &horizontal_arc_shape_and_mesh_controls_emit_complete_surface_edits},
    {"project replacement clears mapping document state",
     &project_replacement_clears_mapping_document_state},
    {"a detected machine drags onto the map already configured",
     &a_detected_machine_drags_onto_the_map_already_configured},
    {"a click adds a detected machine and the first brings its port",
     &a_click_adds_a_detected_machine_and_the_first_brings_its_port},
    {"a detected machine already on the map selects its card",
     &a_detected_machine_already_on_the_map_selects_its_card},
    {"detected machines say why they would not connect",
     &detected_machines_say_why_they_would_not_connect},
    {"the detection strip leaves the map its own input",
     &the_detection_strip_leaves_the_map_its_own_input},
    {"detected machines past the strip scroll into view",
     &detected_machines_past_the_strip_scroll_into_view},
    {"mapping panels draw without devices or a GPU",
     &mapping_panels_draw_without_devices_or_a_gpu},
}};

} // namespace

int main() {
  std::size_t failures = 0U;
  for (const Case &entry : cases) {
    try {
      entry.function();
      std::cout << "[pass] " << entry.name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
    }
  }
  std::cout << (cases.size() - failures) << '/' << cases.size()
            << " tests passed\n";
  return failures == 0U ? 0 : 1;
}
