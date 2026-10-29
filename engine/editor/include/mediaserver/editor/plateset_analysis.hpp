#pragma once

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/core/types.hpp>

#include <array>
#include <optional>
#include <string>
#include <vector>

// The Director's motion analysis of a plateset as the editor shows it.
namespace mediaserver::editor {

// What the Director's motion analysis has found for the open plateset.
// Runtime observation only: nothing here is authored until the operator
// applies it.
enum class PlatesetAnalysisState : core::u8 {
  Idle,
  Queued,
  Running,
  Done,
  Failed,
};

// A moment the analysis noticed in the front plate, in that plate's source
// frames, offered as a timeline marker.
struct PlatesetMotionEvent final {
  core::i64 frame{};
  std::string name;
  core::u8 color{};

  bool operator==(const PlatesetMotionEvent &) const = default;
};

// Which analysis the operator asked for: every plate, to sync them, or the
// front plate alone, for its moments.
enum class PlatesetAnalysisPathway : core::u8 {
  Sync,
  Moments,
};

struct PlatesetAnalysisView final {
  PlatesetAnalysisState state{PlatesetAnalysisState::Idle};
  // 0..1 while running.
  float progress{};
  std::string detail;
  // Proposed sync offsets, per slot, and how sure the analysis is of each
  // (0..1). Slots it could not place are absent.
  std::array<std::optional<core::i64>, assets::plateset_slot_count> offsets{};
  std::array<float, assets::plateset_slot_count> confidence{};
  // How far each plate drifts from the front between the start and the end
  // of the take, in frames; absent where it could not be measured.
  std::array<std::optional<core::i64>, assets::plateset_slot_count> drift{};
  std::vector<PlatesetMotionEvent> events;
  // Which pathways have results: the offsets above come from a sync of
  // every plate; the events from the front plate alone.
  bool synced{};
  bool moments_found{};

  bool operator==(const PlatesetAnalysisView &) const = default;
};

} // namespace mediaserver::editor
