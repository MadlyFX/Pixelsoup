#include <mediaserver/topology/hardware_map_model.hpp>

#include <mediaserver/topology/hardware_map_schema.hpp>
#include <mediaserver/topology/hardware_runtime.hpp>

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

void oversized_legacy_window_survives_an_explicit_mode_cycle() {
  auto authored =
      device(1U, schema::render_server_kind, "Legacy Render", 0, 0);
  authored.properties.insert_or_assign(
      std::string{schema::width},
      doc::PropertyValue{static_cast<std::int64_t>(
          topology::max_hardware_output_width + 1U)});
  authored.properties.insert_or_assign(
      std::string{schema::height},
      doc::PropertyValue{static_cast<std::int64_t>(
          topology::max_hardware_output_height + 1U)});

  doc::Root running = doc::Root{}.set(authored);
  topology::HardwareMapModel model;
  model.reset(running);
  topology::HardwareMap evaluated = model.evaluate();
  const topology::HardwareDevice *visible =
      evaluated.device(doc::NodeId{1U});
  TOPOLOGY_CHECK(visible != nullptr);
  TOPOLOGY_CHECK(visible->window_mode ==
                 topology::HardwareWindowMode::Inherit);

  authored.properties.insert_or_assign(
      std::string{schema::window_mode},
      doc::PropertyValue{std::string{schema::windowed_window_mode}});
  doc::Root next = running.set(authored);
  model.apply(diff(running, next));
  running = next;
  evaluated = model.evaluate();
  visible = evaluated.device(doc::NodeId{1U});
  TOPOLOGY_CHECK(visible != nullptr);
  TOPOLOGY_CHECK(visible->window_mode ==
                 topology::HardwareWindowMode::Windowed);
  TOPOLOGY_CHECK(visible->width ==
                 topology::max_hardware_output_width + 1U);
  TOPOLOGY_CHECK(visible->height ==
                 topology::max_hardware_output_height + 1U);

  authored.properties.insert_or_assign(
      std::string{schema::width},
      doc::PropertyValue{static_cast<std::int64_t>(
          topology::max_hardware_output_width)});
  next = running.set(authored);
  model.apply(diff(running, next));
  evaluated = model.evaluate();
  visible = evaluated.device(doc::NodeId{1U});
  TOPOLOGY_CHECK(visible != nullptr);
  TOPOLOGY_CHECK(visible->width == topology::max_hardware_output_width);
  TOPOLOGY_CHECK(model.counters().rejected == 0U);
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

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 14U> cases{{
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
    {"oversized legacy window survives an explicit mode cycle",
     &oversized_legacy_window_survives_an_explicit_mode_cycle},
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
