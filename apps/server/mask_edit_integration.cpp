#include "mask_edit_integration.hpp"

#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>
#include <variant>

namespace mediaserver::app {
namespace {

using Error = scene::MaskAuthoringError;
using ErrorCode = scene::MaskAuthoringErrorCode;
using Result = MaskEditIntegrationResult;

[[nodiscard]] Result failure(ErrorCode code, doc::NodeId subject = {},
                             doc::NodeId related = {}) {
    return Result::failure(Error{code, subject, related});
}

[[nodiscard]] core::u64 next_after(const scene::MaskAuthoringPlan& plan,
                                   core::u64 next_node) noexcept {
    for (const doc::Node& node : plan.puts) {
        if (node.id.value() >= next_node &&
            node.id.value() != std::numeric_limits<core::u64>::max()) {
            next_node = node.id.value() + 1U;
        }
    }
    return next_node;
}

[[nodiscard]] Result finished(scene::MaskAuthoringPlan plan,
                              core::u64 next_node) {
    next_node = next_after(plan, next_node);
    return Result::success(MaskEditIntegrationPlan{
        std::move(plan.after), next_node, plan.mask, plan.point});
}

struct MaskLookup final {
    std::vector<scene::MaskInstance> masks;
    const scene::MaskInstance* mask{};
};

[[nodiscard]] MaskLookup find_mask(const doc::Root& document,
                                   scene::SceneId scene_id,
                                   doc::NodeId mask_id) {
    scene::SceneModel model;
    model.reset(document);
    MaskLookup lookup{};
    lookup.masks = model.masks(scene_id);
    const auto found = std::find_if(
        lookup.masks.begin(), lookup.masks.end(),
        [mask_id](const scene::MaskInstance& mask) noexcept {
            return mask.source == mask_id;
        });
    lookup.mask = found == lookup.masks.end() ? nullptr : &*found;
    return lookup;
}

[[nodiscard]] std::optional<Error> validate_target(
    const doc::Root& document, scene::SceneId scene_id,
    doc::NodeId target, doc::NodeId mask_id) {
    MaskLookup lookup = find_mask(document, scene_id, mask_id);
    if (lookup.mask == nullptr) {
        return Error{ErrorCode::MaskNotFound, mask_id, {}};
    }
    if (lookup.mask->target != target) {
        return Error{ErrorCode::InvalidMask, mask_id, target};
    }
    return std::nullopt;
}

[[nodiscard]] scene::MaskBounds default_bounds(
    const editor::MaskCreateModifiers& modifiers) noexcept {
    if (modifiers.constrain_aspect) {
        return scene::MaskBounds{0.25F, 0.25F, 0.75F, 0.75F};
    }
    // A bounded centred shape is immediately visible and remains editable by
    // the Stage. `from_center` is already satisfied by these symmetric bounds;
    // it becomes material when a future press-drag shape gesture supplies an
    // asymmetric start/end pair.
    return scene::MaskBounds{0.20F, 0.25F, 0.80F, 0.75F};
}

[[nodiscard]] scene::BezierMaskShape default_bezier() {
    // A closed four-point path with collinear third-point handles. It reads as
    // a soft ellipse, yet every authored handle is explicit and deterministic.
    return scene::BezierMaskShape{{
        scene::MaskPointGeometry{0.50F, 0.20F, 0.335F, 0.20F,
                                 0.665F, 0.20F},
        scene::MaskPointGeometry{0.80F, 0.50F, 0.80F, 0.335F,
                                 0.80F, 0.665F},
        scene::MaskPointGeometry{0.50F, 0.80F, 0.665F, 0.80F,
                                 0.335F, 0.80F},
        scene::MaskPointGeometry{0.20F, 0.50F, 0.20F, 0.665F,
                                 0.20F, 0.335F},
    }};
}

[[nodiscard]] Result create_mask(
    const doc::Root& document, const editor::MaskCreateIntent& intent,
    core::u64 next_node) {
    const doc::Node* const target = document.get(intent.target);
    if (target == nullptr) {
        return failure(ErrorCode::TargetMissing, intent.target);
    }
    const scene::MaskCoordinateSpace expected =
        target->type == scene::schema::group_type
            ? scene::MaskCoordinateSpace::AuthoredCanvas
            : scene::MaskCoordinateSpace::SourceLayer;
    if (intent.coordinate_space != expected) {
        return failure(ErrorCode::InvalidShape, intent.target);
    }

    scene::SceneModel model;
    model.reset(document);
    core::i64 order = -1;
    for (const scene::MaskInstance& mask : model.masks(intent.scene)) {
        if (mask.target == intent.target) {
            order = std::max(order, mask.order);
        }
    }
    if (order == std::numeric_limits<core::i64>::max()) {
        return failure(ErrorCode::InvalidValue, intent.target);
    }

    scene::CreateMaskRequest request{};
    request.scene = intent.scene;
    request.target = intent.target;
    request.order = order + 1;
    const scene::MaskBounds bounds = default_bounds(intent.modifiers);
    switch (intent.tool) {
    case editor::MaskCreateTool::Rectangle:
        request.name = "Rectangle Mask";
        request.shape = scene::RectangleMaskShape{bounds};
        break;
    case editor::MaskCreateTool::Ellipse:
        request.name = "Ellipse Mask";
        request.shape = scene::EllipseMaskShape{bounds};
        break;
    case editor::MaskCreateTool::Bezier:
        request.name = "Bezier Mask";
        request.shape = default_bezier();
        break;
    }
    auto planned = scene::plan_create_mask(
        document, std::move(request), doc::NodeId{next_node});
    return planned ? finished(std::move(planned).value(), next_node)
                   : Result::failure(planned.error());
}

[[nodiscard]] Result translate_mask(
    const doc::Root& document,
    const editor::MaskViewportTranslateIntent& intent,
    core::u64 next_node) {
    if (!std::isfinite(intent.delta_x) || !std::isfinite(intent.delta_y)) {
        return failure(ErrorCode::InvalidValue, intent.mask);
    }
    MaskLookup lookup = find_mask(document, intent.scene, intent.mask);
    if (lookup.mask == nullptr) {
        return failure(ErrorCode::MaskNotFound, intent.mask);
    }
    if (lookup.mask->target != intent.target) {
        return failure(ErrorCode::InvalidMask, intent.mask, intent.target);
    }
    if (lookup.mask->kind != scene::MaskKind::Bezier) {
        const scene::MaskBounds& before = lookup.mask->bounds;
        scene::MaskPropertyPatch patch{};
        patch.bounds = scene::MaskBounds{
            before.left + intent.delta_x, before.top + intent.delta_y,
            before.right + intent.delta_x, before.bottom + intent.delta_y};
        auto planned = scene::plan_edit_mask_properties(
            document, intent.scene, intent.mask, patch);
        return planned ? finished(std::move(planned).value(), next_node)
                       : Result::failure(planned.error());
    }
    if (lookup.mask->points.empty()) {
        return failure(ErrorCode::InvalidMask, intent.mask);
    }

    doc::Root working = document;
    doc::NodeId last_point{};
    for (const scene::MaskPoint& point : lookup.mask->points) {
        scene::MaskPointPatch patch{};
        patch.x = point.x + intent.delta_x;
        patch.y = point.y + intent.delta_y;
        patch.in_x = point.in_x + intent.delta_x;
        patch.in_y = point.in_y + intent.delta_y;
        patch.out_x = point.out_x + intent.delta_x;
        patch.out_y = point.out_y + intent.delta_y;
        auto planned = scene::plan_edit_mask_point(
            working, intent.scene, intent.mask, point.source, patch);
        if (!planned) {
            return Result::failure(planned.error());
        }
        working = std::move(planned).value().after;
        last_point = point.source;
    }
    return Result::success(MaskEditIntegrationPlan{
        std::move(working), next_node, intent.mask, last_point});
}

[[nodiscard]] Result plan_panel_intent(
    const doc::Root& document, const editor::MasksPanelIntent& request,
    core::u64 next_node) {
    return std::visit(
        [&](const auto& intent) -> Result {
            using Intent = std::decay_t<decltype(intent)>;
            if constexpr (std::is_same_v<Intent,
                                         editor::MaskCreateIntent>) {
                return create_mask(document, intent, next_node);
            } else {
                if (const auto invalid = validate_target(
                        document, intent.scene, intent.target, intent.mask)) {
                    return Result::failure(*invalid);
                }
                if constexpr (std::is_same_v<Intent,
                                             editor::MaskRenameIntent>) {
                    auto planned = scene::plan_rename_mask(
                        document, intent.scene, intent.mask, intent.name);
                    return planned
                        ? finished(std::move(planned).value(), next_node)
                        : Result::failure(planned.error());
                } else if constexpr (std::is_same_v<
                                         Intent,
                                         editor::MaskEnabledIntent>) {
                    auto planned = scene::plan_set_mask_enabled(
                        document, intent.scene, intent.mask, intent.enabled);
                    return planned
                        ? finished(std::move(planned).value(), next_node)
                        : Result::failure(planned.error());
                } else if constexpr (std::is_same_v<
                                         Intent,
                                         editor::MaskCombineIntent>) {
                    auto planned = scene::plan_set_mask_combine(
                        document, intent.scene, intent.mask, intent.combine);
                    return planned
                        ? finished(std::move(planned).value(), next_node)
                        : Result::failure(planned.error());
                } else if constexpr (std::is_same_v<
                                         Intent,
                                         editor::MaskInvertIntent>) {
                    auto planned = scene::plan_set_mask_invert(
                        document, intent.scene, intent.mask, intent.invert);
                    return planned
                        ? finished(std::move(planned).value(), next_node)
                        : Result::failure(planned.error());
                } else if constexpr (std::is_same_v<
                                         Intent,
                                         editor::MaskScalarEditIntent>) {
                    scene::MaskPropertyPatch patch{};
                    switch (intent.property) {
                    case editor::MaskScalarProperty::FeatherCanvasPx:
                        patch.feather_canvas_px = intent.value;
                        break;
                    case editor::MaskScalarProperty::ExpansionCanvasPx:
                        patch.expansion_canvas_px = intent.value;
                        break;
                    case editor::MaskScalarProperty::Opacity:
                        patch.opacity = intent.value;
                        break;
                    }
                    auto planned = scene::plan_edit_mask_properties(
                        document, intent.scene, intent.mask, patch);
                    return planned
                        ? finished(std::move(planned).value(), next_node)
                        : Result::failure(planned.error());
                } else if constexpr (std::is_same_v<
                                         Intent,
                                         editor::MaskBoundsEditIntent>) {
                    scene::MaskPropertyPatch patch{};
                    patch.bounds = intent.bounds;
                    auto planned = scene::plan_edit_mask_properties(
                        document, intent.scene, intent.mask, patch);
                    return planned
                        ? finished(std::move(planned).value(), next_node)
                        : Result::failure(planned.error());
                } else if constexpr (std::is_same_v<
                                         Intent,
                                         editor::MaskReorderIntent>) {
                    auto planned = scene::plan_reorder_mask(
                        document, intent.scene, intent.mask,
                        intent.destination_index);
                    return planned
                        ? finished(std::move(planned).value(), next_node)
                        : Result::failure(planned.error());
                } else if constexpr (std::is_same_v<
                                         Intent,
                                         editor::MaskDeleteIntent>) {
                    auto planned = scene::plan_delete_mask(
                        document, intent.scene, intent.mask);
                    return planned
                        ? finished(std::move(planned).value(), next_node)
                        : Result::failure(planned.error());
                } else if constexpr (std::is_same_v<
                                         Intent,
                                         editor::MaskPointInsertIntent>) {
                    auto planned = scene::plan_insert_mask_point(
                        document, intent.scene, intent.mask, intent.index,
                        intent.geometry, doc::NodeId{next_node});
                    return planned
                        ? finished(std::move(planned).value(), next_node)
                        : Result::failure(planned.error());
                } else if constexpr (std::is_same_v<
                                         Intent,
                                         editor::MaskPointEditIntent>) {
                    auto planned = scene::plan_edit_mask_point(
                        document, intent.scene, intent.mask, intent.point,
                        intent.patch);
                    return planned
                        ? finished(std::move(planned).value(), next_node)
                        : Result::failure(planned.error());
                } else {
                    static_assert(std::is_same_v<
                                  Intent,
                                  editor::MaskPointDeleteIntent>);
                    auto planned = scene::plan_delete_mask_point(
                        document, intent.scene, intent.mask, intent.point);
                    return planned
                        ? finished(std::move(planned).value(), next_node)
                        : Result::failure(planned.error());
                }
            }
        },
        request);
}

} // namespace

MaskEditIntegrationResult plan_mask_edit_integration(
    const doc::Root& document, const editor::MaskEditIntent& edit,
    core::u64 next_node) {
    try {
        if (next_node == 0U) {
            return failure(ErrorCode::InvalidFirstId);
        }
        return std::visit(
            [&](const auto& request) -> Result {
                using Request = std::decay_t<decltype(request)>;
                if constexpr (std::is_same_v<Request,
                                             editor::MasksPanelIntent>) {
                    return plan_panel_intent(document, request, next_node);
                } else {
                    static_assert(std::is_same_v<
                                  Request,
                                  editor::MaskViewportTranslateIntent>);
                    return translate_mask(document, request, next_node);
                }
            },
            edit.request);
    } catch (const std::bad_alloc&) {
        return failure(ErrorCode::AllocationFailed);
    } catch (const std::length_error&) {
        return failure(ErrorCode::AllocationFailed);
    }
}

} // namespace mediaserver::app
