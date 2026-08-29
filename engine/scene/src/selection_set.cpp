#include <mediaserver/scene/selection_set.hpp>

#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace mediaserver::scene {
namespace {

constexpr core::u64 max_reference_id =
    static_cast<core::u64>(std::numeric_limits<core::i64>::max());

[[nodiscard]] SelectionSetError error(SelectionSetErrorCode code,
                                      doc::NodeId subject = {}) noexcept {
    return SelectionSetError{code, subject};
}

[[nodiscard]] bool valid_utf8(std::string_view value) noexcept {
    std::size_t index{};
    while (index < value.size()) {
        const auto first = static_cast<std::uint8_t>(
            static_cast<unsigned char>(value[index]));
        if (first <= 0x7fU) {
            ++index;
            continue;
        }
        std::size_t continuation_count{};
        std::uint32_t code_point{};
        std::uint32_t minimum{};
        if ((first & 0xe0U) == 0xc0U) {
            continuation_count = 1U;
            code_point = first & 0x1fU;
            minimum = 0x80U;
        } else if ((first & 0xf0U) == 0xe0U) {
            continuation_count = 2U;
            code_point = first & 0x0fU;
            minimum = 0x800U;
        } else if ((first & 0xf8U) == 0xf0U) {
            continuation_count = 3U;
            code_point = first & 0x07U;
            minimum = 0x10000U;
        } else {
            return false;
        }
        if (continuation_count > value.size() - index - 1U) {
            return false;
        }
        for (std::size_t offset = 1U; offset <= continuation_count;
             ++offset) {
            const auto continuation = static_cast<std::uint8_t>(
                static_cast<unsigned char>(value[index + offset]));
            if ((continuation & 0xc0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (continuation & 0x3fU);
        }
        if (code_point < minimum || code_point > 0x10ffffU
            || (code_point >= 0xd800U && code_point <= 0xdfffU)) {
            return false;
        }
        index += continuation_count + 1U;
    }
    return true;
}

[[nodiscard]] const doc::PropertyValue* find(
    const doc::PropertyMap& properties, std::string_view name) noexcept {
    const auto found = properties.find(name);
    return found == properties.end() ? nullptr : &found->second;
}

[[nodiscard]] bool read_optional_reference(
    const doc::Node& node, std::string_view property,
    std::optional<doc::NodeId>& value) noexcept {
    const auto* const held = find(node.properties, property);
    if (held == nullptr) {
        value.reset();
        return true;
    }
    const auto* const whole = std::get_if<core::i64>(held);
    if (whole == nullptr || *whole < 0) {
        return false;
    }
    value = doc::NodeId{static_cast<core::u64>(*whole)};
    return true;
}

[[nodiscard]] bool read_required_reference(const doc::Node& node,
                                           std::string_view property,
                                           doc::NodeId& value) noexcept {
    std::optional<doc::NodeId> held;
    if (!read_optional_reference(node, property, held)
        || !held.has_value()) {
        return false;
    }
    value = *held;
    return true;
}

[[nodiscard]] bool read_integer(const doc::Node& node,
                                std::string_view property,
                                core::i64& value) noexcept {
    const auto* const held = find(node.properties, property);
    const auto* const whole = held == nullptr
        ? nullptr
        : std::get_if<core::i64>(held);
    if (whole == nullptr) {
        return false;
    }
    value = *whole;
    return true;
}

[[nodiscard]] bool read_boolean(const doc::Node& node,
                                std::string_view property, bool fallback,
                                bool& value) noexcept {
    const auto* const held = find(node.properties, property);
    if (held == nullptr) {
        value = fallback;
        return true;
    }
    if (const auto* const boolean = std::get_if<bool>(held)) {
        value = *boolean;
        return true;
    }
    if (const auto* const whole = std::get_if<core::i64>(held)) {
        value = *whole != 0;
        return true;
    }
    if (const auto* const real = std::get_if<double>(held)) {
        if (!std::isfinite(*real)) {
            return false;
        }
        value = *real >= 0.5;
        return true;
    }
    return false;
}

void write_reference(doc::PropertyMap& properties, std::string_view name,
                     doc::NodeId value) {
    properties.insert_or_assign(
        std::string{name}, static_cast<core::i64>(value.value()));
}

class DocumentIndex final {
public:
    explicit DocumentIndex(const doc::Root& document) {
        for (const doc::NodeChange& change :
             doc::diff(doc::Root{}, document).changes) {
            if (change.after != nullptr) {
                nodes.emplace(change.id, *change.after);
            }
        }
        for (const auto& [id, node] : nodes) {
            if (node.type == schema::stage_type) {
                scenes.insert(id);
            }
        }
    }

    [[nodiscard]] const doc::Node* node(doc::NodeId id) const noexcept {
        const auto found = nodes.find(id);
        return found == nodes.end() ? nullptr : &found->second;
    }

    [[nodiscard]] bool valid_scene(SceneId scene) const noexcept {
        return scenes.contains(scene);
    }

    std::map<doc::NodeId, doc::Node> nodes;
    std::set<SceneId> scenes;
};

[[nodiscard]] bool direct_scene(const doc::Node& node,
                                SceneId& scene) noexcept {
    return read_required_reference(node, schema::scene_id, scene);
}

[[nodiscard]] bool is_drawable(std::string_view type) noexcept {
    return type == schema::object_type || type == schema::media_type
        || type == schema::model_type;
}

[[nodiscard]] bool is_selectable(std::string_view type) noexcept {
    return type == schema::group_type || is_drawable(type)
        || type == schema::light_type;
}

[[nodiscard]] std::optional<SelectionSetErrorCode> target_status(
    const DocumentIndex& index, SceneId owner,
    doc::NodeId target_id) noexcept {
    const doc::Node* target = index.node(target_id);
    if (target == nullptr) {
        return SelectionSetErrorCode::TargetMissing;
    }
    if (target_id.value() > max_reference_id) {
        return SelectionSetErrorCode::InvalidReference;
    }
    if (!is_selectable(target->type)) {
        return SelectionSetErrorCode::UnsupportedTarget;
    }

    SceneId target_scene{};
    if (!direct_scene(*target, target_scene)) {
        return SelectionSetErrorCode::InvalidTarget;
    }
    if (target_scene != owner) {
        return SelectionSetErrorCode::CrossSceneTarget;
    }

    bool locked{};
    if (!read_boolean(*target, schema::locked, false, locked)) {
        return SelectionSetErrorCode::InvalidTarget;
    }
    if (locked) {
        return SelectionSetErrorCode::TargetLocked;
    }

    if (target->type == schema::media_type) {
        std::optional<doc::NodeId> lane;
        if (!read_optional_reference(*target, schema::lane_id, lane)) {
            return SelectionSetErrorCode::InvalidTarget;
        }
        if (lane.has_value()) {
            const doc::Node* lane_node = index.node(*lane);
            SceneId lane_scene{};
            if (lane_node == nullptr
                || lane_node->type != schema::timeline_lane_type
                || !direct_scene(*lane_node, lane_scene)
                || lane_scene != owner
                || !read_boolean(
                    *lane_node, schema::lane_locked, false, locked)) {
                return SelectionSetErrorCode::InvalidTarget;
            }
            if (locked) {
                return SelectionSetErrorCode::TargetLocked;
            }
        }
    }

    if (target->type != schema::group_type && !is_drawable(target->type)) {
        return std::nullopt;
    }
    std::set<doc::NodeId> visited;
    const doc::Node* current = target;
    while (true) {
        if (!visited.insert(current->id).second) {
            return SelectionSetErrorCode::InvalidTarget;
        }
        std::optional<doc::NodeId> parent;
        if (!read_optional_reference(*current, schema::parent_group, parent)) {
            return SelectionSetErrorCode::InvalidTarget;
        }
        if (!parent.has_value()) {
            break;
        }
        current = index.node(*parent);
        SceneId parent_scene{};
        if (current == nullptr || current->type != schema::group_type
            || !direct_scene(*current, parent_scene)
            || parent_scene != owner
            || !read_boolean(*current, schema::locked, false, locked)) {
            return SelectionSetErrorCode::InvalidTarget;
        }
        if (locked) {
            return SelectionSetErrorCode::TargetLocked;
        }
    }
    return std::nullopt;
}

[[nodiscard]] SelectionSetErrorCode authoring_code(
    SelectionSetErrorCode status) noexcept {
    return status;
}

struct CanonicalTargets final {
    std::vector<doc::NodeId> values;
    std::size_t duplicate_inputs{};
};

[[nodiscard]] core::Result<CanonicalTargets, SelectionSetError>
canonical_targets(const DocumentIndex& index, SceneId scene,
                  std::span<const doc::NodeId> targets) {
    using Result = core::Result<CanonicalTargets, SelectionSetError>;
    CanonicalTargets canonical{};
    canonical.values.assign(targets.begin(), targets.end());
    std::sort(canonical.values.begin(), canonical.values.end());
    const auto unique =
        std::unique(canonical.values.begin(), canonical.values.end());
    canonical.duplicate_inputs =
        static_cast<std::size_t>(canonical.values.end() - unique);
    canonical.values.erase(unique, canonical.values.end());
    if (canonical.values.empty()) {
        return Result::failure(error(SelectionSetErrorCode::EmptySelection));
    }
    if (canonical.values.size() > max_selection_set_members) {
        return Result::failure(error(SelectionSetErrorCode::MemberCapacity));
    }
    for (const doc::NodeId target : canonical.values) {
        if (const auto status = target_status(index, scene, target)) {
            return Result::failure(error(authoring_code(*status), target));
        }
    }
    return Result::success(std::move(canonical));
}

[[nodiscard]] std::optional<SelectionSetError> validate_scene(
    const DocumentIndex& index, SceneId scene) noexcept {
    if (scene.value() > max_reference_id || !index.valid_scene(scene)) {
        return error(SelectionSetErrorCode::InvalidScene, scene);
    }
    return std::nullopt;
}

struct ParsedSet final {
    doc::Node node;
    SceneId scene{};
    std::string name;
};

[[nodiscard]] core::Result<ParsedSet, SelectionSetError> parse_set(
    const DocumentIndex& index, SceneId requested,
    doc::NodeId set_id) {
    using Result = core::Result<ParsedSet, SelectionSetError>;
    const doc::Node* const set = index.node(set_id);
    if (set == nullptr) {
        return Result::failure(
            error(SelectionSetErrorCode::SetNotFound, set_id));
    }
    if (set->type != selection_set_schema::set_type) {
        return Result::failure(
            error(SelectionSetErrorCode::NotASelectionSet, set_id));
    }
    if (set_id.value() > max_reference_id) {
        return Result::failure(
            error(SelectionSetErrorCode::InvalidSet, set_id));
    }
    doc::NodeId owner{};
    const auto* const name_value =
        find(set->properties, selection_set_schema::name);
    const auto* const name = name_value == nullptr
        ? nullptr
        : std::get_if<std::string>(name_value);
    if (!read_required_reference(*set, schema::scene_id, owner)
        || name == nullptr || name->empty() || !valid_utf8(*name)
        || name->size() > max_selection_set_name_bytes) {
        return Result::failure(
            error(SelectionSetErrorCode::InvalidSet, set_id));
    }
    if (owner != requested) {
        return Result::failure(
            error(SelectionSetErrorCode::CrossSceneSet, set_id));
    }
    return Result::success(ParsedSet{*set, owner, *name});
}

struct RawMember final {
    doc::Node node;
    std::optional<doc::NodeId> target;
    std::optional<core::i64> order;
};

[[nodiscard]] std::vector<RawMember> members_for(
    const DocumentIndex& index, doc::NodeId set_id) {
    std::vector<RawMember> result;
    for (const auto& [id, node] : index.nodes) {
        if (node.type != selection_set_schema::member_type) {
            continue;
        }
        doc::NodeId owner{};
        if (!read_required_reference(
                node, selection_set_schema::set_id, owner)
            || owner != set_id) {
            continue;
        }
        RawMember member{};
        member.node = node;
        doc::NodeId target{};
        if (read_required_reference(
                node, selection_set_schema::target_id, target)) {
            member.target = target;
        }
        core::i64 order{};
        if (read_integer(
                node, selection_set_schema::member_order, order)) {
            member.order = order;
        }
        result.push_back(std::move(member));
    }
    std::stable_sort(
        result.begin(), result.end(),
        [](const RawMember& left, const RawMember& right) noexcept {
            if (left.order.has_value() != right.order.has_value()) {
                return left.order.has_value();
            }
            if (left.order.has_value() && left.order != right.order) {
                return *left.order < *right.order;
            }
            return left.node.id < right.node.id;
        });
    return result;
}

[[nodiscard]] bool valid_first_id(doc::NodeId first_id) noexcept {
    return first_id.value() != 0U && first_id.value() <= max_reference_id;
}

[[nodiscard]] core::Result<std::vector<doc::NodeId>, SelectionSetError>
allocate_ids(const doc::Root& document, doc::NodeId first_id,
             std::size_t count) {
    using Result =
        core::Result<std::vector<doc::NodeId>, SelectionSetError>;
    if (!valid_first_id(first_id)) {
        return Result::failure(
            error(SelectionSetErrorCode::InvalidFirstId, first_id));
    }
    std::vector<doc::NodeId> result;
    result.reserve(count);
    core::u64 candidate = first_id.value();
    while (result.size() < count) {
        while (document.contains(doc::NodeId{candidate})) {
            if (candidate == max_reference_id) {
                return Result::failure(
                    error(SelectionSetErrorCode::IdExhausted, first_id));
            }
            ++candidate;
        }
        result.push_back(doc::NodeId{candidate});
        if (result.size() != count) {
            if (candidate == max_reference_id) {
                return Result::failure(
                    error(SelectionSetErrorCode::IdExhausted, first_id));
            }
            ++candidate;
        }
    }
    return Result::success(std::move(result));
}

[[nodiscard]] doc::Node make_member(doc::NodeId member_id,
                                    doc::NodeId set_id,
                                    doc::NodeId target,
                                    core::i64 order) {
    doc::Node member{};
    member.id = member_id;
    member.type = std::string{selection_set_schema::member_type};
    write_reference(
        member.properties, selection_set_schema::set_id, set_id);
    write_reference(
        member.properties, selection_set_schema::target_id, target);
    member.properties.emplace(
        std::string{selection_set_schema::member_order}, order);
    return member;
}

void finalize_plan(const doc::Root& document, SelectionSetPlan& plan) {
    std::sort(plan.puts.begin(), plan.puts.end(),
              [](const doc::Node& left, const doc::Node& right) noexcept {
                  return left.id < right.id;
              });
    std::sort(plan.erases.begin(), plan.erases.end());
    plan.erases.erase(
        std::unique(plan.erases.begin(), plan.erases.end()),
        plan.erases.end());

    std::map<doc::NodeId, std::optional<doc::Node>> edits;
    for (const doc::NodeId id : plan.erases) {
        edits.insert_or_assign(id, std::nullopt);
    }
    for (const doc::Node& node : plan.puts) {
        edits.insert_or_assign(node.id, node);
    }
    plan.after = document;
    for (auto& [id, edit] : edits) {
        if (edit.has_value()) {
            plan.after = plan.after.set(std::move(*edit));
        } else if (const auto erased = plan.after.erase(id)) {
            plan.after = std::move(*erased);
        }
    }
}

[[nodiscard]] SelectionSetRecallSkipReason recall_reason(
    SelectionSetErrorCode status) noexcept {
    switch (status) {
    case SelectionSetErrorCode::TargetMissing:
        return SelectionSetRecallSkipReason::MissingTarget;
    case SelectionSetErrorCode::UnsupportedTarget:
    case SelectionSetErrorCode::InvalidReference:
        return SelectionSetRecallSkipReason::UnsupportedTarget;
    case SelectionSetErrorCode::CrossSceneTarget:
        return SelectionSetRecallSkipReason::CrossSceneTarget;
    case SelectionSetErrorCode::TargetLocked:
        return SelectionSetRecallSkipReason::LockedTarget;
    case SelectionSetErrorCode::InvalidTarget:
        return SelectionSetRecallSkipReason::InvalidTarget;
    case SelectionSetErrorCode::InvalidScene:
    case SelectionSetErrorCode::InvalidName:
    case SelectionSetErrorCode::NameTooLong:
    case SelectionSetErrorCode::EmptySelection:
    case SelectionSetErrorCode::MemberCapacity:
    case SelectionSetErrorCode::InvalidFirstId:
    case SelectionSetErrorCode::IdExhausted:
    case SelectionSetErrorCode::SetNotFound:
    case SelectionSetErrorCode::NotASelectionSet:
    case SelectionSetErrorCode::InvalidSet:
    case SelectionSetErrorCode::CrossSceneSet:
    case SelectionSetErrorCode::OrderOverflow:
    case SelectionSetErrorCode::AllocationFailed:
        break;
    }
    return SelectionSetRecallSkipReason::InvalidTarget;
}

} // namespace

SelectionSetCatalogResult selection_sets(const doc::Root& document,
                                         SceneId scene) {
    try {
        const DocumentIndex index{document};
        if (const auto invalid = validate_scene(index, scene)) {
            return SelectionSetCatalogResult::failure(*invalid);
        }
        SelectionSetCatalog catalog{};
        for (const auto& [id, node] : index.nodes) {
            if (node.type != selection_set_schema::set_type) {
                continue;
            }
            doc::NodeId owner{};
            if (!read_required_reference(node, schema::scene_id, owner)
                || owner != scene) {
                continue;
            }
            const auto* const name_value =
                find(node.properties, selection_set_schema::name);
            const auto* const name = name_value == nullptr
                ? nullptr
                : std::get_if<std::string>(name_value);
            if (name == nullptr || name->empty() || !valid_utf8(*name)
                || name->size() > max_selection_set_name_bytes
                || id.value() > max_reference_id) {
                catalog.diagnostics.push_back(
                    error(SelectionSetErrorCode::InvalidSet, id));
                continue;
            }
            SelectionSetSummary summary{};
            summary.node = id;
            summary.scene = scene;
            summary.name = *name;
            const std::vector<RawMember> members = members_for(index, id);
            summary.member_count = members.size();
            for (const RawMember& member : members) {
                if (!member.target.has_value()
                    || index.node(*member.target) == nullptr) {
                    ++summary.missing_count;
                }
            }
            catalog.sets.push_back(std::move(summary));
        }
        return SelectionSetCatalogResult::success(std::move(catalog));
    } catch (const std::bad_alloc&) {
        return SelectionSetCatalogResult::failure(
            error(SelectionSetErrorCode::AllocationFailed));
    }
}

SelectionSetPlanResult plan_create_selection_set(
    const doc::Root& document, SceneId scene, std::string_view name,
    std::span<const doc::NodeId> targets, doc::NodeId first_id) {
    try {
        const DocumentIndex index{document};
        if (const auto invalid = validate_scene(index, scene)) {
            return SelectionSetPlanResult::failure(*invalid);
        }
        if (name.empty() || !valid_utf8(name)) {
            return SelectionSetPlanResult::failure(
                error(SelectionSetErrorCode::InvalidName));
        }
        if (name.size() > max_selection_set_name_bytes) {
            return SelectionSetPlanResult::failure(
                error(SelectionSetErrorCode::NameTooLong));
        }
        if (!valid_first_id(first_id)) {
            return SelectionSetPlanResult::failure(
                error(SelectionSetErrorCode::InvalidFirstId, first_id));
        }
        auto canonical = canonical_targets(index, scene, targets);
        if (!canonical.has_value()) {
            return SelectionSetPlanResult::failure(canonical.error());
        }
        auto allocated = allocate_ids(
            document, first_id, canonical.value().values.size() + 1U);
        if (!allocated.has_value()) {
            return SelectionSetPlanResult::failure(allocated.error());
        }

        SelectionSetPlan plan{};
        plan.set = allocated.value().front();
        plan.duplicate_inputs = canonical.value().duplicate_inputs;
        doc::Node set{};
        set.id = plan.set;
        set.type = std::string{selection_set_schema::set_type};
        write_reference(set.properties, schema::scene_id, scene);
        set.properties.emplace(
            std::string{selection_set_schema::name}, std::string{name});
        plan.puts.push_back(std::move(set));
        plan.members.reserve(canonical.value().values.size());
        for (std::size_t index_value = 0U;
             index_value < canonical.value().values.size(); ++index_value) {
            const core::i64 order = static_cast<core::i64>(index_value);
            const doc::NodeId member_id =
                allocated.value()[index_value + 1U];
            const doc::NodeId target = canonical.value().values[index_value];
            plan.puts.push_back(
                make_member(member_id, plan.set, target, order));
            plan.members.push_back(
                SelectionSetMember{member_id, target, order});
        }
        finalize_plan(document, plan);
        return SelectionSetPlanResult::success(std::move(plan));
    } catch (const std::bad_alloc&) {
        return SelectionSetPlanResult::failure(
            error(SelectionSetErrorCode::AllocationFailed));
    }
}

SelectionSetPlanResult plan_replace_selection_set(
    const doc::Root& document, SceneId scene, doc::NodeId set_id,
    std::span<const doc::NodeId> targets, doc::NodeId first_id) {
    try {
        const DocumentIndex index{document};
        if (const auto invalid = validate_scene(index, scene)) {
            return SelectionSetPlanResult::failure(*invalid);
        }
        auto set = parse_set(index, scene, set_id);
        if (!set.has_value()) {
            return SelectionSetPlanResult::failure(set.error());
        }
        if (!valid_first_id(first_id)) {
            return SelectionSetPlanResult::failure(
                error(SelectionSetErrorCode::InvalidFirstId, first_id));
        }
        auto canonical = canonical_targets(index, scene, targets);
        if (!canonical.has_value()) {
            return SelectionSetPlanResult::failure(canonical.error());
        }

        const std::vector<RawMember> existing = members_for(index, set_id);
        std::map<doc::NodeId, doc::NodeId> retained;
        std::vector<doc::NodeId> reusable;
        const std::set<doc::NodeId> wanted(
            canonical.value().values.begin(), canonical.value().values.end());
        for (const RawMember& member : existing) {
            if (member.target.has_value() && wanted.contains(*member.target)
                && retained.try_emplace(
                    *member.target, member.node.id).second) {
                continue;
            }
            reusable.push_back(member.node.id);
        }
        std::sort(reusable.begin(), reusable.end());

        const std::size_t needed =
            canonical.value().values.size() - retained.size();
        const std::size_t allocate_count =
            needed > reusable.size() ? needed - reusable.size() : 0U;
        auto allocated = allocate_ids(document, first_id, allocate_count);
        if (!allocated.has_value()) {
            return SelectionSetPlanResult::failure(allocated.error());
        }

        SelectionSetPlan plan{};
        plan.set = set_id;
        plan.duplicate_inputs = canonical.value().duplicate_inputs;
        std::set<doc::NodeId> used;
        std::size_t reusable_index{};
        std::size_t allocated_index{};
        for (std::size_t index_value = 0U;
             index_value < canonical.value().values.size(); ++index_value) {
            const doc::NodeId target = canonical.value().values[index_value];
            doc::NodeId member_id{};
            const auto kept = retained.find(target);
            if (kept != retained.end()) {
                member_id = kept->second;
            } else if (reusable_index < reusable.size()) {
                member_id = reusable[reusable_index++];
            } else {
                member_id = allocated.value()[allocated_index++];
            }
            used.insert(member_id);
            const core::i64 order = static_cast<core::i64>(index_value);
            doc::Node member =
                make_member(member_id, set_id, target, order);
            const doc::Node* const prior = index.node(member_id);
            if (prior == nullptr || *prior != member) {
                plan.puts.push_back(std::move(member));
            }
            plan.members.push_back(
                SelectionSetMember{member_id, target, order});
        }
        for (const RawMember& member : existing) {
            if (!used.contains(member.node.id)) {
                plan.erases.push_back(member.node.id);
            }
        }
        finalize_plan(document, plan);
        return SelectionSetPlanResult::success(std::move(plan));
    } catch (const std::bad_alloc&) {
        return SelectionSetPlanResult::failure(
            error(SelectionSetErrorCode::AllocationFailed));
    }
}

SelectionSetPlanResult plan_rename_selection_set(
    const doc::Root& document, SceneId scene, doc::NodeId set_id,
    std::string_view name) {
    try {
        const DocumentIndex index{document};
        if (const auto invalid = validate_scene(index, scene)) {
            return SelectionSetPlanResult::failure(*invalid);
        }
        if (name.empty() || !valid_utf8(name)) {
            return SelectionSetPlanResult::failure(
                error(SelectionSetErrorCode::InvalidName, set_id));
        }
        if (name.size() > max_selection_set_name_bytes) {
            return SelectionSetPlanResult::failure(
                error(SelectionSetErrorCode::NameTooLong, set_id));
        }
        auto parsed = parse_set(index, scene, set_id);
        if (!parsed.has_value()) {
            return SelectionSetPlanResult::failure(parsed.error());
        }
        SelectionSetPlan plan{};
        plan.set = set_id;
        doc::Node renamed = parsed.value().node;
        renamed.properties.insert_or_assign(
            std::string{selection_set_schema::name}, std::string{name});
        if (renamed != parsed.value().node) {
            plan.puts.push_back(std::move(renamed));
        }
        for (const RawMember& member : members_for(index, set_id)) {
            if (member.target.has_value() && member.order.has_value()) {
                plan.members.push_back(SelectionSetMember{
                    member.node.id, *member.target, *member.order});
            }
        }
        finalize_plan(document, plan);
        return SelectionSetPlanResult::success(std::move(plan));
    } catch (const std::bad_alloc&) {
        return SelectionSetPlanResult::failure(
            error(SelectionSetErrorCode::AllocationFailed));
    }
}

SelectionSetPlanResult plan_delete_selection_set(
    const doc::Root& document, SceneId scene, doc::NodeId set_id) {
    try {
        const DocumentIndex index{document};
        if (const auto invalid = validate_scene(index, scene)) {
            return SelectionSetPlanResult::failure(*invalid);
        }
        auto parsed = parse_set(index, scene, set_id);
        if (!parsed.has_value()) {
            return SelectionSetPlanResult::failure(parsed.error());
        }
        SelectionSetPlan plan{};
        plan.set = set_id;
        plan.erases.push_back(set_id);
        for (const RawMember& member : members_for(index, set_id)) {
            plan.erases.push_back(member.node.id);
        }
        finalize_plan(document, plan);
        return SelectionSetPlanResult::success(std::move(plan));
    } catch (const std::bad_alloc&) {
        return SelectionSetPlanResult::failure(
            error(SelectionSetErrorCode::AllocationFailed));
    }
}

SelectionSetRecallResult recall_selection_set(
    const doc::Root& document, SceneId scene, doc::NodeId set_id) {
    try {
        const DocumentIndex index{document};
        if (const auto invalid = validate_scene(index, scene)) {
            return SelectionSetRecallResult::failure(*invalid);
        }
        auto parsed = parse_set(index, scene, set_id);
        if (!parsed.has_value()) {
            return SelectionSetRecallResult::failure(parsed.error());
        }

        SelectionSetRecall recalled{};
        recalled.set.node = set_id;
        recalled.set.scene = scene;
        recalled.set.name = parsed.value().name;
        std::set<doc::NodeId> seen;
        for (const RawMember& member : members_for(index, set_id)) {
            if (!member.target.has_value() || !member.order.has_value()) {
                recalled.skips.push_back(SelectionSetRecallSkip{
                    member.node.id,
                    member.target,
                    SelectionSetRecallSkipReason::MalformedMember,
                });
                continue;
            }
            recalled.set.members.push_back(SelectionSetMember{
                member.node.id, *member.target, *member.order});
            if (!seen.insert(*member.target).second) {
                recalled.skips.push_back(SelectionSetRecallSkip{
                    member.node.id,
                    member.target,
                    SelectionSetRecallSkipReason::DuplicateTarget,
                });
                continue;
            }
            if (const auto status =
                    target_status(index, scene, *member.target)) {
                recalled.skips.push_back(SelectionSetRecallSkip{
                    member.node.id,
                    member.target,
                    recall_reason(*status),
                });
                continue;
            }
            recalled.targets.push_back(*member.target);
            if (!recalled.primary.has_value()) {
                recalled.primary = *member.target;
            }
        }
        return SelectionSetRecallResult::success(std::move(recalled));
    } catch (const std::bad_alloc&) {
        return SelectionSetRecallResult::failure(
            error(SelectionSetErrorCode::AllocationFailed));
    }
}

} // namespace mediaserver::scene
