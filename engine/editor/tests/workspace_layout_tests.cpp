#include <mediaserver/editor/workspace_layout.hpp>

#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define LAYOUT_CHECK(...)                                                     \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

const gui::PanelId viewport = gui::ui_id("workspace.viewport");
const gui::PanelId inspector = gui::ui_id("workspace.inspector");
const gui::PanelId timeline = gui::ui_id("workspace.timeline");

[[nodiscard]] gui::DockLayout sample_layout() {
  gui::DockLayout dock;
  const gui::DockId view = dock.add(viewport, "Viewport");
  const gui::DockId properties =
      dock.dock(inspector, "Inspector", view, gui::DockSide::Right, 0.27F);
  dock.dock(timeline, "Timeline", properties, gui::DockSide::Center);
  dock.set_active(properties, timeline);
  dock.close(inspector);
  return dock;
}

void a_layout_codec_round_trip_is_stable() {
  const gui::DockLayout original = sample_layout();
  const auto encoded = editor::encode_workspace_layout(
      editor::WorkspaceLayoutState{original.snapshot(), 1536U, 864U,
          editor::PreviewResolution::Half, editor::PreviewResolution::Quarter});
  LAYOUT_CHECK(encoded);
  LAYOUT_CHECK(!encoded.value().empty());

  const auto decoded = editor::decode_workspace_layout(encoded.value());
  LAYOUT_CHECK(decoded);

  gui::DockLayout restored;
  restored.register_panel(viewport, "Viewport");
  restored.register_panel(inspector, "Inspector");
  restored.register_panel(timeline, "Timeline");
  LAYOUT_CHECK(decoded.value().window_width == 1536U);
  LAYOUT_CHECK(decoded.value().window_height == 864U);
  LAYOUT_CHECK(decoded.value().stage_resolution == editor::PreviewResolution::Half);
  LAYOUT_CHECK(decoded.value().program_resolution == editor::PreviewResolution::Quarter);
  LAYOUT_CHECK(restored.restore(decoded.value().docks));
  LAYOUT_CHECK(restored.is_open(viewport));
  LAYOUT_CHECK(restored.is_open(timeline));
  LAYOUT_CHECK(!restored.is_open(inspector));
  LAYOUT_CHECK(restored.active_of(restored.leaf_of(timeline)) == timeline);

  const auto reencoded = editor::encode_workspace_layout(
      editor::WorkspaceLayoutState{restored.snapshot(), 1536U, 864U,
          decoded.value().stage_resolution, decoded.value().program_resolution});
  LAYOUT_CHECK(reencoded);
  LAYOUT_CHECK(reencoded.value() == encoded.value());
}

void truncation_trailing_bytes_and_other_versions_are_rejected() {
  const auto encoded =
      editor::encode_workspace_layout(editor::WorkspaceLayoutState{
          sample_layout().snapshot(), 1400U, 900U});
  LAYOUT_CHECK(encoded);

  for (std::size_t size = 0U; size < encoded.value().size(); ++size) {
    LAYOUT_CHECK(!editor::decode_workspace_layout(
        std::span<const std::byte>{encoded.value()}.first(size)));
  }

  auto trailing = encoded.value();
  trailing.push_back(std::byte{0U});
  LAYOUT_CHECK(!editor::decode_workspace_layout(trailing));

  auto newer = encoded.value();
  newer[8U] = std::byte{0xffU};
  const auto unsupported = editor::decode_workspace_layout(newer);
  LAYOUT_CHECK(!unsupported);
  LAYOUT_CHECK(unsupported.error() ==
               editor::WorkspaceLayoutError::UnsupportedVersion);

  // Only the current version is read: an older record is refused, and the
  // editor opens its default layout instead.
  auto older = encoded.value();
  older[8U] = std::byte{5U};
  const auto refused = editor::decode_workspace_layout(older);
  LAYOUT_CHECK(!refused);
  LAYOUT_CHECK(refused.error() ==
               editor::WorkspaceLayoutError::UnsupportedVersion);
}

void interface_zoom_round_trips() {
  editor::WorkspaceLayoutState state{sample_layout().snapshot(), 1400U, 900U};
  state.ui_zoom_percent = 150U;
  const auto encoded = editor::encode_workspace_layout(state).value();
  const auto decoded = editor::decode_workspace_layout(encoded);
  LAYOUT_CHECK(decoded);
  LAYOUT_CHECK(decoded.value().ui_zoom_percent == 150U);
}

// The operator's Parameters width survives a restart, and nonsense is refused.
void compositor_parameter_width_round_trips() {
  editor::WorkspaceLayoutState state{sample_layout().snapshot(), 1400U, 900U};
  state.compositor_parameter_width = 312.5F;
  const auto encoded = editor::encode_workspace_layout(state).value();
  const auto decoded = editor::decode_workspace_layout(encoded);
  LAYOUT_CHECK(decoded);
  LAYOUT_CHECK(decoded.value().compositor_parameter_width == 312.5F);

  // NaN's bit pattern, then a negative width, in the width's word.
  auto not_a_number = encoded;
  not_a_number[40U] = std::byte{0x00U};
  not_a_number[41U] = std::byte{0x00U};
  not_a_number[42U] = std::byte{0xc0U};
  not_a_number[43U] = std::byte{0x7fU};
  LAYOUT_CHECK(!editor::decode_workspace_layout(not_a_number));
  auto negative = encoded;
  negative[43U] = std::byte{0xc3U};
  LAYOUT_CHECK(!editor::decode_workspace_layout(negative));
  state.compositor_parameter_width = -1.0F;
  LAYOUT_CHECK(!editor::encode_workspace_layout(state));
  state.compositor_parameter_width = 5'000.0F;
  LAYOUT_CHECK(!editor::encode_workspace_layout(state));
}

// Whether the Compositor shows its minimap, and whether the night dim is on,
// survive a restart, and a flag the editor does not know is refused rather
// than dropped.
void compositor_minimap_round_trips() {
  editor::WorkspaceLayoutState state{sample_layout().snapshot(), 1400U, 900U};
  state.compositor_parameter_width = 300.0F;
  state.compositor_minimap = true;
  const auto encoded = editor::encode_workspace_layout(state).value();
  const auto decoded = editor::decode_workspace_layout(encoded);
  LAYOUT_CHECK(decoded);
  LAYOUT_CHECK(decoded.value().compositor_minimap);
  LAYOUT_CHECK(decoded.value().compositor_parameter_width == 300.0F);
  state.compositor_minimap = false;
  const auto hidden = editor::decode_workspace_layout(
      editor::encode_workspace_layout(state).value());
  LAYOUT_CHECK(hidden);
  LAYOUT_CHECK(!hidden.value().compositor_minimap);

  // Bit 1 is the night dim, which shares the word.
  state.night_dim = true;
  const auto night = editor::decode_workspace_layout(
      editor::encode_workspace_layout(state).value());
  LAYOUT_CHECK(night);
  LAYOUT_CHECK(night.value().night_dim);
  LAYOUT_CHECK(!night.value().compositor_minimap);

  // Bit 2 puts the History strip away.
  state.history_hidden = true;
  const auto put_away = editor::decode_workspace_layout(
      editor::encode_workspace_layout(state).value());
  LAYOUT_CHECK(put_away);
  LAYOUT_CHECK(put_away.value().history_hidden);
  LAYOUT_CHECK(put_away.value().night_dim);
  LAYOUT_CHECK(!night.value().history_hidden);

  // Bits 3 and 4 are the Stage and Program media renditions, each on its
  // own; clear bits decode proxies.
  LAYOUT_CHECK(night.value().stage_media == editor::MediaRendition::Proxy);
  LAYOUT_CHECK(night.value().program_media == editor::MediaRendition::Proxy);
  state.program_media = editor::MediaRendition::Full;
  const auto program_full = editor::decode_workspace_layout(
      editor::encode_workspace_layout(state).value());
  LAYOUT_CHECK(program_full);
  LAYOUT_CHECK(program_full.value().program_media ==
               editor::MediaRendition::Full);
  LAYOUT_CHECK(program_full.value().stage_media ==
               editor::MediaRendition::Proxy);
  LAYOUT_CHECK(program_full.value().history_hidden);
  state.program_media = editor::MediaRendition::Proxy;
  state.stage_media = editor::MediaRendition::Full;
  const auto stage_full = editor::decode_workspace_layout(
      editor::encode_workspace_layout(state).value());
  LAYOUT_CHECK(stage_full);
  LAYOUT_CHECK(stage_full.value().stage_media == editor::MediaRendition::Full);
  LAYOUT_CHECK(stage_full.value().program_media ==
               editor::MediaRendition::Proxy);

  auto unknown = encoded;
  unknown[44U] = std::byte{0x21U};
  LAYOUT_CHECK(!editor::decode_workspace_layout(unknown));
}

void corrupt_preview_settings_are_rejected() {
  editor::WorkspaceLayoutState state{sample_layout().snapshot(), 1400U, 900U};
  const auto encoded = editor::encode_workspace_layout(state).value();
  for (const std::size_t offset : {36U, 37U, 39U}) {
    auto corrupt = encoded;
    corrupt[offset] = std::byte{255U};
    LAYOUT_CHECK(!editor::decode_workspace_layout(corrupt));
  }
  // The zoom word: zero and anything outside the offered range are refused.
  auto no_zoom = encoded;
  no_zoom[38U] = std::byte{0U};
  no_zoom[39U] = std::byte{0U};
  LAYOUT_CHECK(!editor::decode_workspace_layout(no_zoom));
  auto tiny_zoom = encoded;
  tiny_zoom[38U] = std::byte{10U};
  tiny_zoom[39U] = std::byte{0U};
  LAYOUT_CHECK(!editor::decode_workspace_layout(tiny_zoom));
  state.ui_zoom_percent = 20U;
  LAYOUT_CHECK(!editor::encode_workspace_layout(state));
  state.ui_zoom_percent = 100U;
  state.stage_resolution = static_cast<editor::PreviewResolution>(3U);
  LAYOUT_CHECK(!editor::encode_workspace_layout(state));
  state.stage_resolution = editor::PreviewResolution::Full;
  state.program_resolution = static_cast<editor::PreviewResolution>(255U);
  LAYOUT_CHECK(!editor::encode_workspace_layout(state));
}

void preview_extents_scale_both_axes_and_remain_nonzero() {
  using Resolution = editor::PreviewResolution;
  using Extent = editor::PreviewExtent;
  LAYOUT_CHECK(editor::preview_extent(1920U, 1080U, Resolution::Full) == Extent{1920U, 1080U});
  LAYOUT_CHECK(editor::preview_extent(1920U, 1080U, Resolution::Half) == Extent{960U, 540U});
  LAYOUT_CHECK(editor::preview_extent(1920U, 1080U, Resolution::Quarter) == Extent{480U, 270U});
  LAYOUT_CHECK(editor::preview_extent(1001U, 563U, Resolution::Half) == Extent{500U, 281U});
  LAYOUT_CHECK(editor::preview_extent(0U, 1U, Resolution::Quarter) == Extent{1U, 1U});
  constexpr auto maximum = std::numeric_limits<mediaserver::core::u32>::max();
  LAYOUT_CHECK(editor::preview_extent(maximum, maximum, Resolution::Half) == Extent{maximum / 2U, maximum / 2U});
}

void oversized_records_are_bounded_before_allocation() {
  std::vector<std::byte> oversized(editor::max_workspace_layout_bytes + 1U);
  const auto decoded = editor::decode_workspace_layout(oversized);
  LAYOUT_CHECK(!decoded);
  LAYOUT_CHECK(decoded.error() ==
               editor::WorkspaceLayoutError::SizeLimitExceeded);
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 8U> cases{{
    {"Compositor minimap round-trips", &compositor_minimap_round_trips},
    {"Compositor parameter width round-trips", &compositor_parameter_width_round_trips},
    {"corrupt preview settings are rejected", &corrupt_preview_settings_are_rejected},
    {"interface zoom round trips", &interface_zoom_round_trips},
    {"preview extents scale both axes and remain nonzero", &preview_extents_scale_both_axes_and_remain_nonzero},
    {"a layout codec round trip is stable",
     &a_layout_codec_round_trip_is_stable},
    {"truncation, trailing bytes, and other versions are rejected",
     &truncation_trailing_bytes_and_other_versions_are_rejected},
    {"oversized records are bounded before allocation",
     &oversized_records_are_bounded_before_allocation},
}};

} // namespace

int main() {
  std::size_t failures{};
  for (const Case &test : cases) {
    try {
      test.function();
      std::cout << "[pass] " << test.name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[fail] " << test.name << ": " << error.what() << '\n';
    }
  }
  std::cout << (cases.size() - failures) << '/' << cases.size()
            << " tests passed\n";
  return failures == 0U ? 0 : 1;
}
