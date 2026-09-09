#pragma once
#include <mediaserver/core/types.hpp>

namespace mediaserver::core {

// Give the replicated permit time to reach the cohort before its activation.
// A missed activation still holds; this lead is never permission to reveal
// late.
inline constexpr u64 asset_commit_lead_frames = 30;

// Allow heartbeat confirmations to arrive before scheduling another attempt.
inline constexpr u64 asset_commit_confirmation_frames = 30;

[[nodiscard]] inline bool
asset_commit_confirmation_expired(u64 activation_frame,
                                 u64 control_frame) noexcept {
  return activation_frame != 0 && control_frame >= activation_frame &&
         control_frame - activation_frame >= asset_commit_confirmation_frames;
}

// A complete, immutable requested asset set. Its identity comes from authored
// source/content generations, never a worker ticket, address or local clock.
struct AssetSetReadiness final {
  u64 digest{};
  bool ready{};
  u64 visible_commit{};
  bool operator==(const AssetSetReadiness &) const = default;
};

// Authored by the Director only after every Program member reports Ready.
// It travels as ordinary replicated document state in a FrameBatch, protected
// by the existing all-node FramePrepared / FrameCommitPermit barrier.
struct AssetCommit final {
  u64 digest{};
  u64 control_frame{};
  u64 show_frame{};
  bool operator==(const AssetCommit &) const = default;
};

// A ready renderer must get a chance to submit the activation snapshot even
// when playback drains a catch-up batch. Cold/late readiness cannot rewind it.
[[nodiscard]] inline u64
asset_presentation_frame(AssetSetReadiness assets, AssetCommit permit,
                         u64 presented_control_frame) noexcept {
  return assets.ready && assets.digest == permit.digest &&
                 assets.visible_commit < permit.control_frame &&
                 permit.control_frame >= presented_control_frame
             ? permit.control_frame
             : 0;
}

class AssetPublicationGate final {
public:
  // The offscreen candidate may advance while this returns false. The
  // displayed Program surface must retain its previous complete pixels.
  [[nodiscard]] bool present(AssetSetReadiness prepared, AssetCommit permit,
                             u64 control_frame, u64 show_frame,
                             bool cohort_required) noexcept {
    if (!cohort_required)
      return true;
    if (!prepared.ready) {
      ++holds_;
      return false;
    }
    if (prepared.digest == 0)
      return true;
    if (visible_digest_ == prepared.digest) {
      // A retry republishes the same immutable set at a fresh cohort frame.
      // Keep displaying it, but acknowledge that attempt only at its exact
      // activation. Otherwise members retain different visible commits forever.
      if (permit.digest == prepared.digest &&
          permit.control_frame > visible_commit_ &&
          permit.control_frame == control_frame &&
          permit.show_frame == show_frame)
        visible_commit_ = permit.control_frame;
      return true;
    }
    if (permit.digest != prepared.digest || permit.control_frame == 0 ||
        permit.control_frame != control_frame ||
        permit.show_frame != show_frame) {
      ++holds_;
      return false;
    }
    visible_digest_ = prepared.digest;
    visible_commit_ = permit.control_frame;
    return true;
  }
  [[nodiscard]] u64 visible_digest() const noexcept { return visible_digest_; }
  [[nodiscard]] u64 visible_commit() const noexcept { return visible_commit_; }
  [[nodiscard]] u64 holds() const noexcept { return holds_; }
  void reset() noexcept {
    visible_digest_ = 0;
    visible_commit_ = 0;
  }

private:
  u64 visible_digest_{}, visible_commit_{}, holds_{};
};

// Membership is supplied by the authoritative Program topology. Missing,
// stale, failed or differently requested members fail closed. This is shared
// by probe, still/video, model, mesh, LUT and environment asset publication.
struct AssetCohort final {
  bool operator==(const AssetCohort &) const = default;
  u64 digest{};
  u64 visible_commit{};
  u32 members{};
  u32 ready_members{};
  u32 reported_members{};
  bool ready{true};
  bool visible{true};
  bool some_visible{};
  void observe(const AssetSetReadiness *value) noexcept {
    ++members;
    if (value)
      ++reported_members;
    some_visible = some_visible || (value && value->visible_commit != 0);
    if (!value || !value->ready || value->digest == 0) {
      ready = false;
      visible = false;
      return;
    }
    if (digest == 0) {
      digest = value->digest;
      visible_commit = value->visible_commit;
    }
    if (value->digest == digest)
      ++ready_members;
    if (value->digest != digest)
      ready = false;
    if (value->digest != digest || value->visible_commit == 0 ||
        value->visible_commit != visible_commit)
      visible = false;
  }
  [[nodiscard]] bool can_commit() const noexcept {
    return ready && members != 0 && digest != 0;
  }
  [[nodiscard]] bool needs_commit(AssetCommit previous,
                                 u64 control_frame) const noexcept {
    // Partial activation must retry too. Already-visible members acknowledge
    // the fresh activation alongside members still holding the old image.
    return can_commit() && !visible &&
           (previous.digest != digest || previous.control_frame == 0 ||
            asset_commit_confirmation_expired(previous.control_frame,
                                             control_frame));
  }
};
} // namespace mediaserver::core
