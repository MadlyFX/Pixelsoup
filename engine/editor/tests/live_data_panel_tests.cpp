#include <mediaserver/editor/live_data_panel.hpp>

#include <array>
#include <cstddef>
#include <exception>
#include <span>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

namespace core = mediaserver::core;
namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;
namespace doc = mediaserver::doc;
namespace scene = mediaserver::scene;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

constexpr float title_height_for_test = 25.0F;

#define LIVE_CHECK(...)                                                       \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

struct Harness final {
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::LiveDataPanel panel{gui::ui_id("live-data.test")};
  static constexpr gui::Rect bounds{0.0F, 0.0F, 360.0F, 620.0F};

  [[nodiscard]] std::size_t draw(const editor::LiveDataState &state) {
    input.new_frame();
    ctx.new_frame();
    panel.draw(ctx, bounds, state);
    return ctx.draw_list().vertices().size();
  }

  // Wheel and pointer applied *after* new_frame, which clears the per-frame
  // accumulators - setting them before it would leave the panel reading zero.
  void draw_with_wheel(const editor::LiveDataState &state, float wheel,
                       float x, float y) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.add_wheel(wheel);
    ctx.new_frame();
    panel.draw(ctx, bounds, state);
  }

  void spin(const editor::LiveDataState &state, float wheel, int times) {
    for (int turn = 0; turn < times; ++turn) {
      draw_with_wheel(state, wheel, bounds.width * 0.5F, bounds.height * 0.5F);
    }
  }
};

[[nodiscard]] editor::TrackedCameraObservation receiving_camera(
    core::u64 node = 7U) {
  editor::TrackedCameraObservation camera{};
  camera.camera = doc::NodeId{node};
  camera.name = "CAMERA A";
  camera.icvfx.inner_frustum = true;
  camera.tracking.state = editor::TrackingLinkState::Receiving;
  camera.tracking.source.enabled = true;
  camera.tracking.source.protocol = scene::TrackerProtocolKind::FreeD;
  camera.tracking.source.port = 40'000U;
  camera.tracking.datagrams = 1'200U;
  camera.tracking.accepted = 1'198U;
  camera.tracking.has_pose = true;
  camera.tracking.position = scene::Vec3{1.5F, 1.6F, -3.0F};
  camera.tracking.yaw = 0.125F;
  camera.tracking.field_of_view = 0.06F;
  camera.tracking.poses_sent = 900U;
  camera.inner_rect = scene::CanvasRect{1'200U, 600U, 900U, 500U};
  return camera;
}

[[nodiscard]] editor::LiveDataState receiving_state() {
  editor::LiveDataState state{};
  state.canvas = scene::Canvas{3'840U, 2'160U};
  state.show_frame = 900U;
  state.icvfx.inner_frustum = true;
  state.cameras.push_back(receiving_camera());
  return state;
}

void every_link_state_has_a_name() {
  // The badge is the one thing an operator reads from across a room, so every
  // state has to say something rather than falling through to a blank.
  constexpr std::array<editor::TrackingLinkState, 4U> states{{
      editor::TrackingLinkState::Idle,
      editor::TrackingLinkState::BindFailed,
      editor::TrackingLinkState::Waiting,
      editor::TrackingLinkState::Receiving,
  }};
  for (const editor::TrackingLinkState state : states) {
    LIVE_CHECK(!editor::tracking_link_state_name(state).empty());
  }
  LIVE_CHECK(editor::tracking_link_state_name(
                 editor::TrackingLinkState::Receiving) == "RECEIVING");
  LIVE_CHECK(editor::tracking_link_state_name(
                 editor::TrackingLinkState::BindFailed) == "BIND FAILED");
}

void the_panel_draws_in_every_state_it_can_be_in() {
  // The test font atlas has no glyphs, so text contributes nothing to the
  // draw list and a vertex count proves very little. What this asserts is the
  // thing that would actually break a control room: the panel produces a frame
  // for every state rather than faulting on one of them. The content decisions
  // are pure functions, asserted directly below.
  Harness harness;
  LIVE_CHECK(harness.draw(editor::LiveDataState{}) > 0U);

  editor::LiveDataState failed{};
  failed.cameras.push_back(receiving_camera());
  failed.cameras.front().tracking.state =
      editor::TrackingLinkState::BindFailed;
  failed.cameras.front().tracking.last_error = "address already in use";
  LIVE_CHECK(harness.draw(failed) > 0U);

  editor::LiveDataState waiting = receiving_state();
  waiting.cameras.front().tracking.state = editor::TrackingLinkState::Waiting;
  waiting.cameras.front().tracking.has_pose = false;
  waiting.cameras.front().inner_rect.reset();
  LIVE_CHECK(harness.draw(waiting) > 0U);

  LIVE_CHECK(harness.draw(receiving_state()) > 0U);

  // A NatNet feed shows its group, command port and rigid body, and the
  // server's name once it has answered - and draws before it has.
  editor::LiveDataState natnet = receiving_state();
  natnet.cameras.front().tracking.source.protocol =
      scene::TrackerProtocolKind::NatNet;
  natnet.cameras.front().tracking.source.port = 1'511U;
  natnet.cameras.front().tracking.awaiting_handshake = 12U;
  LIVE_CHECK(harness.draw(natnet) > 0U);
  natnet.cameras.front().tracking.server_description =
      "Motive 3.1.0, NatNet 4.1";
  natnet.cameras.front().tracking.source.camera_id = 5U;
  LIVE_CHECK(harness.draw(natnet) > 0U);

  // A stage with several cameras draws all of them, which is the case a panel
  // built around one would silently drop.
  editor::LiveDataState several = receiving_state();
  several.cameras.push_back(receiving_camera(9U));
  several.cameras.back().name = "CAMERA B";
  several.cameras.back().icvfx.render_order = 4;
  LIVE_CHECK(harness.draw(several) > 0U);
}

// The three ways an authored inner frustum can fail need three different
// fixes, so the panel must never merge them into one "not working".
void the_inner_frustum_report_distinguishes_every_reason() {
  const auto report = [](const editor::LiveDataState &state) {
    return editor::inner_frustum_report(state, state.cameras.front());
  };

  // Either switch being off means no plate, and the fix is the same act.
  editor::LiveDataState show_off = receiving_state();
  show_off.icvfx.inner_frustum = false;
  LIVE_CHECK(report(show_off) == editor::InnerFrustumReport::Off);
  // Off wins even when a pose is arriving: the operator switched it off, and
  // saying anything else would send them hunting a fault that is not there.
  LIVE_CHECK(show_off.cameras.front().tracking.has_pose);

  editor::LiveDataState camera_off = receiving_state();
  camera_off.cameras.front().icvfx.inner_frustum = false;
  LIVE_CHECK(report(camera_off) == editor::InnerFrustumReport::Off);

  editor::LiveDataState waiting = receiving_state();
  waiting.cameras.front().tracking.has_pose = false;
  waiting.cameras.front().inner_rect.reset();
  LIVE_CHECK(report(waiting) == editor::InnerFrustumReport::WaitingForPose);

  editor::LiveDataState off_canvas = receiving_state();
  off_canvas.cameras.front().inner_rect.reset();
  LIVE_CHECK(report(off_canvas) == editor::InnerFrustumReport::OffCanvas);

  // A curved or UV-mesh wall the Director does not hold: the camera may well
  // be drawing a plate, so it is not reported as seeing nothing.
  editor::LiveDataState unknown = receiving_state();
  unknown.cameras.front().inner_rect.reset();
  unknown.cameras.front().inner_rect_unknown =
      "the Director is still loading this wall's UV mesh";
  LIVE_CHECK(report(unknown) == editor::InnerFrustumReport::Unknown);
  // No pose still outranks it: there is nothing to place without one.
  unknown.cameras.front().tracking.has_pose = false;
  LIVE_CHECK(report(unknown) == editor::InnerFrustumReport::WaitingForPose);

  LIVE_CHECK(report(receiving_state()) ==
             editor::InnerFrustumReport::Drawing);

  // One camera failing says nothing about the other. A stage runs several and
  // they drop out independently, which is the whole reason each has a row.
  editor::LiveDataState mixed = receiving_state();
  mixed.cameras.push_back(receiving_camera(9U));
  mixed.cameras.back().inner_rect.reset();
  mixed.cameras.back().tracking.has_pose = false;
  LIVE_CHECK(editor::inner_frustum_report(mixed, mixed.cameras.front()) ==
             editor::InnerFrustumReport::Drawing);
  LIVE_CHECK(editor::inner_frustum_report(mixed, mixed.cameras.back()) ==
             editor::InnerFrustumReport::WaitingForPose);
}

// Only the counters that actually fired are listed. A column of zeroes is
// noise an operator has to read past to find the one number that is not zero.
void only_nonzero_refusals_are_listed() {
  std::array<editor::RefusalRow, editor::max_refusal_rows> rows{};
  const auto view =
      std::span<editor::RefusalRow, editor::max_refusal_rows>{rows};

  editor::TrackingRuntimeObservation clean{};
  LIVE_CHECK(editor::nonzero_refusals(clean, view) == 0U);

  editor::TrackingRuntimeObservation some{};
  some.checksum_failures = 4U;
  some.foreign_camera = 11U;
  LIVE_CHECK(editor::nonzero_refusals(some, view) == 2U);
  LIVE_CHECK(rows[0] == editor::RefusalRow{"Checksum", 4U});
  LIVE_CHECK(rows[1] == editor::RefusalRow{"Other camera", 11U});

  // The NatNet counters have rows of their own: a frame held for want of
  // the server's version and a body Motive listed but could not solve are
  // fixed at the server, not at the cable.
  editor::TrackingRuntimeObservation natnet{};
  natnet.awaiting_handshake = 3U;
  natnet.untracked = 9U;
  LIVE_CHECK(editor::nonzero_refusals(natnet, view) == 2U);
  LIVE_CHECK(rows[0] == editor::RefusalRow{"No handshake", 3U});
  LIVE_CHECK(rows[1] == editor::RefusalRow{"Not tracked", 9U});

  editor::TrackingRuntimeObservation all{};
  all.truncated = 1U;
  all.unrecognized = 2U;
  all.checksum_failures = 3U;
  all.invalid_values = 4U;
  all.foreign_camera = 5U;
  all.awaiting_handshake = 6U;
  all.untracked = 7U;
  LIVE_CHECK(editor::nonzero_refusals(all, view) ==
             editor::max_refusal_rows);
  for (std::size_t index = 0U; index < editor::max_refusal_rows; ++index) {
    LIVE_CHECK(!rows[index].name.empty());
    LIVE_CHECK(rows[index].count == index + 1U);
  }
}

// A panel squeezed to nothing is an ordinary docking outcome, not a fault.
void a_degenerate_panel_draws_without_faulting() {
  Harness harness;
  gui::InputState input;
  gui::UiDrawList list;
  gui::FontAtlas font{gui::FontAtlas::empty()};
  gui::UiContext ctx{input, list, font};
  editor::LiveDataPanel panel{gui::ui_id("live-data.degenerate")};
  input.new_frame();
  ctx.new_frame();
  panel.draw(ctx, gui::Rect{0.0F, 0.0F, 0.0F, 0.0F}, receiving_state());
  panel.draw(ctx, gui::Rect{10.0F, 10.0F, 40.0F, 12.0F}, receiving_state());
}

struct Case final {
  const char *name;
  void (*function)();
};

// A stage runs several cameras and every one of them has diagnostics. The
// panel used to walk y downward under a clip and simply lose whatever fell past
// the bottom: one fully-populated camera is a little over 400 px against
// roughly 630 px of body, so the second was cut mid-way and a third was never
// drawn, with no way for an operator to reach it.
void the_panel_scrolls_when_its_content_outgrows_it() {
  Harness harness;

  // One camera fits, so there is nothing to scroll and the wheel must not
  // invent a scroll position for a panel with no hidden content.
  editor::LiveDataState small{};
  small.cameras.push_back(receiving_camera(1U));
  static_cast<void>(harness.draw(small));
  const float one_camera = harness.panel.content_height();
  LIVE_CHECK(one_camera > 0.0F);
  LIVE_CHECK(one_camera < Harness::bounds.height);
  harness.spin(small, -5.0F, 3);
  LIVE_CHECK(harness.panel.scroll_offset() == 0.0F);

  // Four cameras do not, and now the wheel reaches the ones past the bottom.
  editor::LiveDataState many{};
  for (core::u64 node = 1U; node <= 4U; ++node) {
    many.cameras.push_back(receiving_camera(node));
  }
  static_cast<void>(harness.draw(many));
  const float four_cameras = harness.panel.content_height();
  LIVE_CHECK(four_cameras > one_camera);
  LIVE_CHECK(four_cameras > Harness::bounds.height);

  harness.spin(many, -3.0F, 1);
  const float scrolled = harness.panel.scroll_offset();
  LIVE_CHECK(scrolled > 0.0F);

  // Scrolling past the end stops at the end rather than running the content
  // off the top.
  harness.spin(many, -10.0F, 50);
  const float body_height = Harness::bounds.height - title_height_for_test;
  const float maximum = harness.panel.content_height() - body_height;
  LIVE_CHECK(harness.panel.scroll_offset() > scrolled);
  LIVE_CHECK(harness.panel.scroll_offset() <= maximum + 1.0F);

  // And back to the top, which is where a panel sits once an operator has
  // scrolled up as far as it goes.
  harness.spin(many, 10.0F, 50);
  LIVE_CHECK(harness.panel.scroll_offset() == 0.0F);

  // A pointer outside the panel does not scroll it, so a wheel meant for
  // whatever is under the cursor does not move this one behind it.
  harness.spin(many, -10.0F, 1);
  const float before_outside = harness.panel.scroll_offset();
  harness.draw_with_wheel(many, -10.0F, Harness::bounds.width + 200.0F,
                          Harness::bounds.height + 200.0F);
  LIVE_CHECK(harness.panel.scroll_offset() == before_outside);
}

// A clamped plate is not a plate that happens to fill the wall.
//
// It means the camera is looking at the wall but its frustum is not bounded by
// it, so the conservative answer covers everything - correct, and expensive
// enough that an operator should know. The panel used to print only a coverage
// percentage, and "100.0 %" reads as a camera framing the wall exactly rather
// than one overshooting it, which are opposite situations.
void a_clamped_plate_is_reported_as_clamped() {
  Harness harness;
  editor::LiveDataState fitted = receiving_state();
  fitted.canvas = scene::Canvas{1'920U, 1'080U};
  fitted.icvfx.inner_frustum = true;
  fitted.cameras.front().inner_rect =
      scene::CanvasRect{0U, 0U, 1'920U, 1'080U};
  fitted.cameras.front().inner_rect_clamped = false;

  editor::LiveDataState clamped = fitted;
  clamped.cameras.front().inner_rect_clamped = true;

  // Same rectangle, same coverage, opposite meaning. Reading the percentage
  // alone cannot tell them apart, which is exactly why the flag has to survive
  // to the panel instead of being computed and dropped.
  const auto fitted_coverage =
      editor::plate_coverage(fitted, fitted.cameras.front());
  const auto clamped_coverage =
      editor::plate_coverage(clamped, clamped.cameras.front());
  LIVE_CHECK(fitted_coverage.drawing && clamped_coverage.drawing);
  LIVE_CHECK(fitted_coverage.percent == clamped_coverage.percent);
  LIVE_CHECK(!fitted_coverage.clamped);
  LIVE_CHECK(clamped_coverage.clamped);

  // A plate covering part of the wall reports that part, and is not clamped.
  editor::LiveDataState partial = fitted;
  partial.cameras.front().inner_rect =
      scene::CanvasRect{0U, 0U, 960U, 540U};
  const auto partial_coverage =
      editor::plate_coverage(partial, partial.cameras.front());
  LIVE_CHECK(!partial_coverage.clamped);
  LIVE_CHECK(partial_coverage.percent > 24.0F);
  LIVE_CHECK(partial_coverage.percent < 26.0F);

  // No plate, no coverage - and no division by a zero canvas either.
  editor::LiveDataState none = fitted;
  none.cameras.front().inner_rect.reset();
  LIVE_CHECK(!editor::plate_coverage(none, none.cameras.front()).drawing);
  editor::LiveDataState degenerate = fitted;
  degenerate.canvas = scene::Canvas{0U, 0U};
  LIVE_CHECK(editor::plate_coverage(degenerate, degenerate.cameras.front())
                 .percent == 0.0F);

  // And every one of those still draws.
  LIVE_CHECK(harness.draw(fitted) > 0U);
  LIVE_CHECK(harness.draw(clamped) > 0U);
  LIVE_CHECK(harness.draw(partial) > 0U);
  LIVE_CHECK(harness.draw(none) > 0U);
  LIVE_CHECK(harness.draw(degenerate) > 0U);
}

// A node reports six distinguishable situations and they are not orderings of
// one number: silent, receiving nothing, planning nothing, planning and never
// intersecting, drawing, and drawing behind. From the Director all six look the
// same - a wall with no plate on it - so the panel has to tell them apart.
void the_node_report_distinguishes_every_reason() {
  editor::NodeTrackingObservation node{};
  LIVE_CHECK(editor::node_tracking_report(node) ==
             editor::NodeTrackingReport::Silent);

  node.reported = true;
  LIVE_CHECK(editor::node_tracking_report(node) ==
             editor::NodeTrackingReport::NoPose);

  // A pose arrived, but nothing on this node's outputs ever planned a plate.
  node.pose_sequence = 12U;
  LIVE_CHECK(editor::node_tracking_report(node) ==
             editor::NodeTrackingReport::NotPlanned);

  // Planned, and this node's slice never intersected it. Correct, and on a
  // wide wall it is the usual answer - so it must not read as a fault.
  node.plates_skipped = 400U;
  LIVE_CHECK(editor::node_tracking_report(node) ==
             editor::NodeTrackingReport::NoSlice);

  node.plates_drawn = 90U;
  LIVE_CHECK(editor::node_tracking_report(node) ==
             editor::NodeTrackingReport::Drawing);

  // Drawing is not enough on its own: a node drawing from poses it partly
  // missed is behind its neighbours, which is a seam an operator can see.
  node.poses_missed = 2U;
  LIVE_CHECK(editor::node_tracking_report(node) ==
             editor::NodeTrackingReport::DrawingBehind);

  // A pose this node dropped itself is a different fault from one that never
  // arrived, and it points at a different machine. Both at once reports the
  // link, because a node cannot drain what it never received.
  node.poses_coalesced = 5U;
  LIVE_CHECK(editor::node_tracking_report(node) ==
             editor::NodeTrackingReport::DrawingBehind);
  node.poses_missed = 0U;
  LIVE_CHECK(editor::node_tracking_report(node) ==
             editor::NodeTrackingReport::DrawingSlow);
  node.poses_coalesced = 0U;
  node.poses_missed = 2U;

  // A missed pose on a node that draws no plate is not worth interrupting
  // anyone about - there is nothing on this node's wall to be behind.
  node.plates_drawn = 0U;
  LIVE_CHECK(editor::node_tracking_report(node) ==
             editor::NodeTrackingReport::NoSlice);
}

// The node rows are drawn, and an unreported node still gets one. A node that
// has never answered is exactly the state that must not vanish from the panel.
void node_rows_draw_including_unreported_ones() {
  Harness harness{};
  editor::LiveDataState state = receiving_state();
  editor::NodeTrackingObservation drawing{};
  drawing.node_index = 0U;
  drawing.name = "wall-left";
  drawing.reported = true;
  drawing.pose_sequence = 900U;
  drawing.poses_coalesced = 3U;
  drawing.poses_applied = 880U;
  drawing.held_frames = 20U;
  drawing.plates_drawn = 400U;
  drawing.plates_skipped = 480U;
  drawing.plate_refusals = 1U;
  editor::NodeTrackingObservation silent{};
  silent.node_index = 1U;
  silent.name = "wall-right";
  state.nodes = {drawing, silent};

  const std::size_t with_nodes = harness.draw(state);
  LIVE_CHECK(with_nodes > 0U);
  const float tall = harness.panel.content_height();

  state.nodes.clear();
  LIVE_CHECK(harness.draw(state) > 0U);
  // The rows are really being drawn rather than the state being ignored.
  LIVE_CHECK(harness.panel.content_height() < tall);
}

void prediction_diagnostics_cover_states_errors_and_scroll() {
  using State = scene::PosePredictionState;
  for (const auto status : {State::Waiting, State::Off, State::Warming,
       State::Predicting, State::Filtering, State::Held, State::Repeated, State::Refused})
    LIVE_CHECK(!editor::pose_prediction_state_name(status).empty());
  LIVE_CHECK(editor::pose_prediction_state_name(State::Held) == "Held: no fresh pose");
  LIVE_CHECK(editor::pose_prediction_state_name(State::Repeated) == "Held: repeated stamp");
  Harness harness;
  auto state = receiving_state();
  static_cast<void>(harness.draw(state));
  const float initial = harness.panel.content_height();
  state.icvfx.pose_prediction = true;
  auto& prediction = state.cameras.front().tracking.prediction;
  prediction.state = State::Predicting;
  prediction.has_sample = true;
  prediction.raw_position = {1.0F, 2.0F, 3.0F};
  prediction.output_position = {1.5F, 2.25F, 3.125F};
  prediction.raw_angles = {0.125F, 0.25F, 0.375F};
  prediction.output_angles = {0.25F, 0.375F, 0.5F};
  prediction.sampled_frame = 150;
  prediction.target_frame = 153;
  prediction.fit_samples = 8;
  prediction.next_error = {3, 2, 149, 0.001, 0.0001};
  prediction.latency_error = {2, 1, 148, 0.003, 0.0002};
  prediction.resets = 4;
  prediction.refused = 5;
  prediction.coalesced = 6;
  static_cast<void>(harness.draw(state));
  LIVE_CHECK(harness.panel.content_height() > initial + 300.0F);
  harness.spin(state, -5.0F, 6);
  LIVE_CHECK(harness.panel.scroll_offset() > 0.0F);
}

constexpr std::array<Case, 10U> cases{{
    {"prediction diagnostics cover states errors and scroll", &prediction_diagnostics_cover_states_errors_and_scroll},
    {"every link state has a name", &every_link_state_has_a_name},
    {"the panel draws in every state it can be in",
     &the_panel_draws_in_every_state_it_can_be_in},
    {"the inner frustum report distinguishes every reason",
     &the_inner_frustum_report_distinguishes_every_reason},
    {"only nonzero refusals are listed", &only_nonzero_refusals_are_listed},
    {"a degenerate panel draws without faulting",
     &a_degenerate_panel_draws_without_faulting},
    {"the panel scrolls when its content outgrows it",
     &the_panel_scrolls_when_its_content_outgrows_it},
    {"a clamped plate is reported as clamped",
     &a_clamped_plate_is_reported_as_clamped},
    {"the node report distinguishes every reason",
     &the_node_report_distinguishes_every_reason},
    {"node rows draw including unreported ones",
     &node_rows_draw_including_unreported_ones},
}};

} // namespace

int main() {
  std::size_t failures = 0U;
  for (const auto &entry : cases) {
    try {
      entry.function();
      std::cout << "[pass] " << entry.name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
    }
  }
  std::cout << (cases.size() - failures) << '/' << cases.size()
            << " tests passed\n";
  return failures == 0U ? 0 : 1;
}
