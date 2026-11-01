#include <mediaserver/output/feed_layout.hpp>

#include <algorithm>
#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace output = mediaserver::output;
namespace scene = mediaserver::scene;
namespace topology = mediaserver::topology;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define LAYOUT_CHECK(...)                                                      \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] topology::HardwareDevice
device(core::u64 id, topology::HardwareDeviceKind kind, std::string name,
       core::i32 x, core::i32 y) {
  topology::HardwareDevice made{};
  made.node = doc::NodeId{id};
  made.kind = kind;
  made.name = std::move(name);
  made.grid_x = x;
  made.grid_y = y;
  return made;
}

[[nodiscard]] topology::HardwareOutputBinding
binding(core::u64 id, core::u64 server, core::u8 port, core::u32 feed,
        core::u32 width, core::u32 height) {
  topology::HardwareOutputBinding made{};
  made.node = doc::NodeId{id};
  made.render_server = doc::NodeId{server};
  made.logical_port = port;
  made.feed_index = feed;
  made.width = width;
  made.height = height;
  return made;
}

[[nodiscard]] topology::HardwareLink link(core::u64 id, core::u64 from,
                                          core::u8 from_port, core::u64 to,
                                          core::u8 to_port) {
  return topology::HardwareLink{doc::NodeId{id},
                                {doc::NodeId{from}, from_port},
                                {doc::NodeId{to}, to_port}};
}

// Render A drives a COEX processor feeding the main wall with two outputs;
// Render B drives a generic processor feeding a side wall placed to the left
// of it on the map.
[[nodiscard]] topology::HardwareMap stage() {
  topology::HardwareMap map{};
  using Kind = topology::HardwareDeviceKind;
  auto coex = device(3U, Kind::LedProcessor, "MX40", 2, 0);
  coex.protocol = topology::HardwareProcessorProtocol::NovastarCoex;
  map.devices = {
      device(1U, Kind::RenderServer, "Render A", 0, 0),
      device(2U, Kind::RenderServer, "Render B", 0, 4),
      coex,
      device(4U, Kind::LedProcessor, "Side processor", 2, 4),
      device(5U, Kind::LedWall, "Main wall", 5, 0),
      device(6U, Kind::LedWall, "Side wall", 3, 7),
  };
  map.links = {
      link(10U, 1U, 1U, 3U, topology::processor_input_port(1U)),
      link(11U, 1U, 2U, 3U, topology::processor_input_port(2U)),
      link(12U, 3U, topology::processor_led_output_port, 5U, 0U),
      link(13U, 2U, 1U, 4U, topology::processor_input_port(1U)),
      link(14U, 2U, 2U, 4U, topology::processor_input_port(2U)),
      link(15U, 4U, topology::processor_led_output_port, 6U, 0U),
  };
  map.output_bindings = {
      binding(20U, 1U, 1U, 0U, 3840U, 2160U),
      binding(21U, 1U, 2U, 1U, 3840U, 2160U),
      binding(22U, 2U, 1U, 2U, 1920U, 1080U),
      binding(23U, 2U, 2U, 3U, 1920U, 1080U),
  };
  return map;
}

[[nodiscard]] topology::HardwareProcessorNode reported_processor() {
  topology::HardwareProcessorNode node{};
  node.device = doc::NodeId{3U};
  node.reachability = topology::HardwareRuntimeReachability::Online;
  topology::HardwareProcessorInput first{};
  first.number = 1U;
  first.name = "HDMI 1";
  topology::HardwareProcessorInput second{};
  second.number = 2U;
  second.name = "HDMI 2";
  node.inputs = {first, second};
  // A 6912x1536 screen: HDMI 1's top 3456x1536 on the left, HDMI 2's on
  // the right, both one-to-one.
  topology::HardwareProcessorScreen screen{};
  screen.name = "Screen 1";
  screen.width = 6912U;
  screen.height = 1536U;
  topology::HardwareProcessorLayer left{};
  left.input = 1U;
  left.source = {0U, 0U, 3456U, 1536U};
  left.screen = {0U, 0U, 3456U, 1536U};
  topology::HardwareProcessorLayer right = left;
  right.input = 2U;
  right.screen = {3456U, 0U, 3456U, 1536U};
  screen.layers = {left, right};
  node.screens = {screen};
  return node;
}

void a_reporting_processor_lays_out_its_screen() {
  const topology::HardwareMap map = stage();
  topology::HardwareRuntimeSnapshot runtime{};
  runtime.processors.push_back(reported_processor());

  const output::FeedLayoutPlan plan = output::plan_feed_layout(map, runtime);
  // The side wall is further left on the map (grid x - y = -4), so it comes
  // first; the main wall follows it.
  LAYOUT_CHECK(plan.screens.size() == 2U);
  LAYOUT_CHECK(plan.screens[0].name == "Side wall" && !plan.screens[0].reported);
  LAYOUT_CHECK(plan.screens[0].canvas == scene::CanvasRect{0U, 0U, 3840U, 1080U});
  LAYOUT_CHECK(plan.screens[1].name == "Main wall" && plan.screens[1].reported);
  LAYOUT_CHECK(plan.screens[1].canvas ==
               scene::CanvasRect{3840U, 0U, 6912U, 1536U});
  LAYOUT_CHECK(plan.canvas == scene::Canvas{3840U + 6912U, 1536U});

  LAYOUT_CHECK(plan.viewports.size() == 4U);
  // Side wall: two whole 1920x1080 windows side by side.
  LAYOUT_CHECK(plan.viewports[0].window == 2U &&
               plan.viewports[0].source ==
                   scene::CanvasRect{0U, 0U, 1920U, 1080U} &&
               plan.viewports[0].viewport ==
                   scene::CanvasRect{0U, 0U, 1920U, 1080U});
  LAYOUT_CHECK(plan.viewports[1].window == 3U &&
               plan.viewports[1].source ==
                   scene::CanvasRect{1920U, 0U, 1920U, 1080U});
  // Main wall: each window's top-left 3456x1536, the processor's crop.
  LAYOUT_CHECK(plan.viewports[2].window == 0U &&
               plan.viewports[2].name == "Main wall - HDMI 1" &&
               plan.viewports[2].viewport ==
                   scene::CanvasRect{0U, 0U, 3456U, 1536U} &&
               plan.viewports[2].source ==
                   scene::CanvasRect{3840U, 0U, 3456U, 1536U});
  LAYOUT_CHECK(plan.viewports[3].window == 1U &&
               plan.viewports[3].source ==
                   scene::CanvasRect{3840U + 3456U, 0U, 3456U, 1536U});
  LAYOUT_CHECK((plan.windows == std::vector<core::u32>{0U, 1U, 2U, 3U}));
  LAYOUT_CHECK(plan.notes.empty());
}

void a_silent_processor_says_how_it_was_laid_out() {
  const topology::HardwareMap map = stage();
  const output::FeedLayoutPlan plan =
      output::plan_feed_layout(map, topology::HardwareRuntimeSnapshot{});
  LAYOUT_CHECK(plan.canvas == scene::Canvas{3840U + 7680U, 2160U});
  LAYOUT_CHECK(plan.viewports.size() == 4U);
  LAYOUT_CHECK(plan.viewports[2].name == "Main wall - input 1" &&
               plan.viewports[2].source ==
                   scene::CanvasRect{3840U, 0U, 3840U, 2160U});
  LAYOUT_CHECK(plan.notes.size() == 1U &&
               plan.notes[0] == "MX40 has not reported its screens, so its "
                                "inputs are laid side by side");
}

void layers_that_do_not_fit_are_cut_and_named() {
  topology::HardwareMap map = stage();
  topology::HardwareRuntimeSnapshot runtime{};
  topology::HardwareProcessorNode node = reported_processor();
  // The left layer wants a 4096-wide crop of a 3840-wide window; the right
  // shows input 3, which nothing feeds; input 2 is then on no screen.
  node.screens[0].layers[0].source.width = 4096U;
  node.screens[0].layers[1].input = 3U;
  runtime.processors.push_back(node);
  const output::FeedLayoutPlan plan = output::plan_feed_layout(map, runtime);
  LAYOUT_CHECK(plan.viewports.size() == 3U);
  LAYOUT_CHECK(plan.viewports[2].viewport ==
               scene::CanvasRect{0U, 0U, 3840U, 1536U});
  LAYOUT_CHECK(plan.notes.size() == 3U);
  LAYOUT_CHECK(plan.notes[0] == "HDMI 1 takes more than its 3840x2160 signal; "
                                "the viewport is cut to fit");
  LAYOUT_CHECK(plan.notes[1] == "Main wall shows input 3, which no render "
                                "output feeds");
  LAYOUT_CHECK(plan.notes[2] == "Main wall: HDMI 2 is cabled but not on any "
                                "screen");
}

void a_brompton_says_it_reports_no_layout() {
  topology::HardwareMap map = stage();
  map.devices[2].protocol = topology::HardwareProcessorProtocol::BromptonTessera;
  map.devices[2].name = "Brompton S8";
  const output::FeedLayoutPlan plan =
      output::plan_feed_layout(map, topology::HardwareRuntimeSnapshot{});
  LAYOUT_CHECK(plan.notes.size() == 1U &&
               plan.notes[0] == "Brompton S8 reports no screen layout over IP "
                                "control, so its inputs are laid side by side");
}

void nothing_cabled_plans_nothing() {
  topology::HardwareMap map = stage();
  map.links.clear();
  const output::FeedLayoutPlan plan =
      output::plan_feed_layout(map, topology::HardwareRuntimeSnapshot{});
  LAYOUT_CHECK(plan.empty() && plan.windows.empty() &&
               plan.canvas == scene::Canvas{});
}

void very_wide_walls_wrap_onto_rows() {
  topology::HardwareMap map{};
  using Kind = topology::HardwareDeviceKind;
  map.devices = {device(1U, Kind::RenderServer, "Render", 0, 0)};
  for (core::u64 wall = 0U; wall < 5U; ++wall) {
    map.devices.push_back(device(10U + wall, Kind::LedWall,
                                 "Wall " + std::to_string(wall),
                                 static_cast<core::i32>(wall), 0));
    map.links.push_back(link(20U + wall, 1U, static_cast<core::u8>(wall + 1U),
                             10U + wall, 0U));
    map.output_bindings.push_back(binding(30U + wall, 1U,
                                          static_cast<core::u8>(wall + 1U),
                                          static_cast<core::u32>(wall), 3840U,
                                          2160U));
  }
  const output::FeedLayoutPlan plan =
      output::plan_feed_layout(map, topology::HardwareRuntimeSnapshot{});
  // Four 3840 walls fill a 15360 row; the fifth starts the next one.
  LAYOUT_CHECK(plan.canvas == scene::Canvas{15360U, 4320U});
  LAYOUT_CHECK(plan.screens[4].canvas ==
               scene::CanvasRect{0U, 2160U, 3840U, 2160U});
  LAYOUT_CHECK(plan.viewports[4].name == "Wall 4");
}

// One 5760x1080 output fans out to three processors (NVIDIA Mosaic). Each
// processor's viewport crops its own third of the one window, and the walls
// land side by side on the canvas.
void a_shared_output_gives_each_processor_its_tile() {
  using Kind = topology::HardwareDeviceKind;
  topology::HardwareMap map{};
  map.devices = {
      device(1U, Kind::RenderServer, "Render A", 0, 0),
      device(2U, Kind::LedProcessor, "Left", 2, 0),
      device(3U, Kind::LedProcessor, "Middle", 4, 0),
      device(4U, Kind::LedProcessor, "Right", 6, 0),
  };
  map.links = {
      link(10U, 1U, 1U, 3U, topology::processor_input_port(1U)),
      link(11U, 1U, 1U, 2U, topology::processor_input_port(1U)),
      link(12U, 1U, 1U, 4U, topology::processor_input_port(1U)),
  };
  map.output_bindings = {binding(20U, 1U, 1U, 7U, 5760U, 1080U)};

  const output::FeedLayoutPlan plan =
      output::plan_feed_layout(map, topology::HardwareRuntimeSnapshot{});
  LAYOUT_CHECK(plan.windows == std::vector<core::u32>{7U});
  LAYOUT_CHECK(plan.viewports.size() == 3U);
  for (std::size_t index = 0U; index < 3U; ++index) {
    const output::FeedLayoutViewport &viewport = plan.viewports[index];
    const core::u32 x = static_cast<core::u32>(index) * 1920U;
    LAYOUT_CHECK(viewport.window == 7U);
    LAYOUT_CHECK(viewport.source == (scene::CanvasRect{x, 0U, 1920U, 1080U}));
    LAYOUT_CHECK(viewport.viewport == (scene::CanvasRect{x, 0U, 1920U, 1080U}));
  }
  LAYOUT_CHECK(plan.viewports[0].name == "Left - input 1");
  LAYOUT_CHECK(plan.viewports[2].name == "Right - input 1");
  LAYOUT_CHECK(plan.canvas == (scene::Canvas{5760U, 1080U}));
}

// Four 3840x2160 outputs, each into a processor that shows its top left
// 2880x1536 on its own wall section. None reports a layout, so the wall cards'
// sizes say how much each takes: the canvas is the four sections side by
// side, one continuous 11520x1536 wall.
void wall_cards_size_a_silent_processors_viewport() {
  using Kind = topology::HardwareDeviceKind;
  topology::HardwareMap map{};
  map.devices = {device(1U, Kind::RenderServer, "Render", 0, 0)};
  for (core::u64 index = 0U; index < 4U; ++index) {
    const auto column = static_cast<core::i32>(index) * 3;
    map.devices.push_back(device(10U + index, Kind::LedProcessor,
                                 "Processor " + std::to_string(index + 1U),
                                 column, 2));
    topology::HardwareDevice wall =
        device(20U + index, Kind::LedWall,
               "Section " + std::to_string(index + 1U), column, 5);
    wall.width = 2880U;
    wall.height = 1536U;
    map.devices.push_back(wall);
    const auto port = static_cast<core::u8>(index + 1U);
    map.links.push_back(link(30U + index, 1U, port, 10U + index,
                             topology::processor_input_port(1U)));
    map.links.push_back(link(40U + index, 10U + index,
                             topology::processor_led_output_port, 20U + index,
                             0U));
    map.output_bindings.push_back(binding(50U + index, 1U, port,
                                          static_cast<core::u32>(index), 3840U,
                                          2160U));
  }
  // An evaluated map keeps every array in node id order.
  std::sort(map.devices.begin(), map.devices.end(),
            [](const auto &left, const auto &right) {
              return left.node < right.node;
            });
  std::sort(map.links.begin(), map.links.end(),
            [](const auto &left, const auto &right) {
              return left.node < right.node;
            });
  const output::FeedLayoutPlan plan =
      output::plan_feed_layout(map, topology::HardwareRuntimeSnapshot{});
  LAYOUT_CHECK(plan.canvas == (scene::Canvas{11520U, 1536U}));
  LAYOUT_CHECK(plan.viewports.size() == 4U);
  for (std::size_t index = 0U; index < 4U; ++index) {
    const output::FeedLayoutViewport &viewport = plan.viewports[index];
    LAYOUT_CHECK(viewport.window == index);
    LAYOUT_CHECK(viewport.source ==
                 (scene::CanvasRect{static_cast<core::u32>(index) * 2880U, 0U,
                                    2880U, 1536U}));
    LAYOUT_CHECK(viewport.viewport == (scene::CanvasRect{0U, 0U, 2880U, 1536U}));
  }
  LAYOUT_CHECK(plan.notes.size() == 4U &&
               plan.notes[0] == "Section 1 takes the top left 2880x1536 of "
                                "its signal, the size on its card");

  // A wall as big as its signal, or bigger, shows the whole signal.
  for (topology::HardwareDevice &device : map.devices) {
    if (device.kind == Kind::LedWall) {
      device.width = 3840U;
      device.height = 2160U;
    }
  }
  const output::FeedLayoutPlan whole =
      output::plan_feed_layout(map, topology::HardwareRuntimeSnapshot{});
  LAYOUT_CHECK(whole.canvas == (scene::Canvas{15360U, 2160U}));
  LAYOUT_CHECK(whole.notes.empty());
}

// A processor driving two walls, one per screen it reports: each screen is
// named after the wall card that carries its name.
void screens_take_the_names_of_their_walls() {
  topology::HardwareMap map = stage();
  topology::HardwareDevice second = device(7U, topology::HardwareDeviceKind::LedWall,
                                           "Ceiling", 6, 0);
  map.devices.push_back(second);
  map.links.push_back(link(16U, 3U, topology::processor_led_output_port, 7U, 0U));
  topology::HardwareRuntimeSnapshot runtime{};
  topology::HardwareProcessorNode node = reported_processor();
  topology::HardwareProcessorScreen ceiling = node.screens[0];
  node.screens[0].name = "Main wall";
  node.screens[0].layers.pop_back();
  ceiling.name = "Ceiling";
  ceiling.width = 3456U;
  ceiling.layers.erase(ceiling.layers.begin());
  ceiling.layers[0].screen.x = 0U;
  node.screens.push_back(ceiling);
  runtime.processors.push_back(node);
  const output::FeedLayoutPlan plan = output::plan_feed_layout(map, runtime);
  // The side wall stands left of both on the map, so it comes first.
  LAYOUT_CHECK(plan.screens.size() == 3U);
  LAYOUT_CHECK(plan.screens[1].name == "Main wall");
  LAYOUT_CHECK(plan.screens[2].name == "Ceiling");
  LAYOUT_CHECK(plan.screens[2].canvas.width == 3456U);
  LAYOUT_CHECK(plan.viewports.size() == 4U);
  LAYOUT_CHECK(plan.viewports[2].name == "Main wall - HDMI 1");
  LAYOUT_CHECK(plan.viewports[3].name == "Ceiling - HDMI 2");
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 9U> cases{{
    {"wall cards size a silent processor's viewport",
     &wall_cards_size_a_silent_processors_viewport},
    {"screens take the names of their walls",
     &screens_take_the_names_of_their_walls},
    {"a shared output gives each processor its tile",
     &a_shared_output_gives_each_processor_its_tile},
    {"a brompton says it reports no layout",
     &a_brompton_says_it_reports_no_layout},
    {"a reporting processor lays out its screen",
     &a_reporting_processor_lays_out_its_screen},
    {"a silent processor says how it was laid out",
     &a_silent_processor_says_how_it_was_laid_out},
    {"layers that do not fit are cut and named",
     &layers_that_do_not_fit_are_cut_and_named},
    {"nothing cabled plans nothing", &nothing_cabled_plans_nothing},
    {"very wide walls wrap onto rows", &very_wide_walls_wrap_onto_rows},
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
