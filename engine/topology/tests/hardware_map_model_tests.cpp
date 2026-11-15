#include <mediaserver/topology/hardware_map_model.hpp>

#include <mediaserver/topology/hardware_map_schema.hpp>
#include <mediaserver/topology/hardware_runtime.hpp>
#include <mediaserver/topology/hardware_signal.hpp>
#include <mediaserver/topology/hardware_walls.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <optional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace schema = mediaserver::topology::schema;
namespace topology = mediaserver::topology;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define TOPOLOGY_CHECK(...)                                                    \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

using Property = std::pair<std::string, doc::PropertyValue>;

[[nodiscard]] doc::Node node(core::u64 id, std::string_view type,
                             std::initializer_list<Property> properties) {
  doc::Node built{};
  built.id = doc::NodeId{id};
  built.type = std::string{type};
  for (const auto &property : properties) {
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

[[nodiscard]] doc::Node device(core::u64 id, std::string_view kind,
                               std::string_view name, core::i32 x,
                               core::i32 y) {
  return node(id, schema::device_type,
              {text(schema::kind, kind), text(schema::name, name),
               whole(schema::grid_x, x), whole(schema::grid_y, y),
               text(schema::address, "192.0.2.10"),
               text(schema::lifecycle_endpoint, "192.0.2.10:47840"),
               whole(schema::node_index, 2), whole(schema::width, 3840),
               whole(schema::height, 2160),
               whole(schema::refresh_numerator, 60),
               whole(schema::refresh_denominator, 1)});
}

[[nodiscard]] doc::Node link(core::u64 id, core::u64 from_device,
                             core::u8 from_port, core::u64 to_device,
                             core::u8 to_port) {
  return node(
      id, schema::link_type,
      {whole(schema::from_device, static_cast<std::int64_t>(from_device)),
       whole(schema::from_port, from_port),
       whole(schema::to_device, static_cast<std::int64_t>(to_device)),
       whole(schema::to_port, to_port)});
}

[[nodiscard]] doc::Node settings(core::u64 id, core::u32 base_port) {
  return node(id, schema::settings_type,
              {whole(schema::control_base_port,
                     static_cast<std::int64_t>(base_port))});
}

[[nodiscard]] doc::Node output_binding(
    core::u64 id, core::u64 server, core::u8 port, core::u32 feed,
    core::u64 adapter, core::u32 target, core::u32 connector,
    core::u32 width = 1920U, core::u32 height = 1080U,
    core::u32 refresh_numerator = 60U,
    core::u32 refresh_denominator = 1U) {
  return node(
      id, schema::output_binding_type,
      {whole(schema::render_server, static_cast<std::int64_t>(server)),
       whole(schema::logical_port, port),
       whole(schema::feed_index, feed),
       text(schema::display_adapter_id,
            topology::format_display_adapter_id(adapter)),
       whole(schema::display_target_id, target),
       whole(schema::connector_instance, connector),
       whole(schema::width, width), whole(schema::height, height),
       whole(schema::refresh_numerator, refresh_numerator),
       whole(schema::refresh_denominator, refresh_denominator),
       text(schema::window_mode, schema::fullscreen_window_mode)});
}

[[nodiscard]] std::vector<doc::Node> configured_chain() {
  return {
      device(1U, schema::operator_machine_kind, "FOH Operator", -3, 0),
      device(2U, schema::render_server_kind, "Render A", 0, 0),
      device(3U, schema::led_processor_kind, "Processor A", 3, 0),
      device(4U, schema::led_wall_kind, "Main Wall", 6, 0),
      link(11U, 1U, 0U, 2U, 0U),
      link(12U, 2U, 1U, 3U, 0U),
      link(13U, 3U, 1U, 4U, 0U),
  };
}

[[nodiscard]] doc::Root root_of(const std::vector<doc::Node> &nodes) {
  doc::Root root;
  for (const auto &one : nodes) {
    root = root.set(one);
  }
  return root;
}

void a_document_becomes_the_configured_hardware_chain() {
  topology::HardwareMapModel model;
  model.reset(root_of(configured_chain()));

  TOPOLOGY_CHECK(model.counters().rejected == 0U);
  TOPOLOGY_CHECK(model.counters().devices == 4U);
  TOPOLOGY_CHECK(model.counters().links == 3U);
  TOPOLOGY_CHECK(model.counters().invalid_links == 0U);

  const auto map = model.evaluate();
  TOPOLOGY_CHECK(map.devices.size() == 4U);
  TOPOLOGY_CHECK(map.links.size() == 3U);
  const auto *const render = map.device(doc::NodeId{2U});
  TOPOLOGY_CHECK(render != nullptr);
  TOPOLOGY_CHECK(render->name == "Render A");
  TOPOLOGY_CHECK(render->lifecycle_endpoint == "192.0.2.10:47840");
  TOPOLOGY_CHECK(render->node_index == 2U);
  TOPOLOGY_CHECK(render->width == 3840U);
  TOPOLOGY_CHECK(render->window_mode == topology::HardwareWindowMode::Inherit);
  TOPOLOGY_CHECK(!map.control.authored);
  const auto legacy = topology::lifecycle_endpoint_for(map, *render);
  TOPOLOGY_CHECK(legacy.has_value());
  TOPOLOGY_CHECK(*legacy == "192.0.2.10:47840");
  TOPOLOGY_CHECK(map.link(doc::NodeId{12U}) != nullptr);

  const auto render_ports =
      topology::ports_for(topology::HardwareDeviceKind::RenderServer);
  TOPOLOGY_CHECK(render_ports.size() == 2U);
  TOPOLOGY_CHECK(render_ports[0U].direction ==
                 topology::HardwarePortDirection::Input);
  TOPOLOGY_CHECK(render_ports[1U].direction ==
                 topology::HardwarePortDirection::Output);
}

void authored_control_and_window_settings_round_trip() {
  auto nodes = configured_chain();
  nodes[1U].properties.insert_or_assign(
      std::string{schema::address}, doc::PropertyValue{std::string{}});
  nodes[1U].properties.insert_or_assign(
      std::string{schema::window_mode},
      doc::PropertyValue{std::string{schema::borderless_window_mode}});
  nodes[1U].properties.insert_or_assign(
      std::string{schema::desktop_x}, doc::PropertyValue{std::int64_t{-1920}});
  nodes[1U].properties.insert_or_assign(
      std::string{schema::desktop_y}, doc::PropertyValue{std::int64_t{-120}});
  nodes.push_back(settings(88U, 50'000U));

  topology::HardwareMapModel model;
  model.reset(root_of(nodes));
  TOPOLOGY_CHECK(model.counters().rejected == 0U);
  TOPOLOGY_CHECK(model.size() == nodes.size());
  const topology::HardwareMap map = model.evaluate();
  TOPOLOGY_CHECK(map.control.authored);
  TOPOLOGY_CHECK(map.control.node == doc::NodeId{88U});
  TOPOLOGY_CHECK(map.control.base_port == 50'000U);

  const topology::HardwareDevice *const render =
      map.device(doc::NodeId{2U});
  TOPOLOGY_CHECK(render != nullptr);
  TOPOLOGY_CHECK(render->window_mode ==
                 topology::HardwareWindowMode::Borderless);
  TOPOLOGY_CHECK(render->desktop_x == -1920);
  TOPOLOGY_CHECK(render->desktop_y == -120);
  const auto endpoint = topology::lifecycle_endpoint_for(map, *render);
  TOPOLOGY_CHECK(endpoint.has_value());
  TOPOLOGY_CHECK(*endpoint == "127.0.0.1:50002");

  topology::HardwareMap ipv6 = map;
  TOPOLOGY_CHECK(ipv6.devices.size() > 1U);
  topology::HardwareDevice &mutable_render = ipv6.devices[1U];
  TOPOLOGY_CHECK(mutable_render.node == doc::NodeId{2U});
  mutable_render.address = "2001:db8::25";
  const auto ipv6_endpoint =
      topology::lifecycle_endpoint_for(ipv6, mutable_render);
  TOPOLOGY_CHECK(ipv6_endpoint.has_value());
  TOPOLOGY_CHECK(*ipv6_endpoint == "[2001:db8::25]:50002");
}

void invalid_control_and_window_vocabulary_are_rejected() {
  doc::Root root;
  auto oversized = device(1U, schema::render_server_kind, "Too Wide", 0, 0);
  oversized.properties.insert_or_assign(
      std::string{schema::window_mode},
      doc::PropertyValue{std::string{schema::windowed_window_mode}});
  oversized.properties.insert_or_assign(
      std::string{schema::width},
      doc::PropertyValue{static_cast<std::int64_t>(
          topology::max_hardware_output_width + 1U)});
  root = root.set(oversized);
  auto unknown_mode = device(2U, schema::render_server_kind, "Bad Mode", 0, 0);
  unknown_mode.properties.insert_or_assign(
      std::string{schema::window_mode},
      doc::PropertyValue{std::string{"cinema"}});
  root = root.set(unknown_mode);
  auto bad_index = device(3U, schema::render_server_kind, "Bad Index", 0, 0);
  bad_index.properties.insert_or_assign(
      std::string{schema::node_index},
      doc::PropertyValue{static_cast<std::int64_t>(
          topology::max_hardware_node_index + 1U)});
  root = root.set(bad_index);
  root = root.set(settings(4U, topology::max_hardware_control_base_port + 1U));

  // Both inherited and explicit modes preserve legacy dimensions above the
  // live presenter ceiling. Runtime resolution remains responsible for
  // refusing them; removing the authored device would make it unrepairable.
  auto inherited = device(5U, schema::render_server_kind, "Inherited", 0, 0);
  inherited.properties.insert_or_assign(
      std::string{schema::width},
      doc::PropertyValue{static_cast<std::int64_t>(
          topology::max_hardware_output_width + 1U)});
  root = root.set(inherited);

  topology::HardwareMapModel model;
  model.reset(root);
  TOPOLOGY_CHECK(model.counters().rejected == 3U);
  const topology::HardwareMap map = model.evaluate();
  TOPOLOGY_CHECK(map.devices.size() == 2U);
  TOPOLOGY_CHECK(map.devices[0U].node == doc::NodeId{1U});
  TOPOLOGY_CHECK(map.devices[0U].window_mode ==
                 topology::HardwareWindowMode::Windowed);
  TOPOLOGY_CHECK(map.devices[0U].width ==
                 topology::max_hardware_output_width + 1U);
  TOPOLOGY_CHECK(map.devices[1U].node == doc::NodeId{5U});
  TOPOLOGY_CHECK(!map.control.authored);

  topology::HardwareMap max_port{};
  max_port.control = topology::HardwareControlSettings{
      doc::NodeId{90U}, topology::max_hardware_control_base_port, true};
  topology::HardwareDevice last = map.devices.front();
  last.node_index = topology::max_hardware_node_index;
  last.lifecycle_endpoint.clear();
  const auto endpoint = topology::lifecycle_endpoint_for(max_port, last);
  TOPOLOGY_CHECK(endpoint.has_value());
  TOPOLOGY_CHECK(*endpoint == "192.0.2.10:65535");
}

void checkpoint_and_streamed_deltas_agree() {
  const auto nodes = configured_chain();
  topology::HardwareMapModel checkpointed;
  checkpointed.reset(root_of(nodes));

  topology::HardwareMapModel streamed;
  doc::Root running;
  for (const auto &one : nodes) {
    const auto next = running.set(one);
    streamed.apply(diff(running, next));
    running = next;
  }

  TOPOLOGY_CHECK(topology::digest(checkpointed.evaluate()) ==
                 topology::digest(streamed.evaluate()));
  TOPOLOGY_CHECK(checkpointed.counters() == streamed.counters());
}

void bad_authored_properties_are_rejected() {
  topology::HardwareMapModel model;
  doc::Root root;
  root = root.set(device(1U, "teleporter", "Unknown", 0, 0));
  root = root.set(node(2U, schema::device_type,
                       {text(schema::kind, schema::led_wall_kind),
                        whole(schema::grid_x, 0), whole(schema::grid_y, 0),
                        whole(schema::width, 0), whole(schema::height, 1080)}));
  root = root.set(node(3U, schema::device_type,
                       {text(schema::kind, schema::render_server_kind),
                        text(schema::grid_x, "left")}));
  root = root.set(
      node(4U, schema::link_type,
           {whole(schema::from_device, -1), whole(schema::from_port, 0),
            whole(schema::to_device, 2), whole(schema::to_port, 0)}));
  model.reset(root);

  TOPOLOGY_CHECK(model.counters().rejected == 4U);
  TOPOLOGY_CHECK(model.counters().devices == 0U);
  TOPOLOGY_CHECK(model.counters().links == 0U);
  TOPOLOGY_CHECK(model.evaluate().devices.empty());
}

void dangling_and_incompatible_links_are_not_evaluated() {
  doc::Root root;
  root = root.set(device(1U, schema::render_server_kind, "Render", 0, 0));
  root = root.set(device(2U, schema::led_processor_kind, "Processor", 2, 0));
  root = root.set(device(3U, schema::led_wall_kind, "Wall", 4, 0));
  root = root.set(link(10U, 1U, 1U, 2U, 0U));  // valid video
  root = root.set(link(11U, 1U, 0U, 2U, 0U));  // input as source
  root = root.set(link(12U, 2U, 1U, 99U, 0U)); // dangling
  root = root.set(link(13U, 2U, 1U, 2U, 0U));  // self
  root = root.set(link(14U, 2U, 1U, 3U, 7U));  // absent port

  topology::HardwareMapModel model;
  model.reset(root);
  TOPOLOGY_CHECK(model.counters().links == 5U);
  TOPOLOGY_CHECK(model.counters().invalid_links == 4U);
  const auto map = model.evaluate();
  TOPOLOGY_CHECK(map.links.size() == 1U);
  TOPOLOGY_CHECK(map.links.front().node == doc::NodeId{10U});
}

void the_lowest_link_node_owns_an_input() {
  doc::Root root;
  root = root.set(device(1U, schema::render_server_kind, "Render A", 0, 0));
  root = root.set(device(2U, schema::render_server_kind, "Render B", 0, 2));
  root = root.set(device(3U, schema::led_processor_kind, "Processor", 3, 1));
  root = root.set(link(20U, 1U, 1U, 3U, 0U));
  root = root.set(link(10U, 2U, 1U, 3U, 0U));

  topology::HardwareMapModel model;
  model.reset(root);
  TOPOLOGY_CHECK(model.counters().invalid_links == 1U);
  const auto map = model.evaluate();
  TOPOLOGY_CHECK(map.links.size() == 1U);
  TOPOLOGY_CHECK(map.links.front().node == doc::NodeId{10U});
  TOPOLOGY_CHECK(map.links.front().from.device == doc::NodeId{2U});
}

void render_servers_keep_a_bounded_logical_video_port_range() {
  doc::Root root;
  root = root.set(device(1U, schema::render_server_kind, "Render", 0, 0));
  root = root.set(device(2U, schema::led_processor_kind, "Processor A", 2, 0));
  root = root.set(device(3U, schema::led_processor_kind, "Processor B", 2, 2));
  root =
      root.set(link(10U, 1U, topology::last_render_server_video_port, 2U, 0U));
  root = root.set(
      link(11U, 1U,
           static_cast<core::u8>(topology::last_render_server_video_port + 1U),
           3U, 0U));

  topology::HardwareMapModel model;
  model.reset(root);
  const topology::HardwareMap map = model.evaluate();
  TOPOLOGY_CHECK(model.counters().invalid_links == 1U);
  TOPOLOGY_CHECK(map.link(doc::NodeId{10U}) != nullptr);
  TOPOLOGY_CHECK(map.link(doc::NodeId{11U}) == nullptr);
  const topology::HardwarePort *const last =
      topology::find_port(topology::HardwareDeviceKind::RenderServer,
                          topology::last_render_server_video_port);
  TOPOLOGY_CHECK(last != nullptr);
  TOPOLOGY_CHECK(last->index == topology::last_render_server_video_port);
  TOPOLOGY_CHECK(last->signal == topology::HardwareSignal::Video);
  TOPOLOGY_CHECK(
      topology::find_port(topology::HardwareDeviceKind::RenderServer,
                          static_cast<core::u8>(
                              topology::last_render_server_video_port + 1U)) ==
      nullptr);
  // Runtime discovery must not expand the baseline authored presentation.
  TOPOLOGY_CHECK(
      topology::ports_for(topology::HardwareDeviceKind::RenderServer).size() ==
      2U);

  const core::u64 authored_digest = topology::digest(map);
  topology::HardwareRuntimeSnapshot runtime{};
  topology::HardwareRuntimeNode observed{};
  observed.device = doc::NodeId{1U};
  observed.reachability = topology::HardwareRuntimeReachability::Online;
  observed.video_outputs.resize(topology::max_hardware_video_outputs);
  runtime.nodes.push_back(std::move(observed));
  TOPOLOGY_CHECK(runtime.node(doc::NodeId{1U}) != nullptr);
  TOPOLOGY_CHECK(topology::digest(map) == authored_digest);
}

void output_bindings_are_stable_explicit_and_deterministic() {
  auto nodes = configured_chain();
  constexpr core::u64 adapter = 0x1122334455667788ULL;
  nodes.push_back(output_binding(30U, 2U, 1U, 7U, adapter, 10U, 3U,
                                 3840U, 2160U, 60'000U, 1001U));
  nodes.push_back(output_binding(31U, 2U, 2U, 8U, adapter, 11U, 4U));
  // Lowest document ID owns a logical port, feed, and physical target.
  nodes.push_back(output_binding(32U, 2U, 1U, 9U, adapter, 12U, 5U));
  nodes.push_back(output_binding(33U, 2U, 3U, 10U, adapter, 11U, 4U));
  nodes.push_back(output_binding(34U, 99U, 4U, 11U, adapter, 13U, 6U));

  topology::HardwareMapModel model;
  model.reset(root_of(nodes));
  TOPOLOGY_CHECK(model.counters().rejected == 0U);
  TOPOLOGY_CHECK(model.counters().output_bindings == 5U);
  TOPOLOGY_CHECK(model.counters().invalid_output_bindings == 3U);
  const topology::HardwareMap map = model.evaluate();
  TOPOLOGY_CHECK(map.output_bindings.size() == 2U);
  const auto *const first = map.output_binding(doc::NodeId{30U});
  TOPOLOGY_CHECK(first != nullptr);
  TOPOLOGY_CHECK(first->render_server == doc::NodeId{2U});
  TOPOLOGY_CHECK(first->logical_port == 1U);
  TOPOLOGY_CHECK(first->feed_index == 7U);
  TOPOLOGY_CHECK(first->target.adapter_id == adapter);
  TOPOLOGY_CHECK(first->target.target_id == 10U);
  TOPOLOGY_CHECK(first->target.connector_instance == 3U);
  TOPOLOGY_CHECK(first->width == 3840U);
  TOPOLOGY_CHECK(first->refresh_numerator == 60'000U);
  TOPOLOGY_CHECK(first->refresh_denominator == 1001U);
  TOPOLOGY_CHECK(map.output_binding_for(doc::NodeId{2U}, 2U) ==
                 map.output_binding(doc::NodeId{31U}));
  TOPOLOGY_CHECK(map.output_binding(doc::NodeId{32U}) == nullptr);

  const std::string encoded = topology::format_display_adapter_id(adapter);
  TOPOLOGY_CHECK(encoded == "1122334455667788");
  TOPOLOGY_CHECK(topology::parse_display_adapter_id(encoded) == adapter);
  TOPOLOGY_CHECK(!topology::parse_display_adapter_id("112233445566778A"));
  TOPOLOGY_CHECK(!topology::parse_display_adapter_id("0000000000000000"));

  auto changed = nodes;
  changed[7U].properties.insert_or_assign(
      std::string{schema::display_target_id},
      doc::PropertyValue{std::int64_t{14}});
  topology::HardwareMapModel changed_model;
  changed_model.reset(root_of(changed));
  TOPOLOGY_CHECK(topology::digest(changed_model.evaluate()) !=
                 topology::digest(map));
}

void removing_or_retyping_a_device_invalidates_its_links() {
  doc::Root root = root_of(configured_chain());
  topology::HardwareMapModel model;
  model.reset(root);

  const auto erased = root.erase(doc::NodeId{3U});
  TOPOLOGY_CHECK(erased.has_value());
  model.apply(diff(root, *erased));
  TOPOLOGY_CHECK(model.counters().devices == 3U);
  TOPOLOGY_CHECK(model.counters().invalid_links == 2U);
  TOPOLOGY_CHECK(model.evaluate().links.size() == 1U);

  topology::HardwareMapModel retyped;
  retyped.reset(root);
  const auto changed = root.set(node(2U, "scene.object", {}));
  retyped.apply(diff(root, changed));
  TOPOLOGY_CHECK(retyped.counters().devices == 3U);
  TOPOLOGY_CHECK(retyped.counters().invalid_links == 2U);
  TOPOLOGY_CHECK(retyped.size() == 6U);
}

void every_authored_field_contributes_to_the_digest() {
  topology::HardwareMapModel base;
  base.reset(root_of(configured_chain()));
  const auto reference = topology::digest(base.evaluate());

  auto changed_nodes = configured_chain();
  changed_nodes[1U].properties.insert_or_assign(
      std::string{schema::grid_x}, doc::PropertyValue{std::int64_t{1}});
  topology::HardwareMapModel moved;
  moved.reset(root_of(changed_nodes));
  TOPOLOGY_CHECK(topology::digest(moved.evaluate()) != reference);

  changed_nodes = configured_chain();
  changed_nodes[1U].properties.insert_or_assign(
      std::string{schema::address},
      doc::PropertyValue{std::string{"render-b"}});
  topology::HardwareMapModel addressed;
  addressed.reset(root_of(changed_nodes));
  TOPOLOGY_CHECK(topology::digest(addressed.evaluate()) != reference);

  changed_nodes = configured_chain();
  changed_nodes[1U].properties.insert_or_assign(
      std::string{schema::lifecycle_endpoint},
      doc::PropertyValue{std::string{"192.0.2.11:47840"}});
  topology::HardwareMapModel endpoint_changed;
  endpoint_changed.reset(root_of(changed_nodes));
  TOPOLOGY_CHECK(topology::digest(endpoint_changed.evaluate()) != reference);

  changed_nodes = configured_chain();
  changed_nodes[1U].properties.insert_or_assign(
      std::string{schema::node_index}, doc::PropertyValue{std::int64_t{7}});
  topology::HardwareMapModel index_changed;
  index_changed.reset(root_of(changed_nodes));
  TOPOLOGY_CHECK(topology::digest(index_changed.evaluate()) != reference);

  changed_nodes = configured_chain();
  changed_nodes[1U].properties.insert_or_assign(
      std::string{schema::window_mode},
      doc::PropertyValue{std::string{schema::windowed_window_mode}});
  changed_nodes[1U].properties.insert_or_assign(
      std::string{schema::desktop_x}, doc::PropertyValue{std::int64_t{-1200}});
  topology::HardwareMapModel window_changed;
  window_changed.reset(root_of(changed_nodes));
  TOPOLOGY_CHECK(topology::digest(window_changed.evaluate()) != reference);

  changed_nodes = configured_chain();
  changed_nodes.push_back(settings(80U, 50'000U));
  topology::HardwareMapModel control_changed;
  control_changed.reset(root_of(changed_nodes));
  TOPOLOGY_CHECK(topology::digest(control_changed.evaluate()) != reference);

  changed_nodes = configured_chain();
  changed_nodes.back().properties.insert_or_assign(
      std::string{schema::to_device}, doc::PropertyValue{std::int64_t{3}});
  topology::HardwareMapModel relinked;
  relinked.reset(root_of(changed_nodes));
  TOPOLOGY_CHECK(topology::digest(relinked.evaluate()) != reference);
}

void device_capacity_is_bounded_and_deterministic() {
  doc::Root root;
  const auto count = topology::max_hardware_devices + 8U;
  for (core::u32 index = 0U; index < count; ++index) {
    root = root.set(device(static_cast<core::u64>(index) + 1U,
                           schema::led_wall_kind, "Wall", 0,
                           static_cast<core::i32>(index)));
  }
  topology::HardwareMapModel checkpointed;
  checkpointed.reset(root);
  TOPOLOGY_CHECK(checkpointed.counters().devices == count);
  TOPOLOGY_CHECK(checkpointed.counters().devices_over_capacity() == 8U);
  const auto map = checkpointed.evaluate();
  TOPOLOGY_CHECK(map.devices.size() == topology::max_hardware_devices);
  TOPOLOGY_CHECK(map.devices.front().node == doc::NodeId{1U});
  TOPOLOGY_CHECK(map.devices.back().node ==
                 doc::NodeId{topology::max_hardware_devices});

  topology::HardwareMapModel streamed;
  doc::Root running;
  for (core::u32 index = count; index > 0U; --index) {
    const auto *const one = root.get(doc::NodeId{index});
    TOPOLOGY_CHECK(one != nullptr);
    const auto next = running.set(*one);
    streamed.apply(diff(running, next));
    running = next;
  }
  TOPOLOGY_CHECK(topology::digest(streamed.evaluate()) ==
                 topology::digest(map));
}

void the_main_node_is_one_active_render_server() {
  const auto flag = [](bool value) {
    return Property{std::string{schema::main_node}, doc::PropertyValue{value}};
  };
  // Two linked Render Servers, on node indices 2 and 5; the second is the
  // eighth node in the list, after the chain and its links.
  auto nodes = configured_chain();
  doc::Node second = device(5U, schema::render_server_kind, "Render B", 0, 2);
  second.properties.insert_or_assign(std::string{schema::node_index},
                                     doc::PropertyValue{std::int64_t{5}});
  nodes.push_back(second);
  // Its control input is cabled from the operator machine.
  nodes.push_back(link(14U, 1U, 0U, 5U, 0U));

  topology::HardwareMapModel unmarked;
  unmarked.reset(root_of(nodes));
  TOPOLOGY_CHECK(!topology::main_render_node(unmarked.evaluate()).has_value());
  const auto reference = topology::digest(unmarked.evaluate());

  auto marked_nodes = nodes;
  marked_nodes[7U].properties.insert(flag(true));
  topology::HardwareMapModel marked;
  marked.reset(root_of(marked_nodes));
  TOPOLOGY_CHECK(marked.counters().rejected == 0U);
  TOPOLOGY_CHECK(topology::main_render_node(marked.evaluate()) ==
                 std::optional<core::u32>{5U});
  TOPOLOGY_CHECK(topology::digest(marked.evaluate()) != reference);

  // Two marked at once: the lower document id wins on every reader.
  marked_nodes[1U].properties.insert(flag(true));
  topology::HardwareMapModel both;
  both.reset(root_of(marked_nodes));
  TOPOLOGY_CHECK(topology::main_render_node(both.evaluate()) ==
                 std::optional<core::u32>{2U});

  // Only a Render Server can be one, and only while it is connected.
  auto other_kind = nodes;
  other_kind[2U].properties.insert(flag(true));
  topology::HardwareMapModel processor;
  processor.reset(root_of(other_kind));
  TOPOLOGY_CHECK(!topology::main_render_node(processor.evaluate()).has_value());
  auto unlinked = nodes;
  unlinked.pop_back();
  unlinked[7U].properties.insert(flag(true));
  topology::HardwareMapModel spare;
  spare.reset(root_of(unlinked));
  TOPOLOGY_CHECK(!topology::main_render_node(spare.evaluate()).has_value());

  // A flag that is not a boolean is malformed.
  auto wrong = nodes;
  wrong[7U].properties.insert_or_assign(std::string{schema::main_node},
                                        doc::PropertyValue{std::int64_t{1}});
  topology::HardwareMapModel rejected;
  rejected.reset(root_of(wrong));
  TOPOLOGY_CHECK(rejected.counters().rejected == 1U);
}

void processors_take_many_inputs_and_keep_their_old_ports() {
  doc::Root root;
  root = root.set(device(1U, schema::render_server_kind, "Render", 0, 0));
  root = root.set(node(2U, schema::device_type,
                       {text(schema::kind, schema::led_processor_kind),
                        text(schema::name, "MX40"),
                        text(schema::address, "10.0.0.40"),
                        text(schema::protocol, schema::novastar_coex_protocol)}));
  root = root.set(device(3U, schema::led_wall_kind, "Wall", 4, 0));
  // Output one into input one (port 0), output two into input three (port
  // 3), and LED OUT (port 1) to the wall: one cable per input.
  root = root.set(link(10U, 1U, 1U, 2U, 0U));
  root = root.set(link(11U, 1U, 2U, 2U, 3U));
  root = root.set(link(12U, 2U, topology::processor_led_output_port, 3U, 0U));
  // A seventeenth input does not exist.
  root = root.set(link(13U, 1U, 3U, 2U, 17U));

  topology::HardwareMapModel model;
  model.reset(root);
  const topology::HardwareMap map = model.evaluate();
  TOPOLOGY_CHECK(map.link(doc::NodeId{10U}) != nullptr);
  TOPOLOGY_CHECK(map.link(doc::NodeId{11U}) != nullptr);
  TOPOLOGY_CHECK(map.link(doc::NodeId{12U}) != nullptr);
  TOPOLOGY_CHECK(map.link(doc::NodeId{13U}) == nullptr);
  TOPOLOGY_CHECK(model.counters().invalid_links == 1U);

  const topology::HardwareDevice *const processor =
      map.device(doc::NodeId{2U});
  TOPOLOGY_CHECK(processor != nullptr);
  TOPOLOGY_CHECK(processor->protocol ==
                 topology::HardwareProcessorProtocol::NovastarCoex);
  TOPOLOGY_CHECK(processor->address == "10.0.0.40");

  TOPOLOGY_CHECK(topology::processor_input_number(0U) == 1U);
  TOPOLOGY_CHECK(topology::processor_input_number(1U) == 0U);
  TOPOLOGY_CHECK(topology::processor_input_number(3U) == 3U);
  TOPOLOGY_CHECK(topology::processor_input_number(16U) == 16U);
  TOPOLOGY_CHECK(topology::processor_input_number(17U) == 0U);
  TOPOLOGY_CHECK(topology::processor_input_port(1U) == 0U);
  TOPOLOGY_CHECK(topology::processor_input_port(9U) == 9U);
  const topology::HardwarePort *const third = topology::find_port(
      topology::HardwareDeviceKind::LedProcessor, 3U);
  TOPOLOGY_CHECK(third != nullptr && third->name == "VIDEO IN 3" &&
                 third->direction == topology::HardwarePortDirection::Input);
  // Walls do not gain processor inputs.
  TOPOLOGY_CHECK(topology::find_port(topology::HardwareDeviceKind::LedWall,
                                     3U) == nullptr);
}

void a_protocol_is_a_processor_property_in_the_digest() {
  const auto processor = [](std::string_view protocol) {
    doc::Root root;
    root = root.set(node(2U, schema::device_type,
                         {text(schema::kind, schema::led_processor_kind),
                          text(schema::name, "Processor"),
                          text(schema::protocol, protocol)}));
    topology::HardwareMapModel model;
    model.reset(root);
    return std::pair{model.evaluate(), model.counters().rejected};
  };
  const auto [generic, generic_rejected] =
      processor(schema::generic_protocol);
  const auto [coex, coex_rejected] =
      processor(schema::novastar_coex_protocol);
  const auto [brompton, brompton_rejected] =
      processor(schema::brompton_tessera_protocol);
  const auto [unknown, unknown_rejected] = processor("colorlight");
  TOPOLOGY_CHECK(brompton_rejected == 0U &&
                 brompton.devices.front().protocol ==
                     topology::HardwareProcessorProtocol::BromptonTessera);
  TOPOLOGY_CHECK(topology::digest(brompton) != topology::digest(coex));
  TOPOLOGY_CHECK(generic_rejected == 0U && coex_rejected == 0U);
  TOPOLOGY_CHECK(unknown_rejected == 1U && unknown.devices.empty());
  TOPOLOGY_CHECK(generic.devices.front().protocol ==
                 topology::HardwareProcessorProtocol::Generic);
  TOPOLOGY_CHECK(coex.devices.front().protocol ==
                 topology::HardwareProcessorProtocol::NovastarCoex);
  TOPOLOGY_CHECK(topology::digest(generic) != topology::digest(coex));

  // An absent protocol is Generic.
  doc::Root unset;
  unset = unset.set(node(2U, schema::device_type,
                         {text(schema::kind, schema::led_processor_kind),
                          text(schema::name, "Processor")}));
  topology::HardwareMapModel unset_model;
  unset_model.reset(unset);
  TOPOLOGY_CHECK(topology::digest(unset_model.evaluate()) ==
                 topology::digest(generic));

  // Only a processor keeps a protocol.
  doc::Root server;
  server = server.set(node(1U, schema::device_type,
                           {text(schema::kind, schema::render_server_kind),
                            text(schema::name, "Render"),
                            text(schema::protocol,
                                 schema::novastar_coex_protocol)}));
  topology::HardwareMapModel server_model;
  server_model.reset(server);
  TOPOLOGY_CHECK(server_model.evaluate().devices.front().protocol ==
                 topology::HardwareProcessorProtocol::Generic);
}

void processor_signals_are_checked_against_the_output_mode() {
  doc::Root root;
  root = root.set(device(1U, schema::render_server_kind, "Render", 0, 0));
  root = root.set(node(2U, schema::device_type,
                       {text(schema::kind, schema::led_processor_kind),
                        text(schema::name, "MX40"),
                        text(schema::protocol, schema::novastar_coex_protocol)}));
  root = root.set(node(3U, schema::device_type,
                       {text(schema::kind, schema::led_processor_kind),
                        text(schema::name, "Unmonitored")}));
  // Four outputs: three into the COEX processor, one into a generic one.
  root = root.set(output_binding(20U, 1U, 1U, 0U, 0x101U, 1U, 0U, 3840U,
                                 2160U, 60000U, 1001U));
  root = root.set(output_binding(21U, 1U, 2U, 1U, 0x101U, 2U, 0U, 3840U,
                                 2160U, 60000U, 1001U));
  root = root.set(output_binding(22U, 1U, 3U, 2U, 0x101U, 3U, 0U, 1920U,
                                 1080U, 50U, 1U));
  root = root.set(link(10U, 1U, 1U, 2U, 0U));
  root = root.set(link(11U, 1U, 2U, 2U, 2U));
  root = root.set(link(12U, 1U, 3U, 2U, 3U));
  root = root.set(link(13U, 1U, 4U, 3U, 0U));
  topology::HardwareMapModel model;
  model.reset(root);
  const topology::HardwareMap map = model.evaluate();

  topology::HardwareRuntimeSnapshot runtime{};
  // Nothing is said about a processor that has not answered.
  TOPOLOGY_CHECK(topology::check_processor_signals(map, runtime).empty());

  topology::HardwareProcessorNode observed{};
  observed.device = doc::NodeId{2U};
  observed.reachability = topology::HardwareRuntimeReachability::Online;
  topology::HardwareProcessorInput matching{};
  matching.number = 1U;
  matching.signal = true;
  matching.width = 3840U;
  matching.height = 2160U;
  matching.refresh_millihertz = 59'940U;
  matching.edid_width = 3840U;
  matching.edid_height = 2160U;
  matching.edid_refresh_millihertz = 59'940U;
  topology::HardwareProcessorInput edid = matching;
  edid.number = 2U;
  edid.edid_refresh_millihertz = 60'000U;
  edid.refresh_millihertz = 60'000U;
  edid.edid_writable = true;
  topology::HardwareProcessorInput dark{};
  dark.number = 3U;
  observed.inputs = {matching, edid, dark};
  runtime.processors.push_back(observed);

  const auto checks = topology::check_processor_signals(map, runtime);
  TOPOLOGY_CHECK(checks.size() == 3U);
  TOPOLOGY_CHECK(checks[0].link == doc::NodeId{10U} &&
                 checks[0].issue == topology::HardwareSignalIssue::None &&
                 checks[0].refresh_millihertz == 59'940U);
  TOPOLOGY_CHECK(checks[1].issue ==
                     topology::HardwareSignalIssue::EdidMismatch &&
                 checks[1].input == 2U && checks[1].edid_writable &&
                 checks[1].seen_refresh_millihertz == 60'000U);
  TOPOLOGY_CHECK(topology::signal_check_text(checks[1]) ==
                 "EDID mismatch: processor input 2 offers 3840x2160 at 60, "
                 "output sends 3840x2160 at 59.94");
  TOPOLOGY_CHECK(checks[2].issue == topology::HardwareSignalIssue::NoSignal &&
                 checks[2].width == 1920U);
  TOPOLOGY_CHECK(topology::signal_check_text(checks[2]) ==
                 "No signal at processor input 3");

  // A signal in another mode, with the EDID agreeing, is a mode mismatch.
  runtime.processors.front().inputs[0].width = 1920U;
  runtime.processors.front().inputs[0].height = 1080U;
  const auto moved = topology::check_processor_signals(map, runtime);
  TOPOLOGY_CHECK(moved[0].issue ==
                     topology::HardwareSignalIssue::ModeMismatch &&
                 moved[0].seen_width == 1920U);

  // A stale report is history, not a reason to warn.
  runtime.processors.front().stale = true;
  TOPOLOGY_CHECK(topology::check_processor_signals(map, runtime).empty());
  TOPOLOGY_CHECK(topology::refresh_millihertz(24000U, 1001U) == 23'976U);
}

// One 7680x1080 output feeding four processors - an NVIDIA Mosaic surface
// that is one output to Windows. The cables share it left to right as the
// processors sit on the map, take the render machine's reported tiles when
// there are some, and an authored region beats both.
void one_output_feeds_several_processors_a_region_each() {
  doc::Root root;
  root = root.set(device(1U, schema::render_server_kind, "Render", 0, 0));
  // Map positions left to right: 4, 2, 5, 3 - not document order.
  const std::array<std::pair<core::u64, core::i32>, 4U> processors{
      {{2U, 4}, {3U, 8}, {4U, 2}, {5U, 6}}};
  for (const auto &[id, x] : processors) {
    root = root.set(node(id, schema::device_type,
                         {text(schema::kind, schema::led_processor_kind),
                          text(schema::name, "Processor"),
                          text(schema::protocol, schema::novastar_coex_protocol),
                          whole(schema::grid_x, x), whole(schema::grid_y, 0)}));
  }
  root = root.set(output_binding(20U, 1U, 1U, 0U, 0x101U, 7U, 0U, 7680U,
                                 1080U, 60U, 1U));
  for (core::u64 id = 2U; id <= 5U; ++id) {
    root = root.set(link(8U + id, 1U, 1U, id, 0U));
  }
  topology::HardwareMapModel model;
  model.reset(root);
  const topology::HardwareMap map = model.evaluate();
  TOPOLOGY_CHECK(map.links.size() == 4U);
  topology::HardwareRuntimeSnapshot runtime{};

  const auto region = [&](core::u64 link_id) {
    return topology::link_output_region(map, *map.link(doc::NodeId{link_id}),
                                        runtime);
  };
  // Processor 4 is leftmost (link 12), then 2 (10), 5 (13), 3 (11).
  TOPOLOGY_CHECK(region(12U).region ==
                 (topology::HardwareOutputRegion{0U, 0U, 1920U, 1080U}));
  TOPOLOGY_CHECK(region(10U).region ==
                 (topology::HardwareOutputRegion{1920U, 0U, 1920U, 1080U}));
  TOPOLOGY_CHECK(region(13U).region ==
                 (topology::HardwareOutputRegion{3840U, 0U, 1920U, 1080U}));
  TOPOLOGY_CHECK(region(11U).region ==
                 (topology::HardwareOutputRegion{5760U, 0U, 1920U, 1080U}));
  TOPOLOGY_CHECK(region(11U).source == topology::HardwareRegionSource::EvenShare);

  // Each processor input is checked against its own tile, not the surface.
  topology::HardwareProcessorNode observed{};
  observed.device = doc::NodeId{2U};
  observed.reachability = topology::HardwareRuntimeReachability::Online;
  topology::HardwareProcessorInput input{};
  input.number = 1U;
  input.signal = true;
  input.width = 1920U;
  input.height = 1080U;
  input.refresh_millihertz = 60'000U;
  observed.inputs.push_back(input);
  runtime.processors.push_back(observed);
  const auto checks = topology::check_processor_signals(map, runtime);
  TOPOLOGY_CHECK(checks.size() == 1U);
  TOPOLOGY_CHECK(checks[0].issue == topology::HardwareSignalIssue::None);
  TOPOLOGY_CHECK(checks[0].width == 1920U);

  // The render machine reports the surface as a 2x2 grid of 3840x540.
  topology::HardwareRuntimeNode machine{};
  machine.device = doc::NodeId{1U};
  topology::HardwareVideoOutput surface{};
  surface.target = topology::HardwareDisplayTargetKey{0x101U, 7U, 0U};
  surface.width = 7680U;
  surface.height = 1080U;
  surface.mosaic_tiles = {{0U, 0U, 3840U, 540U},
                          {3840U, 0U, 3840U, 540U},
                          {0U, 540U, 3840U, 540U},
                          {3840U, 540U, 3840U, 540U}};
  machine.video_outputs.push_back(surface);
  runtime.nodes.push_back(machine);
  TOPOLOGY_CHECK(region(13U).region ==
                 (topology::HardwareOutputRegion{0U, 540U, 3840U, 540U}));
  TOPOLOGY_CHECK(region(13U).source ==
                 topology::HardwareRegionSource::MosaicTile);

  // An authored region wins, cut to the output it is part of.
  doc::Node authored = *root.get(doc::NodeId{13U});
  authored.properties.insert_or_assign(std::string{schema::region_x},
                                       doc::PropertyValue{std::int64_t{7000}});
  authored.properties.insert_or_assign(std::string{schema::region_y},
                                       doc::PropertyValue{std::int64_t{0}});
  authored.properties.insert_or_assign(std::string{schema::region_width},
                                       doc::PropertyValue{std::int64_t{1920}});
  authored.properties.insert_or_assign(std::string{schema::region_height},
                                       doc::PropertyValue{std::int64_t{1080}});
  const core::u64 before = topology::digest(map);
  model.reset(root.set(authored));
  const topology::HardwareMap edited = model.evaluate();
  TOPOLOGY_CHECK(topology::digest(edited) != before);
  const topology::HardwareLinkRegion cut = topology::link_output_region(
      edited, *edited.link(doc::NodeId{13U}), runtime);
  TOPOLOGY_CHECK(cut.source == topology::HardwareRegionSource::Authored);
  TOPOLOGY_CHECK(cut.region ==
                 (topology::HardwareOutputRegion{7000U, 0U, 680U, 1080U}));

  // A lone cable carries its whole output.
  doc::Root lone;
  lone = lone.set(*root.get(doc::NodeId{1U}));
  lone = lone.set(*root.get(doc::NodeId{2U}));
  lone = lone.set(*root.get(doc::NodeId{20U}));
  lone = lone.set(*root.get(doc::NodeId{10U}));
  model.reset(lone);
  const topology::HardwareMap single = model.evaluate();
  const topology::HardwareLinkRegion all = topology::link_output_region(
      single, single.links.front(), topology::HardwareRuntimeSnapshot{});
  TOPOLOGY_CHECK(all.source == topology::HardwareRegionSource::WholeOutput);
  TOPOLOGY_CHECK(all.region ==
                 (topology::HardwareOutputRegion{0U, 0U, 7680U, 1080U}));
}

// Rates come back from processors in thousandths of a hertz, rounded; the
// NTSC family must come out as the exact n*1000/1001 fractions a GPU uses.
void decimal_rates_become_exact_fractions() {
  using Refresh = topology::HardwareRefresh;
  TOPOLOGY_CHECK(topology::refresh_from_millihertz(23'976U) ==
                 (Refresh{24'000U, 1'001U}));
  TOPOLOGY_CHECK(topology::refresh_from_millihertz(29'970U) ==
                 (Refresh{30'000U, 1'001U}));
  // A rate rounded the other way is still 30000/1001.
  TOPOLOGY_CHECK(topology::refresh_from_millihertz(29'980U) ==
                 (Refresh{30'000U, 1'001U}));
  TOPOLOGY_CHECK(topology::refresh_from_millihertz(59'940U) ==
                 (Refresh{60'000U, 1'001U}));
  TOPOLOGY_CHECK(topology::refresh_from_millihertz(119'880U) ==
                 (Refresh{120'000U, 1'001U}));
  TOPOLOGY_CHECK(topology::refresh_from_millihertz(24'000U) ==
                 (Refresh{24U, 1U}));
  TOPOLOGY_CHECK(topology::refresh_from_millihertz(50'000U) ==
                 (Refresh{50U, 1U}));
  TOPOLOGY_CHECK(topology::refresh_from_millihertz(47'500U) ==
                 (Refresh{47'500U, 1'000U}));
  TOPOLOGY_CHECK(topology::refresh_from_millihertz(0U) == (Refresh{0U, 1U}));
  // And back: the fraction is the rate the processor reported.
  TOPOLOGY_CHECK(topology::refresh_millihertz(24'000U, 1'001U) == 23'976U);

  // Typed hertz take the same path, held to a sane range.
  TOPOLOGY_CHECK(topology::refresh_from_hertz(23.976) ==
                 (Refresh{24'000U, 1'001U}));
  TOPOLOGY_CHECK(topology::refresh_from_hertz(59.94) ==
                 (Refresh{60'000U, 1'001U}));
  TOPOLOGY_CHECK(topology::refresh_from_hertz(25.0) == (Refresh{25U, 1U}));
  TOPOLOGY_CHECK(topology::refresh_from_hertz(5'000.0) ==
                 (Refresh{1'000U, 1U}));
  TOPOLOGY_CHECK(topology::refresh_from_hertz(-1.0) == (Refresh{0U, 1U}));
  TOPOLOGY_CHECK(topology::refresh_decimals(60'000U) == 0);
  TOPOLOGY_CHECK(topology::refresh_decimals(59'940U) == 2);
  TOPOLOGY_CHECK(topology::refresh_decimals(23'976U) == 3);
  TOPOLOGY_CHECK(topology::refresh_decimals(47'500U) == 1);
}

void a_reporting_processor_proposes_its_walls() {
  topology::HardwareMap map{};
  topology::HardwareDevice processor{};
  processor.node = doc::NodeId{4U};
  processor.kind = topology::HardwareDeviceKind::LedProcessor;
  processor.name = "MX40 Pro";
  processor.grid_x = 2;
  processor.grid_y = -1;
  topology::HardwareDevice neighbour{};
  neighbour.node = doc::NodeId{5U};
  neighbour.kind = topology::HardwareDeviceKind::RenderServer;
  // On the first cell a wall would take.
  neighbour.grid_x = 5;
  neighbour.grid_y = -4;
  map.devices = {processor, neighbour};

  topology::HardwareProcessorNode report{};
  report.device = processor.node;
  report.reachability = topology::HardwareRuntimeReachability::Online;
  topology::HardwareProcessorScreen main{};
  main.name = "Main";
  main.width = 2880U;
  main.height = 1536U;
  main.refresh_millihertz = 23'976U;
  topology::HardwareProcessorScreen unnamed{};
  unnamed.width = 1152U;
  unnamed.height = 768U;
  topology::HardwareProcessorScreen empty{};
  empty.name = "Unmapped";
  report.screens = {main, unnamed, empty};

  const std::vector<topology::HardwareWallProposal> walls =
      topology::proposed_walls(map, report);
  TOPOLOGY_CHECK(walls.size() == 2U);
  TOPOLOGY_CHECK(walls[0].name == "Main");
  TOPOLOGY_CHECK(walls[0].width == 2880U && walls[0].height == 1536U);
  TOPOLOGY_CHECK(walls[0].refresh_numerator == 24'000U &&
                 walls[0].refresh_denominator == 1'001U);
  TOPOLOGY_CHECK(walls[1].name == "MX40 Pro wall");
  // A screen that gives no rate keeps a new card's 60 Hz.
  TOPOLOGY_CHECK(walls[1].refresh_numerator == 60U &&
                 walls[1].refresh_denominator == 1U);
  // Beside the processor, clear of the server and of each other.
  TOPOLOGY_CHECK(walls[0].grid_x == 5 && walls[0].grid_y == -1);
  TOPOLOGY_CHECK(walls[1].grid_x == 5 && walls[1].grid_y == 2);

  // A stale report, or an LED OUT that drives something, proposes nothing.
  topology::HardwareProcessorNode stale = report;
  stale.stale = true;
  TOPOLOGY_CHECK(topology::proposed_walls(map, stale).empty());
  topology::HardwareDevice wall{};
  wall.node = doc::NodeId{6U};
  wall.kind = topology::HardwareDeviceKind::LedWall;
  map.devices.push_back(wall);
  map.links.push_back(topology::HardwareLink{
      doc::NodeId{9U},
      topology::HardwareEndpoint{processor.node,
                                 topology::processor_led_output_port},
      topology::HardwareEndpoint{wall.node, 0U}});
  TOPOLOGY_CHECK(topology::proposed_walls(map, report).empty());
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 19U> cases{{
    {"decimal rates become exact fractions",
     &decimal_rates_become_exact_fractions},
    {"a reporting processor proposes its walls",
     &a_reporting_processor_proposes_its_walls},
    {"one output feeds several processors a region each",
     &one_output_feeds_several_processors_a_region_each},
    {"processors take many inputs and keep their old ports",
     &processors_take_many_inputs_and_keep_their_old_ports},
    {"a protocol is a processor property in the digest",
     &a_protocol_is_a_processor_property_in_the_digest},
    {"processor signals are checked against the output mode",
     &processor_signals_are_checked_against_the_output_mode},
    {"the main node is one active render server",
     &the_main_node_is_one_active_render_server},
    {"a document becomes the configured hardware chain",
     &a_document_becomes_the_configured_hardware_chain},
    {"checkpoint and streamed deltas agree",
     &checkpoint_and_streamed_deltas_agree},
    {"authored control and window settings round trip",
     &authored_control_and_window_settings_round_trip},
    {"invalid control and window vocabulary are rejected",
     &invalid_control_and_window_vocabulary_are_rejected},
    {"bad authored properties are rejected",
     &bad_authored_properties_are_rejected},
    {"dangling and incompatible links are not evaluated",
     &dangling_and_incompatible_links_are_not_evaluated},
    {"the lowest link node owns an input", &the_lowest_link_node_owns_an_input},
    {"render servers keep a bounded logical video port range",
     &render_servers_keep_a_bounded_logical_video_port_range},
    {"output bindings are stable explicit and deterministic",
     &output_bindings_are_stable_explicit_and_deterministic},
    {"removing or retyping a device invalidates its links",
     &removing_or_retyping_a_device_invalidates_its_links},
    {"every authored field contributes to the digest",
     &every_authored_field_contributes_to_the_digest},
    {"device capacity is bounded and deterministic",
     &device_capacity_is_bounded_and_deterministic},
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
