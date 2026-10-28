#include <mediaserver/editor/asset_viewer_panel.hpp>
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

#include <algorithm>
#include <array>
#include <cmath>
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
//       [hover_x hover_y] [view: media|model|light|renderer|environment|curves|handles|summaries|timeline-tall|timeline-keys|timeline-tracks|feed]
//   feed draws Feed Mapping for one 15360x2160 Mosaic window carrying four
//   2880x1536 wall sections, two of them named, with the first selected;
//   curves draws the editor with the Timeline switched to Curves mode;
//   handles does too, with two Opacity keys selected - one Aligned, one
//   Broken with authored weights - so their tangent handles show;
//   summaries closes the LED Wall group so its row summarises its clips;
//   compositor draws an LED Car Process graph - plates into the car, a blur
//   feeding one plate, a composite into Output - with the car selected so
//   its Parameters show; compositor-far and compositor-near draw it zoomed
//   out and in; compositor-wire holds a wire being drawn, near a port;
//   compositor-refuse holds one over an input it cannot feed;
//   compositor-menu lets one go on empty canvas, opening the node search;
//   compositor-drop previews a Library asset dropped on a car input. The
//   car's wire to Composite is routed through a reroute point, and a
//   spare Scene Layer that reaches nothing is drawn faded;
//   compositor-organise draws the plates' comment box and the minimap,
//   rubber-bands three plates and opens their right-click menu;
//   compositor-rich adds a bypassed effect and a key after the blur
//   on the front-right plate, so its FILTERS wires are numbered, collapses the ceiling plate, marks an error
//   on the right plate, and previews a Luma Key drop on the blur's wire
//   at the hover point;
//   compositor-merge screens a plate over a gradient Background with one
//   Merge, selected, and darkens the result with a second whose LAYERS
//   are unwired; compositor-background selects the Background instead;
//   asset-viewer draws the Asset Viewer on a 24 fps plate with a marked
//   range and its filmstrip (the picture is the filmstrip atlas, standing
//   in for a render), and asset-viewer-empty draws it with nothing open.
namespace {

namespace assets = mediaserver::assets;
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
  content.sequence_count = 360U;
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
                              30.0F + static_cast<float>(index) * 124.0F,
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
  // Blur is a filter: it goes into the plate's FILTERS input.
  connect(blur, 0U, layers[1], 2U);
  for (std::size_t index = 0U; index < layers.size(); ++index) {
    connect(layers[index], 0U, car, plate_ports[index]);
  }
  connect(car, 0U, composite, 0U);
  sample.graph.edges.back().points = {{720.0F, 420.0F}};
  connect(composite, 0U, out, 0U);
  // A spare layer wired to nothing, which the graph reports as a dead branch.
  editor::ElementInfo spare = element(950U, editor::ElementKind::Media,
                                      "Plate_Spare_Wide.mov");
  sample.elements.push_back(spare);
  const doc::NodeId unused = add_node(
      compositor::NodeKind::SceneLayer, 20.0F, 470.0F,
      compositor::SceneLayerParameters{doc::NodeId{950U}});
  sample.graph.diagnostics.push_back(compositor::GraphDiagnostic{
      compositor::DiagnosticCode::DeadBranch, unused, {}, 0U,
      compositor::DiagnosticSeverity::Warning});
  sample.graph.comments.push_back(compositor::GraphComment{
      doc::NodeId{next_id++}, {}, "Plates into the car", 225.0F, -10.0F, 230.0F,
      540.0F, 1U});
  return sample;
}

// A plate screened over a gradient Background by one Merge, whose result a
// second Merge - nothing on its LAYERS - darkens on its way to Output. The
// first Merge, or the Background, sits where the car does, so the same press
// selects it.
[[nodiscard]] CompositorSample merge_compositor(bool select_background) {
  namespace compositor = mediaserver::compositor;
  CompositorSample sample;
  sample.elements.push_back(element(900U, editor::ElementKind::Media,
                                    "Plate_Front_A_Cam.mov"));
  const auto add_node = [&](core::u64 id, compositor::NodeKind kind, float x,
                            float y, compositor::NodeParameters parameters) {
    compositor::GraphNode node{};
    node.node = doc::NodeId{id};
    node.kind = kind;
    node.label = std::string{compositor::node_kind_name(kind)};
    node.editor_x = x;
    node.editor_y = y;
    node.parameters = std::move(parameters);
    sample.graph.nodes.push_back(node);
  };
  const auto connect = [&](core::u64 id, core::u64 from, core::u64 to,
                           core::u8 to_port) {
    sample.graph.edges.push_back(compositor::GraphEdge{
        doc::NodeId{id}, compositor::EdgeEndpoint{doc::NodeId{from}, 0U},
        compositor::EdgeEndpoint{doc::NodeId{to}, to_port},
        static_cast<core::i64>(sample.graph.edges.size())});
  };
  compositor::BackgroundParameters gradient{};
  gradient.mode = compositor::BackgroundMode::Linear;
  gradient.color_a = compositor::BackgroundColor{0.05F, 0.10F, 0.30F, 1.0F};
  gradient.color_b = compositor::BackgroundColor{0.90F, 0.45F, 0.20F, 1.0F};
  gradient.angle = 0.25F;
  add_node(100U, compositor::NodeKind::SceneLayer, 240.0F, 200.0F,
           compositor::SceneLayerParameters{doc::NodeId{900U}});
  add_node(101U, compositor::NodeKind::Background,
           select_background ? 500.0F : 240.0F,
           select_background ? 40.0F : 30.0F, gradient);
  add_node(102U, compositor::NodeKind::Merge,
           select_background ? 760.0F : 500.0F,
           select_background ? 300.0F : 40.0F,
           compositor::MergeParameters{compositor::MergeBlendMode::Screen,
                                       0.8F, 1.0F});
  add_node(103U, compositor::NodeKind::Merge, 760.0F, 120.0F,
           compositor::MergeParameters{compositor::MergeBlendMode::Multiply,
                                       1.0F, 0.75F});
  add_node(104U, compositor::NodeKind::Output, 1000.0F, 160.0F,
           compositor::OutputParameters{});
  connect(105U, 101U, 102U, 0U);
  connect(106U, 100U, 102U, 1U);
  connect(107U, 102U, 103U, 0U);
  connect(108U, 103U, 104U, 0U);
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

// A small project for the Library views: plates and an interview in
// folders, a still, and two models, with the metadata import records.
[[nodiscard]] assets::AssetRegistry sample_library() {
  assets::AssetRegistry registry;
  const auto media = [&](core::u64 id, std::string name, core::u32 width,
                         core::u32 height, core::u32 frames,
                         assets::MediaFrameRate rate, std::string codec,
                         std::string timecode, core::u64 bytes,
                         core::u64 folder) {
    assets::MediaAsset asset{};
    asset.id = doc::NodeId{id};
    asset.display_name = name;
    asset.source = "D:/Show/Media/" + name;
    asset.dimensions = assets::PixelDimensions{width, height};
    asset.frame_count = frames;
    if (rate.numerator != 0U) {
      asset.frame_rate = rate;
    }
    asset.codec = std::move(codec);
    asset.start_timecode = std::move(timecode);
    asset.fingerprint = assets::ContentFingerprint{bytes, {}};
    asset.folder = doc::NodeId{folder};
    registry.assets.emplace_back(std::move(asset));
  };
  media(20U, "Opening_Plate_A001.mov", 3840U, 2160U, 1440U, {24'000U, 1'001U},
        "prores", "14:22:05:11", 6'400'000'000U, 0U);
  media(21U, "Drive_Rear_Long_Lens.mov", 3840U, 2160U, 8640U,
        {24'000U, 1'001U}, "hevc", "09:41:00:00", 21'300'000'000U, 40U);
  media(22U, "Interview_CamB.mxf", 1920U, 1080U, 26'973U, {30'000U, 1'001U},
        "dnxhd", "01:00:00;00", 9'100'000'000U, 0U);
  media(23U, "Show_Logo.png", 2048U, 1024U, 1U, {}, "", "", 1'840'000U, 0U);
  assets::ModelAsset car{};
  car.id = doc::NodeId{30U};
  car.display_name = "Hero_Car.glb";
  car.source = "D:/Show/Models/Hero_Car.glb";
  car.vertex_count = 486'211U;
  car.triangle_count = 812'004U;
  car.material_count = 14U;
  car.texture_count = 22U;
  car.largest_texture = assets::PixelDimensions{8192U, 8192U};
  car.fingerprint = assets::ContentFingerprint{412'000'000U, {}};
  registry.assets.emplace_back(car);
  assets::ModelAsset set{};
  set.id = doc::NodeId{31U};
  set.display_name = "Diner_Set.gltf";
  set.source = "D:/Show/Models/Diner_Set.gltf";
  set.vertex_count = 1'204'400U;
  set.triangle_count = 1'960'118U;
  set.material_count = 61U;
  set.texture_count = 88U;
  set.largest_texture = assets::PixelDimensions{4096U, 4096U};
  set.fingerprint = assets::ContentFingerprint{1'210'000'000U, {}};
  set.folder = doc::NodeId{41U};
  registry.assets.emplace_back(set);
  registry.folders.push_back(
      assets::AssetFolder{doc::NodeId{40U}, "Plates", doc::NodeId{}});
  registry.folders.push_back(
      assets::AssetFolder{doc::NodeId{41U}, "Sets", doc::NodeId{}});
  std::sort(registry.assets.begin(), registry.assets.end(),
            [](const assets::Asset &left, const assets::Asset &right) {
              return assets::asset_id(left) < assets::asset_id(right);
            });
  return registry;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    std::puts("usage: mediaserver_editor_ui_snapshot <out.bmp> [scale] "
              "[width] [height] [hover_x hover_y] [view: media|model|light|renderer|environment|curves|handles|summaries|timeline-tall|timeline-keys|timeline-tracks|feed]");
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
  // Every clip row Large, by a click on the navigator's L, with a waveform
  // under Particle_Ribbons.mov.
  const bool tall = view == "timeline-tall";
  // The keyboard reference sheet, opened by its button at the list's foot.
  const bool keys_sheet = view == "timeline-keys";
  // The two clips on tracks of their own, one sync locked, and the logo at
  // Half: the track switches and the Full/Half switch.
  const bool tracks = view == "timeline-tracks";
  const bool graph_view = view.substr(0U, 10U) == "compositor";
  // The Library with a small sample project: `library` (List), then
  // `library-grid` and `library-drag` (an asset carried out past the panel).
  const bool library_view = view.substr(0U, 7U) == "library";
  const bool viewer_view = view.substr(0U, 12U) == "asset-viewer";
  const std::string_view properties =
      curves || summaries || feed || tall || keys_sheet || tracks ||
              graph_view ||
              library_view || viewer_view
          ? std::string_view{}
          : view;

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
  if (tracks) {
    for (editor::ElementInfo &held : show.elements) {
      if (held.node == doc::NodeId{31U}) {
        held.timeline_lane = doc::NodeId{50U};
      } else if (held.node == doc::NodeId{32U}) {
        held.timeline_lane = doc::NodeId{51U};
        held.media_resolution = scene::MediaResolution::Half;
      }
    }
    scene::TimelineLane plates{doc::NodeId{50U}, scene::SceneId{1U}, 10,
                               "Plates", false, false,
                               scene::BlendMode::Normal, false};
    scene::TimelineLane titles{doc::NodeId{51U}, scene::SceneId{1U}, 20,
                               "Titles", false, false,
                               scene::BlendMode::Normal, false};
    titles.sync_locked = true;
    show.lanes = {plates, titles};
  }
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
  CompositorSample compositor_sample =
      view == "compositor-merge" || view == "compositor-background"
          ? merge_compositor(view == "compositor-background")
          : sample_compositor();
  if (view == "compositor-rich") {
    namespace compositor = mediaserver::compositor;
    // Ids follow sample_compositor(): plates 100..103, Blur 104, the car 105;
    // 117 onwards are free, keeping both lists in id order.
    compositor::GraphNode brightness{};
    brightness.node = doc::NodeId{117U};
    brightness.kind = compositor::NodeKind::Brightness;
    brightness.editor_x = 20.0F;
    brightness.editor_y = 190.0F;
    brightness.parameters = compositor::BrightnessParameters{1.2F};
    brightness.bypassed = true;
    compositor_sample.graph.nodes.push_back(brightness);
    compositor_sample.graph.edges.push_back(compositor::GraphEdge{
        doc::NodeId{118U}, compositor::EdgeEndpoint{brightness.node, 0U},
        compositor::EdgeEndpoint{doc::NodeId{101U}, 0U}, 0});
    // A key after the blur on the same FILTERS input, so the two are
    // numbered.
    compositor::GraphNode key{};
    key.node = doc::NodeId{119U};
    key.kind = compositor::NodeKind::ChromaKey;
    key.editor_x = 20.0F;
    key.editor_y = 310.0F;
    key.parameters = compositor::ChromaKeyParameters{};
    compositor_sample.graph.nodes.push_back(key);
    compositor_sample.graph.edges.push_back(compositor::GraphEdge{
        doc::NodeId{120U}, compositor::EdgeEndpoint{key.node, 0U},
        compositor::EdgeEndpoint{doc::NodeId{101U}, 2U}, 1});
    compositor_sample.graph.nodes[3].collapsed = true;
    compositor_sample.graph.diagnostics.push_back(compositor::GraphDiagnostic{
        compositor::DiagnosticCode::DuplicateSceneLayerReference,
        doc::NodeId{102U}, {}, 0U, compositor::DiagnosticSeverity::Error});
  }
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
  const bool clicks = curves || summaries || feed || tall || keys_sheet;
  assets::WaveformEnvelope envelope{};
  for (std::size_t bin = 0U; bin < envelope.size(); ++bin) {
    const float phase = static_cast<float>(bin) * 0.021F;
    const float swell = 0.55F + 0.4F * std::sin(phase * 0.13F);
    envelope[bin].peak = std::clamp(swell * (0.7F + 0.3F * std::sin(phase * 7.0F)), 0.0F, 1.0F);
    envelope[bin].rms = envelope[bin].peak * 0.55F;
  }
  // An eight-thumbnail atlas, one hue per thumbnail with a frame number
  // bar, so the tiles a clip draws show which source frame each one is.
  std::optional<rhi::Texture> filmstrip_texture;
  std::vector<core::u8> filmstrip_pixels;
  std::vector<editor::TimelineFilmstripObservation> filmstrips;
  if (tall || viewer_view) {
    constexpr core::u32 tile_w = 160U;
    constexpr core::u32 tile_h = 90U;
    rhi::TextureDesc strip{};
    strip.width = tile_w * 8U;
    strip.height = tile_h;
    strip.format = rhi::TextureFormat::Rgba8Unorm;
    strip.sampled = true;
    if (auto made = device.create_texture(strip)) {
      filmstrip_texture = std::move(made).value();
      filmstrip_pixels.resize(std::size_t{strip.width} * strip.height * 4U);
      for (core::u32 y = 0U; y < tile_h; ++y) {
        for (core::u32 x = 0U; x < strip.width; ++x) {
          const core::u32 tile = x / tile_w;
          const float hue = static_cast<float>(tile) / 8.0F;
          // A bar along the foot, longer for each later thumbnail.
          const bool bar = y > tile_h - 18U && (x % tile_w) < 18U * (tile + 1U);
          const float shade = 0.35F + 0.5F * static_cast<float>(y) / tile_h;
          core::u8 *px = &filmstrip_pixels[(std::size_t{y} * strip.width + x) * 4U];
          px[0] = bar ? 240U : static_cast<core::u8>(255.0F * shade * (0.5F + 0.5F * std::sin(6.283F * hue)));
          px[1] = bar ? 240U : static_cast<core::u8>(255.0F * shade * (0.5F + 0.5F * std::sin(6.283F * hue + 2.094F)));
          px[2] = bar ? 240U : static_cast<core::u8>(255.0F * shade * (0.5F + 0.5F * std::sin(6.283F * hue + 4.189F)));
          px[3] = 255U;
        }
      }
      filmstrips.push_back(editor::TimelineFilmstripObservation{
          doc::NodeId{31U}, {}, assets::DerivativeState::Ready,
          &*filmstrip_texture});
    }
  }
  std::vector<editor::TimelineWaveformObservation> waveforms;
  if (tall) {
    waveforms.push_back(editor::TimelineWaveformObservation{
        doc::NodeId{31U}, {}, assets::DerivativeState::Ready, envelope});
  }
  const float tall_list_width =
      std::clamp(timeline_bounds.width * 0.26F, 250.0F, 340.0F);
  const int passes = clicks || graph_view || library_view ? 8 : 3;
  editor::AssetViewerPanel viewer_panel{gui::ui_id("snapshot.viewer")};
  assets::AssetRegistry viewer_assets{};
  {
    assets::MediaAsset plate{};
    plate.id = doc::NodeId{40U};
    plate.display_name = "Driving_Plate_Front.mov";
    plate.source = "D:/media/Driving_Plate_Front.mov";
    plate.dimensions = assets::PixelDimensions{3840U, 2160U};
    plate.frame_count = 1440U;
    plate.frame_rate = assets::MediaFrameRate{24U, 1U};
    plate.codec = "prores";
    plate.start_timecode = "01:00:00:00";
    viewer_assets.assets.push_back(assets::Asset{plate});
  }
  if (view == "asset-viewer") {
    viewer_panel.open(doc::NodeId{40U}, 312U, std::pair{240U, 455U});
  }
  for (int pass = 0; pass < passes; ++pass) {
    input.new_frame();
    if (graph_view) {
      // Select the car with a click, then zoom about the graph's middle or
      // begin a wire from the blur, as the view asks.
      const bool press = pass == 1;
      const bool wire_view = view == "compositor-wire" ||
                             view == "compositor-refuse" ||
                             view == "compositor-menu";
      float x = pass < 3 ? car_press_x : hover_x;
      float y = pass < 3 ? car_press_y : hover_y;
      bool down = press;
      if (wire_view && pass >= 3) {
        // Blur EFFECT OUT sits at the right edge of its first row; drag
        // from it to just short of the first Scene Layer's EFFECTS input,
        // over the car's FRONT image input, or out to empty canvas and let
        // go there.
        float target_x = 226.0F;
        float target_y = 26.0F + 30.0F + 58.0F;
        if (view == "compositor-refuse") {
          target_x = 520.0F;
          target_y = 26.0F + 40.0F + 56.0F;
        } else if (view == "compositor-menu") {
          target_x = 640.0F;
          target_y = 520.0F;
        }
        x = pass == 3 ? 20.0F + 166.0F : target_x;
        y = pass == 3 ? 26.0F + 70.0F + 56.0F : target_y;
        down = view != "compositor-menu" || pass < 6;
      }
      if (view == "compositor-organise") {
        compositor_panel.show_minimap(true);
        // A band from right to left touches three plates; then a right
        // click on the second opens the selection's menu.
        constexpr std::array<std::array<float, 3U>, 8U> steps{{
            {470.0F, 60.0F, 0.0F},  {470.0F, 60.0F, 1.0F},
            {300.0F, 330.0F, 1.0F}, {300.0F, 330.0F, 0.0F},
            {300.0F, 180.0F, 0.0F}, {300.0F, 180.0F, 0.0F},
            {330.0F, 245.0F, 0.0F}, {330.0F, 245.0F, 0.0F}}};
        x = steps[static_cast<std::size_t>(pass)][0];
        y = steps[static_cast<std::size_t>(pass)][1];
        down = steps[static_cast<std::size_t>(pass)][2] > 0.5F;
        input.set_mouse_button(gui::MouseButton::Right, pass == 4);
      }
      if (view == "compositor-drop" && pass >= 3) {
        // Over the car's BACK input, the fifth row.
        x = 492.0F;
        y = 26.0F + 40.0F + 44.0F + 25.0F * 4.0F + 12.0F;
      }
      input.set_mouse_position(x, y);
      input.set_mouse_button(gui::MouseButton::Left, down);
      if (pass == 4 && (view == "compositor-far" || view == "compositor-near")) {
        input.set_mouse_position(logical_width * 0.35F, logical_height * 0.45F);
        input.add_wheel(view == "compositor-far" ? -4.0F : 3.0F);
      }
      ctx.new_frame(1.0);
      ctx.panel(window, ctx.theme().background);
      static_cast<void>(compositor_panel.draw(ctx, window,
                                              compositor_sample.graph,
                                              compositor_sample.elements));
      if (view == "compositor-drop") {
        if (const auto target = compositor_panel.port_drop_target(
                compositor_sample.graph, x, y,
                mediaserver::compositor::PortValueType::Image)) {
          compositor_panel.draw_port_drop(ctx, *target,
                                          "Plate_Rear_Long_Lens.mov");
        }
      }
      if (view == "compositor-rich" && pass >= 3) {
        if (const auto drop = compositor_panel.effect_wire_at(
                compositor_sample.graph, x, y,
                mediaserver::compositor::PortValueType::Filter)) {
          compositor_panel.draw_wire_drop(ctx, *drop, "LUMA KEY");
        }
      }
      ctx.draw_overlays(window);
      continue;
    }
    if (viewer_view) {
      input.set_mouse_position(hover_x, hover_y);
      input.set_mouse_button(gui::MouseButton::Left, false);
      ctx.new_frame(1.0);
      ctx.panel(window, ctx.theme().background);
      editor::AssetViewerInput director{};
      director.shown = editor::AssetViewerRequest{doc::NodeId{40U}, 312U};
      director.rendition = editor::AssetViewerRendition::Source;
      if (filmstrip_texture.has_value()) {
        director.filmstrip = editor::AssetViewerFilmstrip{
            {}, assets::DerivativeState::Ready, &*filmstrip_texture};
      }
      std::vector<editor::ElementInfo> uses;
      editor::ElementInfo use{};
      use.node = doc::NodeId{31U};
      use.kind = editor::ElementKind::Media;
      use.display_name = "Driving_Plate_Front.mov";
      use.source = "D:/media/Driving_Plate_Front.mov";
      use.timing = scene::ClipTiming{120, 240, 0, 0, 0};
      use.timeline_lane = doc::NodeId{50U};
      uses.push_back(use);
      const std::array<scene::TimelineLane, 1U> lanes{
          scene::TimelineLane{doc::NodeId{50U}, scene::SceneId{1U}, 10,
                              "Plates", false, false,
                              scene::BlendMode::Normal, false}};
      editor::AssetViewerDrawInput in{};
      in.assets = &viewer_assets;
      in.director = &director;
      in.image = view == "asset-viewer" && filmstrip_texture.has_value()
                     ? &*filmstrip_texture
                     : nullptr;
      in.show_rate = scene::MediaFrameRate{25U, 1U};
      in.target_lane = doc::NodeId{50U};
      in.target_name = "Plates";
      in.elements = uses;
      in.lanes = lanes;
      in.pixels_per_unit = scale;
      static_cast<void>(viewer_panel.draw(ctx, window, in));
      ctx.draw_overlays(window);
      continue;
    }
    if (library_view) {
      static editor::LibraryPanel library_panel{gui::ui_id("snapshot.library")};
      static const assets::AssetRegistry library = sample_library();
      const gui::Rect library_bounds{4.0F, 4.0F,
                                     view == "library" ? logical_width - 8.0F
                                                       : 520.0F,
                                     logical_height - 8.0F};
      if (pass == 0) {
        // Two files changed on disk since import, as a RESCAN reports them.
        const std::array<doc::NodeId, 2U> changed{
            {doc::NodeId{20U}, doc::NodeId{22U}}};
        library_panel.set_changed_sources(changed);
        library_panel.set_view(view == "library-grid"
                                   ? editor::LibraryView::Grid
                                   : editor::LibraryView::List);
        if (view == "library-grid") {
          library_panel.set_icon_size(0.3F);
        }
      }
      float x = hover_x;
      float y = hover_y;
      bool down = false;
      if (view == "library-drag") {
        // Opening Plate's row, pressed and carried out to the right.
        constexpr std::array<std::array<float, 3U>, 3U> steps{{
            {120.0F, 380.0F, 0.0F}, {120.0F, 380.0F, 1.0F},
            {700.0F, 260.0F, 1.0F}}};
        const auto &step = steps[std::min<std::size_t>(
            static_cast<std::size_t>(pass), steps.size() - 1U)];
        x = step[0];
        y = step[1];
        down = step[2] > 0.5F;
      } else if (pass == 1 || pass == 2) {
        // Collapse the generators so the assets lead.
        x = 80.0F;
        y = 85.0F;
        down = pass == 1;
      }
      input.set_mouse_position(x, y);
      input.set_mouse_button(gui::MouseButton::Left, down);
      ctx.new_frame(1.0);
      ctx.panel(window, ctx.theme().background);
      static_cast<void>(library_panel.draw(ctx, library_bounds, library, {},
                                           {}));
      library_panel.draw_drag_ghost(ctx);
      ctx.draw_overlays(window);
      continue;
    }
    const bool clicking = clicks && pass < 2;
    // Feed selects its first viewport row the same way.
    input.set_mouse_position(
        clicking ? (feed ? 60.0F
                    : tall ? timeline_bounds.x + tall_list_width - 20.0F
                    : keys_sheet ? timeline_bounds.x + 16.0F
                         : timeline_bounds.x + (curves ? 608.0F : 38.0F))
                 : hover_x,
        clicking ? (feed ? 101.0F
                    : tall || keys_sheet
                        ? timeline_bounds.y + timeline_bounds.height - 9.0F
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
          show.selection, show.selection.front(), {}, {}, {}, show.lanes,
          std::nullopt, std::nullopt, filmstrips, waveforms));
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
  if (filmstrip_texture.has_value() &&
      !commands.upload(*filmstrip_texture, filmstrip_pixels)) {
    std::puts("filmstrip upload failed");
    return 1;
  }
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
