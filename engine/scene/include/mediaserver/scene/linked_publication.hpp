#pragma once
#include <mediaserver/doc/document.hpp>
#include <mediaserver/scene/scene_schema.hpp>

namespace mediaserver::scene::publication {
// Persisted on scene stages; only candidate and activation are transient.
inline constexpr std::string_view candidate = "publication_candidate";
inline constexpr std::string_view rollback = "publication_rollback";
inline constexpr std::string_view previous = "publication_previous";
inline constexpr std::string_view revision = "publication_revision";
inline constexpr std::string_view manifest = "publication_manifest";
inline constexpr std::string_view manifest_hash = "publication_manifest_hash";
inline constexpr std::string_view storage = "publication_storage";
inline constexpr std::string_view settings_type = "publication.settings";
inline constexpr std::string_view document_revision = "publication_document_revision";
// The authoring scene a frozen revision was taken from, so the operator can go
// back to it (and to its Blender file) from the revision on Program.
inline constexpr std::string_view source_scene = "publication_source_scene";
inline core::u64 integer(const doc::Node* node, std::string_view key) noexcept {
    if (node) {
        const auto it = node->properties.find(key);
        if (it != node->properties.end()) if (const auto* n = std::get_if<core::i64>(&it->second))
            return static_cast<core::u64>(*n);
    }
    return 0;
}
inline std::string text(const doc::Node* node, std::string_view key) {
    if (node) {
        const auto it = node->properties.find(key);
        if (it != node->properties.end()) if (const auto* s = std::get_if<std::string>(&it->second)) return *s;
    }
    return {};
}
inline doc::NodeId render_scene(const doc::Root& root, doc::NodeId program) noexcept {
    const doc::NodeId pending{integer(root.get(program), candidate)};
    return pending.value() && root.get(pending) ? pending : program;
}
inline bool linked(const doc::Root& root, doc::NodeId scene) {
    for (const auto& c : doc::diff({}, root).changes)
        if (c.after->type == "link.instance" && integer(c.after.get(), schema::scene_id) == scene.value()) return true;
    return false;
}
} // namespace mediaserver::scene::publication
