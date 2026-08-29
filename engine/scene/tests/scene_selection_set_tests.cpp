#include <mediaserver/scene/selection_set.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace scene = mediaserver::scene;
namespace schema = mediaserver::scene::schema;
namespace set_schema = mediaserver::scene::selection_set_schema;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define SET_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

using Property = std::pair<std::string, doc::PropertyValue>;

[[nodiscard]] Property whole(std::string_view name, core::i64 value) {
    return {std::string{name}, value};
}

[[nodiscard]] Property flag(std::string_view name, bool value) {
    return {std::string{name}, value};
}

[[nodiscard]] Property text(std::string_view name, std::string_view value) {
    return {std::string{name}, std::string{value}};
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

[[nodiscard]] doc::Node set_node(core::u64 id, core::u64 owner,
                                 std::string_view name) {
    return node(
        id, set_schema::set_type,
        {whole(schema::scene_id, static_cast<core::i64>(owner)),
         text(set_schema::name, name)});
}

[[nodiscard]] doc::Node member_node(
    core::u64 id, core::u64 set, core::u64 target, core::i64 order) {
    return node(
        id, set_schema::member_type,
        {whole(set_schema::set_id, static_cast<core::i64>(set)),
         whole(set_schema::target_id, static_cast<core::i64>(target)),
         whole(set_schema::member_order, order)});
}

[[nodiscard]] doc::Root two_scene_root() {
    doc::Root root;
    root = root.set(node(1U, schema::stage_type));
    root = root.set(node(2U, schema::stage_type));
    root = root.set(node(
        10U, schema::object_type, {whole(schema::scene_id, 1)}));
    root = root.set(node(
        20U, schema::media_type, {whole(schema::scene_id, 1)}));
    root = root.set(node(
        30U, schema::model_type, {whole(schema::scene_id, 1)}));
    root = root.set(node(
        40U, schema::light_type, {whole(schema::scene_id, 1)}));
    root = root.set(node(
        50U, schema::object_type, {whole(schema::scene_id, 2)}));
    return root;
}

[[nodiscard]] scene::SelectionSetErrorCode failure_code(
    const scene::SelectionSetPlanResult& result) {
    SET_CHECK(!result.has_value());
    return result.error().code;
}

[[nodiscard]] const doc::PropertyValue* property(
    const doc::Node& node, std::string_view name) {
    const auto found = node.properties.find(name);
    return found == node.properties.end() ? nullptr : &found->second;
}

void create_canonicalizes_members_and_allocates_ascending_free_ids() {
    doc::Root root = two_scene_root();
    root = root.set(node(5U, "unrelated"));
    const std::array targets{
        doc::NodeId{30U}, doc::NodeId{10U}, doc::NodeId{30U},
        doc::NodeId{20U}};
    const auto created = scene::plan_create_selection_set(
        root, doc::NodeId{1U}, "Hero layers", targets,
        doc::NodeId{5U});
    SET_CHECK(created.has_value());
    const auto& plan = created.value();
    SET_CHECK(plan.set == doc::NodeId{6U});
    SET_CHECK(plan.duplicate_inputs == 1U);
    SET_CHECK(plan.puts.size() == 4U);
    SET_CHECK(plan.puts[0U].id == doc::NodeId{6U});
    SET_CHECK(plan.puts[1U].id == doc::NodeId{7U});
    SET_CHECK(plan.puts[2U].id == doc::NodeId{8U});
    SET_CHECK(plan.puts[3U].id == doc::NodeId{9U});
    SET_CHECK(plan.members.size() == 3U);
    SET_CHECK(plan.members[0U]
              == (scene::SelectionSetMember{doc::NodeId{7U},
                                             doc::NodeId{10U}, 0}));
    SET_CHECK(plan.members[1U].target == doc::NodeId{20U});
    SET_CHECK(plan.members[2U].target == doc::NodeId{30U});
    const doc::Node& set = *plan.after.get(doc::NodeId{6U});
    SET_CHECK(set.type == set_schema::set_type);
    SET_CHECK(std::get<core::i64>(*property(set, schema::scene_id)) == 1);
    SET_CHECK(std::get<std::string>(*property(set, set_schema::name))
              == "Hero layers");
    SET_CHECK(root.get(doc::NodeId{7U}) == nullptr);
}

void explicit_scene_ownership_is_required_and_validation_is_atomic() {
    doc::Root stage_less;
    stage_less = stage_less.set(node(
        10U, schema::object_type, {whole(schema::scene_id, 1)}));
    const std::array stage_less_target{doc::NodeId{10U}};
    SET_CHECK(failure_code(scene::plan_create_selection_set(
                  stage_less, doc::NodeId{1U}, "No stage",
                  stage_less_target, doc::NodeId{20U}))
              == scene::SelectionSetErrorCode::InvalidScene);

    doc::Root root = two_scene_root();
    root = root.set(node(9U, schema::object_type));
    root = root.set(node(
        11U, schema::object_type,
        {whole(schema::scene_id, 1), flag(schema::locked, true)}));
    root = root.set(node(
        12U, schema::group_type,
        {whole(schema::scene_id, 1), flag(schema::locked, true)}));
    root = root.set(node(
        13U, schema::model_type,
        {whole(schema::scene_id, 1), whole(schema::parent_group, 12)}));
    root = root.set(node(
        60U, schema::timeline_lane_type,
        {whole(schema::scene_id, 1), flag(schema::lane_locked, true)}));
    root = root.set(node(
        14U, schema::media_type,
        {whole(schema::scene_id, 1), whole(schema::lane_id, 60)}));

    const auto attempt = [&root](doc::NodeId target) {
        const std::array selection{target};
        return scene::plan_create_selection_set(
            root, doc::NodeId{1U}, "Set", selection, doc::NodeId{100U});
    };
    SET_CHECK(failure_code(attempt(doc::NodeId{99U}))
              == scene::SelectionSetErrorCode::TargetMissing);
    SET_CHECK(failure_code(attempt(doc::NodeId{9U}))
              == scene::SelectionSetErrorCode::InvalidTarget);
    SET_CHECK(failure_code(attempt(doc::NodeId{1U}))
              == scene::SelectionSetErrorCode::UnsupportedTarget);
    SET_CHECK(failure_code(attempt(doc::NodeId{50U}))
              == scene::SelectionSetErrorCode::CrossSceneTarget);
    SET_CHECK(failure_code(attempt(doc::NodeId{11U}))
              == scene::SelectionSetErrorCode::TargetLocked);
    SET_CHECK(failure_code(attempt(doc::NodeId{13U}))
              == scene::SelectionSetErrorCode::TargetLocked);
    SET_CHECK(failure_code(attempt(doc::NodeId{14U}))
              == scene::SelectionSetErrorCode::TargetLocked);
    const std::array one{doc::NodeId{10U}};
    SET_CHECK(failure_code(scene::plan_create_selection_set(
                  root, doc::NodeId{99U}, "Set", one, doc::NodeId{100U}))
              == scene::SelectionSetErrorCode::InvalidScene);
    SET_CHECK(failure_code(scene::plan_create_selection_set(
                  root, doc::NodeId{1U}, "", one, doc::NodeId{100U}))
              == scene::SelectionSetErrorCode::InvalidName);
    const std::string invalid_utf8{"\xc0\x80", 2U};
    SET_CHECK(failure_code(scene::plan_create_selection_set(
                  root, doc::NodeId{1U}, invalid_utf8, one,
                  doc::NodeId{100U}))
              == scene::SelectionSetErrorCode::InvalidName);
    const std::string oversized_name(
        scene::max_selection_set_name_bytes + 1U, 'n');
    SET_CHECK(failure_code(scene::plan_create_selection_set(
                  root, doc::NodeId{1U}, oversized_name, one,
                  doc::NodeId{100U}))
              == scene::SelectionSetErrorCode::NameTooLong);
    SET_CHECK(failure_code(scene::plan_create_selection_set(
                  root, doc::NodeId{1U}, "Set", {}, doc::NodeId{100U}))
              == scene::SelectionSetErrorCode::EmptySelection);
    SET_CHECK(failure_code(scene::plan_create_selection_set(
                  root, doc::NodeId{1U}, "Set", one, doc::NodeId{}))
              == scene::SelectionSetErrorCode::InvalidFirstId);

    doc::Root over_capacity = root;
    std::vector<doc::NodeId> too_many;
    for (std::size_t index = 0U;
         index <= scene::max_selection_set_members; ++index) {
        const doc::NodeId id{1'000U + static_cast<core::u64>(index)};
        over_capacity = over_capacity.set(node(
            id.value(), schema::object_type,
            {whole(schema::scene_id, 1)}));
        too_many.push_back(id);
    }
    SET_CHECK(failure_code(scene::plan_create_selection_set(
                  over_capacity, doc::NodeId{1U}, "Too many", too_many,
                  doc::NodeId{2'000U}))
              == scene::SelectionSetErrorCode::MemberCapacity);
}

void replace_retains_targets_recycles_ids_and_emits_dense_order() {
    doc::Root root = two_scene_root();
    root = root.set(set_node(100U, 1U, "Working"));
    root = root.set(member_node(101U, 100U, 10U, 0));
    root = root.set(member_node(102U, 100U, 20U, 1));
    // A malformed associated member is still safely recyclable/deletable.
    root = root.set(node(
        103U, set_schema::member_type,
        {whole(set_schema::set_id, 100),
         whole(set_schema::member_order, 2)}));
    const std::array targets{
        doc::NodeId{30U}, doc::NodeId{20U}, doc::NodeId{30U}};
    const auto replaced = scene::plan_replace_selection_set(
        root, doc::NodeId{1U}, doc::NodeId{100U}, targets,
        doc::NodeId{100U});
    SET_CHECK(replaced.has_value());
    const auto& plan = replaced.value();
    SET_CHECK(plan.duplicate_inputs == 1U);
    SET_CHECK(plan.members.size() == 2U);
    SET_CHECK(plan.members[0U]
              == (scene::SelectionSetMember{doc::NodeId{102U},
                                             doc::NodeId{20U}, 0}));
    SET_CHECK(plan.members[1U]
              == (scene::SelectionSetMember{doc::NodeId{101U},
                                             doc::NodeId{30U}, 1}));
    SET_CHECK(plan.puts.size() == 2U);
    SET_CHECK(plan.puts[0U].id == doc::NodeId{101U});
    SET_CHECK(plan.puts[1U].id == doc::NodeId{102U});
    SET_CHECK((plan.erases
               == std::vector<doc::NodeId>{doc::NodeId{103U}}));
    SET_CHECK(plan.after.get(doc::NodeId{103U}) == nullptr);
    SET_CHECK(std::get<std::string>(*property(
                  *plan.after.get(doc::NodeId{100U}), set_schema::name))
              == "Working");

    const std::array same{doc::NodeId{20U}, doc::NodeId{30U}};
    const auto no_op = scene::plan_replace_selection_set(
        plan.after, doc::NodeId{1U}, doc::NodeId{100U}, same,
        doc::NodeId{200U});
    SET_CHECK(no_op.has_value());
    SET_CHECK(no_op.value().puts.empty());
    SET_CHECK(no_op.value().erases.empty());
    SET_CHECK(no_op.value().after.same_snapshot(plan.after));
}

void replace_allocates_only_excess_members_after_existing_ids() {
    doc::Root root = two_scene_root();
    root = root.set(set_node(100U, 1U, "Growing"));
    root = root.set(member_node(101U, 100U, 10U, 0));
    root = root.set(node(102U, "occupied"));
    const std::array targets{
        doc::NodeId{10U}, doc::NodeId{20U}, doc::NodeId{30U}};
    const auto replaced = scene::plan_replace_selection_set(
        root, doc::NodeId{1U}, doc::NodeId{100U}, targets,
        doc::NodeId{100U});
    SET_CHECK(replaced.has_value());
    SET_CHECK(replaced.value().members[0U].node == doc::NodeId{101U});
    SET_CHECK(replaced.value().members[1U].node == doc::NodeId{103U});
    SET_CHECK(replaced.value().members[2U].node == doc::NodeId{104U});
    SET_CHECK(replaced.value().puts.size() == 2U);
    SET_CHECK(replaced.value().puts[0U].id == doc::NodeId{103U});
    SET_CHECK(replaced.value().puts[1U].id == doc::NodeId{104U});
}

void rename_and_delete_are_replacement_ready_and_cascade_only_members() {
    doc::Root root = two_scene_root();
    root = root.set(set_node(100U, 1U, "Before"));
    root = root.set(member_node(101U, 100U, 10U, 0));
    root = root.set(node(
        102U, set_schema::member_type,
        {whole(set_schema::set_id, 100)}));
    root = root.set(member_node(103U, 999U, 20U, 0));
    const auto renamed = scene::plan_rename_selection_set(
        root, doc::NodeId{1U}, doc::NodeId{100U}, "After");
    SET_CHECK(renamed.has_value());
    SET_CHECK(renamed.value().puts.size() == 1U);
    SET_CHECK(renamed.value().puts.front().id == doc::NodeId{100U});
    SET_CHECK(std::get<std::string>(*property(
                  *renamed.value().after.get(doc::NodeId{100U}),
                  set_schema::name))
              == "After");
    SET_CHECK(renamed.value().after.get(doc::NodeId{101U}) != nullptr);

    const auto same = scene::plan_rename_selection_set(
        renamed.value().after, doc::NodeId{1U}, doc::NodeId{100U},
        "After");
    SET_CHECK(same.has_value());
    SET_CHECK(same.value().puts.empty());
    SET_CHECK(same.value().after.same_snapshot(renamed.value().after));

    const auto deleted = scene::plan_delete_selection_set(
        renamed.value().after, doc::NodeId{1U}, doc::NodeId{100U});
    SET_CHECK(deleted.has_value());
    SET_CHECK((deleted.value().erases
               == std::vector<doc::NodeId>{
                   doc::NodeId{100U}, doc::NodeId{101U},
                   doc::NodeId{102U}}));
    SET_CHECK(deleted.value().after.get(doc::NodeId{100U}) == nullptr);
    SET_CHECK(deleted.value().after.get(doc::NodeId{101U}) == nullptr);
    SET_CHECK(deleted.value().after.get(doc::NodeId{10U}) != nullptr);
    SET_CHECK(deleted.value().after.get(doc::NodeId{103U}) != nullptr);
}

void recall_is_local_ordered_and_reports_every_skipped_member() {
    doc::Root root = two_scene_root();
    root = root.set(set_node(100U, 1U, "Recall"));
    root = root.set(node(
        11U, schema::object_type,
        {whole(schema::scene_id, 1), flag(schema::locked, true)}));
    root = root.set(node(
        14U, schema::group_type, {whole(schema::scene_id, 1)}));
    root = root.set(node(
        15U, schema::model_type,
        {whole(schema::scene_id, 1), whole(schema::parent_group, 999)}));
    root = root.set(member_node(101U, 100U, 10U, 20));
    root = root.set(member_node(102U, 100U, 99U, 10));
    root = root.set(member_node(103U, 100U, 11U, 30));
    root = root.set(member_node(104U, 100U, 50U, 40));
    root = root.set(member_node(105U, 100U, 1U, 50));
    root = root.set(member_node(106U, 100U, 10U, 60));
    root = root.set(node(
        107U, set_schema::member_type,
        {whole(set_schema::set_id, 100),
         whole(set_schema::target_id, 14)}));
    root = root.set(member_node(108U, 100U, 14U, 5));
    root = root.set(member_node(109U, 100U, 15U, 15));

    const auto recalled = scene::recall_selection_set(
        root, doc::NodeId{1U}, doc::NodeId{100U});
    SET_CHECK(recalled.has_value());
    const auto& value = recalled.value();
    SET_CHECK(value.set.name == "Recall");
    SET_CHECK((value.targets
               == std::vector<doc::NodeId>{
                   doc::NodeId{14U}, doc::NodeId{10U}}));
    SET_CHECK(value.primary == doc::NodeId{14U});
    SET_CHECK(value.skips.size() == 7U);
    SET_CHECK(value.skips[0U].reason
              == scene::SelectionSetRecallSkipReason::MissingTarget);
    SET_CHECK(value.skips[1U].reason
              == scene::SelectionSetRecallSkipReason::InvalidTarget);
    SET_CHECK(value.skips[2U].reason
              == scene::SelectionSetRecallSkipReason::LockedTarget);
    SET_CHECK(value.skips[3U].reason
              == scene::SelectionSetRecallSkipReason::CrossSceneTarget);
    SET_CHECK(value.skips[4U].reason
              == scene::SelectionSetRecallSkipReason::UnsupportedTarget);
    SET_CHECK(value.skips[5U].reason
              == scene::SelectionSetRecallSkipReason::DuplicateTarget);
    SET_CHECK(value.skips[6U].reason
              == scene::SelectionSetRecallSkipReason::MalformedMember);
    SET_CHECK(value.set.members.size() == 8U);
    SET_CHECK(root.get(doc::NodeId{100U}) != nullptr);

    const auto catalog = scene::selection_sets(root, doc::NodeId{1U});
    SET_CHECK(catalog.has_value());
    SET_CHECK(catalog.value().sets.size() == 1U);
    SET_CHECK(catalog.value().sets.front().node == doc::NodeId{100U});
    SET_CHECK(catalog.value().sets.front().member_count == 9U);
    SET_CHECK(catalog.value().sets.front().missing_count == 1U);
    SET_CHECK(catalog.value().diagnostics.empty());
}

void set_identity_scene_and_id_boundaries_fail_explicitly() {
    doc::Root root = two_scene_root();
    root = root.set(set_node(100U, 1U, "Set"));
    const std::array selection{doc::NodeId{10U}};
    SET_CHECK(failure_code(scene::plan_rename_selection_set(
                  root, doc::NodeId{2U}, doc::NodeId{100U}, "Other"))
              == scene::SelectionSetErrorCode::CrossSceneSet);
    SET_CHECK(failure_code(scene::plan_delete_selection_set(
                  root, doc::NodeId{1U}, doc::NodeId{99U}))
              == scene::SelectionSetErrorCode::SetNotFound);
    SET_CHECK(failure_code(scene::plan_delete_selection_set(
                  root, doc::NodeId{1U}, doc::NodeId{10U}))
              == scene::SelectionSetErrorCode::NotASelectionSet);

    constexpr core::u64 maximum =
        static_cast<core::u64>(std::numeric_limits<core::i64>::max());
    root = root.set(node(maximum, "occupied"));
    SET_CHECK(failure_code(scene::plan_create_selection_set(
                  root, doc::NodeId{1U}, "Set", selection,
                  doc::NodeId{maximum}))
              == scene::SelectionSetErrorCode::IdExhausted);
    SET_CHECK(failure_code(scene::plan_create_selection_set(
                  root, doc::NodeId{1U}, "Set", selection,
                  doc::NodeId{maximum + 1U}))
              == scene::SelectionSetErrorCode::InvalidFirstId);

    doc::Root malformed = two_scene_root();
    malformed = malformed.set(node(
        100U, set_schema::set_type,
        {whole(schema::scene_id, 1)}));
    SET_CHECK(failure_code(scene::plan_replace_selection_set(
                  malformed, doc::NodeId{1U}, doc::NodeId{100U},
                  selection, doc::NodeId{200U}))
              == scene::SelectionSetErrorCode::InvalidSet);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 7U> cases{{
    {"create canonicalizes members and allocates ascending free ids",
     &create_canonicalizes_members_and_allocates_ascending_free_ids},
    {"explicit scene ownership is required and validation is atomic",
     &explicit_scene_ownership_is_required_and_validation_is_atomic},
    {"replace retains targets recycles ids and emits dense order",
     &replace_retains_targets_recycles_ids_and_emits_dense_order},
    {"replace allocates only excess members after existing ids",
     &replace_allocates_only_excess_members_after_existing_ids},
    {"rename and delete are replacement ready and cascade only members",
     &rename_and_delete_are_replacement_ready_and_cascade_only_members},
    {"recall is local ordered and reports every skipped member",
     &recall_is_local_ordered_and_reports_every_skipped_member},
    {"set identity scene and id boundaries fail explicitly",
     &set_identity_scene_and_id_boundaries_fail_explicitly},
}};

} // namespace

int main() {
    std::size_t failures{};
    for (const Case& test : cases) {
        try {
            std::cout << "[run ] " << test.name << std::endl;
            test.function();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[fail] " << test.name << ": "
                      << exception.what() << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
