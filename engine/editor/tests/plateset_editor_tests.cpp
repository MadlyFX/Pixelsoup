#include <mediaserver/editor/plateset_editor.hpp>

#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>

#include <array>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace assets = mediaserver::assets;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;
using assets::PlatesetSlot;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define UI_CHECK(...)                                                          \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

// The page at 900 x 600: a 34-unit header, the readiness column 300 wide at
// the right, and the rig square centred in what is left, inset 12. That
// square is 542 on a side at (29, 46), three 175.3-unit cells with 8 between.
constexpr float rig_x = 29.0F;
constexpr float rig_y = 46.0F;
constexpr float cell = (542.0F - 16.0F) / 3.0F;
constexpr float cell_centre(int index, float origin) {
  return origin + static_cast<float>(index) * (cell + 8.0F) + cell * 0.5F;
}

struct Harness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::PlatesetEditor editor{gui::ui_id("plateset.test")};
  gui::Rect bounds{0.0F, 0.0F, 900.0F, 600.0F};
  assets::AssetRegistry registry;
  assets::PlatesetAsset plateset;

  editor::PlatesetEditorFrame frame(float x, float y, bool down) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, down);
    ctx.new_frame();
    editor::PlatesetEditorInput in{};
    in.plateset = &plateset;
    in.assets = &registry;
    editor::PlatesetEditorFrame result = editor.draw(ctx, bounds, in);
    ctx.draw_overlays(gui::Rect{0.0F, 0.0F, 1600.0F, 900.0F});
    return result;
  }
  editor::PlatesetEditorFrame click(float x, float y) {
    static_cast<void>(frame(x, y, false));
    static_cast<void>(frame(x, y, true));
    return frame(x, y, false);
  }
};

[[nodiscard]] Harness with_take(const std::array<const char *, 3U> &timecodes) {
  Harness harness;
  const std::array<std::pair<core::u64, PlatesetSlot>, 3U> plates{{
      {11U, PlatesetSlot::Front},
      {12U, PlatesetSlot::Left},
      {13U, PlatesetSlot::Right},
  }};
  harness.plateset.id = doc::NodeId{40U};
  harness.plateset.display_name = "Take";
  for (std::size_t index = 0U; index < plates.size(); ++index) {
    assets::MediaAsset media{};
    media.id = doc::NodeId{plates[index].first};
    media.display_name = "plate";
    media.source = "C:/plates/p" + std::to_string(index) + ".mp4";
    media.dimensions = assets::PixelDimensions{1'920U, 1'080U};
    media.frame_count = 1'000U;
    media.frame_rate = assets::MediaFrameRate{24U, 1U};
    media.start_timecode = timecodes[index];
    harness.registry.assets.emplace_back(std::move(media));
    harness.plateset.slots[static_cast<std::size_t>(plates[index].second)]
        .media = doc::NodeId{plates[index].first};
  }
  harness.registry.assets.emplace_back(harness.plateset);
  return harness;
}

void slots_sit_around_the_car_front_up() {
  Harness harness = with_take({"", "", ""});
  static_cast<void>(harness.frame(-1.0F, -1.0F, false));
  const auto at = [&](int column, int row) {
    return harness.editor.slot_at(cell_centre(column, rig_x),
                                  cell_centre(row, rig_y));
  };
  UI_CHECK(at(1, 0) == PlatesetSlot::Front);
  UI_CHECK(at(0, 0) == PlatesetSlot::FrontLeft);
  UI_CHECK(at(2, 0) == PlatesetSlot::FrontRight);
  UI_CHECK(at(0, 1) == PlatesetSlot::Left);
  UI_CHECK(at(2, 1) == PlatesetSlot::Right);
  UI_CHECK(at(1, 1) == PlatesetSlot::Ceiling);
  UI_CHECK(at(0, 2) == PlatesetSlot::BackLeft);
  UI_CHECK(at(1, 2) == PlatesetSlot::Back);
  UI_CHECK(at(2, 2) == PlatesetSlot::BackRight);
  UI_CHECK(harness.editor.rig_contains(rig_x + 2.0F, rig_y + 2.0F));
  UI_CHECK(!harness.editor.rig_contains(700.0F, 300.0F));
}

void the_rig_buttons_change_the_template() {
  Harness harness = with_take({"", "", ""});
  // With the empty test font each rig button is 18 wide, right-aligned in
  // the header from x = 892 with 4 between: 9 camera, 5 camera, 3 camera.
  const editor::PlatesetEditorFrame chosen = harness.click(839.0F, 17.0F);
  UI_CHECK(chosen.edits.size() == 1U);
  UI_CHECK(chosen.edits.front().kind ==
           assets::LibraryEdit::Kind::SetPlatesetRig);
  UI_CHECK(chosen.edits.front().rig == assets::PlatesetRig::Three);
  UI_CHECK(chosen.edits.front().target == doc::NodeId{40U});
  // The current template authors nothing.
  UI_CHECK(harness.click(883.0F, 17.0F).edits.empty());
}

void a_slot_is_emptied_from_its_title() {
  Harness harness = with_take({"", "", ""});
  // The Front tile's clear mark ends its title row, 5 in from the tile.
  const float right = rig_x + (cell + 8.0F) + cell - 5.0F;
  const editor::PlatesetEditorFrame cleared =
      harness.click(right - 8.0F, rig_y + 12.0F);
  UI_CHECK(cleared.edits.size() == 1U);
  UI_CHECK(cleared.edits.front().kind ==
           assets::LibraryEdit::Kind::SetPlatesetSlot);
  UI_CHECK(cleared.edits.front().slot == PlatesetSlot::Front);
  UI_CHECK(cleared.edits.front().media == doc::NodeId{});
}

// The Sync button sits somewhere down the readiness column, below rows that
// depend on what is wrong with the set; find it by pressing down the column.
[[nodiscard]] std::optional<editor::PlatesetEditorFrame>
press_sync(Harness &harness) {
  for (float y = 50.0F; y < 590.0F; y += 4.0F) {
    editor::PlatesetEditorFrame pressed = harness.click(640.0F, y);
    // The half resolution checkbox sits above the button; pass it by.
    const bool checkbox =
        !pressed.edits.empty() &&
        pressed.edits.front().kind ==
            assets::LibraryEdit::Kind::SetPlatesetHalfResolution;
    if (!checkbox &&
        (!pressed.edits.empty() || pressed.message.has_value())) {
      return pressed;
    }
  }
  return std::nullopt;
}

void sync_by_timecode_authors_the_offsets() {
  Harness harness =
      with_take({"10:00:00:12", "10:00:00:00", "10:00:01:02"});
  const std::optional<editor::PlatesetEditorFrame> synced =
      press_sync(harness);
  UI_CHECK(synced.has_value());
  UI_CHECK(synced->edits.size() == 1U);
  const assets::LibraryEdit &edit = synced->edits.front();
  UI_CHECK(edit.kind == assets::LibraryEdit::Kind::SetPlatesetOffsets);
  // The right plate rolled last; the front 14 frames before it, the left 26.
  UI_CHECK(edit.offsets[static_cast<std::size_t>(PlatesetSlot::Right)] == 0);
  UI_CHECK(edit.offsets[static_cast<std::size_t>(PlatesetSlot::Front)] == 14);
  UI_CHECK(edit.offsets[static_cast<std::size_t>(PlatesetSlot::Left)] == 26);
}

void sync_by_timecode_says_why_it_cannot() {
  Harness harness = with_take({"10:00:00:12", "", "10:00:01:02"});
  const std::optional<editor::PlatesetEditorFrame> refused =
      press_sync(harness);
  UI_CHECK(refused.has_value());
  UI_CHECK(refused->edits.empty());
  UI_CHECK(refused->message.has_value());
  UI_CHECK(refused->message->find("LEFT") != std::string::npos);
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 5U> cases{{
    {"slots sit around the car, front up", &slots_sit_around_the_car_front_up},
    {"the rig buttons change the template",
     &the_rig_buttons_change_the_template},
    {"a slot is emptied from its title", &a_slot_is_emptied_from_its_title},
    {"sync by timecode authors the offsets",
     &sync_by_timecode_authors_the_offsets},
    {"sync by timecode says why it cannot",
     &sync_by_timecode_says_why_it_cannot},
}};

} // namespace

int main() {
  std::size_t failures = 0U;
  for (const Case &entry : cases) {
    try {
      entry.function();
      std::cout << "[pass] " << entry.name << std::endl;
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
    }
  }
  std::cout << (cases.size() - failures) << '/' << cases.size()
            << " tests passed\n";
  return failures == 0U ? 0 : 1;
}
