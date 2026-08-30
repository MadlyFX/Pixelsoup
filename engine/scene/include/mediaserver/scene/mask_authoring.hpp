#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/scene/scene.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// Pure, deterministic authoring for first-class scene masks. Every successful
// operation returns one complete replacement Root plus sorted puts/erases, so
// callers schedule exactly one undoable document transaction. No editor state,
// renderer state, implicit scene ownership, or local id allocator participates.
namespace mediaserver::scene {

struct MaskPointGeometry final {
    float x{};
    float y{};
    float in_x{};
    float in_y{};
    float out_x{};
    float out_y{};

    bool operator==(const MaskPointGeometry&) const = default;
};

struct RectangleMaskShape final {
    MaskBounds bounds{};
    bool operator==(const RectangleMaskShape&) const = default;
};

struct EllipseMaskShape final {
    MaskBounds bounds{};
    bool operator==(const EllipseMaskShape&) const = default;
};

struct BezierMaskShape final {
    // Authored closed-path order. Child node ids are allocated independently
    // and never used to reinterpret this order.
    std::vector<MaskPointGeometry> points;
    bool operator==(const BezierMaskShape&) const = default;
};

using MaskShape =
    std::variant<RectangleMaskShape, EllipseMaskShape, BezierMaskShape>;

struct CreateMaskRequest final {
    SceneId scene{};
    doc::NodeId target{};
    std::string name{"Mask"};
    MaskCombine combine{MaskCombine::Add};
    core::i64 order{};
    bool enabled{true};
    float feather_canvas_px{};
    float expansion_canvas_px{};
    float opacity{1.0F};
    bool invert{};
    MaskShape shape{RectangleMaskShape{}};

    bool operator==(const CreateMaskRequest&) const = default;
};

struct MaskPropertyPatch final {
    std::optional<bool> enabled;
    std::optional<MaskCombine> combine;
    std::optional<bool> invert;
    std::optional<float> feather_canvas_px;
    std::optional<float> expansion_canvas_px;
    std::optional<float> opacity;
    // Rectangle/Ellipse only. Bezier geometry lives exclusively in child
    // nodes, so accepting bounds there would create a hidden second shape.
    std::optional<MaskBounds> bounds;
    std::optional<float> rotation_turns;

    bool operator==(const MaskPropertyPatch&) const = default;
};

struct MaskPointPatch final {
    std::optional<float> x;
    std::optional<float> y;
    std::optional<float> in_x;
    std::optional<float> in_y;
    std::optional<float> out_x;
    std::optional<float> out_y;

    bool operator==(const MaskPointPatch&) const = default;
};

enum class MaskAuthoringErrorCode : core::u8 {
    InvalidScene,
    TargetMissing,
    UnsupportedTarget,
    MissingTargetSceneOwner,
    CrossSceneTarget,
    MaskCapacity,
    PointCapacity,
    InvalidName,
    NameTooLong,
    InvalidValue,
    InvalidShape,
    InvalidFirstId,
    IdExhausted,
    MaskNotFound,
    NotAMask,
    InvalidMask,
    CrossSceneMask,
    PointNotFound,
    NotAMaskPoint,
    InvalidPoint,
    CrossScenePoint,
    PointMaskMismatch,
    NonBezierMask,
    InvalidMaskIndex,
    InvalidPointIndex,
    MinimumBezierPoints,
    InvalidFirstActiveCombine,
    AllocationFailed,
};

struct MaskAuthoringError final {
    MaskAuthoringErrorCode code{MaskAuthoringErrorCode::InvalidMask};
    doc::NodeId subject{};
    doc::NodeId related{};

    bool operator==(const MaskAuthoringError&) const = default;
};

struct MaskAuthoringPlan final {
    doc::Root after;
    // Complete changed/created nodes and erased ids. Both are independently
    // ascending by node id and never overlap.
    std::vector<doc::Node> puts;
    std::vector<doc::NodeId> erases;
    doc::NodeId mask{};
    std::optional<doc::NodeId> point;
};

using MaskAuthoringResult =
    core::Result<MaskAuthoringPlan, MaskAuthoringError>;

// Creates one shape and, for Bezier, one child node per authored point.
// first_id starts a collision-skipping ascending id scan.
[[nodiscard]] MaskAuthoringResult plan_create_mask(
    const doc::Root& document, CreateMaskRequest request,
    doc::NodeId first_id);

[[nodiscard]] MaskAuthoringResult plan_rename_mask(
    const doc::Root& document, SceneId scene, doc::NodeId mask,
    std::string name);

// Applies only explicitly supplied fields. Finite values are canonicalized to
// the same bounds as SceneModel; equal/reversed shape bounds are rejected.
[[nodiscard]] MaskAuthoringResult plan_edit_mask_properties(
    const doc::Root& document, SceneId scene, doc::NodeId mask,
    const MaskPropertyPatch& patch);

[[nodiscard]] MaskAuthoringResult plan_set_mask_enabled(
    const doc::Root& document, SceneId scene, doc::NodeId mask,
    bool enabled);

[[nodiscard]] MaskAuthoringResult plan_set_mask_combine(
    const doc::Root& document, SceneId scene, doc::NodeId mask,
    MaskCombine combine);

[[nodiscard]] MaskAuthoringResult plan_set_mask_invert(
    const doc::Root& document, SceneId scene, doc::NodeId mask,
    bool invert);

// destination_index is the final target-local index. The sequence is rewritten
// to dense explicit order values without changing stable mask ids.
[[nodiscard]] MaskAuthoringResult plan_reorder_mask(
    const doc::Root& document, SceneId scene, doc::NodeId mask,
    std::size_t destination_index);

[[nodiscard]] MaskAuthoringResult plan_delete_mask(
    const doc::Root& document, SceneId scene, doc::NodeId mask);

// index is the final dense child index; first_id starts the same deterministic
// collision-skipping allocation used by mask creation.
[[nodiscard]] MaskAuthoringResult plan_insert_mask_point(
    const doc::Root& document, SceneId scene, doc::NodeId mask,
    std::size_t index, MaskPointGeometry geometry,
    doc::NodeId first_id);

[[nodiscard]] MaskAuthoringResult plan_edit_mask_point(
    const doc::Root& document, SceneId scene, doc::NodeId mask,
    doc::NodeId point, const MaskPointPatch& patch);

[[nodiscard]] MaskAuthoringResult plan_delete_mask_point(
    const doc::Root& document, SceneId scene, doc::NodeId mask,
    doc::NodeId point);

} // namespace mediaserver::scene
