#include "key_clipboard.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <mediaserver/scene/scene_schema.hpp>

namespace mediaserver::app {
namespace {

using Snapshot = KeyClipboard::Snapshot;

enum class TargetState {
  Editable,
  Locked,
  Incompatible,
};

enum class UpsertResult {
  Changed,
  Unchanged,
  Incompatible,
};

[[nodiscard]] const std::int64_t *
integer_property(const doc::Node &node, std::string_view name) noexcept {
  const auto found = node.properties.find(name);
  return found == node.properties.end()
             ? nullptr
             : std::get_if<std::int64_t>(&found->second);
}

[[nodiscard]] std::optional<doc::NodeId>
node_id_property(const doc::Node &node, std::string_view name) noexcept {
  const std::int64_t *const held = integer_property(node, name);
  if (held == nullptr || *held < 0) {
    return std::nullopt;
  }
  return doc::NodeId{static_cast<core::u64>(*held)};
}

[[nodiscard]] std::optional<bool>
locked_property(const doc::Node &node) noexcept {
  const auto found = node.properties.find(scene::schema::locked);
  if (found == node.properties.end()) {
    return false;
  }
  const bool *const held = std::get_if<bool>(&found->second);
  return held != nullptr ? std::optional<bool>{*held} : std::nullopt;
}

[[nodiscard]] TargetState target_state(const doc::Root &document,
                                       doc::NodeId target) noexcept {
  const doc::Node *node = document.get(target);
  if (node == nullptr) {
    return TargetState::Incompatible;
  }
  const std::optional<bool> own_lock = locked_property(*node);
  if (!own_lock.has_value()) {
    return TargetState::Incompatible;
  }
  if (*own_lock) {
    return TargetState::Locked;
  }

  // Effect keys inherit the lock of the drawable they modify, matching the
  // Timeline's selection rule. Other key targets own their lock directly.
  if (node->type != scene::schema::effect_type) {
    return TargetState::Editable;
  }
  const std::optional<doc::NodeId> owner =
      node_id_property(*node, scene::schema::effect_target);
  if (!owner.has_value()) {
    return TargetState::Incompatible;
  }
  node = document.get(*owner);
  if (node == nullptr) {
    return TargetState::Incompatible;
  }
  const std::optional<bool> owner_lock = locked_property(*node);
  if (!owner_lock.has_value()) {
    return TargetState::Incompatible;
  }
  return *owner_lock ? TargetState::Locked : TargetState::Editable;
}

[[nodiscard]] std::string_view
property_name(scene::PropertyChannel property) noexcept {
  switch (property) {
  case scene::PropertyChannel::PositionX:
    return scene::schema::position_x;
  case scene::PropertyChannel::PositionY:
    return scene::schema::position_y;
  case scene::PropertyChannel::PositionZ:
    return scene::schema::position_z;
  case scene::PropertyChannel::RotationYaw:
    return scene::schema::rotation_yaw;
  case scene::PropertyChannel::RotationPitch:
    return scene::schema::rotation_pitch;
  case scene::PropertyChannel::RotationRoll:
    return scene::schema::rotation_roll;
  case scene::PropertyChannel::ScaleX:
    return scene::schema::scale_x;
  case scene::PropertyChannel::ScaleY:
    return scene::schema::scale_y;
  case scene::PropertyChannel::ScaleZ:
    return scene::schema::scale_z;
  case scene::PropertyChannel::Opacity:
    return scene::schema::opacity;
  case scene::PropertyChannel::Brightness:
    return scene::schema::brightness;
  case scene::PropertyChannel::Saturation:
    return scene::schema::saturation;
  case scene::PropertyChannel::BlurRadius:
    return scene::schema::blur_radius;
  case scene::PropertyChannel::FilmGrainAmount:
    return scene::schema::film_grain_amount;
  case scene::PropertyChannel::BoxBlurRadius:
    return scene::schema::box_blur_radius;
  case scene::PropertyChannel::DirectionalBlurLength:
    return scene::schema::directional_blur_length;
  case scene::PropertyChannel::ChromaKeyMix:
    return scene::schema::chroma_key_mix;
  case scene::PropertyChannel::LumaKeyMix:
    return scene::schema::luma_key_mix;
  case scene::PropertyChannel::EnvironmentIntensity:
      return scene::schema::environment_map_intensity;
  case scene::PropertyChannel::EnvironmentRotation:
      return scene::schema::environment_map_rotation;
  case scene::PropertyChannel::FogDensity:
      return scene::schema::environment_fog_density;
  case scene::PropertyChannel::FogHeight:
      return scene::schema::environment_fog_height;
  case scene::PropertyChannel::FogFalloff:
      return scene::schema::environment_fog_height_falloff;
  case scene::PropertyChannel::FogAnisotropy:
      return scene::schema::environment_fog_anisotropy;
  case scene::PropertyChannel::SkyHaze:
      return scene::schema::sky_haze;
  case scene::PropertyChannel::SkyIlluminance:
      return scene::schema::sky_solar_illuminance;
  case scene::PropertyChannel::SkyTimeOfDay:
    return scene::schema::sky_time_of_day;
  case scene::PropertyChannel::Count:
    break;
  }
  return {};
}

[[nodiscard]] bool channel_matches(const doc::Node &node,
                                   const KeyAddress &address) noexcept {
  if (node.type != scene::schema::channel_type) {
    return false;
  }
  const std::optional<doc::NodeId> target =
      node_id_property(node, scene::schema::channel_target);
  if (!target.has_value() || *target != address.target) {
    return false;
  }
  const auto component = node.properties.find(scene::schema::channel_component);
  const std::string_view expected = property_name(address.property);
  if (component == node.properties.end() || expected.empty()) {
    return false;
  }
  const std::string *const held = std::get_if<std::string>(&component->second);
  return held != nullptr && *held == expected;
}

[[nodiscard]] const doc::Node *find_channel(const doc::Root &document,
                                            const KeyAddress &address);

[[nodiscard]] std::optional<scene::KeyInterpolation>
interpolation(const doc::Node &node, const std::string &index);

struct ChannelInspection final {
  bool valid{};
  std::optional<Snapshot> key;
};

[[nodiscard]] ChannelInspection inspect_channel(const doc::Node &channel,
                                                const KeyAddress &address) {
  const std::string prefix{scene::schema::channel_key_prefix};
  const std::string frame_suffix{scene::schema::channel_key_frame_suffix};
  const std::string value_suffix{scene::schema::channel_key_value_suffix};
  ChannelInspection result{};
  bool found_any = false;
  for (core::u32 key = 0U; key < scene::max_channel_keys; ++key) {
    const std::string index = std::to_string(key);
    const auto frame = channel.properties.find(prefix + index + frame_suffix);
    if (frame == channel.properties.end()) {
      result.valid = found_any;
      return result;
    }
    found_any = true;
    const std::int64_t *const held = std::get_if<std::int64_t>(&frame->second);
    if (held == nullptr) {
      return {};
    }

    double evaluated_value = 0.0;
    doc::PropertyValue value{0.0};
    const auto authored =
        channel.properties.find(prefix + index + value_suffix);
    if (authored != channel.properties.end()) {
      if (const double *const real = std::get_if<double>(&authored->second)) {
        evaluated_value = *real;
        value = authored->second;
      } else if (const std::int64_t *const whole =
                     std::get_if<std::int64_t>(&authored->second)) {
        evaluated_value = static_cast<double>(*whole);
        value = authored->second;
      } else {
        return {};
      }
    }
    constexpr double maximum =
        static_cast<double>(std::numeric_limits<float>::max());
    const auto interp = interpolation(channel, index);
    if (!std::isfinite(evaluated_value) || evaluated_value < -maximum ||
        evaluated_value > maximum || !interp.has_value()) {
      return {};
    }
    if (*held == address.frame && !result.key.has_value()) {
      result.key = Snapshot{channel.id, address, value, *interp};
    }
  }
  // SceneModel deliberately truncates a channel after the fixed cap, so a
  // full 64-key channel is valid even if unrelated properties follow it.
  result.valid = found_any;
  return result;
}

[[nodiscard]] const doc::Node *find_channel(const doc::Root &document,
                                            const KeyAddress &address) {
  const doc::Delta all = doc::diff(doc::Root{}, document);
  for (const doc::NodeChange &change : all.changes) {
    if (change.after && channel_matches(*change.after, address) &&
        inspect_channel(*change.after, address).valid) {
      // Root deltas are node-id ordered. Scene evaluation also lets the
      // lowest-id valid channel own a (target, property) slot.
      return change.after.get();
    }
  }
  return nullptr;
}

[[nodiscard]] std::optional<scene::KeyInterpolation>
interpolation(const doc::Node &node, const std::string &index) {
  const auto found = node.properties.find(
      std::string{scene::schema::channel_key_prefix} + index +
      std::string{scene::schema::channel_key_interp_suffix});
  if (found == node.properties.end()) {
    return scene::KeyInterpolation::Linear;
  }
  const std::string *const name = std::get_if<std::string>(&found->second);
  if (name == nullptr) {
    return std::nullopt;
  }
  if (*name == scene::schema::interp_hold) {
    return scene::KeyInterpolation::Hold;
  }
  if (*name == scene::schema::interp_linear) {
    return scene::KeyInterpolation::Linear;
  }
  if (*name == scene::schema::interp_cubic) {
    return scene::KeyInterpolation::Cubic;
  }
  return std::nullopt;
}

[[nodiscard]] std::string_view
interpolation_name(scene::KeyInterpolation value) noexcept {
  switch (value) {
  case scene::KeyInterpolation::Hold:
    return scene::schema::interp_hold;
  case scene::KeyInterpolation::Linear:
    return scene::schema::interp_linear;
  case scene::KeyInterpolation::Cubic:
    return scene::schema::interp_cubic;
  }
  return scene::schema::interp_linear;
}

[[nodiscard]] std::optional<Snapshot> snapshot_key(const doc::Node &channel,
                                                   const KeyAddress &address) {
  const ChannelInspection inspected = inspect_channel(channel, address);
  return inspected.valid ? inspected.key : std::nullopt;
}

[[nodiscard]] UpsertResult upsert_key(doc::Node &channel, core::i64 frame,
                                      const doc::PropertyValue &value,
                                      scene::KeyInterpolation interp) {
  const std::string prefix{scene::schema::channel_key_prefix};
  const std::string frame_suffix{scene::schema::channel_key_frame_suffix};
  const std::string value_suffix{scene::schema::channel_key_value_suffix};
  const std::string interp_suffix{scene::schema::channel_key_interp_suffix};
  std::optional<core::u32> destination;
  for (core::u32 key = 0U; key < scene::max_channel_keys; ++key) {
    const std::string index = std::to_string(key);
    const auto found = channel.properties.find(prefix + index + frame_suffix);
    if (found == channel.properties.end()) {
      destination = key;
      break;
    }
    const std::int64_t *const held = std::get_if<std::int64_t>(&found->second);
    if (held == nullptr) {
      return UpsertResult::Incompatible;
    }
    if (*held == frame) {
      destination = key;
      break;
    }
  }
  if (!destination.has_value()) {
    return UpsertResult::Incompatible;
  }

  const std::string index = std::to_string(*destination);
  const std::string frame_name = prefix + index + frame_suffix;
  const std::string value_name = prefix + index + value_suffix;
  const std::string interp_name = prefix + index + interp_suffix;
  const doc::PropertyValue wanted_frame{static_cast<std::int64_t>(frame)};
  const doc::PropertyValue wanted_interp{
      std::string{interpolation_name(interp)}};
  const auto same = [&](const std::string &name,
                        const doc::PropertyValue &wanted) {
    const auto found = channel.properties.find(name);
    return found != channel.properties.end() && found->second == wanted;
  };
  if (same(frame_name, wanted_frame) && same(value_name, value) &&
      same(interp_name, wanted_interp)) {
    return UpsertResult::Unchanged;
  }
  channel.properties.insert_or_assign(frame_name, wanted_frame);
  channel.properties.insert_or_assign(value_name, value);
  channel.properties.insert_or_assign(interp_name, wanted_interp);
  return UpsertResult::Changed;
}

void append_unique(std::vector<doc::NodeId> &values, doc::NodeId value) {
  if (std::find(values.begin(), values.end(), value) == values.end()) {
    values.push_back(value);
  }
}

void append_unique(std::vector<KeyAddress> &values, KeyAddress value) {
  if (std::find(values.begin(), values.end(), value) == values.end()) {
    values.push_back(value);
  }
}

[[nodiscard]] std::vector<KeyAddress>
canonical_addresses(std::span<const KeyAddress> selection) {
  std::vector<KeyAddress> result{selection.begin(), selection.end()};
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

[[nodiscard]] KeyCopyReport capture(const doc::Root &document,
                                    std::span<const KeyAddress> selection,
                                    std::vector<Snapshot> &out,
                                    core::i64 &anchor) {
  KeyCopyReport report{};
  std::optional<core::i64> earliest;
  const std::vector<KeyAddress> addresses = canonical_addresses(selection);
  out.clear();
  out.reserve(addresses.size());
  for (const KeyAddress &address : addresses) {
    const TargetState state = target_state(document, address.target);
    if (state == TargetState::Locked) {
      append_unique(report.locked_targets, address.target);
      continue;
    }
    if (state == TargetState::Incompatible) {
      append_unique(report.incompatible, address);
      continue;
    }
    const doc::Node *const channel = find_channel(document, address);
    const std::optional<Snapshot> snapshot =
        channel != nullptr ? snapshot_key(*channel, address) : std::nullopt;
    if (!snapshot.has_value()) {
      append_unique(report.incompatible, address);
      continue;
    }
    out.push_back(*snapshot);
    earliest = earliest.has_value() ? std::min(*earliest, address.frame)
                                    : address.frame;
  }
  report.captured = out.size();
  anchor = earliest.value_or(0);
  std::sort(report.locked_targets.begin(), report.locked_targets.end());
  std::sort(report.incompatible.begin(), report.incompatible.end());
  return report;
}

template <typename Destination>
[[nodiscard]] core::Result<KeyDocumentPlan, KeyPasteError>
apply_snapshots(const doc::Root &document, std::span<const Snapshot> snapshots,
                Destination &&destination) {
  struct Candidate final {
    const Snapshot *snapshot{};
    doc::NodeId channel{};
    core::i64 destination{};
  };
  using Slot = std::pair<doc::NodeId, core::i64>;
  std::map<Slot, Candidate> winners;
  KeyDocumentPlan plan{};
  plan.document = document;

  for (const Snapshot &snapshot : snapshots) {
    const TargetState state = target_state(document, snapshot.address.target);
    if (state == TargetState::Locked) {
      append_unique(plan.report.locked_targets, snapshot.address.target);
      continue;
    }
    if (state == TargetState::Incompatible) {
      append_unique(plan.report.incompatible, snapshot.address);
      continue;
    }
    const doc::Node *const channel = find_channel(document, snapshot.address);
    if (channel == nullptr) {
      append_unique(plan.report.incompatible, snapshot.address);
      continue;
    }
    const std::optional<core::i64> shifted =
        destination(snapshot.address.frame);
    if (!shifted.has_value() || *shifted < 0) {
      return core::Result<KeyDocumentPlan, KeyPasteError>::failure(
          KeyPasteError::FrameOutOfRange);
    }
    Candidate candidate{&snapshot, channel->id, *shifted};
    const Slot slot{candidate.channel, candidate.destination};
    const auto found = winners.find(slot);
    // Keys are indexed properties rather than document nodes of their own,
    // so source_channel is the replicated node-id carrier in the required
    // (original frame, node id) collision ordering.
    if (found == winners.end() ||
        std::pair{found->second.snapshot->address.frame,
                  found->second.snapshot->source_channel.value()} <
            std::pair{snapshot.address.frame,
                      snapshot.source_channel.value()}) {
      winners.insert_or_assign(slot, candidate);
    }
  }

  for (const auto &[slot, candidate] : winners) {
    const doc::Node *const current = plan.document.get(slot.first);
    if (current == nullptr) {
      append_unique(plan.report.incompatible, candidate.snapshot->address);
      continue;
    }
    doc::Node changed = *current;
    const UpsertResult upserted =
        upsert_key(changed, candidate.destination, candidate.snapshot->value,
                   candidate.snapshot->interpolation);
    if (upserted == UpsertResult::Incompatible) {
      append_unique(plan.report.incompatible, candidate.snapshot->address);
      continue;
    }
    if (upserted == UpsertResult::Changed) {
      plan.document = plan.document.set(std::move(changed));
      ++plan.report.applied;
    }
  }
  std::sort(plan.report.locked_targets.begin(),
            plan.report.locked_targets.end());
  std::sort(plan.report.incompatible.begin(), plan.report.incompatible.end());
  return core::Result<KeyDocumentPlan, KeyPasteError>::success(std::move(plan));
}

[[nodiscard]] std::optional<core::i64> add_delta(core::i64 frame,
                                                 core::i64 delta) noexcept {
  if ((delta > 0 && frame > std::numeric_limits<core::i64>::max() - delta) ||
      (delta < 0 && frame < std::numeric_limits<core::i64>::min() - delta)) {
    return std::nullopt;
  }
  return frame + delta;
}

[[nodiscard]] std::optional<core::i64>
reanchor_frame(core::i64 frame, core::i64 anchor,
               core::u64 destination_anchor) noexcept {
  if (frame < anchor) {
    return std::nullopt;
  }
  core::u64 distance = 0U;
  if (anchor >= 0) {
    distance = static_cast<core::u64>(frame - anchor);
  } else if (frame < 0) {
    // Two negative values with frame >= anchor differ by at most INT64_MAX.
    distance = static_cast<core::u64>(frame - anchor);
  } else {
    // Avoid signed overflow for the full [INT64_MIN, INT64_MAX] span.
    const core::u64 below_zero = static_cast<core::u64>(-(anchor + 1)) + 1U;
    distance = static_cast<core::u64>(frame) + below_zero;
  }
  constexpr core::u64 maximum =
      static_cast<core::u64>(std::numeric_limits<core::i64>::max());
  if (destination_anchor > maximum || distance > maximum - destination_anchor) {
    return std::nullopt;
  }
  return static_cast<core::i64>(destination_anchor + distance);
}

} // namespace

KeyCopyReport KeyClipboard::copy(const doc::Root &document,
                                 scene::SceneId source_scene,
                                 std::span<const KeyAddress> selection) {
  std::vector<Snapshot> captured;
  core::i64 anchor = 0;
  KeyCopyReport report = capture(document, selection, captured, anchor);
  keys_ = std::move(captured);
  source_scene_ = source_scene;
  anchor_ = anchor;
  return report;
}

core::Result<KeyDocumentPlan, KeyPasteError>
KeyClipboard::paste(const doc::Root &document, scene::SceneId active_scene,
                    core::u64 playhead) const {
  if (keys_.empty()) {
    return core::Result<KeyDocumentPlan, KeyPasteError>::failure(
        KeyPasteError::Empty);
  }
  constexpr core::u64 maximum =
      static_cast<core::u64>(std::numeric_limits<core::i64>::max());
  if (playhead > maximum) {
    return core::Result<KeyDocumentPlan, KeyPasteError>::failure(
        KeyPasteError::FrameOutOfRange);
  }
  if (active_scene != source_scene_) {
    KeyDocumentPlan plan{};
    plan.document = document;
    for (const Snapshot &snapshot : keys_) {
      append_unique(plan.report.incompatible, snapshot.address);
    }
    std::sort(plan.report.incompatible.begin(), plan.report.incompatible.end());
    return core::Result<KeyDocumentPlan, KeyPasteError>::success(
        std::move(plan));
  }
  return apply_snapshots(document, keys_,
                         [&](core::i64 frame) -> std::optional<core::i64> {
                           return reanchor_frame(frame, anchor_, playhead);
                         });
}

void KeyClipboard::clear() noexcept {
  keys_.clear();
  source_scene_ = scene::SceneId{};
  anchor_ = 0;
}

core::Result<KeyDocumentPlan, KeyPasteError>
duplicate_keys(const doc::Root &document, scene::SceneId active_scene,
               std::span<const KeyAddress> selection, core::i64 delta) {
  static_cast<void>(active_scene);
  std::vector<Snapshot> snapshots;
  core::i64 anchor = 0;
  KeyCopyReport report = capture(document, selection, snapshots, anchor);
  if (snapshots.empty()) {
    KeyDocumentPlan plan{};
    plan.document = document;
    plan.report.locked_targets = std::move(report.locked_targets);
    plan.report.incompatible = std::move(report.incompatible);
    return core::Result<KeyDocumentPlan, KeyPasteError>::success(
        std::move(plan));
  }
  return apply_snapshots(document, snapshots, [delta](core::i64 frame) {
    return add_delta(frame, delta);
  });
}

} // namespace mediaserver::app
