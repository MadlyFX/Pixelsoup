#include "history_labels.hpp"

#include <mediaserver/scene/scene_schema.hpp>

#include <cstdint>

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace mediaserver::app {
namespace {

// A node's state on one side of a step: present with its value, or absent.
using NodeState = std::optional<doc::Node>;

[[nodiscard]] NodeState state_of(const cmd::Payload& payload) {
    if (const auto* put = std::get_if<cmd::PutNode>(&payload)) {
        return put->node;
    }
    return std::nullopt;
}

[[nodiscard]] doc::NodeId target_of(const cmd::Payload& payload) {
    if (const auto* put = std::get_if<cmd::PutNode>(&payload)) {
        return put->node.id;
    }
    return std::get<cmd::EraseNode>(payload).id;
}

struct NetChange final {
    doc::NodeId id;
    NodeState before;
    NodeState after;
};

// Each node's state before the first command and after the last one that
// touched it, in the order the original edit first touched them.
[[nodiscard]] std::vector<NetChange> net_changes(
    std::span<const cmd::Command> commands, bool applied) {
    std::vector<NetChange> changes;
    // A whole-document replace can touch thousands of nodes.
    std::map<doc::NodeId, std::size_t> index;
    const auto record = [&](const cmd::Payload& before,
                            const cmd::Payload& after) {
        const doc::NodeId id = target_of(after);
        const auto [existing, inserted] = index.try_emplace(id, changes.size());
        if (inserted) {
            changes.push_back(NetChange{id, state_of(before), state_of(after)});
        } else {
            changes[existing->second].after = state_of(after);
        }
    };
    if (applied) {
        for (const cmd::Command& command : commands) {
            record(command.inverse, command.forward);
        }
    } else {
        // The undo runs the original commands backwards with the payloads
        // swapped; reading it backwards and swapping again recovers the edit.
        for (auto command = commands.rbegin(); command != commands.rend();
             ++command) {
            record(command->forward, command->inverse);
        }
    }
    return changes;
}

[[nodiscard]] bool is_runtime_property(std::string_view key) noexcept {
    return key.starts_with("asset_commit_");
}

// Strips a component suffix so position_x/y/z read as one "position".
[[nodiscard]] std::string_view property_base(std::string_view key) noexcept {
    while (key.starts_with('_')) {
        key.remove_prefix(1U);
    }
    while (key.ends_with('_')) {
        key.remove_suffix(1U);
    }
    constexpr std::array suffixes{
        std::string_view{"_x"},   std::string_view{"_y"},
        std::string_view{"_z"},   std::string_view{"_r"},
        std::string_view{"_g"},   std::string_view{"_b"},
        std::string_view{"_a"},   std::string_view{"_yaw"},
        std::string_view{"_pitch"}, std::string_view{"_roll"},
    };
    for (const std::string_view suffix : suffixes) {
        if (key.size() > suffix.size() && key.ends_with(suffix)) {
            key.remove_suffix(suffix.size());
            break;
        }
    }
    return key;
}

[[nodiscard]] std::string words(std::string_view identifier) {
    std::string text{identifier};
    std::replace(text.begin(), text.end(), '_', ' ');
    return text;
}

[[nodiscard]] std::string string_property(const doc::Node& node,
                                          std::string_view key) {
    const auto found = node.properties.find(key);
    if (found == node.properties.end()) {
        return {};
    }
    const auto* text = std::get_if<std::string>(&found->second);
    return text == nullptr ? std::string{} : *text;
}

[[nodiscard]] std::string noun_of(const doc::Node& node) {
    struct Named final {
        std::string_view type;
        std::string_view noun;
    };
    constexpr std::array nouns{
        Named{"scene.stage", "scene"},
        Named{"scene.media", "clip"},
        Named{"scene.channel", "animation"},
        Named{"scene.timeline_lane", "track"},
        Named{"scene.timeline_marker", "marker"},
        Named{"scene.selection_set", "selection set"},
        Named{"compositor.edge", "connection"},
        Named{"compositor.scene_layer", "scene layer"},
        Named{"output.surface", "output surface"},
        Named{"hardware.device", "device"},
    };
    for (const Named& named : nouns) {
        if (node.type == named.type) {
            return std::string{named.noun};
        }
    }
    if (node.type == "scene.object") {
        // Box, plane and sphere are all objects; the operator added one of
        // them, not "an object".
        const std::string primitive = string_property(node, "primitive");
        return primitive.empty() ? std::string{"object"} : primitive;
    }
    if (node.type == "scene.effect") {
        const std::string kind = string_property(node, "effect_kind");
        return kind.empty() ? std::string{"effect"} : words(kind) + " effect";
    }
    std::string_view type = node.type;
    const bool compositor_node = type.starts_with("compositor.");
    if (const auto dot = type.rfind('.'); dot != std::string_view::npos) {
        type.remove_prefix(dot + 1U);
    }
    std::string noun = words(type);
    if (compositor_node && noun != "comment") {
        noun += " node";
    }
    return noun.empty() ? std::string{"item"} : noun;
}

// The operator's name for a node, if it has one: a `*_name` or `label`
// property, or the file name of a model or clip source.
[[nodiscard]] std::string name_of(const doc::Node& node) {
    for (const auto& [key, value] : node.properties) {
        const auto* text = std::get_if<std::string>(&value);
        if (text != nullptr && !text->empty() &&
            (key == "label" || std::string_view{key}.ends_with("_name"))) {
            return *text;
        }
    }
    if (node.type == "scene.model" || node.type == "scene.media") {
        std::string source = string_property(node, "source");
        const auto slash = source.find_last_of("/\\");
        if (slash != std::string::npos) {
            source.erase(0U, slash + 1U);
        }
        return source;
    }
    return {};
}

[[nodiscard]] std::string plural(std::string noun) {
    if (noun.ends_with('s') || noun.ends_with('x') || noun.ends_with("ch")) {
        return noun + "es";
    }
    return noun + 's';
}

// "clip", "group \"Lights\"", "3 objects" or "mask and 4 more".
[[nodiscard]] std::string subject_of(const std::vector<const doc::Node*>& nodes) {
    if (nodes.empty()) {
        return "nothing";
    }
    const std::string first = noun_of(*nodes.front());
    if (nodes.size() == 1U) {
        const std::string name = name_of(*nodes.front());
        return name.empty() ? first : first + " \"" + name + '"';
    }
    const bool same = std::all_of(
        nodes.begin(), nodes.end(),
        [&](const doc::Node* node) { return noun_of(*node) == first; });
    if (same) {
        return std::to_string(nodes.size()) + ' ' + plural(first);
    }
    return first + " and " + std::to_string(nodes.size() - 1U) + " more";
}

// The single property the modified nodes share, if exactly one changed.
[[nodiscard]] std::string changed_property(
    const std::vector<const NetChange*>& modified) {
    std::set<std::string, std::less<>> bases;
    for (const NetChange* change : modified) {
        const doc::PropertyMap& before = change->before->properties;
        const doc::PropertyMap& after = change->after->properties;
        for (const auto& [key, value] : before) {
            if (is_runtime_property(key)) {
                continue;
            }
            const auto other = after.find(key);
            if (other == after.end() || other->second != value) {
                bases.emplace(property_base(key));
            }
        }
        for (const auto& [key, value] : after) {
            if (!is_runtime_property(key) && !before.contains(key)) {
                bases.emplace(property_base(key));
            }
        }
    }
    return bases.size() == 1U ? words(*bases.begin()) : std::string{};
}

[[nodiscard]] bool changes_only_runtime(const NetChange& change) {
    const doc::PropertyMap& before = change.before->properties;
    const doc::PropertyMap& after = change.after->properties;
    const auto authored = [](const doc::PropertyMap& map) {
        doc::PropertyMap result;
        for (const auto& [key, value] : map) {
            if (!is_runtime_property(key)) {
                result.emplace(key, value);
            }
        }
        return result;
    };
    return change.before->type == change.after->type &&
           authored(before) == authored(after);
}

} // namespace

editor::HistoryStep describe_history_step(
    std::span<const cmd::Command> commands, bool applied) {
    editor::HistoryStep step;
    step.applied = applied;

    std::vector<const doc::Node*> added;
    std::vector<const doc::Node*> deleted;
    std::vector<const doc::Node*> modified_nodes;
    std::vector<const NetChange*> modified;
    const std::vector<NetChange> changes = net_changes(commands, applied);
    for (const NetChange& change : changes) {
        if (!change.before && change.after) {
            added.push_back(&*change.after);
        } else if (change.before && !change.after) {
            deleted.push_back(&*change.before);
        } else if (change.before && change.after &&
                   !changes_only_runtime(change)) {
            modified.push_back(&change);
            modified_nodes.push_back(&*change.after);
        } else {
            continue;
        }
        step.nodes.push_back(change.id);
    }

    const std::size_t groups = static_cast<std::size_t>(!added.empty()) +
                               static_cast<std::size_t>(!deleted.empty()) +
                               static_cast<std::size_t>(!modified.empty());
    if (groups > 1U) {
        step.kind = editor::HistoryStepKind::Mixed;
    } else if (!added.empty()) {
        step.kind = editor::HistoryStepKind::Add;
    } else if (!deleted.empty()) {
        step.kind = editor::HistoryStepKind::Delete;
    } else {
        step.kind = editor::HistoryStepKind::Change;
    }

    // A plateset placement or take swap touches a track, a clip and wiring
    // per plate; what the operator did was one thing.
    const auto plates = [](const auto& nodes) {
        return static_cast<std::size_t>(std::count_if(
            nodes.begin(), nodes.end(), [](const doc::Node* node) {
                return node->type == scene::schema::media_type &&
                       node->properties.contains(scene::schema::plateset_take);
            }));
    };
    const std::size_t swapped = static_cast<std::size_t>(std::count_if(
        modified.begin(), modified.end(), [](const NetChange* change) {
            const auto set = [](const doc::Node& node) {
                const auto found =
                    node.properties.find(scene::schema::plateset_asset);
                return found == node.properties.end()
                           ? doc::PropertyValue{std::int64_t{0}}
                           : found->second;
            };
            return change->after->type == scene::schema::media_type &&
                   set(*change->before) != set(*change->after);
        }));
    if (const std::size_t placed = plates(added); placed > 0U) {
        step.label = "Place plateset (" + std::to_string(placed) +
                     (placed == 1U ? " plate)" : " plates)");
    } else if (swapped > 0U) {
        step.label = "Swap take (" + std::to_string(swapped) +
                     (swapped == 1U ? " plate)" : " plates)");
    } else
    // Adding or deleting usually renumbers or rewires its neighbours too; the
    // node that came or went is what the operator did.
    if (!added.empty()) {
        step.label = "Add " + subject_of(added);
    } else if (!deleted.empty()) {
        step.label = "Delete " + subject_of(deleted);
    } else if (!modified.empty()) {
        const std::string property = changed_property(modified);
        step.label = property.empty()
            ? "Change " + subject_of(modified_nodes)
            : "Change " + property + " of " + subject_of(modified_nodes);
    } else {
        step.label = "No visible change";
    }
    return step;
}

void HistoryDescriptions::name(cmd::UndoContextId context,
                                 cmd::TransactionId transaction,
                                 std::string label) {
    pending_.insert_or_assign(Key{context, transaction}, std::move(label));
}

editor::HistoryContextState HistoryDescriptions::describe(
    const cmd::DirectorScheduler& scheduler, cmd::UndoContextId context) {
    const cmd::HistoryView view = scheduler.history_view(context);
    editor::HistoryContextState state;
    state.retired = view.retired;
    state.revision = view.revision;
    state.steps.reserve(view.steps.size());

    std::set<cmd::HistoryStepId> listed;
    for (const cmd::HistoryStepView& entry : view.steps) {
        listed.insert(entry.step);
        const StepKey key{context, entry.step};
        if (const auto pending = pending_.find(Key{context, entry.transaction});
            pending != pending_.end()) {
            names_.insert_or_assign(key, std::move(pending->second));
            pending_.erase(pending);
            described_.erase(key);
        }
        // Undo and redo replace the transaction; a coalesced drag appends
        // commands. Anything else reuses the earlier description.
        auto cached = described_.find(key);
        if (cached == described_.end() ||
            cached->second.transaction != entry.transaction ||
            cached->second.commands != entry.commands.size()) {
            editor::HistoryStep step =
                describe_history_step(entry.commands, entry.applied);
            step.step = entry.step.value();
            if (const auto named = names_.find(key); named != names_.end()) {
                step.label = named->second;
            }
            cached = described_
                         .insert_or_assign(key, Described{entry.transaction,
                                                          entry.commands.size(),
                                                          std::move(step)})
                         .first;
        }
        state.steps.push_back(cached->second.step);
    }

    // A step that was discarded, retired or never accepted is dropped here so
    // no map outgrows the bounded history.
    const auto unlisted = [&](const auto& entry) {
        return entry.first.context == context &&
               !listed.contains(entry.first.step);
    };
    std::erase_if(names_, unlisted);
    std::erase_if(described_, unlisted);
    std::erase_if(pending_, [&](const auto& entry) {
        return entry.first.context == context;
    });
    return state;
}

} // namespace mediaserver::app
