#include <mediaserver/show/switcher_authority.hpp>
#include <mediaserver/show/switcher_state.hpp>

#include <array>
#include <string>
#include <utility>

#include "show_test_support.hpp"

namespace {

namespace cmd = mediaserver::cmd;
namespace doc = mediaserver::doc;
namespace show = mediaserver::show;

[[nodiscard]] cmd::FrameNumber control(mediaserver::core::u64 value) noexcept {
    return cmd::FrameNumber{value};
}

constexpr doc::NodeId scene_a{100U};
constexpr doc::NodeId scene_b{200U};
constexpr doc::NodeId scene_c{300U};

void the_switcher_document_round_trip_is_exact() {
    show::SwitcherAnchor anchor{};
    anchor.program_scene = scene_b;
    anchor.rehearsal_scene = scene_a;
    anchor.transition_from = scene_a;
    anchor.transition_to = scene_b;
    anchor.anchor_control_frame = control(42U);
    anchor.duration_control_frames = 30U;

    doc::Root root;
    root = root.set(show::make_switcher_node(anchor));
    const auto read = show::read_switcher(root);
    SHOW_CHECK(read.has_value());
    SHOW_CHECK(read.value() == anchor);
}

void a_dissolve_has_exact_control_frame_progress() {
    auto created =
        show::SwitcherAuthority::create(scene_a, scene_b, control(0U));
    SHOW_CHECK(created);
    auto switcher = std::move(created).value();
    const auto swapped = switcher.swap(control(10U), 20U);
    SHOW_CHECK(swapped);
    SHOW_CHECK(swapped.value().program_scene == scene_b);
    SHOW_CHECK(swapped.value().rehearsal_scene == scene_a);

    const auto start = switcher.progress_at(control(10U));
    SHOW_CHECK(start.active);
    SHOW_CHECK(start.from == scene_a);
    SHOW_CHECK(start.to == scene_b);
    SHOW_CHECK(start.elapsed == 0U);
    SHOW_CHECK(start.mix() == 0.0);

    const auto middle = switcher.progress_at(control(20U));
    SHOW_CHECK(middle.active);
    SHOW_CHECK(middle.elapsed == 10U);
    SHOW_CHECK(middle.mix() == 0.5);

    const auto end = switcher.progress_at(control(30U));
    SHOW_CHECK(!end.active);
    SHOW_CHECK(end.elapsed == 20U);
    SHOW_CHECK(end.mix() == 1.0);
}

void rehearsal_is_locked_while_live_pixels_still_use_it() {
    auto created =
        show::SwitcherAuthority::create(scene_a, scene_b, control(0U));
    SHOW_CHECK(created);
    auto switcher = std::move(created).value();
    SHOW_CHECK(switcher.swap(control(5U), 10U));
    SHOW_CHECK(switcher.rehearsal_locked(control(5U)));
    SHOW_CHECK(!switcher.set_rehearsal(scene_c, control(8U)));
    SHOW_CHECK(!switcher.swap(control(8U), 4U));

    SHOW_CHECK(!switcher.rehearsal_locked(control(15U)));
    const auto changed = switcher.set_rehearsal(scene_c, control(15U));
    SHOW_CHECK(changed);
    SHOW_CHECK(changed.value().rehearsal_scene == scene_c);
}

void a_cut_promotes_rehearsal_immediately() {
    auto created =
        show::SwitcherAuthority::create(scene_a, scene_b, control(0U));
    SHOW_CHECK(created);
    auto switcher = std::move(created).value();
    SHOW_CHECK(switcher.swap(control(4U), 0U));
    const auto cut = switcher.progress_at(control(4U));
    SHOW_CHECK(!cut.active);
    SHOW_CHECK(cut.mix() == 1.0);
    SHOW_CHECK(switcher.anchor().program_scene == scene_b);
}

void a_dissolve_requires_one_shared_canvas() {
    constexpr mediaserver::scene::Canvas hd{1'920U, 1'080U};
    constexpr mediaserver::scene::Canvas ultrawide{2'560U, 720U};
    SHOW_CHECK(show::dissolve_canvases_compatible(hd, hd));
    SHOW_CHECK(!show::dissolve_canvases_compatible(hd, ultrawide));
    SHOW_CHECK(!show::dissolve_canvases_compatible({}, {}));
}

void the_switcher_refuses_time_regression_and_aliasing() {
    SHOW_CHECK(!show::SwitcherAuthority::create(scene_a, scene_a, control(0U)));
    auto created =
        show::SwitcherAuthority::create(scene_a, scene_b, control(10U));
    SHOW_CHECK(created);
    auto switcher = std::move(created).value();
    const auto regressed = switcher.swap(control(9U), 5U);
    SHOW_CHECK(!regressed);
    SHOW_CHECK(regressed.error() ==
               show::SwitcherCommandError::ControlFrameRegression);
    SHOW_CHECK(!switcher.set_rehearsal(scene_a, control(10U)));
}

void malformed_switcher_nodes_are_rejected() {
    doc::Root root;
    doc::Node foreign{};
    foreign.id = show::switcher_node_id;
    foreign.type = "show.not-a-switcher";
    SHOW_CHECK(!show::read_switcher(root.set(foreign)));

    show::SwitcherAnchor anchor{};
    anchor.program_scene = scene_a;
    anchor.rehearsal_scene = scene_b;
    anchor.transition_from = scene_a;
    anchor.transition_to = scene_a;
    auto partial = show::make_switcher_node(anchor);
    partial.properties.erase(
        std::string{show::switcher_property::program_scene});
    SHOW_CHECK(!show::read_switcher(root.set(partial)));
}

constexpr std::array<show::test::Case, 7U> cases{{
    {"the switcher document round trip is exact",
     &the_switcher_document_round_trip_is_exact},
    {"a dissolve has exact control frame progress",
     &a_dissolve_has_exact_control_frame_progress},
    {"rehearsal is locked while live pixels still use it",
     &rehearsal_is_locked_while_live_pixels_still_use_it},
    {"a cut promotes rehearsal immediately",
     &a_cut_promotes_rehearsal_immediately},
    {"a dissolve requires one shared canvas",
     &a_dissolve_requires_one_shared_canvas},
    {"the switcher refuses time regression and aliasing",
     &the_switcher_refuses_time_regression_and_aliasing},
    {"malformed switcher nodes are rejected",
     &malformed_switcher_nodes_are_rejected},
}};

} // namespace

int main() {
    return show::test::run(cases);
}
