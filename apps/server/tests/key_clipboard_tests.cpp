#include "key_clipboard.hpp"

#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace app = mediaserver::app;
namespace cmd = mediaserver::cmd;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace scene = mediaserver::scene;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define KEY_CHECK(...)                                                         \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] doc::PropertyValue whole(std::int64_t value) {
  return doc::PropertyValue{value};
}

[[nodiscard]] doc::Node target(core::u64 id, core::u64 scene_id = 1U,
                               bool locked = false) {
  doc::Node node{};
  node.id = doc::NodeId{id};
  node.type = std::string{scene::schema::object_type};
  node.properties.emplace(std::string{scene::schema::scene_id},
                          whole(static_cast<std::int64_t>(scene_id)));
  node.properties.emplace(std::string{scene::schema::locked}, locked);
  return node;
}

[[nodiscard]] doc::Node effect(core::u64 id, core::u64 owner,
                               std::string_view kind) {
  doc::Node node{};
  node.id = doc::NodeId{id};
  node.type = std::string{scene::schema::effect_type};
  node.properties.emplace(std::string{scene::schema::effect_target},
                          whole(static_cast<std::int64_t>(owner)));
  node.properties.emplace(std::string{scene::schema::effect_kind},
                          std::string{kind});
  return node;
}

[[nodiscard]] doc::Node
channel(core::u64 id, core::u64 target_id, std::string_view property,
        std::span<const core::i64> frames, std::span<const double> values,
        std::span<const scene::KeyInterpolation> interpolations) {
  KEY_CHECK(frames.size() == values.size());
  KEY_CHECK(frames.size() == interpolations.size());
  doc::Node node{};
  node.id = doc::NodeId{id};
  node.type = std::string{scene::schema::channel_type};
  node.properties.emplace(std::string{scene::schema::channel_target},
                          whole(static_cast<std::int64_t>(target_id)));
  node.properties.emplace(std::string{scene::schema::channel_component},
                          std::string{property});
  for (std::size_t index = 0U; index < frames.size(); ++index) {
    const std::string prefix =
        std::string{scene::schema::channel_key_prefix} + std::to_string(index);
    node.properties.emplace(
        prefix + std::string{scene::schema::channel_key_frame_suffix},
        whole(frames[index]));
    node.properties.emplace(
        prefix + std::string{scene::schema::channel_key_value_suffix},
        values[index]);
    std::string_view interpolation = scene::schema::interp_linear;
    switch (interpolations[index]) {
    case scene::KeyInterpolation::Hold:
      interpolation = scene::schema::interp_hold;
      break;
    case scene::KeyInterpolation::Linear:
      break;
    case scene::KeyInterpolation::Cubic:
      interpolation = scene::schema::interp_cubic;
      break;
    }
    node.properties.emplace(
        prefix + std::string{scene::schema::channel_key_interp_suffix},
        std::string{interpolation});
  }
  return node;
}

[[nodiscard]] std::int64_t integer(const doc::Node &node,
                                   std::string_view property) {
  const auto found = node.properties.find(property);
  KEY_CHECK(found != node.properties.end());
  const auto *const held = std::get_if<std::int64_t>(&found->second);
  KEY_CHECK(held != nullptr);
  return *held;
}

[[nodiscard]] double real(const doc::Node &node, std::string_view property) {
  const auto found = node.properties.find(property);
  KEY_CHECK(found != node.properties.end());
  const auto *const held = std::get_if<double>(&found->second);
  KEY_CHECK(held != nullptr);
  return *held;
}

[[nodiscard]] std::string text(const doc::Node &node,
                               std::string_view property) {
  const auto found = node.properties.find(property);
  KEY_CHECK(found != node.properties.end());
  const auto *const held = std::get_if<std::string>(&found->second);
  KEY_CHECK(held != nullptr);
  return *held;
}

[[nodiscard]] std::string key_property(std::size_t index,
                                       std::string_view suffix) {
  return std::string{scene::schema::channel_key_prefix} +
         std::to_string(index) + std::string{suffix};
}

[[nodiscard]] doc::Root source_document() {
  doc::Root root;
  root = root.set(
      doc::Node{doc::NodeId{1U}, std::string{scene::schema::stage_type}, {}});
  root = root.set(
      doc::Node{doc::NodeId{2U}, std::string{scene::schema::stage_type}, {}});
  root = root.set(target(10U));
  root = root.set(target(11U, 1U, true));
  root = root.set(effect(20U, 10U, scene::schema::film_grain_kind));

  constexpr std::array position_frames{core::i64{10}, core::i64{20}};
  constexpr std::array position_values{1.25, 2.5};
  constexpr std::array position_interpolations{scene::KeyInterpolation::Hold,
                                               scene::KeyInterpolation::Cubic};
  // A lower-id malformed duplicate must not steal the authoritative slot
  // from the first valid channel, matching SceneModel's parser order.
  doc::Node malformed = channel(
      99U, 10U, scene::schema::position_x,
      std::span<const core::i64>{position_frames}.first(1U),
      std::span<const double>{position_values}.first(1U),
      std::span<const scene::KeyInterpolation>{position_interpolations}.first(
          1U));
  malformed.properties.emplace(
      key_property(1U, scene::schema::channel_key_frame_suffix),
      std::string{"malformed"});
  root = root.set(std::move(malformed));
  doc::Node position =
      channel(100U, 10U, scene::schema::position_x, position_frames,
              position_values, position_interpolations);
  position.properties.insert_or_assign(
      key_property(1U, scene::schema::channel_key_value_suffix), whole(3));
  root = root.set(std::move(position));

  constexpr std::array grain_frames{core::i64{5}};
  constexpr std::array grain_values{0.4};
  constexpr std::array grain_interpolations{scene::KeyInterpolation::Linear};
  root = root.set(channel(101U, 20U, scene::schema::film_grain_amount,
                          grain_frames, grain_values, grain_interpolations));

  constexpr std::array locked_frames{core::i64{7}};
  constexpr std::array locked_values{9.0};
  constexpr std::array locked_interpolations{scene::KeyInterpolation::Linear};
  root = root.set(channel(102U, 11U, scene::schema::position_y, locked_frames,
                          locked_values, locked_interpolations));
  return root;
}

void copy_snapshots_values_and_paste_reanchors_every_property() {
  doc::Root source = source_document();
  app::KeyClipboard clipboard;
  constexpr std::array selection{
      app::KeyAddress{doc::NodeId{10U}, scene::PropertyChannel::PositionX, 20},
      app::KeyAddress{doc::NodeId{20U}, scene::PropertyChannel::FilmGrainAmount,
                      5},
      app::KeyAddress{doc::NodeId{10U}, scene::PropertyChannel::PositionX, 10},
      app::KeyAddress{doc::NodeId{10U}, scene::PropertyChannel::PositionX, 10}};
  const app::KeyCopyReport copied =
      clipboard.copy(source, scene::SceneId{1U}, selection);
  KEY_CHECK(copied.captured == 3U);
  KEY_CHECK(copied.locked_targets.empty());
  KEY_CHECK(copied.incompatible.empty());
  KEY_CHECK(clipboard.size() == 3U);

  // Copy is an immutable command-time snapshot, not a delayed document
  // query. Mutating the source value after Copy must not affect Paste.
  doc::Node changed = *source.get(doc::NodeId{100U});
  changed.properties.insert_or_assign(
      key_property(0U, scene::schema::channel_key_value_suffix), 99.0);
  source = source.set(std::move(changed));

  auto pasted = clipboard.paste(source, scene::SceneId{1U}, 100U);
  KEY_CHECK(pasted);
  KEY_CHECK(pasted.value().report.applied == 3U);
  KEY_CHECK(pasted.value().report.locked_targets.empty());
  KEY_CHECK(pasted.value().report.incompatible.empty());

  const doc::Node &position = *pasted.value().document.get(doc::NodeId{100U});
  KEY_CHECK(integer(position,
                    key_property(
                        2U, scene::schema::channel_key_frame_suffix)) == 105);
  KEY_CHECK(real(position,
                 key_property(2U, scene::schema::channel_key_value_suffix)) ==
            1.25);
  KEY_CHECK(text(position,
                 key_property(2U, scene::schema::channel_key_interp_suffix)) ==
            scene::schema::interp_hold);
  KEY_CHECK(integer(position,
                    key_property(
                        3U, scene::schema::channel_key_frame_suffix)) == 115);
  KEY_CHECK(
      integer(position,
              key_property(3U, scene::schema::channel_key_value_suffix)) == 3);
  KEY_CHECK(text(position,
                 key_property(3U, scene::schema::channel_key_interp_suffix)) ==
            scene::schema::interp_cubic);

  const doc::Node &grain = *pasted.value().document.get(doc::NodeId{101U});
  KEY_CHECK(integer(grain, key_property(
                               1U, scene::schema::channel_key_frame_suffix)) ==
            100);
  KEY_CHECK(
      real(grain, key_property(1U, scene::schema::channel_key_value_suffix)) ==
      0.4);

  cmd::DirectorScheduler scheduler{source};
  auto scheduled = scheduler.schedule_document_replace(
      pasted.value().document, cmd::UndoContextId{2U}, cmd::TransactionId{41U},
      cmd::FrameNumber{1U});
  KEY_CHECK(scheduled);
  KEY_CHECK(scheduled.value().size() == 2U);
  for (const cmd::Command &command : scheduled.value()) {
    KEY_CHECK(command.undo_context == cmd::UndoContextId{2U});
    KEY_CHECK(command.transaction == cmd::TransactionId{41U});
  }
  KEY_CHECK(scheduler.undo_depth(cmd::UndoContextId{2U}) == 1U);
  auto undone = scheduler.schedule_undo(
      cmd::UndoContextId{2U}, cmd::TransactionId{42U}, cmd::FrameNumber{1U});
  KEY_CHECK(undone);
  KEY_CHECK(doc::diff(scheduler.projected_document(), source).changes.empty());
}

void alt_duplicate_overwrites_collisions_and_is_one_undo_step() {
  const doc::Root source = source_document();
  constexpr std::array selection{
      app::KeyAddress{doc::NodeId{10U}, scene::PropertyChannel::PositionX, 10},
      app::KeyAddress{doc::NodeId{10U}, scene::PropertyChannel::PositionX, 20}};
  auto duplicated =
      app::duplicate_keys(source, scene::SceneId{1U}, selection, 10);
  KEY_CHECK(duplicated);
  KEY_CHECK(duplicated.value().report.applied == 2U);

  const doc::Node &channel_node =
      *duplicated.value().document.get(doc::NodeId{100U});
  // The first duplicate deterministically replaces the stationary key at
  // frame 20; the second appends frame 30. Values and interpolation travel.
  KEY_CHECK(real(channel_node,
                 key_property(1U, scene::schema::channel_key_value_suffix)) ==
            1.25);
  KEY_CHECK(text(channel_node,
                 key_property(1U, scene::schema::channel_key_interp_suffix)) ==
            scene::schema::interp_hold);
  KEY_CHECK(
      integer(channel_node,
              key_property(2U, scene::schema::channel_key_frame_suffix)) == 30);
  KEY_CHECK(
      integer(channel_node,
              key_property(2U, scene::schema::channel_key_value_suffix)) == 3);
  KEY_CHECK(text(channel_node,
                 key_property(2U, scene::schema::channel_key_interp_suffix)) ==
            scene::schema::interp_cubic);

  cmd::DirectorScheduler scheduler{source};
  auto scheduled = scheduler.schedule_document_replace(
      duplicated.value().document, cmd::UndoContextId{2U},
      cmd::TransactionId{77U}, cmd::FrameNumber{1U});
  KEY_CHECK(scheduled);
  KEY_CHECK(scheduled.value().size() == 1U);
  KEY_CHECK(scheduler.undo_depth(cmd::UndoContextId{2U}) == 1U);
  auto undone = scheduler.schedule_undo(
      cmd::UndoContextId{2U}, cmd::TransactionId{78U}, cmd::FrameNumber{1U});
  KEY_CHECK(undone);
  KEY_CHECK(doc::diff(scheduler.projected_document(), source).changes.empty());
}

void locked_and_incompatible_targets_are_reported_without_authorship() {
  doc::Root source = source_document();
  app::KeyClipboard clipboard;
  constexpr std::array rejected{
      app::KeyAddress{doc::NodeId{11U}, scene::PropertyChannel::PositionY, 7},
      app::KeyAddress{doc::NodeId{999U}, scene::PropertyChannel::PositionX, 0},
      app::KeyAddress{doc::NodeId{10U}, scene::PropertyChannel::PositionX,
                      999}};
  const app::KeyCopyReport copied =
      clipboard.copy(source, scene::SceneId{1U}, rejected);
  KEY_CHECK(copied.captured == 0U);
  KEY_CHECK(copied.locked_targets ==
            std::vector<doc::NodeId>{doc::NodeId{11U}});
  KEY_CHECK(copied.incompatible.size() == 2U);
  KEY_CHECK(clipboard.empty());

  constexpr std::array valid{
      app::KeyAddress{doc::NodeId{10U}, scene::PropertyChannel::PositionX, 10}};
  KEY_CHECK(clipboard.copy(source, scene::SceneId{1U}, valid).captured == 1U);
  doc::Node locked = *source.get(doc::NodeId{10U});
  locked.properties.insert_or_assign(std::string{scene::schema::locked}, true);
  source = source.set(std::move(locked));
  auto pasted = clipboard.paste(source, scene::SceneId{1U}, 30U);
  KEY_CHECK(pasted);
  KEY_CHECK(pasted.value().report.applied == 0U);
  KEY_CHECK(pasted.value().report.locked_targets ==
            std::vector<doc::NodeId>{doc::NodeId{10U}});
  KEY_CHECK(doc::diff(pasted.value().document, source).changes.empty());

  auto other_scene = clipboard.paste(source, scene::SceneId{2U}, 30U);
  KEY_CHECK(other_scene);
  KEY_CHECK(other_scene.value().report.applied == 0U);
  KEY_CHECK(other_scene.value().report.incompatible ==
            std::vector<app::KeyAddress>{valid.front()});

  clipboard.clear();
  KEY_CHECK(clipboard.empty());
  auto empty = clipboard.paste(source, scene::SceneId{1U}, 30U);
  KEY_CHECK(!empty);
  KEY_CHECK(empty.error() == app::KeyPasteError::Empty);
}

void frame_overflow_rejects_the_complete_batch() {
  doc::Root source;
  source = source.set(target(10U));
  constexpr core::i64 maximum = std::numeric_limits<core::i64>::max();
  constexpr std::array frames{core::i64{0}, maximum};
  constexpr std::array values{1.0, 2.0};
  constexpr std::array interpolations{scene::KeyInterpolation::Linear,
                                      scene::KeyInterpolation::Linear};
  source = source.set(channel(100U, 10U, scene::schema::position_x, frames,
                              values, interpolations));
  constexpr std::array selection{
      app::KeyAddress{doc::NodeId{10U}, scene::PropertyChannel::PositionX, 0},
      app::KeyAddress{doc::NodeId{10U}, scene::PropertyChannel::PositionX,
                      maximum}};
  app::KeyClipboard clipboard;
  KEY_CHECK(
      clipboard.copy(source, scene::legacy_scene_id, selection).captured == 2U);
  auto pasted = clipboard.paste(source, scene::legacy_scene_id, 1U);
  KEY_CHECK(!pasted);
  KEY_CHECK(pasted.error() == app::KeyPasteError::FrameOutOfRange);

  auto duplicated =
      app::duplicate_keys(source, scene::legacy_scene_id, selection, 1);
  KEY_CHECK(!duplicated);
  KEY_CHECK(duplicated.error() == app::KeyPasteError::FrameOutOfRange);
}

void spatial_primary_channels_copy_and_paste_by_typed_name() {
  doc::Root source;
  source = source.set(
      doc::Node{doc::NodeId{1U}, std::string{scene::schema::stage_type}, {}});
  source = source.set(target(10U));
  source = source.set(effect(20U, 10U, scene::schema::box_blur_kind));
  source = source.set(effect(21U, 10U, scene::schema::directional_blur_kind));
  source = source.set(effect(22U, 10U, scene::schema::chroma_key_kind));
  source = source.set(effect(23U, 10U, scene::schema::luma_key_kind));
  constexpr std::array frames{core::i64{5}};
  constexpr std::array box_values{2.5};
  constexpr std::array directional_values{12.0};
  constexpr std::array chroma_values{0.25};
  constexpr std::array luma_values{0.75};
  constexpr std::array interpolation{scene::KeyInterpolation::Linear};
  source = source.set(channel(100U, 20U, scene::schema::box_blur_radius, frames,
                              box_values, interpolation));
  source = source.set(channel(101U, 21U, scene::schema::directional_blur_length,
                              frames, directional_values, interpolation));
  source = source.set(channel(102U, 22U, scene::schema::chroma_key_mix, frames,
                              chroma_values, interpolation));
  source = source.set(channel(103U, 23U, scene::schema::luma_key_mix, frames,
                              luma_values, interpolation));

  constexpr std::array selection{
      app::KeyAddress{doc::NodeId{20U}, scene::PropertyChannel::BoxBlurRadius,
                      5},
      app::KeyAddress{doc::NodeId{21U},
                      scene::PropertyChannel::DirectionalBlurLength, 5},
      app::KeyAddress{doc::NodeId{22U}, scene::PropertyChannel::ChromaKeyMix,
                      5},
      app::KeyAddress{doc::NodeId{23U}, scene::PropertyChannel::LumaKeyMix, 5},
  };
  app::KeyClipboard clipboard;
  const app::KeyCopyReport copied =
      clipboard.copy(source, scene::SceneId{1U}, selection);
  KEY_CHECK(copied.captured == 4U);
  KEY_CHECK(copied.incompatible.empty());
  auto pasted = clipboard.paste(source, scene::SceneId{1U}, 100U);
  KEY_CHECK(pasted);
  KEY_CHECK(pasted.value().report.applied == 4U);
  const doc::Node &box = *pasted.value().document.get(doc::NodeId{100U});
  const doc::Node &directional =
      *pasted.value().document.get(doc::NodeId{101U});
  const doc::Node &chroma = *pasted.value().document.get(doc::NodeId{102U});
  const doc::Node &luma = *pasted.value().document.get(doc::NodeId{103U});
  KEY_CHECK(
      integer(box, key_property(1U, scene::schema::channel_key_frame_suffix)) ==
      100);
  KEY_CHECK(real(box, key_property(
                          1U, scene::schema::channel_key_value_suffix)) == 2.5);
  KEY_CHECK(real(directional,
                 key_property(1U, scene::schema::channel_key_value_suffix)) ==
            12.0);
  KEY_CHECK(
      real(chroma, key_property(1U, scene::schema::channel_key_value_suffix)) ==
      0.25);
  KEY_CHECK(
      real(luma, key_property(1U, scene::schema::channel_key_value_suffix)) ==
      0.75);
}

} // namespace

int main() {
  const std::array cases{
      copy_snapshots_values_and_paste_reanchors_every_property,
      alt_duplicate_overwrites_collisions_and_is_one_undo_step,
      locked_and_incompatible_targets_are_reported_without_authorship,
      frame_overflow_rejects_the_complete_batch,
      spatial_primary_channels_copy_and_paste_by_typed_name,
  };
  std::size_t passed = 0U;
  for (const auto test : cases) {
    try {
      test();
      ++passed;
    } catch (const std::exception &error) {
      std::cerr << "key clipboard test failed: " << error.what() << '\n';
      return 1;
    }
  }
  std::cout << passed << " key clipboard tests passed\n";
  return 0;
}
