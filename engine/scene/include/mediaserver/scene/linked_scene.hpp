#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/scene/scene.hpp>

#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace mediaserver::scene {

// Persisted scalar properties; ordinary document persistence, replication and
// undo carry the complete link. Source records are project-owned; attachments
// and occurrences carry scene_id and are cloned with Preview scenes.
namespace link_schema {
inline constexpr std::string_view source_type = "link.source";
inline constexpr std::string_view object_type = "link.object";
inline constexpr std::string_view prototype_type = "link.prototype";
inline constexpr std::string_view material_type = "link.material";
inline constexpr std::string_view instance_type = "link.instance";
inline constexpr std::string_view uuid = "link_uuid";
inline constexpr std::string_view version = "link_version";
inline constexpr std::string_view source = "link_source";
inline constexpr std::string_view object = "link_object";
inline constexpr std::string_view instance = "link_instance";
inline constexpr std::string_view parent = "link_parent";
inline constexpr std::string_view prototype = "link_prototype";
inline constexpr std::string_view material = "link_material";
inline constexpr std::string_view revision = "link_revision";
inline constexpr std::string_view origin = "link_origin";
inline constexpr std::string_view removed = "link_removed";
inline constexpr std::string_view policy = "link_policy";
inline constexpr std::string_view follow = "follow";
inline constexpr std::string_view hold = "hold";
// Prefixes followed by the column-major index 0..15, each a double.
inline constexpr std::string_view local = "link_local_";
inline constexpr std::string_view alignment = "link_alignment_";
inline constexpr std::string_view offset = "link_offset_";
inline constexpr std::string_view held_world = "link_held_";
inline constexpr std::string_view action = "link_action";
inline constexpr std::string_view paused = "link_paused";
} // namespace link_schema

struct LinkedSceneError final {
    doc::NodeId node{};
    std::string message;
};

struct LinkedOccurrence final {
    doc::NodeId node{}, instance{}, object{}, scene{};
    Mat4 world{identity()};
    // Effective upstream parent * source local, before this occurrence's shot
    // offset. Used to convert an absolute world edit without TRS decomposition.
    Mat4 basis{identity()};
    bool held{}, removed{}, visible{true};
    std::string name, source;
    Material material{};
    core::u32 material_mask{};
    std::string batch;
    doc::PropertyMap light;
    ModelAnimation animation{false};
    bool animated{};
};

struct LinkedSceneState final {
    std::map<doc::NodeId, LinkedOccurrence> occurrences;
};

using LinkedStateResult = core::Result<LinkedSceneState, LinkedSceneError>;
using LinkedPlanResult = core::Result<doc::Root, LinkedSceneError>;

[[nodiscard]] bool is_link_record(const doc::Node& node) noexcept;
[[nodiscard]] bool is_linked_occurrence(const doc::Node& node) noexcept;
[[nodiscard]] bool valid_link_affine(const Mat4& matrix) noexcept;
void write_link_matrix(doc::Node& node, std::string_view prefix, const Mat4& matrix);
[[nodiscard]] std::string link_instance_uuid(doc::NodeId id);
[[nodiscard]] LinkedStateResult read_linked_scene(const doc::Root& root);

// Admission is all-or-nothing, including graph validity, stable identities and
// origin changes. Failure never modifies root. Source deletion is a tombstone;
// its node/UUID and all shot records remain available for source undo/restore.
[[nodiscard]] LinkedPlanResult plan_linked_change(
    const doc::Root& before, const doc::Root& candidate);
[[nodiscard]] LinkedPlanResult plan_link_source_update(
    const doc::Root& root, std::span<const doc::Node> source_records,
    doc::NodeId first_id);
[[nodiscard]] LinkedPlanResult plan_attach_link(
    const doc::Root& root, doc::NodeId source, SceneId scene,
    doc::NodeId first_id, const Mat4& alignment = identity());

enum class LinkedEdit {
    Hold, Follow, ResetPlacement, Hide, FollowVisibility, ResetMaterial,
    ForgetRemoved, DuplicateLink, Pause, Resume, Unlink, Show,
};
// A clipboard snapshot contains only independent scene nodes. Immutable package
// references are retained; no Blender connection or source records are needed.
[[nodiscard]] LinkedPlanResult snapshot_link(const doc::Root& root, doc::NodeId attachment);
[[nodiscard]] LinkedPlanResult paste_link_snapshot(const doc::Root& root,
    const doc::Root& snapshot, SceneId target);
[[nodiscard]] LinkedPlanResult plan_linked_edit(
    const doc::Root& root, doc::NodeId occurrence, LinkedEdit edit);
[[nodiscard]] LinkedPlanResult plan_linked_world(
    const doc::Root& root, doc::NodeId occurrence, const Mat4& world);
[[nodiscard]] LinkedPlanResult plan_link_alignment(
    const doc::Root& root, doc::NodeId instance, const Mat4& alignment);
// Media Server's edits to a linked object are overrides on its occurrence;
// Blender shows them in the object itself. When the artist then edits that
// property in Blender, the revision carrying the edit names it here, and the
// occurrence's override returns to the source in the same transaction - the
// latest edit wins and the two never stack. Applies to `scene`'s occurrences
// of the named objects of `source_uuid`. Properties: "placement",
// "visibility", "light" and "material". Objects the revision removed are
// skipped.
[[nodiscard]] LinkedPlanResult absorb_linked_overrides(const doc::Root& root,
    std::string_view source_uuid, SceneId scene,
    std::span<const std::pair<std::string, std::string>> absorbed);

} // namespace mediaserver::scene
