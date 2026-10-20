#include "../src/timeline/timeline_internal.hpp"

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

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 2U> cases{{
    {"axis channels read red, green, blue",
     &axis_channels_read_red_green_blue},
    {"other channels listed together never share a colour",
     &other_channels_listed_together_never_share_a_colour},
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
