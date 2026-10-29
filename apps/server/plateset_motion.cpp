#include "plateset_motion.hpp"

#include <mediaserver/assets/plateset.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace mediaserver::app {
namespace {

using Signal = std::vector<float>;

// A centred moving average `width` frames wide, shortened at the ends.
[[nodiscard]] Signal smooth(const Signal &values, std::size_t width) {
  Signal result(values.size());
  const std::size_t half = width / 2U;
  double sum = 0.0;
  std::size_t from = 0U;
  std::size_t to = 0U;
  for (std::size_t i = 0U; i < values.size(); ++i) {
    const std::size_t want_to = std::min(values.size(), i + half + 1U);
    const std::size_t want_from = i > half ? i - half : 0U;
    while (to < want_to) {
      sum += values[to++];
    }
    while (from < want_from) {
      sum -= values[from++];
    }
    result[i] = static_cast<float>(sum / static_cast<double>(to - from));
  }
  return result;
}

[[nodiscard]] float median(Signal values) {
  if (values.empty()) {
    return 0.0F;
  }
  const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2U);
  std::nth_element(values.begin(), middle, values.end());
  return *middle;
}

// The median absolute deviation: a spread that a handful of spikes cannot
// inflate, which is what a threshold for spikes needs.
[[nodiscard]] float deviation(const Signal &values) {
  const float centre = median(values);
  Signal spread(values.size());
  std::transform(values.begin(), values.end(), spread.begin(),
                 [centre](float value) { return std::fabs(value - centre); });
  return median(std::move(spread));
}

[[nodiscard]] Signal normalized(Signal values) {
  if (values.empty()) {
    return values;
  }
  const double mean =
      std::accumulate(values.begin(), values.end(), 0.0) /
      static_cast<double>(values.size());
  double variance = 0.0;
  for (const float value : values) {
    variance += (value - mean) * (value - mean);
  }
  const double spread = std::sqrt(variance / static_cast<double>(values.size()));
  for (float &value : values) {
    value = spread > 1.0e-6
                ? static_cast<float>((value - mean) / spread)
                : 0.0F;
  }
  return values;
}

// What two cameras on one car share at the same instant: the jolt of each
// bump - vertical movement less its slow drift. Light is not shared that
// way: driving under a shadow darkens the front camera's view before the
// rear's, by an amount that depends on speed and where each camera faces,
// so it would pull plates apart rather than line them up.
struct Features final {
  Signal bumps;
};

[[nodiscard]] Features features_of(const MotionSignature &plate) {
  Features result;
  const Signal drift = smooth(plate.dy, 15U);
  result.bumps.resize(plate.size());
  for (std::size_t i = 0U; i < plate.size(); ++i) {
    result.bumps[i] = plate.dy[i] - drift[i];
  }
  result.bumps = normalized(std::move(result.bumps));
  return result;
}

// How well `b` shifted by `lag` matches `a` over frames [first, last) of
// `a`: the mean product of both features where they overlap.
[[nodiscard]] double score(const Features &a, const Features &b,
                           core::i64 lag, core::i64 first, core::i64 last) {
  const auto size_a = static_cast<core::i64>(a.bumps.size());
  const auto size_b = static_cast<core::i64>(b.bumps.size());
  first = std::max({first, core::i64{0}, -lag});
  last = std::min({last, size_a, size_b - lag});
  if (last <= first) {
    return 0.0;
  }
  double sum = 0.0;
  for (core::i64 t = first; t < last; ++t) {
    const auto i = static_cast<std::size_t>(t);
    const auto j = static_cast<std::size_t>(t + lag);
    sum += a.bumps[i] * b.bumps[j];
  }
  return sum / static_cast<double>(last - first);
}

struct Best final {
  core::i64 lag{};
  double score{-std::numeric_limits<double>::infinity()};
  // How many standard deviations the best score stands above all the
  // scores searched: a lucky peak on a short overlap stands a few above,
  // a real one many.
  double standing{};
};

[[nodiscard]] Best best_lag(const Features &a, const Features &b,
                            core::i64 lowest, core::i64 highest,
                            core::i64 first, core::i64 last) {
  std::vector<std::pair<core::i64, double>> scores;
  scores.reserve(static_cast<std::size_t>(std::max<core::i64>(
      highest - lowest + 1, 0)));
  Best best;
  for (core::i64 lag = lowest; lag <= highest; ++lag) {
    const double value = score(a, b, lag, first, last);
    scores.emplace_back(lag, value);
    if (value > best.score) {
      best.score = value;
      best.lag = lag;
    }
  }
  double mean = 0.0;
  for (const auto &[lag, value] : scores) {
    static_cast<void>(lag);
    mean += value;
  }
  mean /= static_cast<double>(std::max<std::size_t>(scores.size(), 1U));
  double spread = 0.0;
  for (const auto &[lag, value] : scores) {
    static_cast<void>(lag);
    spread += (value - mean) * (value - mean);
  }
  spread = std::sqrt(spread / static_cast<double>(
                                  std::max<std::size_t>(scores.size(), 1U)));
  best.standing = spread > 1.0e-12 ? (best.score - mean) / spread : 0.0;
  return best;
}

[[nodiscard]] std::size_t frames_for(const assets::MediaFrameRate &rate,
                                     double seconds) {
  const double fps = rate.denominator == 0U
                         ? 24.0
                         : static_cast<double>(rate.numerator) /
                               static_cast<double>(rate.denominator);
  return std::max<std::size_t>(
      1U, static_cast<std::size_t>(std::lround(fps * seconds)));
}

// Runs of frames where `inside` holds for at least `minimum` frames, as
// (first, last) pairs.
template <typename Predicate>
[[nodiscard]] std::vector<std::pair<std::size_t, std::size_t>>
runs(std::size_t count, std::size_t minimum, Predicate inside) {
  std::vector<std::pair<std::size_t, std::size_t>> found;
  std::size_t start = 0U;
  bool open = false;
  for (std::size_t i = 0U; i <= count; ++i) {
    const bool in = i < count && inside(i);
    if (in && !open) {
      start = i;
      open = true;
    } else if (!in && open) {
      if (i - start >= minimum) {
        found.emplace_back(start, i - 1U);
      }
      open = false;
    }
  }
  return found;
}

} // namespace

MotionSync estimate_plateset_sync(
    std::span<const std::optional<MotionSignature>, assets::plateset_slot_count>
        plates) {
  MotionSync sync;
  std::optional<std::size_t> reference;
  if (plates[0U].has_value() && plates[0U]->size() > 0U) {
    reference = 0U;
  } else {
    for (std::size_t slot = 0U; slot < plates.size(); ++slot) {
      if (plates[slot].has_value() && plates[slot]->size() > 0U) {
        reference = slot;
        break;
      }
    }
  }
  if (!reference.has_value()) {
    return sync;
  }
  const Features front = features_of(*plates[*reference]);
  const auto front_size = static_cast<core::i64>(front.bumps.size());
  std::array<std::optional<core::i64>, assets::plateset_slot_count> lags{};
  lags[*reference] = 0;
  sync.confidence[*reference] = 1.0F;
  for (std::size_t slot = 0U; slot < plates.size(); ++slot) {
    if (slot == *reference || !plates[slot].has_value() ||
        plates[slot]->size() == 0U) {
      continue;
    }
    const Features plate = features_of(*plates[slot]);
    const auto size = static_cast<core::i64>(plate.bumps.size());
    // Any lag that leaves a quarter of the shorter plate overlapping, and
    // never less than two seconds' worth.
    const core::i64 overlap = std::max<core::i64>(
        48, std::min(front_size, size) / 4);
    // Plates of one take start within a minute or so of each other;
    // searching further only finds coincidences.
    const auto reach = static_cast<core::i64>(
        frames_for(plates[*reference]->rate, 90.0));
    const Best best =
        best_lag(front, plate, std::max(overlap - front_size, -reach),
                 std::min(size - overlap, reach), 0, front_size);
    if (!(best.score > 0.0)) {
      continue;
    }
    lags[slot] = best.lag;
    // Four deviations is chance on a long search; twelve is certain.
    sync.confidence[slot] = static_cast<float>(
        std::clamp((best.standing - 4.0) / 8.0, 0.0, 1.0));

    // Drift: the same lag found again in the first and the last third of
    // the overlap, each searched only near the whole-take answer.
    const core::i64 first = std::max<core::i64>(0, -best.lag);
    const core::i64 last = std::min(front_size, size - best.lag);
    const core::i64 third = (last - first) / 3;
    if (third >= 48) {
      const Best early = best_lag(front, plate, best.lag - 8, best.lag + 8,
                                  first, first + third);
      const Best late = best_lag(front, plate, best.lag - 8, best.lag + 8,
                                 last - third, last);
      if (early.score > 0.0 && late.score > 0.0) {
        sync.drift[slot] = late.lag - early.lag;
      }
    }
  }
  // Agreement can only take trust away. If two plates are each right
  // against the front they also line up with each other at exactly the
  // difference of their lags, so a plate the others disagree with loses up
  // to half its confidence. It cannot add any: plates that all share the
  // front's signal agree with a chance match against the front too, so
  // only the sharpness of a plate's own peak can vouch for it.
  std::vector<std::size_t> placed;
  for (std::size_t slot = 0U; slot < lags.size(); ++slot) {
    if (slot != *reference && lags[slot].has_value()) {
      placed.push_back(slot);
    }
  }
  if (placed.size() >= 2U) {
    std::array<Features, assets::plateset_slot_count> features{};
    for (const std::size_t slot : placed) {
      features[slot] = features_of(*plates[slot]);
    }
    std::array<std::size_t, assets::plateset_slot_count> agreements{};
    for (std::size_t a = 0U; a < placed.size(); ++a) {
      for (std::size_t b = a + 1U; b < placed.size(); ++b) {
        const std::size_t left = placed[a];
        const std::size_t right = placed[b];
        const auto size_left =
            static_cast<core::i64>(features[left].bumps.size());
        const auto size_right =
            static_cast<core::i64>(features[right].bumps.size());
        const core::i64 overlap = std::max<core::i64>(
            48, std::min(size_left, size_right) / 4);
        const auto reach = static_cast<core::i64>(
            frames_for(plates[left]->rate, 90.0));
        const Best pair = best_lag(
            features[left], features[right],
            std::max(overlap - size_left, -reach),
            std::min(size_right - overlap, reach), 0, size_left);
        const core::i64 expected = *lags[right] - *lags[left];
        if (pair.score > 0.0 && std::llabs(pair.lag - expected) <= 1) {
          ++agreements[left];
          ++agreements[right];
        }
      }
    }
    const auto others = static_cast<float>(placed.size() - 1U);
    for (const std::size_t slot : placed) {
      sync.confidence[slot] *=
          0.5F + 0.5F * static_cast<float>(agreements[slot]) / others;
    }
  }

  // The common start is where the latest trusted plate starts; a plate
  // nobody agrees with must not move the others, and is only clamped to it.
  constexpr float trusted = 0.5F;
  core::i64 lowest = 0;
  for (std::size_t slot = 0U; slot < lags.size(); ++slot) {
    if (lags[slot].has_value() && sync.confidence[slot] >= trusted) {
      lowest = std::min(lowest, *lags[slot]);
    }
  }
  for (std::size_t slot = 0U; slot < lags.size(); ++slot) {
    if (lags[slot].has_value()) {
      sync.offsets[slot] = std::max<core::i64>(0, *lags[slot] - lowest);
    }
  }
  return sync;
}

std::vector<MotionEvent> detect_motion_events(const MotionSignature &front) {
  std::vector<MotionEvent> events;
  const std::size_t count = front.size();
  if (count < 2U) {
    return events;
  }
  const std::size_t half_second = frames_for(front.rate, 0.5);
  const std::size_t second = frames_for(front.rate, 1.0);

  // Turns: the picture sliding sideways faster than it does while driving
  // straight. Content moving left means the car is turning right.
  const Signal sideways = smooth(front.dx, half_second);
  const float straight = median(sideways);
  Signal turning(count);
  for (std::size_t i = 0U; i < count; ++i) {
    turning[i] = sideways[i] - straight;
  }
  const float turn_threshold = std::max(0.8F, 4.0F * deviation(turning));
  for (const auto &[first, last] :
       runs(count, half_second, [&](std::size_t i) {
         return std::fabs(turning[i]) > turn_threshold;
       })) {
    events.push_back(MotionEvent{
        static_cast<core::i64>(first),
        turning[(first + last) / 2U] < 0.0F ? "Turning right"
                                            : "Turning left",
        4U});
  }

  // Stops: a second or more of a picture that barely changes, against how
  // much it changes over the take.
  const Signal settled = smooth(front.change, half_second);
  const float typical = median(front.change);
  if (typical > 1.0e-4F) {
    // People crossing in front of a stopped car break a stop into pieces;
    // pieces less than two seconds apart are one stop.
    const std::size_t pause = frames_for(front.rate, 2.0);
    std::optional<std::size_t> stop_end;
    for (const auto &[first, last] : runs(count, second, [&](std::size_t i) {
           return settled[i] < 0.35F * typical;
         })) {
      if (!stop_end.has_value() || first - *stop_end > pause) {
        events.push_back(
            MotionEvent{static_cast<core::i64>(first), "Stopped", 3U});
      }
      stop_end = last;
    }
  }

  // Bumps: a vertical jolt well outside the road's ordinary shake, one
  // marker per second at most.
  const Signal drift = smooth(front.dy, 15U);
  Signal jolt(count);
  for (std::size_t i = 0U; i < count; ++i) {
    jolt[i] = front.dy[i] - drift[i];
  }
  const float bump_threshold = std::max(1.0F, 6.0F * deviation(jolt));
  std::optional<std::size_t> last_bump;
  for (std::size_t i = 1U; i < count; ++i) {
    if (std::fabs(jolt[i]) > bump_threshold &&
        (!last_bump.has_value() || i - *last_bump > second)) {
      events.push_back(MotionEvent{static_cast<core::i64>(i), "Bump", 1U});
      last_bump = i;
    }
  }

  // Shade: the picture darkening to under 70% of its usual brightness for
  // a quarter second or more - a bridge, a tunnel, a tall building.
  const Signal light = smooth(front.luma, 3U);
  const float usual = median(front.luma);
  if (usual > 1.0e-3F) {
    for (const auto &[first, last] :
         runs(count, frames_for(front.rate, 0.25), [&](std::size_t i) {
           return light[i] < 0.7F * usual;
         })) {
      static_cast<void>(last);
      events.push_back(
          MotionEvent{static_cast<core::i64>(first), "Into shade", 5U});
    }
  }

  std::sort(events.begin(), events.end(),
            [](const MotionEvent &left, const MotionEvent &right) {
              return left.frame < right.frame;
            });
  constexpr std::size_t most = 200U;
  if (events.size() > most) {
    events.resize(most);
  }
  return events;
}

} // namespace mediaserver::app
