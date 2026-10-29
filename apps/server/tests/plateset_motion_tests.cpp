#include "plateset_motion.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace app = mediaserver::app;
namespace assets = mediaserver::assets;
namespace core = mediaserver::core;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define MOTION_CHECK(...)                                                      \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

// One drive, frame by frame: what every camera on the car shares (bumps
// and light) and what only the front camera sees (turns, stops).
struct World final {
  std::vector<float> bump;
  std::vector<float> light;
  std::vector<float> sideways;
  std::vector<float> change;
};

[[nodiscard]] World drive(std::size_t frames, unsigned seed) {
  std::mt19937 random{seed};
  std::normal_distribution<float> shake{0.0F, 0.08F};
  World world;
  world.bump.resize(frames);
  world.light.assign(frames, 0.55F);
  world.sideways.assign(frames, -0.75F);
  world.change.assign(frames, 0.06F);
  for (std::size_t t = 0U; t < frames; ++t) {
    world.bump[t] = shake(random);
  }
  // Irregular bumps, so only one lag lines them all up: a road is not
  // periodic, and a fixture that was would match at every period.
  std::uniform_int_distribution<std::size_t> gap{18U, 70U};
  for (std::size_t at = 37U; at + 10U < frames; at += gap(random)) {
    for (std::size_t k = 0U; k < 8U; ++k) {
      world.bump[at + k] += 2.4F * std::exp(-static_cast<float>(k) / 2.5F) *
                            std::cos(static_cast<float>(k) * 1.3F);
    }
  }
  for (std::size_t t = 300U; t < 316U && t < frames; ++t) {
    world.light[t] = 0.2F;
  }
  return world;
}

[[nodiscard]] app::MotionSignature camera(const World &world, std::size_t start,
                                          std::size_t length);

// A stop interrupted by people crossing is still one stop.
void a_broken_stop_is_one_stop() {
  World world = drive(900U, 13U);
  for (std::size_t t = 100U; t < 220U; ++t) {
    world.sideways[t] = 0.0F;
    world.change[t] = (t / 30U) % 2U == 0U ? 0.004F : 0.05F;
  }
  const std::vector<app::MotionEvent> events =
      app::detect_motion_events(camera(world, 0U, 900U));
  MOTION_CHECK(std::count_if(events.begin(), events.end(),
                             [](const app::MotionEvent &event) {
                               return event.name == "Stopped";
                             }) == 1);
}

// A camera rolling at `start` into the drive, for `length` frames.
[[nodiscard]] app::MotionSignature camera(const World &world, std::size_t start,
                                          std::size_t length) {
  app::MotionSignature plate;
  plate.rate = assets::MediaFrameRate{24U, 1U};
  for (std::size_t i = 0U; i < length && start + i < world.bump.size(); ++i) {
    const std::size_t t = start + i;
    plate.dy.push_back(i == 0U ? 0.0F : world.bump[t]);
    plate.dx.push_back(i == 0U ? 0.0F : world.sideways[t]);
    plate.luma.push_back(world.light[t]);
    plate.change.push_back(i == 0U ? 0.0F : world.change[t]);
  }
  return plate;
}

void plates_are_lined_up_by_what_the_car_shares() {
  const World world = drive(900U, 3U);
  std::array<std::optional<app::MotionSignature>, assets::plateset_slot_count>
      plates{};
  // Front rolls at 10, left at 0, right at 26, back at 61.
  plates[0U] = camera(world, 10U, 760U);
  plates[6U] = camera(world, 0U, 760U);
  plates[2U] = camera(world, 26U, 760U);
  plates[4U] = camera(world, 61U, 760U);
  const app::MotionSync sync = app::estimate_plateset_sync(plates);
  // The back rolled last: everything else skips to where it starts.
  MOTION_CHECK(sync.offsets[4U] == 0);
  MOTION_CHECK(sync.offsets[0U] == 51);
  MOTION_CHECK(sync.offsets[6U] == 61);
  MOTION_CHECK(sync.offsets[2U] == 35);
  MOTION_CHECK(!sync.offsets[1U].has_value());
  MOTION_CHECK(sync.confidence[0U] == 1.0F);
  MOTION_CHECK(sync.confidence[6U] > 0.5F);
  MOTION_CHECK(sync.drift[6U] == 0);
}

void a_plate_that_drifts_is_reported() {
  const World world = drive(900U, 5U);
  std::array<std::optional<app::MotionSignature>, assets::plateset_slot_count>
      plates{};
  plates[0U] = camera(world, 0U, 800U);
  // The left camera's clock runs slow: three frames repeat mid-take, so by
  // the end it is three frames behind where it started.
  app::MotionSignature slow = camera(world, 20U, 800U);
  for (std::vector<float> *signal :
       {&slow.dx, &slow.dy, &slow.luma, &slow.change}) {
    signal->insert(signal->begin() + 400, 3U, (*signal)[400U]);
  }
  plates[6U] = std::move(slow);
  const app::MotionSync sync = app::estimate_plateset_sync(plates);
  MOTION_CHECK(sync.offsets[6U].has_value());
  MOTION_CHECK(sync.drift[6U] == 3);
}

void a_plate_that_shares_nothing_is_not_trusted() {
  const World world = drive(600U, 7U);
  // Another drive altogether: its bumps and its shade fall elsewhere.
  World other = drive(600U, 99U);
  std::rotate(other.bump.begin(), other.bump.begin() + 133, other.bump.end());
  std::rotate(other.light.begin(), other.light.begin() + 211,
              other.light.end());
  std::array<std::optional<app::MotionSignature>, assets::plateset_slot_count>
      plates{};
  plates[0U] = camera(world, 0U, 500U);
  plates[6U] = camera(world, 12U, 500U);
  plates[2U] = camera(other, 0U, 500U);
  plates[4U] = camera(world, 30U, 450U);
  const app::MotionSync sync = app::estimate_plateset_sync(plates);
  // The two honest plates agree with each other as well as with the front,
  // so they clear the bar an operator's Apply uses; the stranger does not.
  MOTION_CHECK(sync.confidence[6U] >= 0.5F);
  MOTION_CHECK(sync.confidence[4U] >= 0.5F);
  MOTION_CHECK(sync.confidence[2U] < 0.5F);
  MOTION_CHECK(sync.offsets[6U] == 18);
  MOTION_CHECK(sync.offsets[4U] == 0);
  MOTION_CHECK(sync.offsets[0U] == 30);
}

[[nodiscard]] bool has_event(const std::vector<app::MotionEvent> &events,
                             const std::string &name, core::i64 from,
                             core::i64 to) {
  return std::any_of(events.begin(), events.end(),
                     [&](const app::MotionEvent &event) {
                       return event.name == name && event.frame >= from &&
                              event.frame <= to;
                     });
}

void the_front_plate_says_what_the_car_did() {
  World world = drive(900U, 11U);
  // Stopped from 100 for two seconds: no slide, nothing changing.
  for (std::size_t t = 100U; t < 148U; ++t) {
    world.sideways[t] = 0.0F;
    world.change[t] = 0.004F;
  }
  // A right turn from 200 for a second: the picture slides left fast.
  for (std::size_t t = 200U; t < 224U; ++t) {
    world.sideways[t] = -3.0F;
  }
  // A left turn from 500.
  for (std::size_t t = 500U; t < 530U; ++t) {
    world.sideways[t] = 2.0F;
  }
  const std::vector<app::MotionEvent> events =
      app::detect_motion_events(camera(world, 0U, 900U));
  MOTION_CHECK(has_event(events, "Stopped", 98, 106));
  MOTION_CHECK(has_event(events, "Turning right", 195, 206));
  MOTION_CHECK(has_event(events, "Turning left", 495, 506));
  MOTION_CHECK(has_event(events, "Into shade", 298, 304));
  MOTION_CHECK(has_event(events, "Bump", 36, 40));
  // Bumps are at most one a second; nothing is reported twice in a row.
  for (std::size_t i = 1U; i < events.size(); ++i) {
    MOTION_CHECK(!(events[i].name == "Bump" && events[i - 1U].name == "Bump" &&
                   events[i].frame - events[i - 1U].frame <= 24));
  }
  MOTION_CHECK(std::is_sorted(events.begin(), events.end(),
                              [](const app::MotionEvent &a,
                                 const app::MotionEvent &b) {
                                return a.frame < b.frame;
                              }));
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 5U> cases{{
    {"a broken stop is one stop", &a_broken_stop_is_one_stop},
    {"plates are lined up by what the car shares",
     &plates_are_lined_up_by_what_the_car_shares},
    {"a plate that drifts is reported", &a_plate_that_drifts_is_reported},
    {"a plate that shares nothing is not trusted",
     &a_plate_that_shares_nothing_is_not_trusted},
    {"the front plate says what the car did",
     &the_front_plate_says_what_the_car_did},
}};

} // namespace

int main() {
  std::size_t failures = 0U;
  for (const Case &entry : cases) {
    try {
      entry.function();
      std::cout << "[pass] " << entry.name << std::endl;
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
    }
  }
  std::cout << (cases.size() - failures) << '/' << cases.size()
            << " tests passed\n";
  return failures == 0U ? 0 : 1;
}
