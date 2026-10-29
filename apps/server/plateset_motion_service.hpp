#pragma once

#include "plateset_motion.hpp"

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/plateset_analysis.hpp>

#include <array>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

// Decodes a plateset's plates into motion signatures on one background
// worker and analyses them. The control thread only queues work and copies
// the latest view: decoding a take's worth of 4K plates takes minutes and
// never runs where frames are recorded. Signatures are cached on disk by
// each plate's content fingerprint, so a take is decoded once.
namespace mediaserver::server {

struct PlateSource final {
  std::string source;
  std::optional<assets::ContentFingerprint> fingerprint;
  core::u32 frame_count{};
  assets::MediaFrameRate rate{};
};

using PlateSources =
    std::array<std::optional<PlateSource>, assets::plateset_slot_count>;

// Decodes one plate. Exposed for tests and tools; returns nothing on a
// file it cannot read or when `stop` is requested. `progress` is called
// with frames decoded so far.
[[nodiscard]] std::optional<app::MotionSignature>
decode_motion_signature(const PlateSource &plate, std::stop_token stop,
                        const std::function<void(core::u32)> &progress = {});

class PlatesetMotionService final {
public:
  explicit PlatesetMotionService(std::filesystem::path cache_directory);
  ~PlatesetMotionService();
  PlatesetMotionService(const PlatesetMotionService &) = delete;
  PlatesetMotionService &operator=(const PlatesetMotionService &) = delete;

  // What a request asks for: every plate, to sync them and find moments,
  // or the front plate alone, for its moments - a ninth of the decoding.
  enum class Pathway : core::u8 {
    Sync,
    Moments,
  };

  // Analyses `plateset`'s plates, replacing any analysis of it queued. At
  // most a few sets wait; the oldest waiting is dropped first. A Moments
  // request keeps the sync a previous Sync request found.
  void analyse(doc::NodeId plateset, PlateSources plates,
               Pathway pathway = Pathway::Sync);

  // What is known about `plateset` now: Idle when it was never asked for.
  [[nodiscard]] editor::PlatesetAnalysisView view(doc::NodeId plateset) const;

private:
  struct Job final {
    doc::NodeId plateset{};
    PlateSources plates;
    Pathway pathway{Pathway::Sync};
  };

  void run(std::stop_token stop);
  [[nodiscard]] std::optional<app::MotionSignature>
  signature(const PlateSource &plate, std::stop_token stop,
            const std::function<void(core::u32)> &progress);
  void publish(doc::NodeId plateset, editor::PlatesetAnalysisView view);

  std::filesystem::path cache_directory_;
  mutable std::mutex mutex_;
  std::condition_variable_any wake_;
  std::deque<Job> queue_;
  std::map<doc::NodeId, editor::PlatesetAnalysisView> views_;
  // Signatures decoded this session, by source and fingerprint.
  std::map<std::string, app::MotionSignature> memory_;
  std::jthread worker_;
};

} // namespace mediaserver::server
