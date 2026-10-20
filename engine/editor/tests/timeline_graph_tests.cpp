#include "../src/timeline/timeline_graph_view.hpp"
#include "../src/timeline/timeline_internal.hpp"

#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

// The Timeline's curve view on its own: channel colours, the value axis, and
// curves sampled from the scene's evaluator.
namespace {

namespace gui = mediaserver::gui;
namespace scene = mediaserver::scene;
namespace timeline = mediaserver::editor::timeline;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define GRAPH_CHECK(...)                                                       \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] bool same(gui::UiColor a, gui::UiColor b) noexcept {
  return a.r == b.r && a.g == b.g && a.b == b.b;
}

[[nodiscard]] bool reddest(gui::UiColor c) noexcept {
  return c.r > c.g && c.r > c.b;
}
[[nodiscard]] bool greenest(gui::UiColor c) noexcept {
  return c.g > c.r && c.g > c.b;
}
[[nodiscard]] bool bluest(gui::UiColor c) noexcept {
  return c.b > c.r && c.b > c.g;
}

void axis_channels_read_red_green_blue() {
  using scene::PropertyChannel;
  for (const auto [x, y, z] :
       std::array<std::array<PropertyChannel, 3>, 4>{{
           {PropertyChannel::PositionX, PropertyChannel::PositionY,
            PropertyChannel::PositionZ},
           {PropertyChannel::ScaleX, PropertyChannel::ScaleY,
            PropertyChannel::ScaleZ},
           {PropertyChannel::RotationPitch, PropertyChannel::RotationYaw,
            PropertyChannel::RotationRoll},
           {PropertyChannel::LightColorR, PropertyChannel::LightColorG,
            PropertyChannel::LightColorB}}}) {
    GRAPH_CHECK(reddest(timeline::channel_color(x)));
    GRAPH_CHECK(greenest(timeline::channel_color(y)));
    GRAPH_CHECK(bluest(timeline::channel_color(z)));
  }
}

void other_channels_listed_together_never_share_a_colour() {
  using scene::PropertyChannel;
  const auto is_axis = [](PropertyChannel channel) {
    const auto index = static_cast<int>(channel);
    return index <= static_cast<int>(PropertyChannel::ScaleZ) ||
           (channel >= PropertyChannel::LightColorR &&
            channel <= PropertyChannel::LightColorB);
  };
  const gui::UiColor red = timeline::channel_color(PropertyChannel::PositionX);
  const gui::UiColor green = timeline::channel_color(PropertyChannel::PositionY);
  const gui::UiColor blue = timeline::channel_color(PropertyChannel::PositionZ);
  // An element's own channels: Opacity and its effects; the environment's;
  // a camera's lens; a light's intensity and range.
  const std::array<std::array<PropertyChannel, 2>, 4> groups{{
      {PropertyChannel::Opacity, PropertyChannel::LumaKeyMix},
      {PropertyChannel::SkyTimeOfDay, PropertyChannel::SkyIlluminance},
      {PropertyChannel::CameraFieldOfView, PropertyChannel::CameraAperture},
      {PropertyChannel::LightIntensity, PropertyChannel::LightRange}}};
  for (const auto [begin, end] : groups) {
    for (auto a = static_cast<int>(begin); a <= static_cast<int>(end); ++a) {
      const auto first = static_cast<PropertyChannel>(a);
      if (is_axis(first)) {
        continue;
      }
      const gui::UiColor colour = timeline::channel_color(first);
      GRAPH_CHECK(!same(colour, red) && !same(colour, green) &&
                  !same(colour, blue));
      for (auto b = a + 1; b <= static_cast<int>(end); ++b) {
        const auto second = static_cast<PropertyChannel>(b);
        if (!is_axis(second)) {
          GRAPH_CHECK(!same(colour, timeline::channel_color(second)));
        }
      }
    }
  }
}

using timeline::GraphChannel;
using timeline::TimelineGraphView;
using timeline::ValueAxis;

[[nodiscard]] bool near(double a, double b, double tolerance) noexcept {
  return std::fabs(a - b) <= tolerance;
}

void the_value_axis_maps_values_and_pixels_both_ways() {
  ValueAxis axis;
  axis.show(-2.0, 6.0);
  const gui::Rect area{0.0F, 100.0F, 500.0F, 400.0F};
  // Top of the area is the top value, bottom the bottom one, linear between.
  GRAPH_CHECK(axis.y(6.0, area) == 100.0F);
  GRAPH_CHECK(axis.y(-2.0, area) == 500.0F);
  GRAPH_CHECK(axis.y(2.0, area) == 300.0F);
  GRAPH_CHECK(axis.value(300.0F, area) == 2.0);
  for (const double value : {-1.75, 0.3, 5.5, 9.0}) {
    GRAPH_CHECK(near(axis.value(axis.y(value, area), area), value, 1.0e-5));
  }
  // An empty or inverted range is refused rather than dividing by zero.
  axis.show(3.0, 3.0);
  GRAPH_CHECK(axis.bottom() == -2.0 && axis.top() == 6.0);
}

void fitting_pads_a_tenth_and_keeps_a_flat_channel_visible() {
  ValueAxis axis;
  axis.fit(1.0, 3.0);
  GRAPH_CHECK(near(axis.bottom(), 0.8, 1.0e-12) && near(axis.top(), 3.2, 1.0e-12));
  axis.fit(5.0, 5.0);
  GRAPH_CHECK(axis.bottom() < 5.0 && axis.top() > 5.0);
  GRAPH_CHECK(near((axis.bottom() + axis.top()) * 0.5, 5.0, 1.0e-12));
}

void zooming_keeps_the_anchor_and_panning_moves_by_pixels() {
  ValueAxis axis;
  axis.show(0.0, 10.0);
  const gui::Rect area{0.0F, 0.0F, 300.0F, 400.0F};
  const float anchor_y = axis.y(4.0, area);
  axis.zoom_about(4.0, 2.0);
  GRAPH_CHECK(near(axis.bottom(), 2.0, 1.0e-12) && near(axis.top(), 7.0, 1.0e-12));
  GRAPH_CHECK(axis.y(4.0, area) == anchor_y);

  axis.show(0.0, 8.0);
  // 50 of 400 pixels is an eighth of the 8-unit span: one unit, upwards.
  axis.pan(50.0F, area);
  GRAPH_CHECK(near(axis.bottom(), 1.0, 1.0e-12) && near(axis.top(), 9.0, 1.0e-12));
}

void ticks_are_round_steps_at_least_the_spacing_apart() {
  ValueAxis axis;
  axis.show(-2.0, 6.0);
  const gui::Rect area{0.0F, 0.0F, 300.0F, 400.0F};
  // 50 pixels per unit: a 40-pixel minimum takes whole units.
  GRAPH_CHECK(axis.tick_step(area, 40.0F) == 1.0);
  const std::vector<double> ticks = axis.ticks(area, 40.0F);
  GRAPH_CHECK(ticks.size() == 9U && ticks.front() == -2.0 && ticks.back() == 6.0);
  // 120 pixels apart needs more than two units: the next round step is 5.
  GRAPH_CHECK(axis.tick_step(area, 120.0F) == 5.0);
  axis.show(0.0, 0.08);
  GRAPH_CHECK(near(axis.tick_step(area, 40.0F), 0.01, 1.0e-15));
}

// Where a curve crosses a frame, read off its points.
[[nodiscard]] std::optional<float>
curve_y_at(const std::vector<gui::UiPoint> &points, float x) {
  for (const gui::UiPoint &point : points) {
    if (std::fabs(point.x - x) < 1.0e-3F) {
      return point.y;
    }
  }
  return std::nullopt;
}

void curves_follow_the_evaluator_not_an_easing_guess() {
  mediaserver::editor::TimelineTimeAxis time{};
  time.left_frame = 0.0;
  time.pixels_per_frame = 4.0F;
  ValueAxis axis;
  axis.show(0.05, 0.25);
  const gui::Rect area{100.0F, 0.0F, 200.0F, 300.0F};

  // Weighted, broken Cubic tangents: 0.2025 at frame 13, the value the
  // Timeline's camera key test pins.
  scene::ChannelKeyframes weighted{{}, scene::PropertyChannel::CameraFieldOfView,
      {0, 32}, {0.1, 0.2},
      {scene::KeyInterpolation::Cubic, scene::KeyInterpolation::Cubic}};
  weighted.tangents = {
      scene::KeyTangent{scene::KeyTangentMode::Broken, 0.0, 0.02, {}, 4.0},
      scene::KeyTangent{scene::KeyTangentMode::Broken, -0.005, 0.0, 12.0, {}}};
  const auto weighted_points =
      TimelineGraphView::sample_curve(weighted, time, axis, area);
  const auto at_13 = curve_y_at(weighted_points, time.x(13.0, area.x));
  GRAPH_CHECK(at_13.has_value() && std::fabs(*at_13 - axis.y(0.2025, area)) < 1.0e-3F);

  // Auto Cubic between two keys: 0.125 at frame 5.
  const scene::ChannelKeyframes automatic{{}, scene::PropertyChannel::CameraFieldOfView,
      {0, 20}, {0.1, 0.2},
      {scene::KeyInterpolation::Cubic, scene::KeyInterpolation::Cubic}};
  const auto auto_points =
      TimelineGraphView::sample_curve(automatic, time, axis, area);
  const auto at_5 = curve_y_at(auto_points, time.x(5.0, area.x));
  GRAPH_CHECK(at_5.has_value() && std::fabs(*at_5 - axis.y(0.125, area)) < 1.0e-3F);

  // Hold keeps its value up to the next key, then steps.
  const scene::ChannelKeyframes held{{}, scene::PropertyChannel::Opacity,
      {0, 10}, {0.1, 0.2},
      {scene::KeyInterpolation::Hold, scene::KeyInterpolation::Hold}};
  const auto held_points = TimelineGraphView::sample_curve(held, time, axis, area);
  GRAPH_CHECK(curve_y_at(held_points, time.x(9.0, area.x)) == axis.y(0.1, area));
  GRAPH_CHECK(curve_y_at(held_points, time.x(10.0, area.x)) == axis.y(0.2, area));
  // One point per frame on screen, plus one beyond each edge.
  GRAPH_CHECK(held_points.size() == 51U);
}

void zoomed_out_curves_take_one_sample_per_column() {
  mediaserver::editor::TimelineTimeAxis time{};
  time.left_frame = 0.0;
  time.pixels_per_frame = 0.25F;
  ValueAxis axis;
  axis.show(0.0, 1.0);
  const gui::Rect area{0.0F, 0.0F, 200.0F, 100.0F};
  const scene::ChannelKeyframes ramp{{}, scene::PropertyChannel::Opacity,
      {0, 800}, {0.0, 1.0},
      {scene::KeyInterpolation::Linear, scene::KeyInterpolation::Linear}};
  const auto points = TimelineGraphView::sample_curve(ramp, time, axis, area);
  // 200 columns and one either side, each a distinct frame four apart.
  GRAPH_CHECK(points.size() == 203U);
  for (std::size_t index = 1U; index < points.size(); ++index) {
    GRAPH_CHECK(points[index].x > points[index - 1U].x);
  }
  // Column 100 is frame 400, half way up the linear ramp.
  const auto middle = curve_y_at(points, 100.0F);
  GRAPH_CHECK(middle.has_value() && std::fabs(*middle - 50.0F) < 1.0e-3F);
}

void fitting_channels_includes_curve_overshoot() {
  // A steep, unweighted out tangent and a flat in tangent carry the curve
  // well above its last key; keys alone would fit [0, 1] to [-0.1, 1.1].
  scene::ChannelKeyframes overshoot{{}, scene::PropertyChannel::Opacity,
      {0, 10}, {0.0, 1.0},
      {scene::KeyInterpolation::Cubic, scene::KeyInterpolation::Cubic}};
  overshoot.tangents = {
      scene::KeyTangent{scene::KeyTangentMode::Broken, 0.0, 1.0, {}, {}},
      scene::KeyTangent{scene::KeyTangentMode::Broken, 0.0, 0.0, {}, {}}};
  TimelineGraphView graph;
  const std::array<GraphChannel, 1U> channels{
      {GraphChannel{{}, &overshoot, gui::UiColor{}, false}}};
  GRAPH_CHECK(graph.fit_requested());
  graph.fit(channels, 0, 100);
  GRAPH_CHECK(!graph.fit_requested());
  GRAPH_CHECK(graph.axis().top() > 1.5);
  GRAPH_CHECK(graph.axis().bottom() < 0.0);
  GRAPH_CHECK(graph.zoom() == 1.0F && graph.pan() == 0.0F);
}

void zoom_and_pan_are_reported_against_the_last_fit() {
  TimelineGraphView graph;
  graph.fit_values(0.0, 10.0);
  GRAPH_CHECK(graph.zoom() == 1.0F && graph.pan() == 0.0F);
  const ValueAxis fitted = graph.axis();
  graph.axis().zoom_about(5.0, 2.0);
  GRAPH_CHECK(near(graph.zoom(), 2.0, 1.0e-6) && near(graph.pan(), 0.0, 1.0e-6));
  const gui::Rect area{0.0F, 0.0F, 100.0F, 100.0F};
  // A tenth of the area at half the fitted span: a twentieth of a fit.
  graph.axis().pan(10.0F, area);
  GRAPH_CHECK(near(graph.pan(), 0.05, 1.0e-6));
  graph.fit_values(0.0, 10.0);
  GRAPH_CHECK(graph.axis() == fitted);
}

void channel_visibility_and_the_active_channel_reset_together() {
  TimelineGraphView graph;
  const timeline::ChannelId x{mediaserver::doc::NodeId{5U}, scene::PropertyChannel::PositionX};
  const timeline::ChannelId y{mediaserver::doc::NodeId{5U}, scene::PropertyChannel::PositionY};
  GRAPH_CHECK(graph.visible(x) && graph.visible(y));
  graph.set_visible(x, false);
  GRAPH_CHECK(!graph.visible(x) && graph.visible(y));
  graph.set_active(y);
  GRAPH_CHECK(graph.active() == y);
  static_cast<void>(graph.fit_requested());
  graph.fit_values(0.0, 1.0);
  graph.reset();
  GRAPH_CHECK(graph.visible(x) && !graph.active().has_value());
  GRAPH_CHECK(graph.fit_requested());
}

// Simplifying a sampled curve keeps every sample within the tolerance of
// what is drawn, and collapses straight runs to their ends.
void simplified_curves_stay_within_a_quarter_pixel() {
  mediaserver::editor::TimelineTimeAxis time{};
  time.pixels_per_frame = 0.5F;
  ValueAxis axis;
  axis.show(-0.2, 1.4);
  const gui::Rect area{0.0F, 0.0F, 600.0F, 300.0F};
  scene::ChannelKeyframes keys{{}, scene::PropertyChannel::Opacity,
      {0, 300, 600, 900, 1200}, {0.0, 1.0, 1.0, 0.2, 0.2},
      {scene::KeyInterpolation::Cubic, scene::KeyInterpolation::Linear,
       scene::KeyInterpolation::Cubic, scene::KeyInterpolation::Hold,
       scene::KeyInterpolation::Hold}};
  const auto full = TimelineGraphView::sample_curve(keys, time, axis, area);
  const auto drawn = TimelineGraphView::simplify(full, 0.25F);
  GRAPH_CHECK(drawn.size() < full.size() / 4U);
  GRAPH_CHECK(drawn.front().x == full.front().x && drawn.back().x == full.back().x);
  // Every sample lies within a quarter pixel (and a hair) of the drawn
  // segment spanning its x.
  for (const gui::UiPoint &point : full) {
    std::size_t segment = 1U;
    while (segment + 1U < drawn.size() && drawn[segment].x < point.x) {
      ++segment;
    }
    const gui::UiPoint a = drawn[segment - 1U];
    const gui::UiPoint b = drawn[segment];
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    const float distance =
        length > 0.0F
            ? std::fabs(dy * (point.x - a.x) - dx * (point.y - a.y)) / length
            : std::hypot(point.x - a.x, point.y - a.y);
    GRAPH_CHECK(distance <= 0.26F);
  }
  // A straight ramp is its two ends.
  const scene::ChannelKeyframes ramp{{}, scene::PropertyChannel::Opacity,
      {0, 1200}, {0.0, 1.0},
      {scene::KeyInterpolation::Linear, scene::KeyInterpolation::Linear}};
  const auto straight = TimelineGraphView::simplify(
      TimelineGraphView::sample_curve(ramp, time, axis, area), 0.25F);
  GRAPH_CHECK(straight.size() <= 4U);
}

// Drawing reuses each curve's samples until its keys, either axis or the
// area change, and forgets curves no longer shown.
void curves_are_sampled_again_only_when_something_changes() {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  TimelineGraphView graph;
  graph.fit_values(0.0, 1.0);
  mediaserver::editor::TimelineTimeAxis time{};
  time.pixels_per_frame = 2.0F;
  scene::ChannelKeyframes keys{{}, scene::PropertyChannel::Opacity, {0, 100},
      {0.0, 1.0},
      {scene::KeyInterpolation::Linear, scene::KeyInterpolation::Linear}};
  const timeline::ChannelId id{mediaserver::doc::NodeId{1U},
                               scene::PropertyChannel::Opacity};
  const std::array<GraphChannel, 1U> shown{
      {GraphChannel{id, &keys, gui::UiColor{}, false, false}}};
  const gui::Rect area{0.0F, 0.0F, 300.0F, 200.0F};
  const auto draw = [&](std::span<const GraphChannel> channels) {
    ctx.new_frame();
    graph.draw(ctx, area, time, channels, {});
    return graph.take_resampled();
  };
  GRAPH_CHECK(draw(shown) == 1U);
  GRAPH_CHECK(draw(shown) == 0U);
  keys.values[1] = 0.5;
  GRAPH_CHECK(draw(shown) == 1U);
  time.left_frame = 10.0;
  GRAPH_CHECK(draw(shown) == 1U);
  graph.axis().pan(20.0F, area);
  GRAPH_CHECK(draw(shown) == 1U);
  GRAPH_CHECK(draw(shown) == 0U);
  GRAPH_CHECK(draw({}) == 0U);
  GRAPH_CHECK(draw(shown) == 1U);
}

[[nodiscard]] bool has_vertex(const gui::UiDrawList &list, float x, float y) {
  for (const gui::UiVertex &vertex : list.vertices()) {
    if (std::fabs(vertex.x - x) < 0.01F && std::fabs(vertex.y - y) < 0.01F) {
      return true;
    }
  }
  return false;
}

// A key reads as how it leaves for the next one; a projection carrying no
// interpolations draws diamonds.
void key_glyphs_follow_the_outgoing_interpolation() {
  using timeline::KeyGlyph;
  GRAPH_CHECK(timeline::key_glyph(scene::KeyInterpolation::Linear) ==
              KeyGlyph::Diamond);
  GRAPH_CHECK(timeline::key_glyph(scene::KeyInterpolation::Cubic) ==
              KeyGlyph::Hourglass);
  GRAPH_CHECK(timeline::key_glyph(scene::KeyInterpolation::Hold) ==
              KeyGlyph::Square);
  const scene::ChannelKeyframes keys{{}, scene::PropertyChannel::Opacity,
      {0, 10, 20}, {0.0, 1.0, 0.5},
      {scene::KeyInterpolation::Cubic, scene::KeyInterpolation::Hold,
       scene::KeyInterpolation::Linear}};
  GRAPH_CHECK(timeline::key_glyph(keys, 0U) == KeyGlyph::Hourglass);
  GRAPH_CHECK(timeline::key_glyph(keys, 1U) == KeyGlyph::Square);
  GRAPH_CHECK(timeline::key_glyph(keys, 2U) == KeyGlyph::Diamond);
  const scene::ChannelKeyframes bare{{}, scene::PropertyChannel::Opacity,
                                     {0, 10}};
  GRAPH_CHECK(timeline::key_glyph(bare, 1U) == KeyGlyph::Diamond);
}

// The graph draws each key in the glyph its interpolation names, at the
// size the dope sheet uses.
void the_graph_draws_keys_by_interpolation() {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  TimelineGraphView graph;
  graph.fit_values(0.0, 1.0);
  mediaserver::editor::TimelineTimeAxis time{};
  time.pixels_per_frame = 2.0F;
  const scene::ChannelKeyframes keys{{}, scene::PropertyChannel::Opacity,
      {0, 10, 20}, {0.0, 1.0, 0.5},
      {scene::KeyInterpolation::Hold, scene::KeyInterpolation::Cubic,
       scene::KeyInterpolation::Linear}};
  const timeline::ChannelId id{mediaserver::doc::NodeId{1U},
                               scene::PropertyChannel::Opacity};
  const std::array<GraphChannel, 1U> shown{
      {GraphChannel{id, &keys, gui::UiColor{}, false, false}}};
  const std::array<timeline::GraphKey, 3U> drawn{{
      {0U, 0U, 0, 40.0F, 50.0F, false},
      {0U, 1U, 10, 90.0F, 60.0F, false},
      {0U, 2U, 20, 140.0F, 70.0F, false},
  }};
  ctx.new_frame();
  graph.draw(ctx, gui::Rect{0.0F, 0.0F, 300.0F, 200.0F}, time, shown, drawn);
  const float r = timeline::key_radius;
  // Hold: a square's corner, and no diamond's top.
  GRAPH_CHECK(has_vertex(list, 40.0F - r * 0.72F, 50.0F - r * 0.72F));
  GRAPH_CHECK(!has_vertex(list, 40.0F, 50.0F - r));
  // Cubic: an hourglass's top corner and its pinch.
  GRAPH_CHECK(has_vertex(list, 90.0F - r * 0.8F, 60.0F - r));
  GRAPH_CHECK(has_vertex(list, 90.0F, 60.0F));
  // Linear: a diamond's top, and neither of the others.
  GRAPH_CHECK(has_vertex(list, 140.0F, 70.0F - r));
  GRAPH_CHECK(!has_vertex(list, 140.0F - r * 0.8F, 70.0F - r));
  GRAPH_CHECK(!has_vertex(list, 140.0F - r * 0.72F, 70.0F - r * 0.72F));
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 15U> cases{{
    {"key glyphs follow the outgoing interpolation",
     &key_glyphs_follow_the_outgoing_interpolation},
    {"the graph draws keys by interpolation",
     &the_graph_draws_keys_by_interpolation},
    {"axis channels read red, green, blue",
     &axis_channels_read_red_green_blue},
    {"other channels listed together never share a colour",
     &other_channels_listed_together_never_share_a_colour},
    {"the value axis maps values and pixels both ways",
     &the_value_axis_maps_values_and_pixels_both_ways},
    {"fitting pads a tenth and keeps a flat channel visible",
     &fitting_pads_a_tenth_and_keeps_a_flat_channel_visible},
    {"zooming keeps the anchor and panning moves by pixels",
     &zooming_keeps_the_anchor_and_panning_moves_by_pixels},
    {"ticks are round steps at least the spacing apart",
     &ticks_are_round_steps_at_least_the_spacing_apart},
    {"curves follow the evaluator, not an easing guess",
     &curves_follow_the_evaluator_not_an_easing_guess},
    {"zoomed-out curves take one sample per column",
     &zoomed_out_curves_take_one_sample_per_column},
    {"fitting channels includes curve overshoot",
     &fitting_channels_includes_curve_overshoot},
    {"zoom and pan are reported against the last fit",
     &zoom_and_pan_are_reported_against_the_last_fit},
    {"channel visibility and the active channel reset together",
     &channel_visibility_and_the_active_channel_reset_together},
    {"curves are sampled again only when something changes",
     &curves_are_sampled_again_only_when_something_changes},
    {"simplified curves stay within a quarter pixel",
     &simplified_curves_stay_within_a_quarter_pixel},
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
