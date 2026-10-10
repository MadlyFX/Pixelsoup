#include "timeline_marker_authoring.hpp"

#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace app = mediaserver::app;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace scene = mediaserver::scene;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line)
                                 + ": check failed: " + expression};
    }
}

#define MARKER_CHECK(...)                                                      \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] doc::Node stage(core::u64 id) {
    return doc::Node{doc::NodeId{id}, std::string{scene::schema::stage_type}, {}};
}

[[nodiscard]] doc::Root two_scenes() {
    doc::Root root;
    root = root.set(stage(1U));
    root = root.set(stage(2U));
    return root;
}

[[nodiscard]] std::vector<scene::TimelineMarker> markers_of(
    const doc::Root& root, core::u64 owner) {
    scene::SceneModel model;
    model.reset(root);
    MARKER_CHECK(model.counters().rejected == 0U);
    return model.markers(scene::SceneId{owner});
}

void create_allocates_past_occupied_ids_and_round_trips_every_field() {
    doc::Root root = two_scenes();
    // The allocator hint lands on an occupied id and must step over it.
    root = root.set(doc::Node{doc::NodeId{10U}, "editor.note", {}});

    scene::TimelineMarker wanted{};
    wanted.frame = 125;
    wanted.name = "Doors";
    wanted.note = "Talent enters stage left";
    wanted.color = 5U;
    wanted.cue = scene::CueAction::Loop;
    wanted.loop_wait_ms = 1'500U;
    wanted.loop_fade_ms = 400U;
    const auto planned = app::plan_marker_authoring(
        root,
        app::MarkerAuthorRequest{app::MarkerAuthorAction::Create,
                                 scene::SceneId{1U}, wanted},
        10U);
    MARKER_CHECK(planned.has_value());
    MARKER_CHECK(planned.value().marker == doc::NodeId{11U});
    MARKER_CHECK(planned.value().next_node == 12U);

    const auto markers = markers_of(planned.value().document, 1U);
    MARKER_CHECK(markers.size() == 1U);
    wanted.id = doc::NodeId{11U};
    wanted.scene = scene::SceneId{1U};
    MARKER_CHECK(markers.front() == wanted);
    MARKER_CHECK(markers_of(planned.value().document, 2U).empty());
}

void update_replaces_the_whole_marker_and_delete_removes_it() {
    doc::Root root = two_scenes();
    scene::TimelineMarker marker{};
    marker.frame = 40;
    marker.name = "Intro";
    auto created = app::plan_marker_authoring(
        root,
        app::MarkerAuthorRequest{app::MarkerAuthorAction::Create,
                                 scene::SceneId{1U}, marker},
        100U);
    MARKER_CHECK(created.has_value());
    root = created.value().document;

    // A drag, a rename, and a turn into a loop cue are one request shape.
    marker.id = created.value().marker;
    marker.frame = 64;
    marker.name = "Vamp";
    marker.cue = scene::CueAction::Loop;
    auto updated = app::plan_marker_authoring(
        root,
        app::MarkerAuthorRequest{app::MarkerAuthorAction::Update,
                                 scene::SceneId{1U}, marker},
        200U);
    MARKER_CHECK(updated.has_value());
    MARKER_CHECK(updated.value().next_node == 200U);
    const auto markers = markers_of(updated.value().document, 1U);
    MARKER_CHECK(markers.size() == 1U);
    MARKER_CHECK(markers.front().frame == 64);
    MARKER_CHECK(markers.front().name == "Vamp");
    MARKER_CHECK(markers.front().cue == scene::CueAction::Loop);

    // An update that changes nothing authors nothing.
    auto unchanged = app::plan_marker_authoring(
        updated.value().document,
        app::MarkerAuthorRequest{app::MarkerAuthorAction::Update,
                                 scene::SceneId{1U}, marker},
        200U);
    MARKER_CHECK(unchanged.has_value());
    MARKER_CHECK(
        unchanged.value().document.same_snapshot(updated.value().document));

    auto deleted = app::plan_marker_authoring(
        updated.value().document,
        app::MarkerAuthorRequest{app::MarkerAuthorAction::Delete,
                                 scene::SceneId{1U}, marker},
        200U);
    MARKER_CHECK(deleted.has_value());
    MARKER_CHECK(markers_of(deleted.value().document, 1U).empty());
}

void stale_cross_scene_and_malformed_requests_author_nothing() {
    doc::Root root = two_scenes();
    scene::TimelineMarker marker{};
    marker.frame = 10;
    auto created = app::plan_marker_authoring(
        root,
        app::MarkerAuthorRequest{app::MarkerAuthorAction::Create,
                                 scene::SceneId{2U}, marker},
        50U);
    MARKER_CHECK(created.has_value());
    root = created.value().document;
    marker.id = created.value().marker;

    const auto error_of = [&](app::MarkerAuthorAction action,
                              core::u64 owner,
                              const scene::TimelineMarker& value) {
        const auto planned = app::plan_marker_authoring(
            root,
            app::MarkerAuthorRequest{action, scene::SceneId{owner}, value},
            60U);
        MARKER_CHECK(!planned.has_value());
        return planned.error();
    };

    // The Preview scene's cue cannot be moved through Program's timeline.
    MARKER_CHECK(error_of(app::MarkerAuthorAction::Update, 1U, marker)
                 == app::MarkerAuthorError::CrossSceneMarker);
    MARKER_CHECK(error_of(app::MarkerAuthorAction::Delete, 1U, marker)
                 == app::MarkerAuthorError::CrossSceneMarker);
    MARKER_CHECK(error_of(app::MarkerAuthorAction::Create, 9U, marker)
                 == app::MarkerAuthorError::InvalidScene);

    scene::TimelineMarker missing = marker;
    missing.id = doc::NodeId{999U};
    MARKER_CHECK(error_of(app::MarkerAuthorAction::Update, 2U, missing)
                 == app::MarkerAuthorError::MarkerNotFound);
    scene::TimelineMarker stage_node = marker;
    stage_node.id = doc::NodeId{1U};
    MARKER_CHECK(error_of(app::MarkerAuthorAction::Delete, 2U, stage_node)
                 == app::MarkerAuthorError::MarkerNotFound);

    scene::TimelineMarker bad = marker;
    bad.frame = -1;
    MARKER_CHECK(error_of(app::MarkerAuthorAction::Update, 2U, bad)
                 == app::MarkerAuthorError::InvalidFrame);
    bad = marker;
    bad.color = scene::timeline_marker_colors;
    MARKER_CHECK(error_of(app::MarkerAuthorAction::Create, 2U, bad)
                 == app::MarkerAuthorError::InvalidColor);
    bad = marker;
    bad.name.assign(scene::max_timeline_marker_name_bytes + 1U, 'x');
    MARKER_CHECK(error_of(app::MarkerAuthorAction::Create, 2U, bad)
                 == app::MarkerAuthorError::InvalidName);
    bad = marker;
    bad.note.assign(scene::max_timeline_marker_note_bytes + 1U, 'x');
    MARKER_CHECK(error_of(app::MarkerAuthorAction::Update, 2U, bad)
                 == app::MarkerAuthorError::InvalidNote);
    bad = marker;
    bad.loop_wait_ms = scene::max_loop_wait_ms + 1U;
    MARKER_CHECK(error_of(app::MarkerAuthorAction::Update, 2U, bad)
                 == app::MarkerAuthorError::InvalidLoopTiming);
    bad = marker;
    bad.loop_fade_ms = scene::max_loop_fade_ms + 1U;
    MARKER_CHECK(error_of(app::MarkerAuthorAction::Create, 2U, bad)
                 == app::MarkerAuthorError::InvalidLoopTiming);
}

void a_scene_holds_a_bounded_number_of_markers() {
    doc::Root root = two_scenes();
    core::u64 next = 100U;
    scene::TimelineMarker marker{};
    for (std::size_t index = 0U; index < app::max_scene_markers; ++index) {
        marker.frame = static_cast<std::int64_t>(index);
        auto created = app::plan_marker_authoring(
            root,
            app::MarkerAuthorRequest{app::MarkerAuthorAction::Create,
                                     scene::SceneId{1U}, marker},
            next);
        MARKER_CHECK(created.has_value());
        root = created.value().document;
        next = created.value().next_node;
    }
    auto refused = app::plan_marker_authoring(
        root,
        app::MarkerAuthorRequest{app::MarkerAuthorAction::Create,
                                 scene::SceneId{1U}, marker},
        next);
    MARKER_CHECK(!refused.has_value());
    MARKER_CHECK(refused.error() == app::MarkerAuthorError::MarkerLimit);
    // The bound is per scene.
    MARKER_CHECK(app::plan_marker_authoring(
                     root,
                     app::MarkerAuthorRequest{app::MarkerAuthorAction::Create,
                                              scene::SceneId{2U}, marker},
                     next)
                     .has_value());
}

struct Case final {
    std::string_view name;
    void (*function)();
};

constexpr std::array<Case, 4U> cases{{
    {"create allocates past occupied ids and round trips every field",
     &create_allocates_past_occupied_ids_and_round_trips_every_field},
    {"update replaces the whole marker and delete removes it",
     &update_replaces_the_whole_marker_and_delete_removes_it},
    {"stale, cross-scene, and malformed requests author nothing",
     &stale_cross_scene_and_malformed_requests_author_nothing},
    {"a scene holds a bounded number of markers",
     &a_scene_holds_a_bounded_number_of_markers},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const Case& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
