#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <vector>

// Internal to the Timeline panel: which of several overlapping pointer
// targets a press or the hover belongs to. Every target answers a press over
// a region larger than what it draws, so a small mark - a key, a fade knob, a
// trim edge - is easy to hit; where those regions overlap, the resolver picks
// one owner rather than leaving it to the order widgets happened to be drawn.
namespace mediaserver::editor::timeline {

// What a target is, lowest first. Where targets overlap, the higher kind
// owns the point: a key over a fade knob over a trim edge over a clip's body
// over the empty lane.
enum class HitPriority : core::u8 {
  Lane,
  ClipBody,
  TrimEdge,
  FadeKnob,
  Key,
  // A selected Bezier key's tangent handle, over the key it belongs to.
  Handle,
};

struct HitTarget final {
  gui::UiId id{};
  // Where the target answers the pointer.
  gui::Rect bounds{};
  HitPriority priority{HitPriority::Lane};
  // What the target draws: a key's or knob's centre as an empty rectangle,
  // an edge as a zero-width one, a body as itself. Among targets of one
  // priority, the one whose mark is nearest the point owns it.
  gui::Rect mark{};
  // The caller's own description of the target.
  core::u64 tag{};
  // How far from its mark the target answers, within its bounds: a key is a
  // disc around its centre rather than the square that holds it.
  float reach{std::numeric_limits<float>::infinity()};
};

// The distance from (x, y) to the nearest point of `mark`; zero inside it.
[[nodiscard]] inline float mark_distance(const gui::Rect &mark, float x,
                                         float y) noexcept {
  const float dx =
      std::max({mark.x - x, 0.0F, x - (mark.right())});
  const float dy =
      std::max({mark.y - y, 0.0F, y - (mark.bottom())});
  return std::sqrt(dx * dx + dy * dy);
}

class HitCollector final {
public:
  void clear() noexcept { targets_.clear(); }

  void add(const HitTarget &target) { targets_.push_back(target); }

  // The one target that owns (x, y): of those whose bounds contain it, the
  // highest priority, then the nearest mark, then the first registered.
  [[nodiscard]] const HitTarget *owner(float x, float y) const noexcept {
    const HitTarget *best = nullptr;
    float best_distance = 0.0F;
    for (const HitTarget &target : targets_) {
      if (!target.bounds.contains(x, y)) {
        continue;
      }
      const float distance = mark_distance(target.mark, x, y);
      if (distance > target.reach) {
        continue;
      }
      if (best == nullptr || target.priority > best->priority ||
          (target.priority == best->priority && distance < best_distance)) {
        best = &target;
        best_distance = distance;
      }
    }
    return best;
  }

  [[nodiscard]] std::span<const HitTarget> targets() const noexcept {
    return targets_;
  }

private:
  std::vector<HitTarget> targets_;
};

} // namespace mediaserver::editor::timeline
