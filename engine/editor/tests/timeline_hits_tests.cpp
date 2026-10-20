#include "../src/timeline/timeline_hits.hpp"

#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

// The Timeline's pointer arbitration on its own: which of several overlapping
// targets owns a point.
namespace {

namespace gui = mediaserver::gui;
namespace timeline = mediaserver::editor::timeline;
using timeline::HitCollector;
using timeline::HitPriority;
using timeline::HitTarget;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define HIT_CHECK(...)                                                         \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

// A point target: answers within `reach` of (x, y).
[[nodiscard]] HitTarget point(gui::UiId id, HitPriority priority, float x,
                              float y, float reach) {
  return HitTarget{id, gui::Rect{x - reach, y - reach, reach * 2.0F, reach * 2.0F},
                   priority, gui::Rect{x, y, 0.0F, 0.0F}, id};
}

// A vertical edge at x over [top, top + height), answering `reach` either
// side of it.
[[nodiscard]] HitTarget edge(gui::UiId id, float x, float top, float height,
                             float reach) {
  return HitTarget{id, gui::Rect{x - reach, top, reach * 2.0F, height},
                   HitPriority::TrimEdge, gui::Rect{x, top, 0.0F, height}, id};
}

void nothing_owns_a_point_outside_every_target() {
  HitCollector hits;
  HIT_CHECK(hits.owner(10.0F, 10.0F) == nullptr);
  hits.add(point(7U, HitPriority::Key, 100.0F, 50.0F, 10.0F));
  HIT_CHECK(hits.owner(120.0F, 50.0F) == nullptr);
  HIT_CHECK(hits.owner(100.0F, 61.0F) == nullptr);
  HIT_CHECK(hits.owner(109.0F, 50.0F) != nullptr);
}

void a_key_over_a_trim_edge_takes_the_point_even_when_farther() {
  HitCollector hits;
  // The edge registers first and its line passes right through the point;
  // the key's centre is eight units away. The key still owns it.
  hits.add(edge(1U, 200.0F, 40.0F, 18.0F, 6.0F));
  hits.add(point(2U, HitPriority::Key, 208.0F, 49.0F, 10.0F));
  const HitTarget *owner = hits.owner(200.0F, 49.0F);
  HIT_CHECK(owner != nullptr && owner->id == 2U);
  // Beyond the key's reach the edge has it.
  owner = hits.owner(197.0F, 49.0F);
  HIT_CHECK(owner != nullptr && owner->id == 1U);
}

void priority_runs_key_fade_trim_body_lane() {
  HitCollector hits;
  const gui::Rect everywhere{0.0F, 0.0F, 100.0F, 100.0F};
  const gui::Rect at{50.0F, 50.0F, 0.0F, 0.0F};
  hits.add(HitTarget{1U, everywhere, HitPriority::Lane, everywhere, 1U});
  hits.add(HitTarget{2U, everywhere, HitPriority::ClipBody, everywhere, 2U});
  HIT_CHECK(hits.owner(10.0F, 10.0F)->id == 2U);
  hits.add(HitTarget{3U, everywhere, HitPriority::TrimEdge, at, 3U});
  HIT_CHECK(hits.owner(10.0F, 10.0F)->id == 3U);
  hits.add(HitTarget{4U, everywhere, HitPriority::FadeKnob, at, 4U});
  HIT_CHECK(hits.owner(10.0F, 10.0F)->id == 4U);
  hits.add(HitTarget{5U, everywhere, HitPriority::Key, at, 5U});
  HIT_CHECK(hits.owner(10.0F, 10.0F)->id == 5U);
}

void within_one_priority_the_nearest_mark_wins() {
  // A two-frame clip 14.8 units wide: both trim regions cover the whole
  // bar, so the edge nearer the pointer takes it.
  HitCollector hits;
  hits.add(edge(1U, 100.0F, 0.0F, 18.0F, 16.0F));
  hits.add(edge(2U, 114.8F, 0.0F, 18.0F, 16.0F));
  HIT_CHECK(hits.owner(103.0F, 9.0F)->id == 1U);
  HIT_CHECK(hits.owner(112.0F, 9.0F)->id == 2U);
  // Two keys a few units apart: the nearer centre.
  hits.add(point(3U, HitPriority::Key, 300.0F, 9.0F, 10.0F));
  hits.add(point(4U, HitPriority::Key, 306.0F, 9.0F, 10.0F));
  HIT_CHECK(hits.owner(302.0F, 9.0F)->id == 3U);
  HIT_CHECK(hits.owner(304.0F, 9.0F)->id == 4U);
}

void an_exact_tie_goes_to_the_first_registered() {
  HitCollector hits;
  hits.add(point(1U, HitPriority::Key, 50.0F, 50.0F, 10.0F));
  hits.add(point(2U, HitPriority::Key, 50.0F, 50.0F, 10.0F));
  HIT_CHECK(hits.owner(52.0F, 50.0F)->id == 1U);
  hits.clear();
  HIT_CHECK(hits.targets().empty());
  HIT_CHECK(hits.owner(52.0F, 50.0F) == nullptr);
}

void distance_is_measured_to_the_mark_not_the_bounds() {
  HIT_CHECK(timeline::mark_distance(gui::Rect{10.0F, 10.0F, 20.0F, 20.0F},
                                    15.0F, 25.0F) == 0.0F);
  HIT_CHECK(timeline::mark_distance(gui::Rect{10.0F, 10.0F, 0.0F, 0.0F},
                                    13.0F, 14.0F) == 5.0F);
  HIT_CHECK(timeline::mark_distance(gui::Rect{10.0F, 0.0F, 0.0F, 18.0F},
                                    7.0F, 9.0F) == 3.0F);
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 6U> cases{{
    {"nothing owns a point outside every target",
     &nothing_owns_a_point_outside_every_target},
    {"a key over a trim edge takes the point even when farther",
     &a_key_over_a_trim_edge_takes_the_point_even_when_farther},
    {"priority runs key, fade, trim, body, lane",
     &priority_runs_key_fade_trim_body_lane},
    {"within one priority the nearest mark wins",
     &within_one_priority_the_nearest_mark_wins},
    {"an exact tie goes to the first registered",
     &an_exact_tie_goes_to_the_first_registered},
    {"distance is measured to the mark, not the bounds",
     &distance_is_measured_to_the_mark_not_the_bounds},
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
