#include <mediaserver/editor/element.hpp>
#include <mediaserver/editor/inspector_panel.hpp>
#include <mediaserver/editor/mapping_panels.hpp>
#include <mediaserver/editor/timeline_panel.hpp>
#include <mediaserver/editor/workspace_panels.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/gui/ui_renderer.hpp>
#include <mediaserver/rhi/device.hpp>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Renders the operator panels with a representative show - groups, lights,
// clips on a track, keyed properties, a cluster of outputs - into an offscreen
// image at any interface scale, and writes it as a BMP.
//
// It exists so the panels can be looked at, at 100% and at 4K's 150% or 200%,
// without a Director, a project, or anyone's desktop: the layout questions
// "does this read", "can that be hit" are visual, and a screenshot of a
// running editor shows only whatever its scene happens to hold.
//
//   mediaserver_editor_ui_snapshot <out.bmp> [scale] [width] [height]
//       [hover_x hover_y] [view: media|model|light|renderer|environment|curves|handles|summaries|feed]
//   feed draws Feed Mapping for one 15360x2160 Mosaic window carrying four
//   2880x1536 wall sections, two of them named, with the first selected;
//   curves draws the editor with the Timeline switched to Curves mode;
//   handles does too, with two Opacity keys selected - one Aligned, one
//   Broken with authored weights - so their tangent handles show;
//   summaries closes the LED Wall group so its row summarises its clips;
//   compositor draws an LED Car Process graph - plates into the car, a blur
//   feeding one plate, a composite into Output - with the car selected so
//   its Parameters show; compositor-far and compositor-near draw it zoomed
//   out and in; compositor-wire holds a wire being drawn, near a port.
namespace {

namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;
namespace rhi = mediaserver::rhi;
namespace output = mediaserver::output;
namespace scene = mediaserver::scene;
namespace topology = mediaserver::topology;

[[nodiscard]] std::optional<gui::FontAtlas> load_font(float scale) {
  constexpr std::array<const char *, 2U> candidates{
      "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf"};
  for (const char *path : candidates) {
    // The editor's own extra sizes, for the Compositor's zoomed text.
    constexpr std::array<float, 9U> sizes{0.5F, 0.625F, 0.75F, 0.875F, 1.25F,
                                          1.5F, 1.75F, 2.0F, 2.5F};
    auto atlas = gui::FontAtlas::create(path, 14U, scale, sizes);
    if (atlas.has_value()) {
      return atlas;
    }
  }
  return std::nullopt;
}

[[nodiscard]] gui::Theme editor_like_theme() {
  // Mirrors editor_shell.cpp's editor_theme(); kept here so the tool needs
  // nothing from the shell.
  gui::Theme theme{};
  theme.background = gui::UiColor{0.031F, 0.039F, 0.051F, 1.0F};
  theme.panel = gui::UiColor{0.067F, 0.080F, 0.098F, 1.0F};
  theme.header = gui::UiColor{0.086F, 0.102F, 0.125F, 1.0F};
  theme.row = gui::UiColor{0.075F, 0.088F, 0.108F, 1.0F};
  theme.row_alt = gui::UiColor{0.067F, 0.080F, 0.098F, 1.0F};
  theme.widget = gui::UiColor{0.102F, 0.122F, 0.149F, 1.0F};
  theme.widget_hover = gui::UiColor{0.141F, 0.169F, 0.204F, 1.0F};
  theme.widget_active = gui::UiColor{0.000F, 0.345F, 0.565F, 1.0F};
  theme.accent = gui::UiColor{0.090F, 0.650F, 0.930F, 1.0F};
  theme.selection = gui::UiColor{0.039F, 0.255F, 0.420F, 1.0F};
  theme.track = gui::UiColor{0.039F, 0.047F, 0.059F, 1.0F};
  theme.text = gui::UiColor{0.850F, 0.880F, 0.910F, 1.0F};
  theme.text_muted = gui::UiColor{0.520F, 0.570F, 0.620F, 1.0F};
  theme.divider = gui::UiColor{0.118F, 0.137F, 0.165F, 1.0F};
  theme.border = gui::UiColor{0.165F, 0.192F, 0.231F, 1.0F};
  theme.playhead = gui::UiColor{0.090F, 0.650F, 0.930F, 1.0F};
  theme.keyframe = gui::UiColor{0.880F, 0.930F, 1.000F, 1.0F};
  theme.good = gui::UiColor{0.250F, 0.840F, 0.420F, 1.0F};
  theme.warning = gui::UiColor{0.960F, 0.400F, 0.320F, 1.0F};
  theme.padding = 5.0F;
  return theme;
}

[[nodiscard]] editor::ElementInfo element(core::u64 id, editor::ElementKind kind,
                                          std::string name = {}) {
  editor::ElementInfo info{};
  info.node = doc::NodeId{id};
  info.kind = kind;
  info.display_name = std::move(name);
  return info;
}

struct Show final {
  std::vector<editor::ElementInfo> elements;
  std::vector<scene::ChannelKeyframes> keyframes;
  std::vector<scene::TimelineLane> lanes;
  std::vector<doc::NodeId> selection;
};

[[nodiscard]] Show sample_show() {
  Show show;
  auto props = element(20U, editor::ElementKind::Group, "Stage Props");
  props.group_order = 5;
  auto box = element(21U, editor::ElementKind::Object, "Floor Riser");
  box.parent_group = doc::NodeId{20U};
  box.group_order = 1;
  auto truss = element(22U, editor::ElementKind::Model, "Truss_Structure");
  truss.parent_group = doc::NodeId{20U};
  auto wall = element(30U, editor::ElementKind::Group, "LED Wall");
  wall.group_order = 4;
  auto content = element(31U, editor::ElementKind::Media, "Particle_Ribbons.mov");
  content.parent_group = doc::NodeId{30U};
  content.timing = scene::ClipTiming{20, 340, 0, 24, 30};
  auto logo = element(32U, editor::ElementKind::Media, "Logo_Intro.png");
  logo.parent_group = doc::NodeId{30U};
  logo.timing = scene::ClipTiming{120, 260, 0, 0, 0};
  logo.visible = false;
  logo.effective_visible = false;
  auto sun = element(40U, editor::ElementKind::DirectionalLight, "Key Sun");
  sun.group_order = 3;
  auto spot = element(41U, editor::ElementKind::PointLight, "Beam Spot");
  spot.local_kind = scene::LocalLightKind::Spot;
  spot.group_order = 2;
  auto wash = element(42U, editor::ElementKind::PointLight);
  wash.group_order = 1;
  auto floor = element(60U, editor::ElementKind::Object, "Floor");
  floor.locked = true;
  auto camera = element(50U, editor::ElementKind::Camera, "Camera_Main");
  show.elements = {props, box, truss, wall, content, logo,
                   sun,   spot, wash, floor, camera};
  show.selection = {doc::NodeId{31U}};

  const auto keys = [](core::u64 target, scene::PropertyChannel channel,
                       std::vector<core::i64> frames,
                       std::vector<double> values) {
    scene::ChannelKeyframes entry{};
    entry.target = doc::NodeId{target};
    entry.component = channel;
    entry.frames = std::move(frames);
    entry.values = std::move(values);
    entry.interpolations.assign(entry.frames.size(),
                                scene::KeyInterpolation::Cubic);
    entry.tangents.assign(entry.frames.size(), scene::KeyTangent{});
    return entry;
  };
  show.keyframes.push_back(keys(31U, scene::PropertyChannel::PositionX,
                                {40, 120, 220, 330}, {0.0, 1.2, -0.4, 0.8}));
  // One key of each interpolation, so each glyph shows.
  show.keyframes.back().interpolations = {
      scene::KeyInterpolation::Linear, scene::KeyInterpolation::Cubic,
      scene::KeyInterpolation::Hold, scene::KeyInterpolation::Cubic};
  show.keyframes.back().tangents = {std::nullopt, scene::KeyTangent{},
                                    std::nullopt, scene::KeyTangent{}};
  show.keyframes.push_back(keys(31U, scene::PropertyChannel::Opacity,
                                {20, 60, 300, 360}, {0.0, 1.0, 1.0, 0.0}));
  show.keyframes.push_back(keys(21U, scene::PropertyChannel::RotationYaw,
                                {0, 300}, {0.0, 0.5}));
  return show;
}

[[nodiscard]] editor::OutputMonitorState sample_outputs(
    std::vector<editor::OutputMonitorState::Node> &nodes) {
  editor::OutputMonitorState state{};
  state.frame = 1'342U;
  state.width = 3840U;
  state.height = 1080U;
  state.frames_per_second = 60.0F;
  state.playing = true;
  const auto node = [](std::string name, core::u32 index, core::u32 width,
                       core::u32 height,
                       editor::OutputMonitorState::Node::Status status) {
    editor::OutputMonitorState::Node entry{};
    entry.device = doc::NodeId{100U + index};
    entry.name = std::move(name);
    entry.endpoint = "10.0.0." + std::to_string(20U + index) + ":47840";
    entry.node_index = index;
    entry.width = width;
    entry.height = height;
    entry.status = status;
    return entry;
  };
  using Status = editor::OutputMonitorState::Node::Status;
  nodes.push_back(node("LED MAIN", 0U, 3840U, 1080U, Status::Running));
  nodes.push_back(node("PROJECTOR L", 1U, 1920U, 1200U, Status::Running));
  nodes.push_back(node("PROJECTOR R", 2U, 1920U, 1200U, Status::Starting));
  nodes.push_back(node("NDI PREVIEW", 3U, 1920U, 1080U, Status::Unreachable));
  nodes[0].sync = editor::OutputMonitorState::Node::SyncStatus::Locked;
  nodes[0].content = editor::OutputMonitorState::Node::ContentStatus::Ready;
  nodes[0].assets_ready = 12U;
  nodes[0].assets_required = 12U;
  nodes[1].content = editor::OutputMonitorState::Node::ContentStatus::Missing;
  nodes[1].assets_missing = 2U;
  nodes[3].detail = "connection refused";
  state.nodes = nodes;
  state.cluster.health = editor::OutputMonitorState::Cluster::Health::Degraded;
  state.cluster.configured_nodes = 4U;
  state.cluster.live_sessions = 2U;
  state.cluster.release_lag_frames = 1U;
  return state;
}

// Plates into an LED Car Process, the car and a key into a Composite, and
// Output - the graph whose labels are the longest the panel draws.
struct CompositorSample final {
  mediaserver::compositor::CompositorGraph graph;
  std::vector<editor::ElementInfo> elements;
};

[[nodiscard]] CompositorSample sample_compositor() {
  namespace compositor = mediaserver::compositor;
  CompositorSample sample;
  core::u64 next_id = 100U;
  const auto add_node = [&](compositor::NodeKind kind, float x, float y,
                            compositor::NodeParameters parameters) {
    compositor::GraphNode node{};
    node.node = doc::NodeId{next_id++};
    node.kind = kind;
    node.editor_x = x;
    node.editor_y = y;
    node.parameters = std::move(parameters);
    sample.graph.nodes.push_back(node);
    return node.node;
  };
  const auto connect = [&](doc::NodeId from, core::u8 from_port,
                           doc::NodeId to, core::u8 to_port) {
    sample.graph.edges.push_back(compositor::GraphEdge{
        doc::NodeId{next_id++}, compositor::EdgeEndpoint{from, from_port},
        compositor::EdgeEndpoint{to, to_port},
        static_cast<core::i64>(sample.graph.edges.size())});
  };
  constexpr std::array<std::string_view, 4U> plates{
      "Plate_Front_A_Cam.mov", "Plate_Front_Right_3-4.mov",
      "Plate_Right_Long_Lens.mov", "Ceiling_Reflection_Pass.mov"};
  constexpr std::array<core::u8, 4U> plate_ports{0U, 1U, 2U, 8U};
  std::vector<doc::NodeId> layers;
  for (std::size_t index = 0U; index < plates.size(); ++index) {
    const core::u64 element_id = 900U + index;
    editor::ElementInfo plate = element(element_id, editor::ElementKind::Media,
                                        std::string{plates[index]});
    sample.elements.push_back(plate);
    layers.push_back(add_node(compositor::NodeKind::SceneLayer, 240.0F,
                              30.0F + static_cast<float>(index) * 112.0F,
                              compositor::SceneLayerParameters{
                                  doc::NodeId{element_id}}));
  }
  const doc::NodeId blur = add_node(compositor::NodeKind::Blur, 20.0F, 70.0F,
                                    compositor::BlurParameters{6.0F});
  const doc::NodeId car =
      add_node(compositor::NodeKind::LedCarProcess, 500.0F, 40.0F,
               compositor::LedCarProcessParameters{});
  const doc::NodeId composite = add_node(compositor::NodeKind::Composite,
                                         780.0F, 120.0F,
                                         compositor::CompositeParameters{});
  const doc::NodeId out = add_node(compositor::NodeKind::Output, 1000.0F, 160.0F,
                                   compositor::OutputParameters{});
  connect(blur, 0U, layers[1], 0U);
  for (std::size_t index = 0U; index < layers.size(); ++index) {
    connect(layers[index], 0U, car, plate_ports[index]);
  }
  connect(car, 0U, composite, 0U);
  connect(composite, 0U, out, 0U);
  return sample;
}

bool write_bmp(const std::string &path, core::u32 width, core::u32 height,
               const std::vector<core::u8> &rgba) {
  std::ofstream out{path, std::ios::binary};
  if (!out) {
    return false;
  }
  const core::u32 row = width * 3U;
  const core::u32 padded = (row + 3U) & ~3U;
  const core::u32 image = padded * height;
  const auto u16 = [&](core::u16 value) {
    out.put(static_cast<char>(value & 0xFFU));
    out.put(static_cast<char>((value >> 8U) & 0xFFU));
  };
  const auto u32 = [&](core::u32 value) {
    for (core::u32 shift = 0U; shift < 32U; shift += 8U) {
      out.put(static_cast<char>((value >> shift) & 0xFFU));
    }
  };
  out.put('B');
  out.put('M');
  u32(54U + image);
  u32(0U);
  u32(54U);
  u32(40U);
  u32(width);
  u32(height);
  u16(1U);
  u16(24U);
  u32(0U);
  u32(image);
  u32(2835U);
  u32(2835U);
  u32(0U);
  u32(0U);
  std::vector<char> line(padded, 0);
  for (core::u32 y = height; y-- > 0U;) {
    for (core::u32 x = 0U; x < width; ++x) {
      const std::size_t at = (static_cast<std::size_t>(y) * width + x) * 4U;
      line[x * 3U + 0U] = static_cast<char>(rgba[at + 2U]);
      line[x * 3U + 1U] = static_cast<char>(rgba[at + 1U]);
      line[x * 3U + 2U] = static_cast<char>(rgba[at + 0U]);
    }
    out.write(line.data(), static_cast<std::streamsize>(line.size()));
  }
  return static_cast<bool>(out);
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    std::puts("usage: mediaserver_editor_ui_snapshot <out.bmp> [scale] "
              "[width] [height] [hover_x hover_y] [view: media|model|light|renderer|environment|curves|handles|summaries|feed]");
    return 2;
  }
  const std::string out_path = argv[1];
  const float scale = argc > 2 ? std::strtof(argv[2], nullptr) : 1.0F;
  const float logical_width = argc > 3 ? std::strtof(argv[3], nullptr) : 1500.0F;
  const float logical_height =
      argc > 4 ? std::strtof(argv[4], nullptr) : 620.0F;
  const float hover_x = argc > 6 ? std::strtof(argv[5], nullptr) : -100.0F;
  const float hover_y = argc > 6 ? std::strtof(argv[6], nullptr) : -100.0F;
  const std::string_view view = argc > 7 ? argv[7] : "";
  const bool handles = view == "handles";
  const bool curves = view == "curves" || handles;
  const bool summaries = view == "summaries";
  const bool feed = view == "feed";
  const bool graph_view = view.substr(0U, 10U) == "compositor";
  const std::string_view properties =
      curves || summaries || feed || graph_view ? std::string_view{} : view;

  rhi::DeviceConfig config{};
  auto created = rhi::Device::create(config);
  if (!created) {
    std::puts("no device");
    return 1;
  }
  rhi::Device device = std::move(created).value();
  auto renderer = gui::UiRenderer::create(device, rhi::TextureFormat::Rgba8Unorm);
  if (!renderer) {
    std::puts("no renderer");
    return 1;
  }
  auto font = load_font(scale);
  if (!font.has_value()) {
    std::puts("no font");
    return 1;
  }

  gui::InputState input;
  gui::UiDrawList list;
  list.set_scale(scale);
  gui::UiContext ctx{input, list, *font, editor_like_theme()};

  Show show = sample_show();
  if (handles) {
    scene::ChannelKeyframes &opacity = show.keyframes[1];
    opacity.tangents[1] =
        scene::KeyTangent{scene::KeyTangentMode::Aligned, 0.004, 0.004};
    opacity.tangents[2] = scene::KeyTangent{scene::KeyTangentMode::Broken,
                                            -0.002, -0.03, 60.0, 15.0};
  }
  std::vector<editor::OutputMonitorState::Node> nodes;
  const editor::OutputMonitorState outputs = sample_outputs(nodes);

  editor::ScenePanel scene_panel{gui::ui_id("snapshot.scene")};
  editor::TimelinePanel timeline_panel{gui::ui_id("snapshot.timeline")};
  if (handles) {
    const std::array<editor::TimelineKeyAddress, 2U> chosen{{
        {doc::NodeId{31U}, scene::PropertyChannel::Opacity, 60},
        {doc::NodeId{31U}, scene::PropertyChannel::Opacity, 300}}};
    timeline_panel.replace_key_selection(chosen);
  }
  editor::OutputMonitorPanel output_panel{gui::ui_id("snapshot.output")};
  editor::InspectorPanel inspector_panel{gui::ui_id("snapshot.inspector")};
  editor::SceneSettingsPanel settings_panel;
  editor::FeedMappingPanel feed_panel{gui::ui_id("snapshot.feed")};
  editor::CompositorPanel compositor_panel{gui::ui_id("snapshot.compositor")};
  const CompositorSample compositor_sample = sample_compositor();
  // The car's position on screen at 100%: the graph canvas starts below the
  // 26-unit toolbar.
  constexpr float car_press_x = 500.0F + 60.0F;
  constexpr float car_press_y = 26.0F + 40.0F + 10.0F;
  const scene::Canvas wall_canvas{11'520U, 1'536U};
  output::FeedMap feed_map;
  for (core::u32 index = 0U; index < 4U; ++index) {
    output::OutputMap section{};
    section.node = doc::NodeId{500U + index};
    section.order = index;
    section.source = scene::CanvasRect{index * 2'880U, 0U, 2'880U, 1'536U};
    section.viewport = scene::CanvasRect{index * 3'840U, 0U, 2'880U, 1'536U};
    feed_map.maps.push_back(section);
  }
  feed_map.maps[0].name = "Processor A - Stage Left";
  feed_map.maps[1].name = "Processor B";
  for (core::u32 index = 0U; index < 2U; ++index) {
    output::ProjectionSurface surface{};
    surface.node = doc::NodeId{600U + index};
    surface.name = index == 0U ? "LED Surface 1" : "Upstage Header";
    surface.locked = index == 1U;
    surface.geometry.raster = scene::Canvas{2'880U, 1'536U};
    feed_map.surfaces.push_back(surface);
  }
  topology::HardwareMap feed_hardware;
  topology::HardwareOutputBinding mosaic{};
  mosaic.width = 15'360U;
  mosaic.height = 2'160U;
  feed_hardware.output_bindings.push_back(mosaic);
  auto inspected = show.elements[4];
  if (properties == "model") inspected = show.elements[2];
  if (properties == "light") inspected = show.elements[7];
  inspected.source = properties == "model"
      ? "T:/Production/Shows/Autumn/Models/Truss_Structure.glb"
      : "T:/Production/Shows/Autumn/Media/Particle_Ribbons.mov";
  inspected.ies_source = "T:/Production/Lighting/Profiles/practical.ies";
  editor::ProjectSettingsState settings;
  settings.stage_node = doc::NodeId{1U};
  settings.environment.map_source = "T:/Production/Lighting/stage_environment.exr";
  editor::EditorTimeline timeline{};
  timeline.show_frame = 140U;
  timeline.program_frame = 140U;
  timeline.first_frame = 0U;
  timeline.last_frame = 420U;
  timeline.rate_numerator = 30U;

  const float scene_width = 300.0F;
  const float output_height = 190.0F;
  const gui::Rect window{0.0F, 0.0F, logical_width, logical_height};
  const gui::Rect scene_bounds{4.0F, 4.0F, scene_width,
                               logical_height - 8.0F};
  const gui::Rect timeline_bounds{scene_width + 8.0F, 4.0F,
                                  logical_width - scene_width - 12.0F,
                                  logical_height - output_height - 12.0F};
  const gui::Rect output_bounds{scene_width + 8.0F,
                                timeline_bounds.y + timeline_bounds.height + 4.0F,
                                logical_width - scene_width - 12.0F,
                                output_height};

  // A few frames: the first lets panels settle their view state (the
  // timeline fits the show range on its first draw); the last is the one kept.
  // Curves is switched on the way an operator does it: a click on the
  // Timeline toolbar's Curves toggle, pressed and released over two frames.
  // The LED Wall closes the same way, by a click on its twirl, its row
  // being the Timeline's second.
  const bool clicks = curves || summaries || feed;
  const int passes = clicks || graph_view ? 8 : 3;
  for (int pass = 0; pass < passes; ++pass) {
    input.new_frame();
    if (graph_view) {
      // Select the car with a click, then zoom about the graph's middle or
      // begin a wire from the blur, as the view asks.
      const bool press = pass == 1;
      const bool wire_view = view == "compositor-wire";
      float x = pass < 3 ? car_press_x : hover_x;
      float y = pass < 3 ? car_press_y : hover_y;
      if (wire_view && pass >= 3) {
        // Blur EFFECT OUT sits at the right edge of its first row; drag
        // from it to just short of the first Scene Layer's EFFECTS input.
        x = pass == 3 ? 20.0F + 166.0F : 226.0F;
        y = pass == 3 ? 26.0F + 70.0F + 56.0F : 26.0F + 30.0F + 58.0F;
      }
      input.set_mouse_position(x, y);
      input.set_mouse_button(gui::MouseButton::Left,
                             press || (wire_view && pass >= 3));
      if (pass == 4 && (view == "compositor-far" || view == "compositor-near")) {
        input.set_mouse_position(logical_width * 0.35F, logical_height * 0.45F);
        input.add_wheel(view == "compositor-far" ? -4.0F : 3.0F);
      }
      ctx.new_frame(1.0);
      ctx.panel(window, ctx.theme().background);
      static_cast<void>(compositor_panel.draw(ctx, window,
                                              compositor_sample.graph,
                                              compositor_sample.elements));
      ctx.draw_overlays(window);
      continue;
    }
    const bool clicking = clicks && pass < 2;
    // Feed selects its first viewport row the same way.
    input.set_mouse_position(
        clicking ? (feed ? 60.0F
                         : timeline_bounds.x + (curves ? 608.0F : 38.0F))
                 : hover_x,
        clicking ? (feed ? 101.0F
                         : timeline_bounds.y + (curves ? 17.0F : 112.0F))
                 : hover_y);
    input.set_mouse_button(gui::MouseButton::Left, clicks && pass == 0);
    ctx.new_frame(1.0);
    ctx.panel(window, ctx.theme().background);
    if (feed) {
      static_cast<void>(feed_panel.draw(ctx, window, wall_canvas, feed_map, 0U,
                                        &feed_hardware));
    } else if (!properties.empty()) {
      if (properties == "renderer" || properties == "environment")
        static_cast<void>(settings_panel.draw(ctx, window,
            properties == "renderer" ? editor::SceneSettingsTarget::Renderer : editor::SceneSettingsTarget::Environment,
            settings, {}, 0U));
      else
        static_cast<void>(inspector_panel.draw(ctx, window, &inspected));
    } else {
      static_cast<void>(scene_panel.draw(
          ctx, scene_bounds, show.elements, show.selection,
          show.selection.empty() ? doc::NodeId{} : show.selection.front(), true));
      static_cast<void>(timeline_panel.draw(
          ctx, timeline_bounds, timeline, show.keyframes, show.elements,
          show.selection, show.selection.front(), {}, {}, {}, show.lanes));
      static_cast<void>(output_panel.draw(ctx, output_bounds, outputs));
    }
    ctx.draw_overlays(window);
  }

  const auto width = static_cast<core::u32>(logical_width * scale);
  const auto height = static_cast<core::u32>(logical_height * scale);
  rhi::TextureDesc desc{};
  desc.width = width;
  desc.height = height;
  desc.format = rhi::TextureFormat::Rgba8Unorm;
  desc.color_target = true;
  auto target = device.create_texture(desc);
  auto readback = device.create_readback(width * height * 4U);
  auto acquired = device.acquire_commands();
  if (!target || !readback || !acquired) {
    std::puts("no target");
    return 1;
  }
  auto commands = std::move(acquired).value();
  if (!renderer.value().upload_atlas(device, commands, font->pixels(),
                                     font->width(), font->height()) ||
      !renderer.value().render(device, commands, target.value(),
                               rhi::Color{0.0F, 0.0F, 0.0F, 1.0F}, list,
                               width, height) ||
      !commands.download(target.value(), readback.value())) {
    std::puts("render failed");
    return 1;
  }
  auto submitted = device.submit(std::move(commands));
  if (!submitted || !std::move(submitted).value().wait()) {
    std::puts("submit failed");
    return 1;
  }
  const auto mapped = readback.value().map();
  std::vector<core::u8> pixels(mapped.begin(),
                               mapped.begin() + width * height * 4U);
  readback.value().unmap();
  if (!write_bmp(out_path, width, height, pixels)) {
    std::puts("write failed");
    return 1;
  }
  std::printf("wrote %s (%ux%u)\n", out_path.c_str(), width, height);
  return 0;
}
