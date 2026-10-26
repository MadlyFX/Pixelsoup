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

void truncation_trailing_bytes_and_newer_versions_are_rejected() {
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
}

void version_one_layouts_still_restore_without_a_window_size() {
  auto legacy = editor::encode_workspace_layout(
                    editor::WorkspaceLayoutState{sample_layout().snapshot(),
                                                 1400U, 900U})
                    .value();
  // V1 used the same magic and body but ended its header after the dock root.
  legacy[8U] = std::byte{1U};
  legacy[9U] = std::byte{0U};
  legacy[10U] = std::byte{28U};
  legacy[11U] = std::byte{0U};
  legacy.erase(legacy.begin() + 28, legacy.begin() + 44);

  const auto decoded = editor::decode_workspace_layout(legacy);
  LAYOUT_CHECK(decoded);
  LAYOUT_CHECK(decoded.value().window_width == 0U);
  LAYOUT_CHECK(decoded.value().window_height == 0U);
  LAYOUT_CHECK(decoded.value().stage_resolution == editor::PreviewResolution::Full);
  LAYOUT_CHECK(decoded.value().program_resolution == editor::PreviewResolution::Full);
  gui::DockLayout restored;
  restored.register_panel(viewport, "Viewport");
  restored.register_panel(inspector, "Inspector");
  restored.register_panel(timeline, "Timeline");
  LAYOUT_CHECK(restored.restore(decoded.value().docks));
}

void version_two_layouts_restore_full_resolution_previews() {
  auto legacy = editor::encode_workspace_layout(editor::WorkspaceLayoutState{
      sample_layout().snapshot(), 1400U, 900U}).value();
  legacy[8U] = std::byte{2U};
  legacy[10U] = std::byte{36U};
  legacy.erase(legacy.begin() + 36, legacy.begin() + 44);
  const auto decoded = editor::decode_workspace_layout(legacy);
  LAYOUT_CHECK(decoded);
  LAYOUT_CHECK(decoded.value().window_width == 1400U);
  LAYOUT_CHECK(decoded.value().window_height == 900U);
  LAYOUT_CHECK(decoded.value().stage_resolution == editor::PreviewResolution::Full);
  LAYOUT_CHECK(decoded.value().program_resolution == editor::PreviewResolution::Full);
  gui::DockLayout restored;
  restored.register_panel(viewport, "Viewport");
  restored.register_panel(inspector, "Inspector");
  restored.register_panel(timeline, "Timeline");
  LAYOUT_CHECK(restored.restore(decoded.value().docks));
}

void interface_zoom_round_trips_and_older_records_read_as_actual_size() {
  editor::WorkspaceLayoutState state{sample_layout().snapshot(), 1400U, 900U};
  state.ui_zoom_percent = 150U;
  const auto encoded = editor::encode_workspace_layout(state).value();
  const auto decoded = editor::decode_workspace_layout(encoded);
  LAYOUT_CHECK(decoded);
  LAYOUT_CHECK(decoded.value().ui_zoom_percent == 150U);

  // A version three record held a reserved zero word where the zoom now is,
  // and ended its header there.
  auto version_three = encoded;
  version_three[8U] = std::byte{3U};
  version_three[10U] = std::byte{40U};
  version_three.erase(version_three.begin() + 40, version_three.begin() + 44);
  version_three[38U] = std::byte{0U};
  version_three[39U] = std::byte{0U};
  const auto older = editor::decode_workspace_layout(version_three);
  LAYOUT_CHECK(older);
  LAYOUT_CHECK(older.value().ui_zoom_percent == 100U);
  version_three[38U] = std::byte{150U};
  LAYOUT_CHECK(!editor::decode_workspace_layout(version_three));
}

// The operator's Parameters width survives a restart; a record from before
// the field opens the panel at its default, and nonsense is refused.
void compositor_parameter_width_round_trips() {
  editor::WorkspaceLayoutState state{sample_layout().snapshot(), 1400U, 900U};
  state.compositor_parameter_width = 312.5F;
  const auto encoded = editor::encode_workspace_layout(state).value();
  const auto decoded = editor::decode_workspace_layout(encoded);
  LAYOUT_CHECK(decoded);
  LAYOUT_CHECK(decoded.value().compositor_parameter_width == 312.5F);

  auto version_four = encoded;
  version_four[8U] = std::byte{4U};
  version_four[10U] = std::byte{40U};
  version_four.erase(version_four.begin() + 40, version_four.begin() + 44);
  const auto older = editor::decode_workspace_layout(version_four);
  LAYOUT_CHECK(older);
  LAYOUT_CHECK(older.value().compositor_parameter_width == 0.0F);
  LAYOUT_CHECK(older.value().ui_zoom_percent == 100U);

  // NaN's bit pattern, then a negative width, in the header's last word.
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

constexpr std::array<Case, 9U> cases{{
    {"version two layouts restore full resolution previews", &version_two_layouts_restore_full_resolution_previews},
    {"Compositor parameter width round-trips", &compositor_parameter_width_round_trips},
    {"corrupt preview settings are rejected", &corrupt_preview_settings_are_rejected},
    {"interface zoom round trips; older records read as actual size",
     &interface_zoom_round_trips_and_older_records_read_as_actual_size},
    {"preview extents scale both axes and remain nonzero", &preview_extents_scale_both_axes_and_remain_nonzero},
    {"a layout codec round trip is stable",
     &a_layout_codec_round_trip_is_stable},
    {"truncation, trailing bytes, and newer versions are rejected",
     &truncation_trailing_bytes_and_newer_versions_are_rejected},
    {"version one layouts restore without a window size",
     &version_one_layouts_still_restore_without_a_window_size},
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
