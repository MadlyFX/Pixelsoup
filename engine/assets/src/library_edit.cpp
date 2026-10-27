#include <mediaserver/assets/library_edit.hpp>

#include <mediaserver/assets/asset_schema.hpp>

#include <cstdint>
#include <string_view>
#include <utility>

namespace mediaserver::assets {
namespace {

[[nodiscard]] bool is_asset(const doc::Node& node) noexcept {
    return node.type == schema::media_type ||
        node.type == schema::model_type ||
        node.type == schema::projection_mesh_type;
}

[[nodiscard]] bool is_folder(const doc::Node& node) noexcept {
    return node.type == schema::folder_type;
}

[[nodiscard]] doc::NodeId folder_of(const doc::Node& node) noexcept {
    const auto found = node.properties.find(schema::folder);
    if (found == node.properties.end()) {
        return {};
    }
    const auto* const value = std::get_if<std::int64_t>(&found->second);
    return value != nullptr && *value > 0
               ? doc::NodeId{static_cast<core::u64>(*value)}
               : doc::NodeId{};
}

void file_under(doc::Node& node, doc::NodeId folder) {
    if (folder == doc::NodeId{}) {
        node.properties.erase(std::string{schema::folder});
    } else {
        node.properties.insert_or_assign(
            std::string{schema::folder},
            doc::PropertyValue{static_cast<std::int64_t>(folder.value())});
    }
}

// True when `folder` is `ancestor` or lies beneath it in `root`.
[[nodiscard]] bool within(const doc::Root& root, doc::NodeId folder,
                          doc::NodeId ancestor) noexcept {
    for (std::size_t steps = 0U; steps <= root.size(); ++steps) {
        if (folder == ancestor) {
            return true;
        }
        const doc::Node* const node = root.get(folder);
        if (node == nullptr || !is_folder(*node)) {
            return false;
        }
        folder = folder_of(*node);
    }
    return false;
}

} // namespace

std::optional<doc::Root> plan_library_edit(const doc::Root& root,
                                           const LibraryEdit& edit,
                                           doc::NodeId new_folder) {
    const auto destination_valid = [&root](doc::NodeId destination) {
        if (destination == doc::NodeId{}) {
            return true;
        }
        const doc::Node* const node = root.get(destination);
        return node != nullptr && is_folder(*node);
    };

    switch (edit.kind) {
    case LibraryEdit::Kind::CreateFolder: {
        if (new_folder == doc::NodeId{} || root.contains(new_folder) ||
            edit.name.empty() || !destination_valid(edit.destination)) {
            return std::nullopt;
        }
        doc::Node folder{};
        folder.id = new_folder;
        folder.type = std::string{schema::folder_type};
        folder.properties.emplace(std::string{schema::display_name},
                                  doc::PropertyValue{edit.name});
        file_under(folder, edit.destination);
        return root.set(std::move(folder));
    }
    case LibraryEdit::Kind::Rename: {
        const doc::Node* const existing = root.get(edit.target);
        if (existing == nullptr || edit.name.empty() ||
            (!is_asset(*existing) && !is_folder(*existing))) {
            return std::nullopt;
        }
        doc::Node changed = *existing;
        changed.properties.insert_or_assign(std::string{schema::display_name},
                                            doc::PropertyValue{edit.name});
        if (changed == *existing) {
            return std::nullopt;
        }
        return root.set(std::move(changed));
    }
    case LibraryEdit::Kind::Move: {
        if (!destination_valid(edit.destination)) {
            return std::nullopt;
        }
        doc::Root after = root;
        bool modified = false;
        for (const doc::NodeId item : edit.items) {
            const doc::Node* const existing = after.get(item);
            if (existing == nullptr ||
                (!is_asset(*existing) && !is_folder(*existing))) {
                continue;
            }
            if (is_folder(*existing) &&
                within(after, edit.destination, item)) {
                continue;
            }
            if (folder_of(*existing) == edit.destination) {
                continue;
            }
            doc::Node changed = *existing;
            file_under(changed, edit.destination);
            after = after.set(std::move(changed));
            modified = true;
        }
        return modified ? std::optional<doc::Root>{std::move(after)}
                        : std::nullopt;
    }
    case LibraryEdit::Kind::DeleteFolder: {
        const doc::Node* const existing = root.get(edit.target);
        if (existing == nullptr || !is_folder(*existing)) {
            return std::nullopt;
        }
        const doc::NodeId parent = folder_of(*existing);
        std::optional<doc::Root> after = root.erase(edit.target);
        if (!after.has_value()) {
            return std::nullopt;
        }
        const doc::Delta contents = doc::diff(doc::Root{}, root);
        for (const doc::NodeChange& change : contents.changes) {
            if (!change.after || change.id == edit.target ||
                (!is_asset(*change.after) && !is_folder(*change.after)) ||
                folder_of(*change.after) != edit.target) {
                continue;
            }
            doc::Node changed = *change.after;
            file_under(changed, parent);
            after = after->set(std::move(changed));
        }
        return after;
    }
    }
    return std::nullopt;
}

} // namespace mediaserver::assets
