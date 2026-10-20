#pragma once

// Characterization digests for the Timeline refactor. Every frame a timeline
// test draws is folded into one FNV-1a digest: the complete draw list
// (vertices, batches and clips), every request in the TimelinePanelFrame, and
// the context's hot, active and focus ids, typing state and cursor. A refactor
// that is meant to change nothing must reproduce every case's digest exactly;
// the per-frame digests say which frame of a case diverged first.
//
// This harness is temporary scaffolding for the structural split of
// TimelinePanel and is deleted once that split is finished, because later
// phases are meant to change pixels.

#include <mediaserver/editor/timeline_panel.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/gui/ui_renderer.hpp>

#include <bit>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace timeline_golden {

namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;
namespace scene = mediaserver::scene;
namespace assets = mediaserver::assets;
namespace doc = mediaserver::doc;

class Digest final {
public:
  void bytes(const void *data, std::size_t size) noexcept {
    const auto *cursor = static_cast<const unsigned char *>(data);
    for (std::size_t index = 0U; index < size; ++index) {
      value_ ^= cursor[index];
      value_ *= 1099511628211ULL;
    }
  }
  void u64(std::uint64_t value) noexcept { bytes(&value, sizeof(value)); }
  void i64(std::int64_t value) noexcept { u64(static_cast<std::uint64_t>(value)); }
  void f32(float value) noexcept { u64(std::bit_cast<std::uint32_t>(value)); }
  void f64(double value) noexcept { u64(std::bit_cast<std::uint64_t>(value)); }
  void flag(bool value) noexcept { u64(value ? 1U : 0U); }
  void text(std::string_view value) noexcept {
    u64(value.size());
    bytes(value.data(), value.size());
  }
  template <typename Enum>
    requires std::is_enum_v<Enum>
  void e(Enum value) noexcept {
    u64(static_cast<std::uint64_t>(value));
  }
  [[nodiscard]] std::uint64_t value() const noexcept { return value_; }

private:
  std::uint64_t value_{14695981039346656037ULL};
};

inline void put(Digest &d, std::uint64_t value) { d.u64(value); }
inline void put(Digest &d, std::int64_t value) { d.i64(value); }
inline void put(Digest &d, bool value) { d.flag(value); }
inline void put(Digest &d, doc::NodeId id) { d.u64(id.value()); }
inline void put(Digest &d, const scene::ClipTiming &t) {
  d.i64(t.start);
  d.i64(t.duration);
  d.i64(t.source_in);
  d.i64(t.fade_in);
  d.i64(t.fade_out);
}
inline void put(Digest &d, const scene::KeyTimeScale &s) {
  d.i64(s.numerator);
  d.i64(s.denominator);
  d.e(s.anchor);
  d.i64(s.playhead);
  d.i64(s.user_anchor);
}
inline void put(Digest &d, const assets::DerivativeKey &k) {
  put(d, k.asset);
  d.u64(k.fingerprint.byte_length);
  d.bytes(k.fingerprint.sha256.bytes.data(), k.fingerprint.sha256.bytes.size());
  d.e(k.profile);
  d.u64(k.profile_version);
  d.u64(k.generator_version);
  d.bytes(k.profile_signature.bytes.data(), k.profile_signature.bytes.size());
  d.bytes(k.digest.bytes.data(), k.digest.bytes.size());
}
inline void put(Digest &d, const editor::TimelineKeyAddress &a) {
  put(d, a.target);
  d.e(a.property);
  d.i64(a.frame);
}
inline void put(Digest &d, const editor::TimelineKeyEdit &k) {
  put(d, k.target);
  d.e(k.component);
  d.u64(k.frame);
  d.f64(k.value);
  d.flag(k.clear);
  d.flag(k.erase);
}
inline void put(Digest &d, const editor::TimelineKeyMoveEdit &k) {
  put(d, k.target);
  d.e(k.component);
  d.i64(k.from_frame);
  d.i64(k.to_frame);
}
inline void put(Digest &d, const editor::TimelineClipEdit &c) {
  put(d, c.clip);
  put(d, c.timing);
  d.flag(c.lane.has_value());
  if (c.lane) put(d, *c.lane);
}
inline void put(Digest &d, const editor::TimelineClipSplit &c) {
  put(d, c.clip);
  d.i64(c.frame);
}
inline void put(Digest &d, const editor::TimelineLaneEdit &l) {
  d.e(l.action);
  put(d, l.lane);
  d.text(l.name);
  put(d, l.destination);
}
inline void put(Digest &d, const editor::TimelineLayerReorder &r) {
  put(d, r.layer);
  put(d, r.destination);
  d.e(r.placement);
}
inline void put(Digest &d, const editor::TimelineElementFlagEdit &f) {
  put(d, f.element);
  d.e(f.flag);
  d.flag(f.value);
}
inline void put(Digest &d, const editor::TimelineRename &r) {
  put(d, r.node);
  d.text(r.name);
}
inline void put(Digest &d, const scene::TimelineMarker &m) {
  put(d, m.id);
  d.u64(m.scene.value());
  d.i64(m.frame);
  d.text(m.name);
  d.text(m.note);
  d.u64(m.color);
  d.e(m.cue);
  d.u64(m.loop_wait_ms);
  d.u64(m.loop_fade_ms);
}
inline void put(Digest &d, const editor::TimelineMarkerEdit &m) {
  d.e(m.action);
  put(d, m.marker);
}
inline void put(Digest &d, const editor::TimelineFilmstripRequestIntent &r) {
  put(d, r.key);
  d.e(r.priority);
}
inline void put(Digest &d, const editor::TimelineWaveformRequestIntent &r) {
  put(d, r.key);
  d.e(r.priority);
}
inline void put(Digest &d, const editor::TimelineRippleTrimRequest &r) {
  put(d, r.clip);
  d.e(r.edge);
  d.i64(r.delta);
}
inline void put(Digest &d, const editor::TimelineRippleDeleteRequest &r) {
  d.u64(r.clips.size());
  for (const doc::NodeId id : r.clips) put(d, id);
}
inline void put(Digest &d, const editor::TimelineRollRequest &r) {
  put(d, r.first);
  put(d, r.second);
  d.i64(r.delta);
}
inline void put(Digest &d, const editor::TimelineSlipRequest &r) {
  put(d, r.clip);
  d.i64(r.delta);
}
inline void put(Digest &d, const editor::TimelineSlideRequest &r) {
  put(d, r.clip);
  d.i64(r.delta);
}
inline void put(Digest &d, const editor::TimelineInsertRequest &r) {
  put(d, r.lane);
}
inline void put(Digest &d, const editor::TimelineOverwriteRequest &r) {
  put(d, r.lane);
}
inline void put(Digest &d, const editor::TimelineEditRequest &r) {
  d.u64(r.index());
  std::visit([&](const auto &request) { put(d, request); }, r);
}

template <typename T> void put(Digest &d, const std::vector<T> &values) {
  d.u64(values.size());
  for (const T &value : values) put(d, value);
}
template <typename T> void put(Digest &d, const std::optional<T> &value) {
  d.flag(value.has_value());
  if (value) put(d, *value);
}
inline void put(Digest &d, const editor::TimelineKeyDuplicateEdit &k) {
  put(d, k.keys);
  d.i64(k.delta);
}
inline void put(Digest &d, const editor::TimelineKeyBatchEdit &k) {
  d.e(k.action);
  put(d, k.keys);
  d.i64(k.delta);
  put(d, k.scale);
}
inline void put(Digest &d, const editor::TimelineKeyScalePreviewRequest &k) {
  d.u64(k.signature);
  put(d, k.keys);
  put(d, k.scale);
}
inline void put(Digest &d, const editor::TimelineKeyClipboardEdit &k) {
  d.e(k.action);
  put(d, k.keys);
}
inline void put(Digest &d, const editor::TimelineLayerDrop &l) {
  put(d, l.layers);
  d.f32(l.screen_x);
  d.f32(l.screen_y);
}
inline void put(Digest &d, const editor::TimelineSelectionRequest &s) {
  put(d, s.node);
  d.e(s.action);
  put(d, s.visible_order);
  d.flag(s.surface);
}
inline void put(Digest &d, const editor::RangeRequest &r) {
  d.u64(r.first);
  d.u64(r.last);
  d.flag(r.loop);
}
inline void put(Digest &d, const editor::ShowRateRequest &r) {
  d.u64(r.numerator);
  d.u64(r.denominator);
}
inline void put(Digest &d, const editor::TimelineFilmstripRetainIntent &r) {
  put(d, r.keys);
}
inline void put(Digest &d, const editor::TimelineWaveformRetainIntent &r) {
  put(d, r.keys);
}

inline void put(Digest &d, const editor::TimelinePanelFrame &f) {
  put(d, f.seek_to_frame);
  put(d, f.keys);
  put(d, f.key_moves);
  put(d, f.key_duplicate);
  put(d, f.key_batch);
  put(d, f.key_scale_preview);
  put(d, f.key_clipboard);
  put(d, f.clip_edits);
  put(d, f.clip_splits);
  put(d, f.lane_edits);
  put(d, f.layer_reorders);
  put(d, f.layer_drop);
  put(d, f.element_flags);
  put(d, f.renames);
  put(d, f.timeline_edits);
  d.text(f.clip_status);
  d.flag(f.clip_status_error);
  put(d, f.marker_edits);
  put(d, f.selection);
  d.e(f.transport);
  put(d, f.range);
  put(d, f.show_rate);
  put(d, f.cues_armed);
  put(d, f.filmstrip_requests);
  put(d, f.filmstrip_retain);
  put(d, f.waveform_requests);
  put(d, f.waveform_retain);
  d.flag(f.editing);
  d.flag(f.consume_delete);
  d.flag(f.consume_clipboard);
  d.flag(f.lane_editing);
}

inline void put(Digest &d, const gui::UiDrawList &list) {
  d.u64(list.vertices().size());
  for (const gui::UiVertex &v : list.vertices()) {
    d.f32(v.x);
    d.f32(v.y);
    d.f32(v.u);
    d.f32(v.v);
    d.f32(v.r);
    d.f32(v.g);
    d.f32(v.b);
    d.f32(v.a);
  }
  d.u64(list.batches().size());
  for (const gui::UiBatch &b : list.batches()) {
    d.u64(b.first_vertex);
    d.u64(b.vertex_count);
    d.f32(b.clip_min_x);
    d.f32(b.clip_min_y);
    d.f32(b.clip_max_x);
    d.f32(b.clip_max_y);
    // Texture addresses differ run to run; whether a batch samples an engine
    // texture rather than the atlas is what the panel decided.
    d.flag(b.texture != nullptr);
  }
}

// One drawn frame, after the shell's overlays.
[[nodiscard]] inline std::uint64_t
frame_digest(const gui::UiContext &ctx, const gui::UiDrawList &list,
             const editor::TimelinePanelFrame &frame) {
  Digest d;
  put(d, list);
  put(d, frame);
  d.u64(ctx.hot());
  d.u64(ctx.active());
  d.u64(ctx.focus());
  d.flag(ctx.typing());
  d.e(ctx.cursor());
  return d.value();
}

// The frames of the case being run, in draw order.
inline std::vector<std::uint64_t> &recorded_frames() {
  static std::vector<std::uint64_t> frames;
  return frames;
}

// Off while timing, so digesting a frame is not part of what is measured.
inline bool &observing() {
  static bool enabled = true;
  return enabled;
}

inline void observe(const gui::UiContext &ctx, const gui::UiDrawList &list,
                    const editor::TimelinePanelFrame &frame) {
  if (observing()) {
    recorded_frames().push_back(frame_digest(ctx, list, frame));
  }
}

} // namespace timeline_golden
