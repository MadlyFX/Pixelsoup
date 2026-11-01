#include <mediaserver/assets/library_edit.hpp>

#include <mediaserver/assets/asset_schema.hpp>
#include <mediaserver/assets/plateset.hpp>

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <utility>

namespace mediaserver::assets {
namespace {

[[nodiscard]] bool is_asset(const doc::Node& node) noexcept {
    return node.type == schema::media_type ||
        node.type == schema::model_type ||
        node.type == schema::projection_mesh_type ||
        node.type == schema::plateset_type ||
        node.type == schema::particle_effect_type;
}

[[nodiscard]] bool is_plateset(const doc::Node& node) noexcept {
    return node.type == schema::plateset_type;
}

[[nodiscard]] bool is_media(const doc::Root& root, doc::NodeId id) noexcept {
    const doc::Node* const node = root.get(id);
    return node != nullptr && node->type == schema::media_type;
}

void assign_slot(doc::Node& node, PlatesetSlot slot, doc::NodeId media) {
    if (media == doc::NodeId{}) {
        node.properties.erase(plateset_slot_property(slot));
    } else {
        node.properties.insert_or_assign(
            plateset_slot_property(slot),
            doc::PropertyValue{static_cast<std::int64_t>(media.value())});
    }
}

void assign_offset(doc::Node& node, PlatesetSlot slot, core::i64 offset) {
    if (offset == 0) {
        node.properties.erase(plateset_offset_property(slot));
    } else {
        node.properties.insert_or_assign(
            plateset_offset_property(slot),
            doc::PropertyValue{static_cast<std::int64_t>(offset)});
    }
}

[[nodiscard]] doc::NodeId slot_media(const doc::Node& node,
                                     PlatesetSlot slot) {
    const auto found = node.properties.find(plateset_slot_property(slot));
    if (found == node.properties.end()) {
        return {};
    }
    const auto* const value = std::get_if<std::int64_t>(&found->second);
    return value != nullptr && *value > 0
               ? doc::NodeId{static_cast<core::u64>(*value)}
               : doc::NodeId{};
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
    case LibraryEdit::Kind::CreatePlateset: {
        if (new_folder == doc::NodeId{} || root.contains(new_folder) ||
            edit.name.empty() || !destination_valid(edit.destination)) {
            return std::nullopt;
        }
        doc::Node plateset{};
        plateset.id = new_folder;
        plateset.type = std::string{schema::plateset_type};
        plateset.properties.emplace(std::string{schema::display_name},
                                    doc::PropertyValue{edit.name});
        if (edit.rig != PlatesetRig::Nine) {
            plateset.properties.emplace(
                std::string{schema::plateset_rig},
                doc::PropertyValue{std::string{plateset_rig_key(edit.rig)}});
        }
        for (const PlatesetSlot slot : all_plateset_slots) {
            const doc::NodeId media =
                edit.slots[static_cast<std::size_t>(slot)];
            if (media != doc::NodeId{} && is_media(root, media)) {
                assign_slot(plateset, slot, media);
            }
        }
        file_under(plateset, edit.destination);
        return root.set(std::move(plateset));
    }
    case LibraryEdit::Kind::SetPlatesetSlot: {
        const doc::Node* const existing = root.get(edit.target);
        if (existing == nullptr || !is_plateset(*existing) ||
            (edit.media != doc::NodeId{} && !is_media(root, edit.media))) {
            return std::nullopt;
        }
        doc::Node changed = *existing;
        if (edit.media != doc::NodeId{}) {
            for (const PlatesetSlot other : all_plateset_slots) {
                if (other != edit.slot &&
                    slot_media(changed, other) == edit.media) {
                    assign_slot(changed, other, {});
                    assign_offset(changed, other, 0);
                }
            }
        }
        if (slot_media(changed, edit.slot) != edit.media) {
            assign_offset(changed, edit.slot, 0);
        }
        assign_slot(changed, edit.slot, edit.media);
        if (changed == *existing) {
            return std::nullopt;
        }
        return root.set(std::move(changed));
    }
    case LibraryEdit::Kind::SetPlatesetRig: {
        const doc::Node* const existing = root.get(edit.target);
        if (existing == nullptr || !is_plateset(*existing)) {
            return std::nullopt;
        }
        doc::Node changed = *existing;
        if (edit.rig == PlatesetRig::Nine) {
            changed.properties.erase(std::string{schema::plateset_rig});
        } else {
            changed.properties.insert_or_assign(
                std::string{schema::plateset_rig},
                doc::PropertyValue{std::string{plateset_rig_key(edit.rig)}});
        }
        if (changed == *existing) {
            return std::nullopt;
        }
        return root.set(std::move(changed));
    }
    case LibraryEdit::Kind::SetPlatesetOffsets: {
        const doc::Node* const existing = root.get(edit.target);
        if (existing == nullptr || !is_plateset(*existing)) {
            return std::nullopt;
        }
        doc::Node changed = *existing;
        for (const PlatesetSlot slot : all_plateset_slots) {
            const core::i64 offset =
                edit.offsets[static_cast<std::size_t>(slot)];
            if (offset < 0) {
                return std::nullopt;
            }
            assign_offset(changed, slot,
                          slot_media(changed, slot) == doc::NodeId{} ? 0
                                                                     : offset);
        }
        if (changed == *existing) {
            return std::nullopt;
        }
        return root.set(std::move(changed));
    }
    case LibraryEdit::Kind::SetPlatesetMoments: {
        const doc::Node* const existing = root.get(edit.target);
        if (existing == nullptr || !is_plateset(*existing) ||
            edit.moments.size() > max_plateset_moments) {
            return std::nullopt;
        }
        doc::Node changed = *existing;
        std::erase_if(changed.properties, [](const auto& entry) {
            return std::string_view{entry.first}.starts_with(
                schema::plateset_moment_prefix);
        });
        std::vector<PlatesetMoment> moments = edit.moments;
        std::stable_sort(moments.begin(), moments.end(),
                         [](const PlatesetMoment& left,
                            const PlatesetMoment& right) {
                             return left.frame < right.frame;
                         });
        for (std::size_t index = 0U; index < moments.size(); ++index) {
            if (moments[index].frame < 0) {
                return std::nullopt;
            }
            changed.properties.insert_or_assign(
                plateset_moment_property(index, "frame"),
                doc::PropertyValue{static_cast<std::int64_t>(moments[index].frame)});
            changed.properties.insert_or_assign(
                plateset_moment_property(index, "kind"),
                doc::PropertyValue{
                    std::string{plateset_moment_key(moments[index].kind)}});
        }
        if (changed == *existing) {
            return std::nullopt;
        }
        return root.set(std::move(changed));
    }
    case LibraryEdit::Kind::SetPlatesetHalfResolution: {
        const doc::Node* const existing = root.get(edit.target);
        if (existing == nullptr || !is_plateset(*existing)) {
            return std::nullopt;
        }
        doc::Node changed = *existing;
        if (edit.half) {
            changed.properties.insert_or_assign(
                std::string{schema::plateset_half_resolution},
                doc::PropertyValue{true});
        } else {
            changed.properties.erase(
                std::string{schema::plateset_half_resolution});
        }
        if (changed == *existing) {
            return std::nullopt;
        }
        return root.set(std::move(changed));
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
