#include <mediaserver/editor/effects_panel.hpp>
#include <mediaserver/editor/inspector_panel.hpp>
#include <mediaserver/editor/workspace_panels.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <array>
#include <cmath>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace {

namespace compositor = mediaserver::compositor;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;
namespace scene = mediaserver::scene;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define SPATIAL_EDITOR_CHECK(...)                                              \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] bool near(double left, double right,
                        double tolerance = 1.0e-5) noexcept {
  return std::abs(left - right) <= tolerance;
}

struct EffectsHarness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::EffectsPanel panel{gui::ui_id("spatial.effects")};
  static constexpr gui::Rect bounds{0.0F, 0.0F, 260.0F, 760.0F};

  [[nodiscard]] std::optional<editor::EffectApplyRequest>
  frame(float x, float y, bool down, std::string_view text = {}) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    input.add_text(text);
    ctx.new_frame();
    return panel.draw(ctx, bounds, doc::NodeId{42U});
  }

  [[nodiscard]] std::optional<editor::EffectApplyRequest>
  search_and_drag(std::string_view query) {
    static_cast<void>(frame(100.0F, 37.0F, false));
    static_cast<void>(frame(100.0F, 37.0F, true));
    static_cast<void>(frame(100.0F, 37.0F, false));
    static_cast<void>(frame(100.0F, 37.0F, false, query));
    static_cast<void>(frame(100.0F, 90.0F, false));
    static_cast<void>(frame(100.0F, 90.0F, true));
    static_cast<void>(frame(320.0F, 400.0F, true));
    return frame(320.0F, 400.0F, false);
  }
};

void effects_palette_exposes_three_distinct_blurs() {
  EffectsHarness gaussian;
  const auto gaussian_result = gaussian.search_and_drag("gaussian");
  SPATIAL_EDITOR_CHECK(gaussian_result.has_value());
  SPATIAL_EDITOR_CHECK(gaussian_result->effect == scene::EffectKind::Blur);

  EffectsHarness box;
  const auto box_result = box.search_and_drag("exact integer-radius");
  SPATIAL_EDITOR_CHECK(box_result.has_value());
  SPATIAL_EDITOR_CHECK(box_result->effect == scene::EffectKind::BoxBlur);

  EffectsHarness directional;
  const auto directional_result = directional.search_and_drag("directional");
  SPATIAL_EDITOR_CHECK(directional_result.has_value());
  SPATIAL_EDITOR_CHECK(directional_result->effect ==
                       scene::EffectKind::DirectionalBlur);
  SPATIAL_EDITOR_CHECK(compositor::node_kind_name(compositor::NodeKind::Blur) ==
                       "Gaussian Blur");
}

void effects_palette_exposes_distinct_keyers() {
  EffectsHarness chroma;
  const auto chroma_result = chroma.search_and_drag("chroma");
  SPATIAL_EDITOR_CHECK(chroma_result.has_value());
  SPATIAL_EDITOR_CHECK(chroma_result->effect == scene::EffectKind::ChromaKey);

  EffectsHarness luma;
  const auto luma_result = luma.search_and_drag("luma");
  SPATIAL_EDITOR_CHECK(luma_result.has_value());
  SPATIAL_EDITOR_CHECK(luma_result->effect == scene::EffectKind::LumaKey);
}

struct InspectorHarness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::InspectorPanel panel{gui::ui_id("spatial.inspector")};
  editor::ElementInfo element{};
  std::vector<scene::EffectInstance> effects;
  std::vector<scene::ChannelKeyframes> keyframes;
  static constexpr gui::Rect bounds{0.0F, 0.0F, 300.0F, 840.0F};

  InspectorHarness() {
    element.node = doc::NodeId{5U};
    element.kind = editor::ElementKind::Object;
  }

  [[nodiscard]] editor::InspectorFrame frame(float x, float y, bool down,
                                             std::string_view text = {},
                                             bool enter = false) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    input.add_text(text);
    if (enter) {
      input.add_key(gui::Key::Enter);
    }
    ctx.new_frame();
    return panel.draw(ctx, bounds, &element, 0U, keyframes, 0U, effects);
  }

  [[nodiscard]] editor::InspectorFrame commit(float y, std::string_view value) {
    constexpr float x = 170.0F;
    static_cast<void>(frame(x, y, false));
    static_cast<void>(frame(x, y, true));
    static_cast<void>(frame(x, y, false));
    static_cast<void>(frame(x, y, false, value));
    return frame(x, y, false, {}, true);
  }
};

void inspector_authors_exact_box_blur_properties() {
  scene::EffectInstance effect{};
  effect.source = doc::NodeId{80U};
  effect.target = doc::NodeId{5U};
  effect.kind = scene::EffectKind::BoxBlur;
  effect.box_blur = scene::BoxBlurParameters{7U, 2U};

  InspectorHarness radius;
  radius.effects.push_back(effect);
  const editor::InspectorFrame radius_edit = radius.commit(493.0F, "999");
  SPATIAL_EDITOR_CHECK(radius_edit.component_edits.size() == 1U);
  SPATIAL_EDITOR_CHECK(radius_edit.component_edits.front().node ==
                       effect.source);
  SPATIAL_EDITOR_CHECK(radius_edit.component_edits.front().component ==
                       scene::PropertyChannel::BoxBlurRadius);
  SPATIAL_EDITOR_CHECK(radius_edit.component_edits.front().value ==
                       scene::max_box_blur_radius_px);
  SPATIAL_EDITOR_CHECK(radius_edit.named_edits.empty());

  InspectorHarness passes;
  passes.effects.push_back(effect);
  const editor::InspectorFrame passes_edit = passes.commit(519.0F, "0");
  SPATIAL_EDITOR_CHECK(passes_edit.named_edits.size() == 1U);
  SPATIAL_EDITOR_CHECK(passes_edit.named_edits.front().property ==
                       scene::schema::box_blur_passes);
  SPATIAL_EDITOR_CHECK(passes_edit.named_edits.front().value == 1.0);
}

void inspector_authors_canonical_directional_blur_properties() {
  scene::EffectInstance effect{};
  effect.source = doc::NodeId{81U};
  effect.target = doc::NodeId{5U};
  effect.kind = scene::EffectKind::DirectionalBlur;
  effect.directional_blur = scene::DirectionalBlurParameters{12.0F, 0.125F};

  InspectorHarness length;
  length.effects.push_back(effect);
  const editor::InspectorFrame length_edit = length.commit(493.0F, "999");
  SPATIAL_EDITOR_CHECK(length_edit.component_edits.size() == 1U);
  SPATIAL_EDITOR_CHECK(length_edit.component_edits.front().component ==
                       scene::PropertyChannel::DirectionalBlurLength);
  SPATIAL_EDITOR_CHECK(length_edit.component_edits.front().value ==
                       scene::max_directional_blur_length_px);

  InspectorHarness angle;
  angle.effects.push_back(effect);
  const editor::InspectorFrame angle_edit = angle.commit(519.0F, "1.25");
  SPATIAL_EDITOR_CHECK(angle_edit.named_edits.size() == 1U);
  SPATIAL_EDITOR_CHECK(angle_edit.named_edits.front().property ==
                       scene::schema::directional_blur_angle_turns);
  SPATIAL_EDITOR_CHECK(near(angle_edit.named_edits.front().value, 0.25));
}

void inspector_keys_primary_spatial_parameters() {
  scene::EffectInstance box{};
  box.source = doc::NodeId{80U};
  box.target = doc::NodeId{5U};
  box.kind = scene::EffectKind::BoxBlur;
  box.box_blur = scene::BoxBlurParameters{7U, 2U};
  InspectorHarness box_harness;
  box_harness.effects.push_back(box);
  static_cast<void>(box_harness.frame(90.0F, 493.0F, false));
  static_cast<void>(box_harness.frame(90.0F, 493.0F, true));
  const editor::InspectorFrame box_key =
      box_harness.frame(90.0F, 493.0F, false);
  SPATIAL_EDITOR_CHECK(box_key.key_edits.size() == 1U);
  SPATIAL_EDITOR_CHECK(box_key.key_edits.front().node == box.source);
  SPATIAL_EDITOR_CHECK(box_key.key_edits.front().component ==
                       scene::PropertyChannel::BoxBlurRadius);
  SPATIAL_EDITOR_CHECK(box_key.key_edits.front().value == 7.0);

  scene::EffectInstance directional{};
  directional.source = doc::NodeId{81U};
  directional.target = doc::NodeId{5U};
  directional.kind = scene::EffectKind::DirectionalBlur;
  directional.directional_blur =
      scene::DirectionalBlurParameters{12.0F, 0.125F};
  InspectorHarness directional_harness;
  directional_harness.effects.push_back(directional);
  static_cast<void>(directional_harness.frame(90.0F, 493.0F, false));
  static_cast<void>(directional_harness.frame(90.0F, 493.0F, true));
  const editor::InspectorFrame directional_key =
      directional_harness.frame(90.0F, 493.0F, false);
  SPATIAL_EDITOR_CHECK(directional_key.key_edits.size() == 1U);
  SPATIAL_EDITOR_CHECK(directional_key.key_edits.front().node ==
                       directional.source);
  SPATIAL_EDITOR_CHECK(directional_key.key_edits.front().component ==
                       scene::PropertyChannel::DirectionalBlurLength);
  SPATIAL_EDITOR_CHECK(directional_key.key_edits.front().value == 12.0);
}

void inspector_authors_and_keys_primary_keyer_mix() {
  scene::EffectInstance chroma{};
  chroma.source = doc::NodeId{82U};
  chroma.target = doc::NodeId{5U};
  chroma.kind = scene::EffectKind::ChromaKey;
  chroma.chroma_key.mix = 0.4F;

  InspectorHarness chroma_edit_harness;
  chroma_edit_harness.effects.push_back(chroma);
  const editor::InspectorFrame chroma_edit =
      chroma_edit_harness.commit(645.0F, "2");
  SPATIAL_EDITOR_CHECK(chroma_edit.component_edits.size() == 1U);
  SPATIAL_EDITOR_CHECK(chroma_edit.component_edits.front().node ==
                       chroma.source);
  SPATIAL_EDITOR_CHECK(chroma_edit.component_edits.front().component ==
                       scene::PropertyChannel::ChromaKeyMix);
  SPATIAL_EDITOR_CHECK(chroma_edit.component_edits.front().value == 1.0);

  scene::EffectInstance luma{};
  luma.source = doc::NodeId{83U};
  luma.target = doc::NodeId{5U};
  luma.kind = scene::EffectKind::LumaKey;
  luma.luma_key.mix = 0.65F;

  InspectorHarness luma_key_harness;
  luma_key_harness.effects.push_back(luma);
  static_cast<void>(luma_key_harness.frame(90.0F, 623.0F, false));
  static_cast<void>(luma_key_harness.frame(90.0F, 623.0F, true));
  const editor::InspectorFrame luma_key =
      luma_key_harness.frame(90.0F, 623.0F, false);
  SPATIAL_EDITOR_CHECK(luma_key.key_edits.size() == 1U);
  SPATIAL_EDITOR_CHECK(luma_key.key_edits.front().node == luma.source);
  SPATIAL_EDITOR_CHECK(luma_key.key_edits.front().component ==
                       scene::PropertyChannel::LumaKeyMix);
  SPATIAL_EDITOR_CHECK(near(luma_key.key_edits.front().value, 0.65));
}

[[nodiscard]] compositor::GraphNode graph_node(compositor::NodeKind kind) {
  compositor::GraphNode node{};
  node.node = doc::NodeId{70U};
  node.kind = kind;
  node.label = std::string{compositor::node_kind_name(kind)};
  node.editor_x = 60.0F;
  node.editor_y = 40.0F;
  if (kind == compositor::NodeKind::BoxBlur) {
    node.parameters = compositor::BoxBlurParameters{4U, 2U};
  } else {
    node.parameters = compositor::DirectionalBlurParameters{16.0F, 0.125F};
  }
  return node;
}

struct CompositorHarness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::CompositorPanel panel{gui::ui_id("spatial.compositor")};

  [[nodiscard]] editor::CompositorFrame
  frame(const compositor::CompositorGraph &graph, const gui::Rect &bounds,
        float x, float y, bool down, std::string_view text = {},
        bool enter = false) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    input.add_text(text);
    if (enter) {
      input.add_key(gui::Key::Enter);
    }
    ctx.new_frame();
    return panel.draw(ctx, bounds, graph);
  }

  void select(const compositor::CompositorGraph &graph) {
    constexpr gui::Rect bounds{0.0F, 0.0F, 900.0F, 640.0F};
    static_cast<void>(frame(graph, bounds, 100.0F, 75.0F, false));
    static_cast<void>(frame(graph, bounds, 100.0F, 75.0F, true));
    static_cast<void>(frame(graph, bounds, 100.0F, 75.0F, false));
  }

  [[nodiscard]] editor::CompositorFrame
  commit(const compositor::CompositorGraph &graph, float y,
         std::string_view value) {
    constexpr gui::Rect bounds{0.0F, 0.0F, 900.0F, 640.0F};
    constexpr float x = 820.0F;
    static_cast<void>(frame(graph, bounds, x, y, false));
    static_cast<void>(frame(graph, bounds, x, y, true));
    static_cast<void>(frame(graph, bounds, x, y, false));
    static_cast<void>(frame(graph, bounds, x, y, false, value));
    return frame(graph, bounds, x, y, false, {}, true);
  }
};

void compositor_palette_exposes_box_and_directional_nodes() {
  constexpr gui::Rect bounds{0.0F, 0.0F, 1100.0F, 640.0F};
  const compositor::CompositorGraph graph{};
  for (const auto [query, kind] :
       std::array<std::pair<std::string_view, compositor::NodeKind>, 2U>{
           {{"box smooth", compositor::NodeKind::BoxBlur},
            {"motion angle", compositor::NodeKind::DirectionalBlur}}}) {
    CompositorHarness harness;
    static_cast<void>(harness.frame(graph, bounds, 120.0F, 13.0F, false));
    static_cast<void>(harness.frame(graph, bounds, 120.0F, 13.0F, true));
    static_cast<void>(harness.frame(graph, bounds, 120.0F, 13.0F, false));
    static_cast<void>(harness.frame(graph, bounds, 80.0F, 42.0F, true));
    static_cast<void>(harness.frame(graph, bounds, 80.0F, 42.0F, false));
    static_cast<void>(
        harness.frame(graph, bounds, 80.0F, 42.0F, false, query));
    static_cast<void>(harness.frame(graph, bounds, 120.0F, 70.0F, false));
    static_cast<void>(harness.frame(graph, bounds, 120.0F, 70.0F, true));
    const editor::CompositorFrame result =
        harness.frame(graph, bounds, 120.0F, 70.0F, false);
    SPATIAL_EDITOR_CHECK(result.add_node.has_value());
    SPATIAL_EDITOR_CHECK(result.add_node->kind == kind);
  }
}

void compositor_property_rows_preserve_integer_and_turn_types() {
  compositor::CompositorGraph box_graph{};
  box_graph.nodes.push_back(graph_node(compositor::NodeKind::BoxBlur));
  CompositorHarness box;
  box.select(box_graph);
  editor::CompositorFrame edit = box.commit(box_graph, 69.0F, "999");
  SPATIAL_EDITOR_CHECK(edit.property_edit.has_value());
  SPATIAL_EDITOR_CHECK(edit.property_edit->property ==
                       editor::CompositorProperty::BoxBlurRadius);
  SPATIAL_EDITOR_CHECK(std::get<core::u32>(edit.property_edit->value) ==
                       scene::max_box_blur_radius_px);
  edit = box.commit(box_graph, 95.0F, "0");
  SPATIAL_EDITOR_CHECK(edit.property_edit.has_value());
  SPATIAL_EDITOR_CHECK(edit.property_edit->property ==
                       editor::CompositorProperty::BoxBlurPasses);
  SPATIAL_EDITOR_CHECK(std::get<core::u32>(edit.property_edit->value) == 1U);

  compositor::CompositorGraph directional_graph{};
  directional_graph.nodes.push_back(
      graph_node(compositor::NodeKind::DirectionalBlur));
  CompositorHarness directional;
  directional.select(directional_graph);
  edit = directional.commit(directional_graph, 69.0F, "999");
  SPATIAL_EDITOR_CHECK(edit.property_edit.has_value());
  SPATIAL_EDITOR_CHECK(edit.property_edit->property ==
                       editor::CompositorProperty::DirectionalBlurLength);
  SPATIAL_EDITOR_CHECK(std::get<float>(edit.property_edit->value) ==
                       scene::max_directional_blur_length_px);
  edit = directional.commit(directional_graph, 95.0F, "1.25");
  SPATIAL_EDITOR_CHECK(edit.property_edit.has_value());
  SPATIAL_EDITOR_CHECK(edit.property_edit->property ==
                       editor::CompositorProperty::DirectionalBlurAngleTurns);
  SPATIAL_EDITOR_CHECK(near(std::get<float>(edit.property_edit->value), 0.25));
}

void project_settings_force_directional_taps_odd() {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::ProjectSettingsPanel panel{gui::ui_id("spatial.settings")};
  editor::ProjectSettingsState state{};
  constexpr gui::Rect bounds{0.0F, 0.0F, 1600.0F, 900.0F};
  constexpr float card_x = 490.0F;
  constexpr float card_y = 218.0F;
  const auto frame = [&](float x, float y, bool down) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    ctx.new_frame();
    return panel.draw(ctx, bounds, state);
  };
  const auto click = [&](const gui::Rect &target) {
    const float x = target.x + target.width * 0.5F;
    const float y = target.y + target.height * 0.5F;
    static_cast<void>(frame(x, y, false));
    static_cast<void>(frame(x, y, true));
    return frame(x, y, false);
  };

  panel.open(state.settings);
  // The midpoint of the 3..65 slider is the even value 34. The panel must
  // immediately canonicalize it to the next supported odd tap count.
  const gui::Rect directional_track{card_x + 14.0F + 150.0F,
                                    card_y + 34.0F + 34.0F + 22.0F +
                                        44.0F * 3.0F + 4.0F + 22.0F,
                                    368.0F, 22.0F};
  static_cast<void>(click(directional_track));
  const gui::Rect apply{card_x + 620.0F - 98.0F, card_y + 464.0F - 38.0F, 84.0F,
                        26.0F};
  const editor::ProjectSettingsFrame result = click(apply);
  SPATIAL_EDITOR_CHECK(result.apply.has_value());
  SPATIAL_EDITOR_CHECK(result.apply->directional_blur_taps == 35U);
  SPATIAL_EDITOR_CHECK((result.apply->directional_blur_taps & 1U) == 1U);
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 9U> cases{{
    {"Effects palette exposes three distinct blurs",
     &effects_palette_exposes_three_distinct_blurs},
    {"Effects palette exposes distinct keyers",
     &effects_palette_exposes_distinct_keyers},
    {"Inspector authors exact Box Blur properties",
     &inspector_authors_exact_box_blur_properties},
    {"Inspector authors canonical Directional Blur properties",
     &inspector_authors_canonical_directional_blur_properties},
    {"Inspector keys primary spatial parameters",
     &inspector_keys_primary_spatial_parameters},
    {"Inspector authors and keys primary keyer mix",
     &inspector_authors_and_keys_primary_keyer_mix},
    {"Compositor palette exposes Box and Directional nodes",
     &compositor_palette_exposes_box_and_directional_nodes},
    {"Compositor property rows preserve integer and turn types",
     &compositor_property_rows_preserve_integer_and_turn_types},
    {"Project Settings force Directional taps odd",
     &project_settings_force_directional_taps_odd},
}};

} // namespace

int main() {
  std::size_t failures{};
  for (const Case &test : cases) {
    try {
      test.function();
      std::cout << "[pass] " << test.name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[fail] " << test.name << ": " << error.what() << '\n';
    }
  }
  std::cout << cases.size() - failures << '/' << cases.size()
            << " tests passed\n";
  return failures == 0U ? 0 : 1;
}
