#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/persist/project_file.hpp>
#include <mediaserver/scene/mask_authoring.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace core = mediaserver::core;
namespace cmd = mediaserver::cmd;
namespace doc = mediaserver::doc;
namespace persist = mediaserver::persist;
namespace scene = mediaserver::scene;
namespace schema = mediaserver::scene::schema;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define MASK_CHECK(...)                                                       \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

using Property = std::pair<std::string, doc::PropertyValue>;

[[nodiscard]] Property whole(std::string_view name, std::int64_t value) {
    return {std::string{name}, value};
}

[[nodiscard]] Property real(std::string_view name, double value) {
    return {std::string{name}, value};
}

[[nodiscard]] Property text(std::string_view name, std::string_view value) {
    return {std::string{name}, std::string{value}};
}

[[nodiscard]] Property boolean(std::string_view name, bool value) {
    return {std::string{name}, value};
}

[[nodiscard]] doc::Node node(
    core::u64 id, std::string_view type,
    std::initializer_list<Property> properties = {}) {
    doc::Node result{};
    result.id = doc::NodeId{id};
    result.type = std::string{type};
    for (const auto& [name, value] : properties) {
        result.properties.emplace(name, value);
    }
    return result;
}

[[nodiscard]] doc::Node stage_node(core::u64 id) {
    return node(id, schema::stage_type,
                {text(schema::scene_name, "Mask Stage")});
}

[[nodiscard]] doc::Node object_node(core::u64 id, core::u64 scene_id) {
    return node(id, schema::object_type,
                {whole(schema::scene_id,
                       static_cast<std::int64_t>(scene_id))});
}

[[nodiscard]] doc::Node group_node(
    core::u64 id, core::u64 scene_id,
    std::optional<core::u64> parent = std::nullopt,
    bool locked = false, bool visible = true) {
    doc::Node result = node(
        id, schema::group_type,
        {whole(schema::scene_id, static_cast<std::int64_t>(scene_id)),
         text(schema::group_name, "Masked Group"),
         boolean(schema::locked, locked), boolean(schema::visible, visible)});
    if (parent.has_value()) {
        result.properties.emplace(
            std::string{schema::parent_group},
            static_cast<std::int64_t>(*parent));
    }
    return result;
}

[[nodiscard]] doc::Node mask_node(
    core::u64 id, core::u64 scene_id, core::u64 target,
    std::string_view kind = schema::mask_rectangle_kind,
    std::string_view combine = schema::mask_add,
    std::int64_t order = 0, bool enabled = true) {
    return node(
        id, schema::mask_type,
        {whole(schema::scene_id, static_cast<std::int64_t>(scene_id)),
         whole(schema::mask_target, static_cast<std::int64_t>(target)),
         text(schema::mask_kind, kind), text(schema::mask_combine, combine),
         whole(schema::mask_order, order),
         boolean(schema::mask_enabled, enabled)});
}

[[nodiscard]] doc::Node point_node(
    core::u64 id, core::u64 scene_id, core::u64 mask,
    std::int64_t order, double x, double y) {
    return node(
        id, schema::mask_point_type,
        {whole(schema::scene_id, static_cast<std::int64_t>(scene_id)),
         whole(schema::mask_id, static_cast<std::int64_t>(mask)),
         whole(schema::mask_point_order, order), real(schema::mask_point_x, x),
         real(schema::mask_point_y, y)});
}

[[nodiscard]] doc::Root root_of(
    std::initializer_list<doc::Node> nodes) {
    doc::Root result;
    for (const doc::Node& value : nodes) {
        result = result.set(value);
    }
    return result;
}

template <typename Value>
[[nodiscard]] const Value& property_as(
    const doc::Node& held, std::string_view name) {
    const auto found = held.properties.find(name);
    MASK_CHECK(found != held.properties.end());
    const auto* const value = std::get_if<Value>(&found->second);
    MASK_CHECK(value != nullptr);
    return *value;
}

[[nodiscard]] bool equivalent_document(const doc::Root& left,
                                       const doc::Root& right) {
    return doc::diff(left, right).changes.empty();
}

[[nodiscard]] bool sorted_puts(
    const scene::MaskAuthoringPlan& plan) {
    return std::is_sorted(
        plan.puts.begin(), plan.puts.end(),
        [](const doc::Node& left, const doc::Node& right) noexcept {
            return left.id < right.id;
        });
}

[[nodiscard]] bool has_diagnostic(
    const std::vector<scene::MaskDiagnostic>& diagnostics,
    scene::MaskDiagnosticKind kind, doc::NodeId subject) {
    return std::any_of(
        diagnostics.begin(), diagnostics.end(),
        [kind, subject](const scene::MaskDiagnostic& value) noexcept {
            return value.kind == kind && value.subject == subject;
        });
}

[[nodiscard]] scene::Scene evaluated(const scene::SceneModel& model,
                                     core::u64 scene_id = 1U) {
    return model.evaluate(doc::NodeId{scene_id},
                          scene::SceneTime{17U, 60U, 1U});
}

[[nodiscard]] bool same_document(const doc::Root& left,
                                 const doc::Root& right) {
    return left.version() == right.version()
        && doc::diff(left, right).changes.empty();
}

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
            / ("mediaserver-scene-mask-" + std::to_string(stamp) + "-"
               + std::to_string(sequence.fetch_add(1U)));
        std::error_code error;
        const bool made = std::filesystem::create_directory(path_, error);
        if (!made || error) {
            throw std::runtime_error{"could not create mask test directory"};
        }
    }

    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TempDirectory(const TempDirectory&) = delete;
    TempDirectory& operator=(const TempDirectory&) = delete;

    [[nodiscard]] std::filesystem::path file() const {
        return path_ / "mask.msproject";
    }

private:
    std::filesystem::path path_;
};

void typed_shapes_and_finite_values_normalize_at_the_model_boundary() {
    doc::Node rectangle = mask_node(20U, 1U, 10U);
    rectangle.properties.insert_or_assign(
        std::string{schema::mask_name}, std::string{"Main Crop"});
    rectangle.properties.insert_or_assign(
        std::string{schema::mask_feather_px}, 50'000.0);
    rectangle.properties.insert_or_assign(
        std::string{schema::mask_expansion_px}, -50'000.0);
    rectangle.properties.insert_or_assign(
        std::string{schema::mask_opacity}, 1.5);
    rectangle.properties.insert_or_assign(
        std::string{schema::mask_invert}, true);
    rectangle.properties.insert_or_assign(
        std::string{schema::mask_left}, -0.5);
    rectangle.properties.insert_or_assign(
        std::string{schema::mask_top}, 0.125);
    rectangle.properties.insert_or_assign(
        std::string{schema::mask_right}, 1.5);
    rectangle.properties.insert_or_assign(
        std::string{schema::mask_bottom}, 0.875);
    rectangle.properties.insert_or_assign(
        std::string{schema::mask_rotation_turns}, 1.125);

    doc::Node ellipse = mask_node(
        21U, 1U, 10U, schema::mask_ellipse_kind, schema::mask_add, 1, false);
    doc::Node bezier = mask_node(
        22U, 1U, 10U, schema::mask_bezier_kind,
        schema::mask_difference, 2);
    doc::Node last = point_node(100U, 1U, 22U, 2, 2.0, -1.0);
    last.properties.insert_or_assign(
        std::string{schema::mask_point_in_x}, -0.5);
    last.properties.insert_or_assign(
        std::string{schema::mask_point_in_y}, 0.25);
    last.properties.insert_or_assign(
        std::string{schema::mask_point_out_x}, 1.5);
    last.properties.insert_or_assign(
        std::string{schema::mask_point_out_y}, 0.75);

    doc::Node nonfinite = mask_node(30U, 1U, 10U);
    nonfinite.properties.insert_or_assign(
        std::string{schema::mask_feather_px},
        std::numeric_limits<double>::infinity());
    doc::Node empty_name = mask_node(31U, 1U, 10U);
    empty_name.properties.insert_or_assign(
        std::string{schema::mask_name}, std::string{});
    doc::Node long_name = mask_node(32U, 1U, 10U);
    long_name.properties.insert_or_assign(
        std::string{schema::mask_name},
        std::string(scene::max_mask_name_bytes + 1U, 'x'));
    doc::Node unknown_kind = mask_node(33U, 1U, 10U);
    unknown_kind.properties.insert_or_assign(
        std::string{schema::mask_kind}, std::string{"polygon"});
    doc::Node unknown_combine = mask_node(34U, 1U, 10U);
    unknown_combine.properties.insert_or_assign(
        std::string{schema::mask_combine}, std::string{"replace"});

    scene::SceneModel model;
    model.reset(root_of({
        stage_node(1U), object_node(10U, 1U), rectangle, ellipse, bezier,
        point_node(102U, 1U, 22U, 0, 0.0, 0.0),
        point_node(101U, 1U, 22U, 1, 0.5, 1.0), last,
        nonfinite, empty_name, long_name, unknown_kind, unknown_combine}));

    MASK_CHECK(model.counters().rejected == 5U);
    const auto masks = model.masks(doc::NodeId{1U});
    MASK_CHECK(masks.size() == 3U);
    MASK_CHECK(masks[0U].kind == scene::MaskKind::Rectangle);
    MASK_CHECK(masks[0U].name == "Main Crop");
    MASK_CHECK(masks[0U].feather_canvas_px ==
               scene::max_mask_feather_canvas_px);
    MASK_CHECK(masks[0U].expansion_canvas_px ==
               -scene::max_mask_expansion_canvas_px);
    MASK_CHECK(masks[0U].opacity == 1.0F);
    MASK_CHECK(masks[0U].invert);
    MASK_CHECK(masks[0U].bounds ==
               (scene::MaskBounds{0.0F, 0.125F, 1.0F, 0.875F}));
    MASK_CHECK(masks[0U].rotation_turns == 0.125F);
    MASK_CHECK(masks[1U].kind == scene::MaskKind::Ellipse);
    MASK_CHECK(masks[1U].name == "Mask");
    MASK_CHECK(!masks[1U].enabled);
    MASK_CHECK(masks[2U].kind == scene::MaskKind::Bezier);
    MASK_CHECK(masks[2U].points.size() == 3U);
    MASK_CHECK(masks[2U].points[0U].source == doc::NodeId{102U});
    MASK_CHECK(masks[2U].points[2U].source == doc::NodeId{100U});
    MASK_CHECK(masks[2U].points[2U].x == 1.0F);
    MASK_CHECK(masks[2U].points[2U].y == 0.0F);
    MASK_CHECK(masks[2U].points[2U].in_x == 0.0F);
    MASK_CHECK(masks[2U].points[2U].out_x == 1.0F);

    const scene::Scene frame = evaluated(model);
    MASK_CHECK(frame.mask_programs.size() == 1U);
    MASK_CHECK(frame.mask_programs.front().coordinate_space ==
               scene::MaskCoordinateSpace::SourceLayer);
    MASK_CHECK(frame.mask_programs.front().operations.size() == 2U);
}

void programs_use_strict_order_and_exact_coverage_folds() {
    scene::SceneModel model;
    model.reset(root_of({
        stage_node(1U), object_node(10U, 1U),
        mask_node(5U, 1U, 10U, schema::mask_rectangle_kind,
                  schema::mask_subtract, -2, false),
        mask_node(40U, 1U, 10U, schema::mask_rectangle_kind,
                  schema::mask_add, 0),
        mask_node(30U, 1U, 10U, schema::mask_rectangle_kind,
                  schema::mask_intersect, 1),
        mask_node(20U, 1U, 10U, schema::mask_rectangle_kind,
                  schema::mask_difference, 1),
        mask_node(50U, 1U, 10U, schema::mask_rectangle_kind,
                  schema::mask_subtract, 2)}));
    const scene::Scene frame = evaluated(model);
    MASK_CHECK(frame.mask_programs.size() == 1U);
    const auto& operations = frame.mask_programs.front().operations;
    MASK_CHECK(operations.size() == 4U);
    MASK_CHECK(operations[0U].source == doc::NodeId{40U});
    MASK_CHECK(operations[1U].source == doc::NodeId{20U});
    MASK_CHECK(operations[2U].source == doc::NodeId{30U});
    MASK_CHECK(operations[3U].source == doc::NodeId{50U});

    float coverage = 0.25F; // the first Add seeds the field
    coverage = scene::fold_mask_coverage(
        coverage, 0.5F, scene::MaskCombine::Add);
    coverage = scene::fold_mask_coverage(
        coverage, 0.2F, scene::MaskCombine::Subtract);
    coverage = scene::fold_mask_coverage(
        coverage, 0.3F, scene::MaskCombine::Intersect);
    coverage = scene::fold_mask_coverage(
        coverage, 0.1F, scene::MaskCombine::Difference);
    MASK_CHECK(std::abs(coverage - 0.2F) < 1.0e-6F);

    scene::SceneModel identity;
    identity.reset(root_of({
        stage_node(1U), object_node(10U, 1U),
        mask_node(20U, 1U, 10U, schema::mask_rectangle_kind,
                  schema::mask_add, 0, false)}));
    MASK_CHECK(evaluated(identity).masks.size() == 1U);
    MASK_CHECK(evaluated(identity).mask_programs.empty());
}

void group_programs_keep_canvas_space_target_identity_and_show_state_independence() {
    doc::Node object = object_node(20U, 1U);
    object.properties.emplace(std::string{schema::parent_group},
                              std::int64_t{11});
    doc::Root root = root_of({
        stage_node(1U), group_node(10U, 1U, std::nullopt, true, false),
        group_node(11U, 1U, 10U), object,
        mask_node(30U, 1U, 10U), mask_node(31U, 1U, 11U),
        mask_node(32U, 1U, 20U)});
    scene::SceneModel model;
    model.reset(root);
    const scene::Scene hidden = evaluated(model);
    MASK_CHECK(hidden.instances.empty());
    MASK_CHECK(hidden.mask_programs.size() == 3U);
    MASK_CHECK(hidden.mask_programs[0U].target == doc::NodeId{10U});
    MASK_CHECK(hidden.mask_programs[0U].coordinate_space ==
               scene::MaskCoordinateSpace::AuthoredCanvas);
    MASK_CHECK(hidden.mask_programs[1U].target == doc::NodeId{11U});
    MASK_CHECK(hidden.mask_programs[1U].coordinate_space ==
               scene::MaskCoordinateSpace::AuthoredCanvas);
    MASK_CHECK(hidden.mask_programs[2U].target == doc::NodeId{20U});
    MASK_CHECK(hidden.mask_programs[2U].coordinate_space ==
               scene::MaskCoordinateSpace::SourceLayer);
    MASK_CHECK(hidden.mask_programs[0U].operations.size() == 1U);

    doc::Node revealed = *root.get(doc::NodeId{10U});
    revealed.properties.insert_or_assign(std::string{schema::visible}, true);
    revealed.properties.insert_or_assign(std::string{schema::locked}, false);
    const doc::Root changed = root.set(revealed);
    model.apply(doc::diff(root, changed));
    const scene::Scene visible = evaluated(model);
    MASK_CHECK(visible.instances.size() == 1U);
    MASK_CHECK(visible.mask_programs == hidden.mask_programs);
}

void explicit_scene_and_target_ownership_fail_with_stable_diagnostics() {
    doc::Node unowned_target = node(11U, schema::object_type);
    doc::Node unowned_mask = mask_node(20U, 1U, 10U);
    unowned_mask.properties.erase(std::string{schema::scene_id});
    doc::Node invalid_owner = mask_node(21U, 999U, 10U);
    doc::Node missing_target_owner = mask_node(22U, 1U, 11U);
    doc::Node cross_target = mask_node(23U, 1U, 12U);
    doc::Node missing_target = mask_node(24U, 1U, 999U);
    doc::Node invalid_target = mask_node(25U, 1U, 1U);
    doc::Node bezier = mask_node(
        26U, 1U, 10U, schema::mask_bezier_kind);
    doc::Node unowned_point = point_node(60U, 1U, 26U, 3, 0.2, 0.2);
    unowned_point.properties.erase(std::string{schema::scene_id});
    doc::Node invalid_point_owner = point_node(61U, 999U, 26U, 4, 0.3, 0.3);

    scene::SceneModel model;
    model.reset(root_of({
        stage_node(1U), stage_node(2U), object_node(10U, 1U), unowned_target,
        object_node(12U, 2U), unowned_mask, invalid_owner,
        missing_target_owner, cross_target, missing_target, invalid_target,
        bezier, point_node(50U, 1U, 26U, 0, 0.0, 0.0),
        point_node(51U, 1U, 26U, 1, 1.0, 0.0),
        point_node(52U, 1U, 26U, 2, 0.5, 1.0),
        unowned_point, invalid_point_owner}));
    const auto diagnostics = model.mask_diagnostics(doc::NodeId{1U});
    MASK_CHECK(has_diagnostic(
        diagnostics, scene::MaskDiagnosticKind::MissingSceneOwner,
        doc::NodeId{20U}));
    MASK_CHECK(has_diagnostic(
        diagnostics, scene::MaskDiagnosticKind::InvalidSceneOwner,
        doc::NodeId{21U}));
    MASK_CHECK(has_diagnostic(
        diagnostics, scene::MaskDiagnosticKind::MissingTargetSceneOwner,
        doc::NodeId{22U}));
    MASK_CHECK(has_diagnostic(
        diagnostics, scene::MaskDiagnosticKind::CrossSceneTarget,
        doc::NodeId{23U}));
    MASK_CHECK(has_diagnostic(
        diagnostics, scene::MaskDiagnosticKind::MissingTarget,
        doc::NodeId{24U}));
    MASK_CHECK(has_diagnostic(
        diagnostics, scene::MaskDiagnosticKind::InvalidTarget,
        doc::NodeId{25U}));
    MASK_CHECK(has_diagnostic(
        diagnostics, scene::MaskDiagnosticKind::MissingSceneOwner,
        doc::NodeId{60U}));
    MASK_CHECK(has_diagnostic(
        diagnostics, scene::MaskDiagnosticKind::InvalidSceneOwner,
        doc::NodeId{61U}));

    scene::SceneModel stage_less;
    stage_less.reset(root_of({object_node(10U, 1U), mask_node(20U, 1U, 10U)}));
    const auto stage_less_diagnostics =
        stage_less.mask_diagnostics(scene::legacy_scene_id);
    MASK_CHECK(has_diagnostic(
        stage_less_diagnostics, scene::MaskDiagnosticKind::InvalidSceneOwner,
        doc::NodeId{20U}));
    MASK_CHECK(stage_less.evaluate(
        scene::legacy_scene_id, scene::SceneTime{}).mask_programs.empty());
}

void malformed_paths_and_child_references_are_reported_without_repair() {
    doc::Node reversed = mask_node(21U, 1U, 10U);
    reversed.properties.insert_or_assign(
        std::string{schema::mask_left}, 0.8);
    reversed.properties.insert_or_assign(
        std::string{schema::mask_right}, 0.2);
    doc::Node cross_parent = mask_node(
        23U, 2U, 12U, schema::mask_bezier_kind);

    scene::SceneModel model;
    model.reset(root_of({
        stage_node(1U), stage_node(2U), object_node(10U, 1U),
        object_node(12U, 2U),
        mask_node(20U, 1U, 10U, schema::mask_bezier_kind), reversed,
        mask_node(22U, 1U, 10U, schema::mask_bezier_kind), cross_parent,
        mask_node(24U, 1U, 10U),
        point_node(40U, 1U, 22U, 0, 0.0, 0.0),
        point_node(41U, 1U, 22U, 0, 1.0, 0.0),
        point_node(42U, 1U, 22U, 1, 0.5, 1.0),
        point_node(43U, 1U, 999U, 0, 0.0, 0.0),
        point_node(44U, 1U, 23U, 0, 0.0, 0.0),
        point_node(45U, 1U, 24U, 0, 0.0, 0.0)}));
    const auto diagnostics = model.mask_diagnostics(doc::NodeId{1U});
    MASK_CHECK(has_diagnostic(
        diagnostics, scene::MaskDiagnosticKind::MalformedPath,
        doc::NodeId{20U}));
    MASK_CHECK(has_diagnostic(
        diagnostics, scene::MaskDiagnosticKind::MalformedPath,
        doc::NodeId{21U}));
    MASK_CHECK(has_diagnostic(
        diagnostics, scene::MaskDiagnosticKind::DuplicatePointOrder,
        doc::NodeId{41U}));
    MASK_CHECK(has_diagnostic(
        diagnostics, scene::MaskDiagnosticKind::MissingMask,
        doc::NodeId{43U}));
    MASK_CHECK(has_diagnostic(
        diagnostics, scene::MaskDiagnosticKind::CrossSceneMask,
        doc::NodeId{44U}));
    MASK_CHECK(has_diagnostic(
        diagnostics, scene::MaskDiagnosticKind::MalformedPath,
        doc::NodeId{24U}));
    MASK_CHECK(std::is_sorted(
        diagnostics.begin(), diagnostics.end(),
        [](const scene::MaskDiagnostic& left,
           const scene::MaskDiagnostic& right) noexcept {
            return std::tuple{left.subject.value(),
                              static_cast<core::u8>(left.kind),
                              left.related.value()}
                < std::tuple{right.subject.value(),
                             static_cast<core::u8>(right.kind),
                             right.related.value()};
        }));
    MASK_CHECK(evaluated(model).mask_programs.empty());
}

void disabled_malformed_masks_stay_visible_but_first_active_must_add() {
    scene::SceneModel model;
    model.reset(root_of({
        stage_node(1U), object_node(10U, 1U), object_node(11U, 1U),
        mask_node(20U, 1U, 10U, schema::mask_bezier_kind,
                  schema::mask_add, -1, false),
        mask_node(21U, 1U, 10U, schema::mask_rectangle_kind,
                  schema::mask_subtract, 0, true),
        mask_node(22U, 1U, 11U, schema::mask_rectangle_kind,
                  schema::mask_add, 0, false)}));
    const scene::Scene frame = evaluated(model);
    MASK_CHECK(frame.masks.size() == 3U);
    MASK_CHECK(frame.mask_programs.empty());
    MASK_CHECK(has_diagnostic(
        frame.mask_diagnostics, scene::MaskDiagnosticKind::MalformedPath,
        doc::NodeId{20U}));
    MASK_CHECK(has_diagnostic(
        frame.mask_diagnostics, scene::MaskDiagnosticKind::InvalidFirstCombine,
        doc::NodeId{21U}));
}

void mask_and_point_caps_fail_the_whole_target_program_visibly() {
    doc::Root too_many_masks =
        root_of({stage_node(1U), object_node(10U, 1U)});
    for (core::u32 index = 0U; index <= scene::max_scene_masks; ++index) {
        too_many_masks = too_many_masks.set(mask_node(
            1'000U + index, 1U, 10U, schema::mask_rectangle_kind,
            schema::mask_add, static_cast<std::int64_t>(index)));
    }
    scene::SceneModel mask_model;
    mask_model.reset(too_many_masks);
    const scene::Scene mask_frame = evaluated(mask_model);
    MASK_CHECK(mask_frame.masks.size() == scene::max_scene_masks);
    MASK_CHECK(mask_frame.mask_programs.empty());
    MASK_CHECK(has_diagnostic(
        mask_frame.mask_diagnostics,
        scene::MaskDiagnosticKind::MaskLimitExceeded,
        doc::NodeId{1'000U + scene::max_scene_masks}));

    doc::Root too_many_points = root_of({
        stage_node(1U), object_node(10U, 1U),
        mask_node(20U, 1U, 10U, schema::mask_bezier_kind)});
    for (core::u32 index = 0U;
         index <= scene::max_mask_points_per_mask; ++index) {
        too_many_points = too_many_points.set(point_node(
            1'000U + index, 1U, 20U,
            static_cast<std::int64_t>(index),
            static_cast<double>(index % 2U),
            static_cast<double>((index / 2U) % 2U)));
    }
    scene::SceneModel point_model;
    point_model.reset(too_many_points);
    const scene::Scene point_frame = evaluated(point_model);
    MASK_CHECK(point_frame.masks.size() == 1U);
    MASK_CHECK(point_frame.masks.front().points.size() ==
               scene::max_mask_points_per_mask);
    MASK_CHECK(point_frame.mask_programs.empty());
    MASK_CHECK(has_diagnostic(
        point_frame.mask_diagnostics,
        scene::MaskDiagnosticKind::PointLimitExceeded,
        doc::NodeId{1'000U + scene::max_mask_points_per_mask}));
}

void authored_masks_programs_and_diagnostics_all_participate_in_digest() {
    doc::Root root = root_of({
        stage_node(1U), object_node(10U, 1U), mask_node(20U, 1U, 10U)});
    scene::SceneModel model;
    model.reset(root);
    const scene::Scene baseline = evaluated(model);

    doc::Node renamed = *root.get(doc::NodeId{20U});
    renamed.properties.insert_or_assign(
        std::string{schema::mask_name}, std::string{"Renamed"});
    doc::Root changed = root.set(renamed);
    model.apply(doc::diff(root, changed));
    MASK_CHECK(scene::digest(baseline) != scene::digest(evaluated(model)));

    scene::SceneModel empty;
    empty.reset(root_of({stage_node(1U), object_node(10U, 1U)}));
    MASK_CHECK(scene::digest(baseline) != scene::digest(evaluated(empty)));

    doc::Node subtract = *root.get(doc::NodeId{20U});
    subtract.properties.insert_or_assign(
        std::string{schema::mask_combine},
        std::string{schema::mask_subtract});
    scene::SceneModel invalid;
    invalid.reset(root.set(subtract));
    const scene::Scene invalid_frame = evaluated(invalid);
    MASK_CHECK(!invalid_frame.mask_diagnostics.empty());
    MASK_CHECK(scene::digest(baseline) != scene::digest(invalid_frame));
}

void checkpoint_stream_and_project_file_round_trip_agree_exactly() {
    doc::Node mask = mask_node(
        20U, 1U, 10U, schema::mask_bezier_kind,
        schema::mask_add, -7);
    mask.properties.insert_or_assign(
        std::string{schema::mask_name}, std::string{"Saved Path"});
    mask.properties.insert_or_assign(
        std::string{schema::mask_feather_px}, 12.5);
    mask.properties.insert_or_assign(
        std::string{schema::mask_expansion_px}, -3.25);
    mask.properties.insert_or_assign(
        std::string{schema::mask_opacity}, 0.625);
    mask.properties.insert_or_assign(
        std::string{schema::mask_invert}, true);
    doc::Node p0 = point_node(30U, 1U, 20U, 0, 0.1, 0.2);
    p0.properties.insert_or_assign(
        std::string{schema::mask_point_out_x}, 0.3);
    p0.properties.insert_or_assign(
        std::string{schema::mask_point_out_y}, 0.4);
    const std::array nodes{
        stage_node(1U), object_node(10U, 1U), mask, p0,
        point_node(31U, 1U, 20U, 1, 0.8, 0.2),
        point_node(32U, 1U, 20U, 2, 0.5, 0.9)};
    doc::Root root;
    for (const doc::Node& value : nodes) {
        root = root.set(value);
    }

    scene::SceneModel checkpointed;
    checkpointed.reset(root);
    scene::SceneModel streamed;
    doc::Root running;
    const std::array<std::size_t, 6U> arrival{3U, 4U, 5U, 2U, 1U, 0U};
    for (const std::size_t index : arrival) {
        const doc::Root next = running.set(nodes[index]);
        streamed.apply(doc::diff(running, next));
        running = next;
    }
    MASK_CHECK(streamed.masks(doc::NodeId{1U}) ==
               checkpointed.masks(doc::NodeId{1U}));
    MASK_CHECK(streamed.mask_diagnostics(doc::NodeId{1U}) ==
               checkpointed.mask_diagnostics(doc::NodeId{1U}));
    MASK_CHECK(scene::digest(evaluated(streamed)) ==
               scene::digest(evaluated(checkpointed)));

    TempDirectory temporary;
    MASK_CHECK(persist::save_project_file(temporary.file(), root));
    auto loaded = persist::load_project_file(temporary.file());
    MASK_CHECK(loaded.has_value());
    MASK_CHECK(same_document(loaded.value(), root));
    scene::SceneModel restored;
    restored.reset(loaded.value());
    MASK_CHECK(restored.masks(doc::NodeId{1U}) ==
               checkpointed.masks(doc::NodeId{1U}));
    MASK_CHECK(scene::digest(evaluated(restored)) ==
               scene::digest(evaluated(checkpointed)));
}

void authoring_creates_typed_shapes_with_canonical_values_and_ids() {
    const doc::Root base = root_of({stage_node(1U), object_node(10U, 1U)});
    scene::CreateMaskRequest rectangle{};
    rectangle.scene = doc::NodeId{1U};
    rectangle.target = doc::NodeId{10U};
    rectangle.name = "Crop";
    rectangle.feather_canvas_px = 50'000.0F;
    rectangle.expansion_canvas_px = -50'000.0F;
    rectangle.opacity = 2.0F;
    rectangle.shape = scene::RectangleMaskShape{
        scene::MaskBounds{-1.0F, 0.25F, 2.0F, 0.75F}};
    auto made_rectangle = scene::plan_create_mask(
        base, rectangle, doc::NodeId{10U});
    MASK_CHECK(made_rectangle.has_value());
    MASK_CHECK(made_rectangle.value().mask == doc::NodeId{11U});
    MASK_CHECK(made_rectangle.value().puts.size() == 1U);
    MASK_CHECK(sorted_puts(made_rectangle.value()));
    const doc::Node* crop =
        made_rectangle.value().after.get(doc::NodeId{11U});
    MASK_CHECK(crop != nullptr);
    MASK_CHECK(property_as<std::string>(*crop, schema::mask_name) == "Crop");
    MASK_CHECK(property_as<double>(*crop, schema::mask_left) == 0.0);
    MASK_CHECK(property_as<double>(*crop, schema::mask_right) == 1.0);
    MASK_CHECK(property_as<double>(*crop, schema::mask_feather_px)
               == static_cast<double>(scene::max_mask_feather_canvas_px));
    MASK_CHECK(property_as<double>(*crop, schema::mask_expansion_px)
               == static_cast<double>(
                   -scene::max_mask_expansion_canvas_px));
    MASK_CHECK(property_as<double>(*crop, schema::mask_opacity) == 1.0);

    scene::CreateMaskRequest ellipse = rectangle;
    ellipse.name = "Vignette";
    ellipse.order = 1;
    ellipse.feather_canvas_px = 0.0F;
    ellipse.expansion_canvas_px = 0.0F;
    ellipse.opacity = 1.0F;
    ellipse.shape = scene::EllipseMaskShape{
        scene::MaskBounds{0.1F, 0.2F, 0.9F, 0.8F}};
    auto made_ellipse = scene::plan_create_mask(
        made_rectangle.value().after, ellipse, doc::NodeId{11U});
    MASK_CHECK(made_ellipse.has_value());
    MASK_CHECK(made_ellipse.value().mask == doc::NodeId{12U});

    scene::CreateMaskRequest bezier = ellipse;
    bezier.name = "Path";
    bezier.order = 2;
    bezier.combine = scene::MaskCombine::Difference;
    bezier.shape = scene::BezierMaskShape{{
        scene::MaskPointGeometry{-1.0F, 0.0F, -1.0F, 0.0F,
                                 0.25F, 0.0F},
        scene::MaskPointGeometry{1.0F, 0.0F, 0.75F, 0.0F,
                                 2.0F, 0.0F},
        scene::MaskPointGeometry{0.5F, 1.0F, 0.25F, 1.0F,
                                 0.75F, 1.0F},
    }};
    auto made_bezier = scene::plan_create_mask(
        made_ellipse.value().after, bezier, doc::NodeId{11U});
    MASK_CHECK(made_bezier.has_value());
    MASK_CHECK(made_bezier.value().mask == doc::NodeId{13U});
    MASK_CHECK(made_bezier.value().point == doc::NodeId{14U});
    MASK_CHECK(made_bezier.value().puts.size() == 4U);
    MASK_CHECK(sorted_puts(made_bezier.value()));
    MASK_CHECK(property_as<double>(
                   *made_bezier.value().after.get(doc::NodeId{14U}),
                   schema::mask_point_x)
               == 0.0);
    MASK_CHECK(property_as<double>(
                   *made_bezier.value().after.get(doc::NodeId{15U}),
                   schema::mask_point_out_x)
               == 1.0);
    scene::SceneModel model;
    model.reset(made_bezier.value().after);
    MASK_CHECK(model.mask_diagnostics(doc::NodeId{1U}).empty());
    MASK_CHECK(evaluated(model).mask_programs.front().operations.size() == 3U);

    scene::CreateMaskRequest invalid = rectangle;
    invalid.shape = scene::RectangleMaskShape{
        scene::MaskBounds{0.8F, 0.0F, 0.2F, 1.0F}};
    auto reversed = scene::plan_create_mask(base, invalid, doc::NodeId{20U});
    MASK_CHECK(!reversed.has_value());
    MASK_CHECK(reversed.error().code ==
               scene::MaskAuthoringErrorCode::InvalidShape);
    invalid = rectangle;
    invalid.shape = scene::BezierMaskShape{{
        scene::MaskPointGeometry{}, scene::MaskPointGeometry{}}};
    auto short_path = scene::plan_create_mask(base, invalid, doc::NodeId{20U});
    MASK_CHECK(!short_path.has_value());
    MASK_CHECK(short_path.error().code ==
               scene::MaskAuthoringErrorCode::MinimumBezierPoints);
    invalid = rectangle;
    invalid.name.clear();
    auto unnamed = scene::plan_create_mask(base, invalid, doc::NodeId{20U});
    MASK_CHECK(!unnamed.has_value());
    MASK_CHECK(unnamed.error().code ==
               scene::MaskAuthoringErrorCode::InvalidName);
    invalid = rectangle;
    invalid.opacity = std::numeric_limits<float>::infinity();
    auto nonfinite = scene::plan_create_mask(base, invalid, doc::NodeId{20U});
    MASK_CHECK(!nonfinite.has_value());
    MASK_CHECK(nonfinite.error().code ==
               scene::MaskAuthoringErrorCode::InvalidValue);
    auto zero_id = scene::plan_create_mask(base, rectangle, doc::NodeId{});
    MASK_CHECK(!zero_id.has_value());
    MASK_CHECK(zero_id.error().code ==
               scene::MaskAuthoringErrorCode::InvalidFirstId);

    doc::Node unowned = object_node(10U, 1U);
    unowned.properties.erase(std::string{schema::scene_id});
    auto missing_owner = scene::plan_create_mask(
        root_of({stage_node(1U), unowned}), rectangle, doc::NodeId{20U});
    MASK_CHECK(!missing_owner.has_value());
    MASK_CHECK(missing_owner.error().code ==
               scene::MaskAuthoringErrorCode::MissingTargetSceneOwner);
    scene::CreateMaskRequest cross = rectangle;
    cross.scene = doc::NodeId{1U};
    auto cross_scene = scene::plan_create_mask(
        root_of({stage_node(1U), stage_node(2U), object_node(10U, 2U)}),
        cross, doc::NodeId{20U});
    MASK_CHECK(!cross_scene.has_value());
    MASK_CHECK(cross_scene.error().code ==
               scene::MaskAuthoringErrorCode::CrossSceneTarget);

    doc::Root full = base;
    for (core::u32 mask = 0U; mask < scene::max_scene_masks; ++mask) {
        full = full.set(mask_node(
            1'000U + mask, 1U, 10U,
            schema::mask_rectangle_kind, schema::mask_add,
            static_cast<std::int64_t>(mask)));
    }
    auto over_cap = scene::plan_create_mask(
        full, rectangle, doc::NodeId{2'000U});
    MASK_CHECK(!over_cap.has_value());
    MASK_CHECK(over_cap.error().code ==
               scene::MaskAuthoringErrorCode::MaskCapacity);
}

void mask_property_authoring_preserves_data_and_active_chain_invariants() {
    doc::Node add = mask_node(
        20U, 1U, 10U, schema::mask_rectangle_kind,
        schema::mask_add, 0, false);
    add.properties.emplace("extension", std::string{"keep"});
    const doc::Root base = root_of({
        stage_node(1U), object_node(10U, 1U), add,
        mask_node(21U, 1U, 10U, schema::mask_rectangle_kind,
                  schema::mask_subtract, 1, false)});

    auto premature = scene::plan_set_mask_enabled(
        base, doc::NodeId{1U}, doc::NodeId{21U}, true);
    MASK_CHECK(!premature.has_value());
    MASK_CHECK(premature.error().code ==
               scene::MaskAuthoringErrorCode::InvalidFirstActiveCombine);
    auto enabled_add = scene::plan_set_mask_enabled(
        base, doc::NodeId{1U}, doc::NodeId{20U}, true);
    MASK_CHECK(enabled_add.has_value());
    auto enabled_both = scene::plan_set_mask_enabled(
        enabled_add.value().after, doc::NodeId{1U}, doc::NodeId{21U}, true);
    MASK_CHECK(enabled_both.has_value());

    scene::MaskPropertyPatch patch{};
    patch.invert = true;
    patch.feather_canvas_px = 50'000.0F;
    patch.expansion_canvas_px = -50'000.0F;
    patch.opacity = 0.375F;
    patch.bounds = scene::MaskBounds{-1.0F, 0.1F, 2.0F, 0.9F};
    patch.rotation_turns = -1.75F;
    auto edited = scene::plan_edit_mask_properties(
        enabled_both.value().after, doc::NodeId{1U}, doc::NodeId{20U},
        patch);
    MASK_CHECK(edited.has_value());
    const doc::Node& changed =
        *edited.value().after.get(doc::NodeId{20U});
    MASK_CHECK(property_as<std::string>(changed, "extension") == "keep");
    MASK_CHECK(property_as<bool>(changed, schema::mask_invert));
    MASK_CHECK(property_as<double>(changed, schema::mask_opacity)
               == static_cast<double>(0.375F));
    MASK_CHECK(property_as<double>(changed, schema::mask_left) == 0.0);
    MASK_CHECK(property_as<double>(changed, schema::mask_right) == 1.0);
    MASK_CHECK(property_as<double>(changed, schema::mask_rotation_turns)
               == 0.25);

    auto renamed = scene::plan_rename_mask(
        edited.value().after, doc::NodeId{1U}, doc::NodeId{20U},
        "Primary ✓");
    MASK_CHECK(renamed.has_value());
    MASK_CHECK(property_as<std::string>(
                   *renamed.value().after.get(doc::NodeId{20U}),
                   schema::mask_name)
               == "Primary ✓");
    auto unnamed = scene::plan_rename_mask(
        renamed.value().after, doc::NodeId{1U}, doc::NodeId{20U}, "");
    MASK_CHECK(!unnamed.has_value());
    MASK_CHECK(unnamed.error().code ==
               scene::MaskAuthoringErrorCode::InvalidName);
    auto too_long = scene::plan_rename_mask(
        renamed.value().after, doc::NodeId{1U}, doc::NodeId{20U},
        std::string(scene::max_mask_name_bytes + 1U, 'x'));
    MASK_CHECK(!too_long.has_value());
    MASK_CHECK(too_long.error().code ==
               scene::MaskAuthoringErrorCode::NameTooLong);

    auto invalid_first = scene::plan_set_mask_combine(
        renamed.value().after, doc::NodeId{1U}, doc::NodeId{20U},
        scene::MaskCombine::Subtract);
    MASK_CHECK(!invalid_first.has_value());
    MASK_CHECK(invalid_first.error().code ==
               scene::MaskAuthoringErrorCode::InvalidFirstActiveCombine);
    scene::MaskPropertyPatch nonfinite{};
    nonfinite.opacity = std::numeric_limits<float>::quiet_NaN();
    auto rejected_value = scene::plan_edit_mask_properties(
        renamed.value().after, doc::NodeId{1U}, doc::NodeId{20U},
        nonfinite);
    MASK_CHECK(!rejected_value.has_value());
    MASK_CHECK(rejected_value.error().code ==
               scene::MaskAuthoringErrorCode::InvalidValue);
}

void mask_reorder_and_delete_are_target_local_and_cascade_points() {
    const doc::Root ordered = root_of({
        stage_node(1U), object_node(10U, 1U),
        mask_node(20U, 1U, 10U, schema::mask_rectangle_kind,
                  schema::mask_add, 100),
        mask_node(21U, 1U, 10U, schema::mask_rectangle_kind,
                  schema::mask_subtract, 200),
        mask_node(22U, 1U, 10U, schema::mask_rectangle_kind,
                  schema::mask_difference, 300)});
    auto moved = scene::plan_reorder_mask(
        ordered, doc::NodeId{1U}, doc::NodeId{22U}, 1U);
    MASK_CHECK(moved.has_value());
    MASK_CHECK(sorted_puts(moved.value()));
    MASK_CHECK(property_as<std::int64_t>(
                   *moved.value().after.get(doc::NodeId{20U}),
                   schema::mask_order)
               == 0);
    MASK_CHECK(property_as<std::int64_t>(
                   *moved.value().after.get(doc::NodeId{22U}),
                   schema::mask_order)
               == 1);
    MASK_CHECK(property_as<std::int64_t>(
                   *moved.value().after.get(doc::NodeId{21U}),
                   schema::mask_order)
               == 2);
    auto invalid_move = scene::plan_reorder_mask(
        ordered, doc::NodeId{1U}, doc::NodeId{21U}, 0U);
    MASK_CHECK(!invalid_move.has_value());
    MASK_CHECK(invalid_move.error().code ==
               scene::MaskAuthoringErrorCode::InvalidFirstActiveCombine);

    const doc::Root path = root_of({
        stage_node(1U), object_node(10U, 1U),
        mask_node(20U, 1U, 10U),
        mask_node(21U, 1U, 10U, schema::mask_bezier_kind,
                  schema::mask_subtract, 1),
        point_node(40U, 1U, 21U, 0, 0.0, 0.0),
        point_node(41U, 1U, 21U, 1, 1.0, 0.0),
        point_node(42U, 1U, 21U, 2, 0.5, 1.0)});
    auto erased = scene::plan_delete_mask(
        path, doc::NodeId{1U}, doc::NodeId{21U});
    MASK_CHECK(erased.has_value());
    MASK_CHECK((erased.value().erases ==
                std::vector<doc::NodeId>{doc::NodeId{21U},
                                         doc::NodeId{40U},
                                         doc::NodeId{41U},
                                         doc::NodeId{42U}}));
    MASK_CHECK(erased.value().after.get(doc::NodeId{21U}) == nullptr);
    MASK_CHECK(erased.value().after.get(doc::NodeId{40U}) == nullptr);
    auto invalid_delete = scene::plan_delete_mask(
        path, doc::NodeId{1U}, doc::NodeId{20U});
    MASK_CHECK(!invalid_delete.has_value());
    MASK_CHECK(invalid_delete.error().code ==
               scene::MaskAuthoringErrorCode::InvalidFirstActiveCombine);
}

void bezier_point_authoring_keeps_dense_order_ownership_and_caps() {
    doc::Node first = point_node(30U, 1U, 20U, 0, 0.0, 0.0);
    first.properties.emplace("extension", std::string{"keep"});
    const doc::Root base = root_of({
        stage_node(1U), object_node(10U, 1U),
        mask_node(20U, 1U, 10U, schema::mask_bezier_kind), first,
        point_node(31U, 1U, 20U, 1, 1.0, 0.0),
        point_node(32U, 1U, 20U, 2, 0.5, 1.0)});
    auto inserted = scene::plan_insert_mask_point(
        base, doc::NodeId{1U}, doc::NodeId{20U}, 1U,
        scene::MaskPointGeometry{2.0F, -1.0F, 0.25F, 0.25F,
                                 0.75F, 0.75F},
        doc::NodeId{31U});
    MASK_CHECK(inserted.has_value());
    MASK_CHECK(inserted.value().point == doc::NodeId{33U});
    MASK_CHECK(sorted_puts(inserted.value()));
    MASK_CHECK(property_as<std::int64_t>(
                   *inserted.value().after.get(doc::NodeId{33U}),
                   schema::mask_point_order)
               == 1);
    MASK_CHECK(property_as<double>(
                   *inserted.value().after.get(doc::NodeId{33U}),
                   schema::mask_point_x)
               == 1.0);
    MASK_CHECK(property_as<std::int64_t>(
                   *inserted.value().after.get(doc::NodeId{31U}),
                   schema::mask_point_order)
               == 2);

    scene::MaskPointPatch patch{};
    patch.x = 2.0F;
    patch.y = -2.0F;
    patch.in_x = 0.2F;
    patch.out_y = 0.8F;
    auto edited = scene::plan_edit_mask_point(
        inserted.value().after, doc::NodeId{1U}, doc::NodeId{20U},
        doc::NodeId{30U}, patch);
    MASK_CHECK(edited.has_value());
    const doc::Node& edited_point =
        *edited.value().after.get(doc::NodeId{30U});
    MASK_CHECK(property_as<std::string>(edited_point, "extension") == "keep");
    MASK_CHECK(property_as<double>(edited_point, schema::mask_point_x) == 1.0);
    MASK_CHECK(property_as<double>(edited_point, schema::mask_point_y) == 0.0);

    auto deleted = scene::plan_delete_mask_point(
        edited.value().after, doc::NodeId{1U}, doc::NodeId{20U},
        doc::NodeId{31U});
    MASK_CHECK(deleted.has_value());
    MASK_CHECK(deleted.value().erases ==
               std::vector<doc::NodeId>{doc::NodeId{31U}});
    MASK_CHECK(property_as<std::int64_t>(
                   *deleted.value().after.get(doc::NodeId{32U}),
                   schema::mask_point_order)
               == 2);
    auto too_few = scene::plan_delete_mask_point(
        deleted.value().after, doc::NodeId{1U}, doc::NodeId{20U},
        doc::NodeId{30U});
    MASK_CHECK(!too_few.has_value());
    MASK_CHECK(too_few.error().code ==
               scene::MaskAuthoringErrorCode::MinimumBezierPoints);

    auto bad_index = scene::plan_insert_mask_point(
        base, doc::NodeId{1U}, doc::NodeId{20U}, 4U,
        scene::MaskPointGeometry{}, doc::NodeId{100U});
    MASK_CHECK(!bad_index.has_value());
    MASK_CHECK(bad_index.error().code ==
               scene::MaskAuthoringErrorCode::InvalidPointIndex);
    auto non_bezier = scene::plan_insert_mask_point(
        root_of({stage_node(1U), object_node(10U, 1U),
                 mask_node(20U, 1U, 10U)}),
        doc::NodeId{1U}, doc::NodeId{20U}, 0U,
        scene::MaskPointGeometry{}, doc::NodeId{100U});
    MASK_CHECK(!non_bezier.has_value());
    MASK_CHECK(non_bezier.error().code ==
               scene::MaskAuthoringErrorCode::NonBezierMask);

    doc::Root duplicate = base.set(
        point_node(31U, 1U, 20U, 0, 1.0, 0.0));
    auto duplicate_rejected = scene::plan_insert_mask_point(
        duplicate, doc::NodeId{1U}, doc::NodeId{20U}, 1U,
        scene::MaskPointGeometry{}, doc::NodeId{100U});
    MASK_CHECK(!duplicate_rejected.has_value());
    MASK_CHECK(duplicate_rejected.error().code ==
               scene::MaskAuthoringErrorCode::InvalidPoint);

    doc::Root cross_owned = base.set(stage_node(2U));
    cross_owned = cross_owned.set(
        point_node(90U, 2U, 20U, 3, 0.25, 0.25));
    auto cross_point = scene::plan_insert_mask_point(
        cross_owned, doc::NodeId{1U}, doc::NodeId{20U}, 1U,
        scene::MaskPointGeometry{}, doc::NodeId{100U});
    MASK_CHECK(!cross_point.has_value());
    MASK_CHECK(cross_point.error().code ==
               scene::MaskAuthoringErrorCode::CrossScenePoint);

    doc::Root full = root_of({
        stage_node(1U), object_node(10U, 1U),
        mask_node(20U, 1U, 10U, schema::mask_bezier_kind)});
    for (core::u32 point = 0U;
         point < scene::max_mask_points_per_mask; ++point) {
        full = full.set(point_node(
            1'000U + point, 1U, 20U,
            static_cast<std::int64_t>(point), 0.5, 0.5));
    }
    auto over_cap = scene::plan_insert_mask_point(
        full, doc::NodeId{1U}, doc::NodeId{20U}, 0U,
        scene::MaskPointGeometry{}, doc::NodeId{2'000U});
    MASK_CHECK(!over_cap.has_value());
    MASK_CHECK(over_cap.error().code ==
               scene::MaskAuthoringErrorCode::PointCapacity);
}

void one_mask_replacement_undoes_and_redoes_exactly() {
    const doc::Root base = root_of({stage_node(1U), object_node(10U, 1U)});
    scene::CreateMaskRequest request{};
    request.scene = doc::NodeId{1U};
    request.target = doc::NodeId{10U};
    request.name = "Undoable Path";
    request.shape = scene::BezierMaskShape{{
        scene::MaskPointGeometry{0.0F, 0.0F, 0.0F, 0.0F, 0.25F, 0.0F},
        scene::MaskPointGeometry{1.0F, 0.0F, 0.75F, 0.0F, 1.0F, 0.0F},
        scene::MaskPointGeometry{0.5F, 1.0F, 0.25F, 1.0F, 0.75F, 1.0F},
    }};
    auto authored = scene::plan_create_mask(base, request, doc::NodeId{100U});
    MASK_CHECK(authored.has_value());
    MASK_CHECK(authored.value().puts.size() == 4U);

    cmd::DirectorScheduler scheduler{base};
    constexpr cmd::UndoContextId context{17U};
    auto scheduled = scheduler.schedule_document_replace(
        authored.value().after, context, cmd::TransactionId{1U},
        cmd::FrameNumber{1U});
    MASK_CHECK(scheduled.has_value());
    MASK_CHECK(equivalent_document(
        scheduler.projected_document(), authored.value().after));
    MASK_CHECK(scheduler.undo_depth(context) == 1U);

    auto undone = scheduler.schedule_undo(
        context, cmd::TransactionId{2U}, cmd::FrameNumber{1U});
    MASK_CHECK(undone.has_value());
    MASK_CHECK(equivalent_document(scheduler.projected_document(), base));
    MASK_CHECK(scheduler.redo_depth(context) == 1U);

    auto redone = scheduler.schedule_redo(
        context, cmd::TransactionId{3U}, cmd::FrameNumber{1U});
    MASK_CHECK(redone.has_value());
    MASK_CHECK(equivalent_document(
        scheduler.projected_document(), authored.value().after));
    MASK_CHECK(scheduler.undo_depth(context) == 1U);
}

void source_plane_and_extent_follow_the_drawable_surface() {
    // Quad and Plane name the corners whose UVs are (0,0), (1,0), (1,1), (0,1).
    const auto quad = scene::mask_source_plane(scene::Primitive::Quad);
    MASK_CHECK(quad.has_value());
    MASK_CHECK((*quad)[0U] == scene::Vec3{-0.5F, 0.5F, 0.0F});
    MASK_CHECK((*quad)[2U] == scene::Vec3{0.5F, -0.5F, 0.0F});
    const auto plane = scene::mask_source_plane(scene::Primitive::Plane);
    MASK_CHECK(plane.has_value());
    MASK_CHECK((*plane)[0U] == scene::Vec3{-0.5F, 0.0F, -0.5F});
    MASK_CHECK((*plane)[2U] == scene::Vec3{0.5F, 0.0F, 0.5F});
    MASK_CHECK(!scene::mask_source_plane(scene::Primitive::Box).has_value());
    MASK_CHECK(!scene::mask_source_plane(scene::Primitive::Sphere).has_value());

    // A 2 x 1 quad facing a 45-degree camera 5 m away on a 1920 x 1080
    // canvas: 540 / tan(22.5 deg) px per unit of depth-scaled size.
    constexpr scene::Canvas canvas{1920U, 1080U};
    const scene::Camera camera{};
    const scene::Mat4 world = scene::multiply(
        scene::translation(scene::Vec3{0.7F, -0.4F, -5.0F}),
        scene::scaling(scene::Vec3{2.0F, 1.0F, 1.0F}));
    const double focal_px = 540.0 / std::tan(3.14159265358979323846 / 8.0);
    const scene::MaskCoordinateExtent extent = scene::mask_source_extent(
        camera, canvas, world, scene::Primitive::Quad);
    MASK_CHECK(std::abs(extent.width - focal_px * 2.0 / 5.0) < 0.05);
    MASK_CHECK(std::abs(extent.height - focal_px * 1.0 / 5.0) < 0.05);

    // Without one plane, or with a corner behind the eye, it is the canvas.
    MASK_CHECK(scene::mask_source_extent(camera, canvas, world,
                                         scene::Primitive::Box) ==
               (scene::MaskCoordinateExtent{1920.0F, 1080.0F}));
    const scene::Mat4 behind = scene::multiply(
        scene::translation(scene::Vec3{0.0F, 0.0F, -0.2F}),
        scene::multiply(scene::rotation(0.25F, 0.0F, 0.0F),
                        scene::scaling(scene::Vec3{4.0F, 1.0F, 1.0F})));
    MASK_CHECK(scene::mask_source_extent(camera, canvas, behind,
                                         scene::Primitive::Quad) ==
               (scene::MaskCoordinateExtent{1920.0F, 1080.0F}));
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 15U> cases{{
    {"typed shapes and finite values normalize at the model boundary",
     &typed_shapes_and_finite_values_normalize_at_the_model_boundary},
    {"programs use strict order and exact coverage folds",
     &programs_use_strict_order_and_exact_coverage_folds},
    {"group programs keep canvas space target identity and show state independence",
     &group_programs_keep_canvas_space_target_identity_and_show_state_independence},
    {"explicit scene and target ownership fail with stable diagnostics",
     &explicit_scene_and_target_ownership_fail_with_stable_diagnostics},
    {"malformed paths and child references are reported without repair",
     &malformed_paths_and_child_references_are_reported_without_repair},
    {"disabled malformed masks stay visible but first active must add",
     &disabled_malformed_masks_stay_visible_but_first_active_must_add},
    {"mask and point caps fail the whole target program visibly",
     &mask_and_point_caps_fail_the_whole_target_program_visibly},
    {"authored masks programs and diagnostics all participate in digest",
     &authored_masks_programs_and_diagnostics_all_participate_in_digest},
    {"checkpoint stream and project file round trip agree exactly",
     &checkpoint_stream_and_project_file_round_trip_agree_exactly},
    {"authoring creates typed shapes with canonical values and ids",
     &authoring_creates_typed_shapes_with_canonical_values_and_ids},
    {"mask property authoring preserves data and active chain invariants",
     &mask_property_authoring_preserves_data_and_active_chain_invariants},
    {"mask reorder and delete are target local and cascade points",
     &mask_reorder_and_delete_are_target_local_and_cascade_points},
    {"bezier point authoring keeps dense order ownership and caps",
     &bezier_point_authoring_keeps_dense_order_ownership_and_caps},
    {"one mask replacement undoes and redoes exactly",
     &one_mask_replacement_undoes_and_redoes_exactly},
    {"source plane and extent follow the drawable surface",
     &source_plane_and_extent_follow_the_drawable_surface},
}};

} // namespace

int main() {
    std::size_t failures{};
    for (const Case& test : cases) {
        try {
            test.function();
            std::cout << "PASS " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << test.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " mask tests passed\n";
    return failures == 0U ? 0 : 1;
}
