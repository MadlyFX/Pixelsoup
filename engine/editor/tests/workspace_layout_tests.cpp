#include <mediaserver/editor/workspace_layout.hpp>

#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
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
      editor::WorkspaceLayoutState{original.snapshot(), 1536U, 864U});
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
  LAYOUT_CHECK(restored.restore(decoded.value().docks));
  LAYOUT_CHECK(restored.is_open(viewport));
  LAYOUT_CHECK(restored.is_open(timeline));
  LAYOUT_CHECK(!restored.is_open(inspector));
  LAYOUT_CHECK(restored.active_of(restored.leaf_of(timeline)) == timeline);

  const auto reencoded = editor::encode_workspace_layout(
      editor::WorkspaceLayoutState{restored.snapshot(), 1536U, 864U});
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
  legacy.erase(legacy.begin() + 28, legacy.begin() + 36);

  const auto decoded = editor::decode_workspace_layout(legacy);
  LAYOUT_CHECK(decoded);
  LAYOUT_CHECK(decoded.value().window_width == 0U);
  LAYOUT_CHECK(decoded.value().window_height == 0U);
  gui::DockLayout restored;
  restored.register_panel(viewport, "Viewport");
  restored.register_panel(inspector, "Inspector");
  restored.register_panel(timeline, "Timeline");
  LAYOUT_CHECK(restored.restore(decoded.value().docks));
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

constexpr std::array<Case, 4U> cases{{
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
