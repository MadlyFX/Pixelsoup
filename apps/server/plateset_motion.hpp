#pragma once

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/core/types.hpp>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Syncing a plateset from its pictures. Every camera on a car shares the
// car's bumps and the light it drives through, so each plate's motion
// signature lines up with the front plate's at exactly one lag. The same
// signature of the front plate also says when the car stops, turns, hits
// a bump or drives under a bridge. All of this is plain arithmetic on
// per-frame signals; decoding the plates into them is the service's job.
namespace mediaserver::app {

// Small per-frame measurements of one plate, at its own rate. Entry i
// describes frame i against frame i - 1; entry 0 is zero.
struct MotionSignature final {
  assets::MediaFrameRate rate{};
  // How far the picture's content moved, in pixels of a 160-wide analysis
  // frame: right and down are positive.
  std::vector<float> dx;
  std::vector<float> dy;
  // Mean brightness, 0..1.
  std::vector<float> luma;
  // Mean absolute difference from the previous frame, 0..1, before any
  // alignment: high while the car moves, low while it stands.
  std::vector<float> change;

  [[nodiscard]] std::size_t size() const noexcept { return dy.size(); }
};

struct MotionSync final {
  // Per slot: frames to skip so every plate starts on the same instant, as
  // a plateset authors them; absent where the plate could not be placed.
  std::array<std::optional<core::i64>, assets::plateset_slot_count> offsets{};
  // 0..1: how far the best lag stands above every other, reduced when the
  // other plates do not line up with this one where the lags say they
  // should.
  std::array<float, assets::plateset_slot_count> confidence{};
  // How many frames the lag moves between the start and the end of the
  // overlap: cameras sharing a start but not a clock.
  std::array<std::optional<core::i64>, assets::plateset_slot_count> drift{};
};

// Lines every plate up against the front plate (or, without one, the first
// plate present). `plates` is indexed by slot.
[[nodiscard]] MotionSync estimate_plateset_sync(
    std::span<const std::optional<MotionSignature>, assets::plateset_slot_count>
        plates);

struct MotionEvent final {
  core::i64 frame{};
  assets::PlatesetMomentKind kind{assets::PlatesetMomentKind::Stopped};

  bool operator==(const MotionEvent &) const = default;
};

// What the car did, from the front plate: stops, turns, bumps and shade,
// each at the frame it begins.
[[nodiscard]] std::vector<MotionEvent>
detect_motion_events(const MotionSignature &front);

} // namespace mediaserver::app
