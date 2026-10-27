#include "scene_duplicate.hpp"

#include <mediaserver/scene/linked_scene.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace mediaserver::app {
namespace {

[[nodiscard]] bool duplicable_type(std::string_view type) noexcept {
    return type == scene::schema::object_type ||
           type == scene::schema::media_type ||
           type == scene::schema::model_type ||
           type == scene::schema::light_type ||
           type == scene::schema::camera_type;
}

[[nodiscard]] bool unlocked(const doc::Node& node) noexcept {
    const auto found = node.properties.find(scene::schema::locked);
    if (found == node.properties.end()) {
        return true;
    }
    const bool* const held = std::get_if<bool>(&found->second);
    return held != nullptr && !*held;
}

[[nodiscard]] std::optional<doc::NodeId>
reference(const doc::Node& node, std::string_view name) noexcept {
    const auto found = node.properties.find(name);
    if (found == node.properties.end()) {
        return std::nullopt;
    }
    const std::int64_t* const held = std::get_if<std::int64_t>(&found->second);
    if (held == nullptr || *held <= 0) {
        return std::nullopt;
    }
    return doc::NodeId{static_cast<core::u64>(*held)};
}

// The properties through which a dependent names what it belongs to.
constexpr std::array<std::string_view, 4U> owner_properties{
    scene::schema::effect_target, scene::schema::channel_target,
    scene::schema::mask_target, scene::schema::mask_id};

} // namespace

std::optional<SceneDuplicatePlan>
duplicate_in_place(const doc::Root& document,
                   std::span<const doc::NodeId> selection,
                   core::u64 next_node) {
    // Original ids in the order they will be copied: the elements first,
    // then whatever hangs off them, found until nothing new turns up (a
    // channel on an effect on a clip is two steps away).
    std::vector<doc::NodeId> originals;
    for (const doc::NodeId id : selection) {
        const doc::Node* const node = document.get(id);
        if (node == nullptr || !duplicable_type(node->type) || !unlocked(*node) ||
            scene::is_linked_occurrence(*node) ||
            std::find(originals.begin(), originals.end(), id) !=
                originals.end()) {
            continue;
        }
        originals.push_back(id);
    }
    if (originals.empty()) {
        return std::nullopt;
    }
    const std::size_t elements = originals.size();
    const doc::Delta all = doc::diff(doc::Root{}, document);
    for (bool grew = true; grew;) {
        grew = false;
        for (const doc::NodeChange& change : all.changes) {
            if (change.after == nullptr ||
                std::find(originals.begin(), originals.end(), change.id) !=
                    originals.end()) {
                continue;
            }
            for (const std::string_view property : owner_properties) {
                const auto owner = reference(*change.after, property);
                if (owner.has_value() &&
                    std::find(originals.begin(), originals.end(), *owner) !=
                        originals.end()) {
                    originals.push_back(change.id);
                    grew = true;
                    break;
                }
            }
        }
    }

    std::vector<std::pair<doc::NodeId, doc::NodeId>> ids;
    ids.reserve(originals.size());
    core::u64 cursor = next_node;
    constexpr core::u64 signed_max =
        static_cast<core::u64>(std::numeric_limits<std::int64_t>::max());
    for (const doc::NodeId original : originals) {
        while (document.contains(doc::NodeId{cursor})) {
            ++cursor;
        }
        if (cursor == 0U || cursor > signed_max) {
            return std::nullopt;
        }
        ids.emplace_back(original, doc::NodeId{cursor});
        ++cursor;
    }
    const auto remapped = [&ids](doc::NodeId original) -> std::optional<doc::NodeId> {
        for (const auto& [from, to] : ids) {
            if (from == original) {
                return to;
            }
        }
        return std::nullopt;
    };

    SceneDuplicatePlan plan{};
    plan.document = document;
    plan.next_node = cursor;
    for (std::size_t index = 0U; index < ids.size(); ++index) {
        doc::Node copy = *document.get(ids[index].first);
        copy.id = ids[index].second;
        for (const std::string_view property : owner_properties) {
            if (const auto owner = reference(copy, property)) {
                if (const auto moved = remapped(*owner)) {
                    copy.properties.insert_or_assign(
                        std::string{property},
                        doc::PropertyValue{
                            static_cast<std::int64_t>(moved->value())});
                }
            }
        }
        plan.document = plan.document.set(std::move(copy));
        if (index < elements) {
            plan.created.push_back(ids[index].second);
        }
    }
    return plan;
}

} // namespace mediaserver::app
